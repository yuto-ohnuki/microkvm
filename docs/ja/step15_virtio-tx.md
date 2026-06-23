# Step 15: virtio-console TX (guest → host)

## 目的

guest が `/dev/hvc0` に書き込むと、transmit queue の descriptor chain を walk して host stdout に出力する。shared memory I/O が実際に動く瞬間 — データは1バイトごとの VM exit なしに vring を通じて流れる。

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

重要なポイント: **データ自体は VM exit を通過しない**。kick (QueueNotify MMIO write) だけが trap を引き起こす。VMM は `guest_ram + 0x1234` から直接 "hello" を読む。

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
| TX (guest → host) | flags = 0 | デバイスがバッファを読む |
| RX (host → guest) | flags = VRING_DESC_F_WRITE | デバイスがバッファに書き込む |

## 実行フロー

```
Guest                                    VMM (microkvm)
─────                                    ──────────────
echo hello > /dev/hvc0
  ↓
virtio-console ドライバ:
  "hello\n" を guest バッファに書き込む
  descriptor を設定: addr=X, len=6
  avail ring に desc index を追加
  avail->idx をインクリメント
  QueueNotify (0xD0000050) に 1 を書き込む
    ↓ EPT violation → KVM_EXIT_MMIO
                                         virtio_mmio_write():
                                           QueueNotify value=1 → transmitq
                                           → virtio_console_tx()

                                         virtio_console_tx():
                                           avail->idx を読む
                                           while (last_avail_idx != avail_idx):
                                             desc_idx = avail->ring[slot]
                                             desc = descriptor_table[desc_idx]
                                             write(stdout, ram + desc.addr, desc.len)
                                             used ring に記録
                                             last_avail_idx++

                                         "hello\n" が host ターミナルに表示
```

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
    uint8_t *ram;       /* guest 物理メモリへのポインタ */
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

```c
static void virtio_console_tx(struct virtio_mmio_dev *dev,
    uint8_t *ram, size_t ram_size)
{
    int qidx = 1;  /* transmitq */
    struct virtqueue_state *vq = &dev->vqs[qidx];

    /* avail->idx を読む */
    uint16_t avail_idx;
    memcpy(&avail_idx, ram + avail_base + 2, sizeof(uint16_t));

    /* last_avail_idx と avail_idx の間の全バッファを処理:
     *   avail_idx = 3, last_avail_idx = 1 → descriptor #1, #2 が未処理 */
    while (vq->last_avail_idx != avail_idx) {
        /* avail ring から descriptor head を取得 */
        uint16_t desc_idx = avail->ring[last_avail_idx % num];

        /* descriptor chain を walk し、各バッファを出力 */
        while (descriptor が有効) {
            if (!(desc.flags & VRING_DESC_F_WRITE))
                write(STDOUT_FILENO, ram + desc.addr, desc.len);
            if (desc.flags & VRING_DESC_F_NEXT)
                next を辿る;
            else break;
        }

        /* used ring に記録 */
        used->ring[used_idx % num] = { .id = desc_idx, .len = 0 };
        used_idx++;  /* guest が確認できるよう used->idx を更新 */
        last_avail_idx++;
    }
}
```

**なぜ `len = 0` か?** TX では console ドライバは返却された長さを確認しない — データは既に host の `write()` で消費済み。RX (Step 16) では実際のバイト数を返し、guest がバッファに何バイト書き込まれたかを知れるようにする。

### microkvm.c の追加

```c
/* virtio デバイスが guest メモリに直接アクセスするためのポインタ */
virtio_dev.ram = (uint8_t *)mem;
virtio_dev.ram_size = GUEST_MEM_SIZE;
```

VMM が mmap した guest RAM ポインタを virtio デバイスに渡す。`virtio_console_tx()` はこれを使って guest バッファを直接読み取る。

## 出力

```
/ # echo hello > /dev/hvc0
hello
```

`hello` が host ターミナルに表示される — guest RAM から vring 経由で直接書き出され、文字列全体で QueueNotify の VM exit は1回だけ。

> **Note:** v2 のコードでは QueueNotify のログを抑制している（boot 時にノイズになるため）。ログを有効にすると `[virtio-mmio] write offset=0x050 ← 0x1` が出力前に表示される。

## 重要な知見

virtio の性能優位性が具体的になる瞬間。UART との比較:

| | UART (Step 11) | virtio TX (Step 15) |
|---|---|---|
| "hello" (5文字) | 5回の PIO exit + 5回の IRQ | **1回の MMIO exit** (QueueNotify) |
| データ経路 | kvm_run → VMM → putchar (1バイトずつ) | VMM が guest RAM から直接読み取り |
| 1バイトあたりの latency | ~5 μs (exit + entry) | ~0 (データに exit なし) |

exit 回数が n バイトに対して O(n) から O(1) に減少する。全ての現代のハイパーバイザーが I/O に virtio を使う理由がここにある。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| Shared memory I/O | VMM が `ram + desc.addr` で guest バッファを直接読む — kvm_run を通さない |
| Descriptor chain walk | NEXT flag がなくなるまで `next` フィールドを辿る |
| Available ring | guest が avail->idx をインクリメントして「新しいワーク」を通知 |
| Used ring | VMM が descriptor ID を記録して「処理完了」を通知 |
| last_avail_idx | VMM のシャドウカウンタ — どこまで処理したかを追跡 |
| Bounds checking | 全 GPA アクセスを ram_size に対して検証 |
| QueueNotify | 1回の MMIO exit で全 pending バッファの処理をトリガー |

## 変わったこと

Step 14 からの変更:
- **virtio_mmio.h**: per-queue 状態に `last_avail_idx`、デバイス構造体に `ram`/`ram_size`、vring 構造体定義、`VRING_DESC_F_*` フラグ
- **virtio_mmio.c**: `virtio_console_tx()` 関数 + vring アドレス計算ヘルパー、QueueNotify ハンドラが TX を呼び出し、QueueNotify のログ抑制
- **microkvm.c**: `virtio_dev.ram = mem` + `virtio_dev.ram_size = GUEST_MEM_SIZE`

## 次のステップ

[Step 16: virtio-console RX](step16_virtio-rx.md) — host stdin を receive queue と IRQ 注入で guest に配送する。Step 15 の逆方向。
