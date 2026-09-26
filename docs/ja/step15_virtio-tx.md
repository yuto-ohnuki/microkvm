# Step 15: virtio-console TX (guest → host)

## 目的

ゲストが `/dev/hvc0` に書き込んだデータを、transmitq (queue 1) の Descriptor を辿って読み取り、ホストの標準出力へ送る。処理後は Used Ring を更新し、ゲストにバッファを返す。

## 背景

### virtio の TX はどう動くか

guest ドライバがデータを guest メモリに配置し、その場所を descriptor に記述し、descriptor index を available ring に追加して、QueueNotify で「kick」する。VMM はその後 guest RAM から直接データを読み取る。

```
Guest が "hello" を /dev/hvc0 に書き込む:

  avail ring               descriptor table           guest memory
  +----------+             +------------------+       +---------+
  | idx: 1   |             | [5] addr=0x1234  |  ──→  | "hello" |
  | ring[0]=5| ──────────→ |     len=5        |       +---------+
  +----------+             |     flags=0      |
                           +------------------+
```

QueueNotify への MMIO 書き込みで `KVM_EXIT_MMIO` が発生する。通知に文字列は含まれず、VMM は Descriptor が指す `ram + 0x1234` から直接 "hello" を読む。QueueNotify のオフセット `0x050` は [virtio 1.2 仕様](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html) の MMIO レジスタ定義に対応し、[Linux の定義](https://github.com/torvalds/linux/blob/master/include/uapi/linux/virtio_mmio.h) も同じ値を使う。

### Descriptor chain

1つの I/O 操作が `flags & VRING_DESC_F_NEXT` でリンクされた複数バッファにまたがることがある。VMM は NEXT フラグがない descriptor まで `next` フィールドを辿る:

```
descriptor[5]          descriptor[7]          descriptor[2]
addr=0x1000            addr=0x2000            addr=0x3000
len=100                len=200                len=50
flags=NEXT             flags=NEXT             flags=0
next=7         ──→     next=2         ──→     (chain 終端)
```

chain の最初の descriptor（**head descriptor**、この例では index 5）が `avail->ring[]` に入る。used ring に返すのもこの head index — 途中の descriptor ではない。

### TX vs RX の descriptor flags

| 方向 | Flag | 意味 |
|-----------|------|---------|
| TX (guest → host) | WRITE ビットなし | デバイスがバッファを読む。chain なら NEXT は立つ |
| RX (host → guest) | WRITE ビットあり | デバイスがバッファに書き込む |

## 実行フロー

```text
Guest (Linux)               KVM                         VMM
     | /dev/hvc0 に書き込む    |                           |
     | バッファと Descriptor を設定                         |
     | Available Ring に head を追加し、idx を進める        |
     |-- QueueNotify=1 ----->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | virtio_console_tx()
     |                       |                           | 未処理の head を取得
     |                       |                           | Descriptor chain を辿る
     |                       |                           | バッファを stdout へ出力
     |                       |                           | Used Ring に head を返す
     |                       |                           | used->idx を進める
     |                       |                           | last_avail_idx を進める
     |                       |<-- KVM_RUN ---------------|
     |<-- 実行再開 -----------|                           |
```

この TX 処理は Used Ring を更新するが、完了割り込みは注入しない。

## 実装

### virtio_mmio.h の追加

```c
#define VRING_DESC_F_NEXT   1   /* descriptor がチェインされている */
#define VRING_DESC_F_WRITE  2   /* デバイスが書き込む (RX 側) */

struct virtqueue_state {
    ...
    uint16_t last_avail_idx;    /* VMM の avail idx シャドウ */
};

struct virtio_mmio_dev {
    ...
    uint8_t *ram;       /* ゲスト RAM を割り当てたホスト側アドレス */
    size_t  ram_size;   /* bounds check 用の guest RAM サイズ */
};

/* vring 構造体 */
struct vring_desc {
    uint64_t addr;      /* バッファの GPA */
    uint32_t len;       /* バッファ長 */
    uint16_t flags;     /* NEXT, WRITE */
    uint16_t next;      /* 次の descriptor (NEXT flag 時) */
};

struct vring_used_elem {
    uint32_t id;        /* descriptor head index */
    uint32_t len;       /* デバイスが書き込んだバイト数 */
};
```

### virtio_mmio.c — TX 処理の核心

QueueNotify に書かれたキュー番号が `1` なら TX 処理を呼ぶ。QueueSel の値ではなく、通知の値で判定する。

```c
case VIRTIO_MMIO_QUEUE_NOTIFY:
    if (value == 1) {   /* transmitq */
        virtio_console_tx(dev, dev->ram, dev->ram_size);
    }
    break;
```

`virtio_console_tx()` は Step 14 の配置式をヘルパー関数で計算する。以下はその後の処理の抜粋。

```c
/* Read avail->idx (guest increments this after adding buffers) */
uint16_t avail_idx;
memcpy(&avail_idx, ram + avail_base + 2, sizeof(uint16_t));

while (vq->last_avail_idx != avail_idx) {
    /* Get descriptor head index from avail ring */
    uint16_t ring_slot = vq->last_avail_idx % vq->num;
    uint16_t desc_idx;
    memcpy(&desc_idx, ram + avail_base + 4 + ring_slot * 2, sizeof(uint16_t));
    /* 以下で chain の処理と Used Ring の更新を行う */
}
```

`last_avail_idx` は VMM が処理済みの位置を覚える16-bitカウンタ。例えば `last_avail_idx=1`、`avail_idx=3` なら、未処理なのは Available Ring の位置1・2であり、Descriptor 番号1・2とは限らない。実際の head 番号はリングの各要素から読む。

取得した head から chain を辿り、WRITE ビットがないバッファを出力する:

```c
struct vring_desc desc;
memcpy(&desc, ram + desc_base + cur * 16, sizeof(desc));

/* TX: device reads from buffer (flags should NOT have WRITE) */
if (!(desc.flags & VRING_DESC_F_WRITE)) {
    if (desc.addr < ram_size && desc.len <= ram_size - desc.addr) {
        write(STDOUT_FILENO, ram + desc.addr, desc.len);
    }
}

if (desc.flags & VRING_DESC_F_NEXT)
    cur = desc.next;
else
    break;
```

chain ごとに、最初の Descriptor 番号を Used Ring に返す:

```c
/* Post to used ring */
uint16_t used_idx;
memcpy(&used_idx, ram + used_base + 2, sizeof(uint16_t));
uint16_t used_slot = used_idx % vq->num;

struct vring_used_elem elem = { .id = desc_idx, .len = 0 };
memcpy(ram + used_base + 4 + used_slot * 8, &elem, sizeof(elem));

used_idx++;
memcpy(ram + used_base + 2, &used_idx, sizeof(uint16_t));

vq->last_avail_idx++;
```

`len=0` は、TX ではデバイスがゲストのデータバッファに書き込まないため。ホストに出力したバイト数を返す欄ではない。

範囲チェックはリングの先頭、Descriptor 番号、データバッファの範囲に対して行う。リング全体の境界や chain の循環までは検証していない。

### microkvm.c の追加

```c
/* virtio デバイスが guest メモリに直接アクセスするためのポインタ */
virtio_dev.ram = (uint8_t *)mem;
virtio_dev.ram_size = GUEST_MEM_SIZE;
```

VMM が mmap した guest RAM ポインタを virtio デバイスに渡す。`virtio_console_tx()` はこれを使って guest バッファを直接読み取る。

## 出力

起動時のログでは、2つのキュー設定と DRIVER_OK への到達を確認できる:

```text
[virtio-mmio] queue 0: desc=0x494000 avail=0x494800 used=0x495000 (num=128)
...
[virtio-mmio] queue 1: desc=0x496000 avail=0x496800 used=0x497000 (num=128)
[virtio-mmio] read  offset=0x070 → 0x3
[virtio-mmio] write offset=0x070 ← 0x7
[virtio-mmio] read  offset=0x070 → 0x7
```

送信確認では `./microkvm` を `grep` に通さず起動し、ゲストのシェルで実行する。`grep virtio` を通すと、確認したい `hello` が表示されない。

| ゲストで実行するコマンド | 出力経路 | 確認できること |
|------------------------|----------|----------------|
| `echo hello` | ttyS0 → UART → PIO | UART の出力 |
| `echo hello > /dev/hvc0` | transmitq → QueueNotify → VMM が RAM を読む | virtio の送信 |

送信時の期待出力:

```text
/ # echo hello > /dev/hvc0
hello
```

QueueNotify のログはキュー番号にかかわらず抑制しているため、起動時の receiveq 通知も送信時の transmitq 通知も表示されない。ログが出ないことは、通知や MMIO exit がなくなったことを意味しない。

## 重要な知見

UART は文字を PIO で渡すが、virtio は共有メモリ上のバッファを通知し、VMM がまとめて読み取れる。ただし、1コマンドが必ず1回の QueueNotify になるわけではなく、ドライバがデータをどう分割・通知するかで回数は変わる。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| Shared memory I/O | VMM が `ram + desc.addr` で guest バッファを直接読む — kvm_run を通さない |
| Descriptor chain walk | NEXT flag がなくなるまで `next` フィールドを辿る |
| Available ring | ゲストが head 番号を登録し、idx を進めて処理対象を公開 |
| Used ring | VMM が head 番号を返し、used->idx を進める |
| last_avail_idx | VMM のシャドウカウンタ — どこまで処理したかを追跡 |
| Bounds checking | リング先頭・Descriptor 番号・データバッファの範囲を確認 |
| QueueNotify | 値が 1 なら、読み取った avail_idx までの未処理 chain を処理 |

## 変わったこと

Step 14 からの変更:

- **virtio_mmio.h**: per-queue 状態に `last_avail_idx`、デバイス構造体に `ram`/`ram_size`、vring 構造体定義、`VRING_DESC_F_*` フラグ
- **virtio_mmio.c**: `virtio_console_tx()` 関数 + vring アドレス計算ヘルパー、QueueNotify ハンドラが TX を呼び出し、QueueNotify のログ抑制
- **microkvm.c**: `virtio_dev.ram = mem` + `virtio_dev.ram_size = GUEST_MEM_SIZE`

## 次のステップ

[Step 16: virtio-console RX](step16_virtio-rx.md) — host stdin を receive queue と IRQ 注入で guest に配送する。Step 15 の逆方向。
