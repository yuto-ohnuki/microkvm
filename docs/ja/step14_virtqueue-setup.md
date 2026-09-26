# Step 14: virtqueue setup

## 目的

送受信用の2つの virtqueue を設定し、Linux ドライバが DRIVER_OK を設定するところまで進める。VMM はキューのサイズ・配置を保存するが、データ転送の処理は次のステップ以降で実装する。

## 背景

### Virtqueue とは?

ここで使う virtqueue は、**vring** という共有メモリ上の管理構造を使ってデータバッファを受け渡す。ゲストが処理対象のバッファを登録し、VMM が処理結果を記録する。QueueNotify への書き込み (kick) は、キューに処理対象があることを VMM に知らせる。

### vring メモリレイアウト

この Legacy レイアウトでは、ゲスト RAM に次の3領域をまとめて確保する。データ本体は Descriptor が指す別のバッファに置く。

```text
GPA = QueuePFN × GuestPageSize

GPA
  Descriptor Table: 16 × num bytes
    [addr, len, flags, next] × num
    バッファのアドレス・長さと、次の Descriptor 番号

GPA + 16 × num
  Available Ring: 6 + 2 × num bytes
    [flags][idx][Descriptor 番号 × num][used_event]
    ゲスト → VMM: 処理対象を登録

  (QueueAlign 境界までのパディング)

Used Ring の先頭
  Used Ring: 6 + 8 × num bytes
    [flags][idx][{id, len} × num][avail_event]
    VMM → ゲスト: 処理済みの Descriptor chain を報告
```

`num` は Descriptor 数。Used Ring の `id` は chain の先頭番号、`len` はデバイスがバッファに書き込んだバイト数を示す。末尾の event フィールド各2バイトを含めたサイズが上記の式で、この段階では event 通知抑制機能は使わない。

### アドレス計算

kernel が `QueuePFN` (ページフレーム番号) を書き込む。これは legacy virtio-mmio 固有の仕組み; Modern MMIO インターフェースでは3領域のアドレスを個別に指定する。ここでは VMM が次の式で配置を計算する:

```
vring GPA       = QueuePFN × GuestPageSize
desc base       = vring GPA
avail base      = vring GPA + num × 16
used base       = align_up(avail base + 6 + 2×num, QueueAlign)

例: QueuePFN = 0x47a, GuestPageSize = 4096
  → GPA = 0x47a × 4096 = 0x47a000
```

### Setup シーケンス (queue ごと)

```
1. ドライバが QueueSel を書く     → 操作対象の queue を選択 (0 or 1)
2. ドライバが QueuePFN を読む     → 0 なら未使用
3. ドライバが QueueNumMax を読む  → 「この queue は最大何個の descriptor を保持できるか?」
4. ドライバが QueueNum を書く     → 「この数だけ使う」(≤ QueueNumMax)
5. ドライバが QueueAlign を書く   → Used Ring のアライメント (4096)
6. ドライバが QueuePFN を書く     → 「vring はこのページフレーム番号にある」
   → VMM は shared memory の場所を知る
```

### virtio-console の queue 構成

| Queue | Index | 方向 | 用途 |
|-------|-------|-----------|---------|
| receiveq | 0 | host → guest | VMM が guest に読ませるデータを書き込む |
| transmitq | 1 | guest → host | Guest が VMM に読ませるデータを書き込む |

## 実行フロー

```text
Guest (Linux)               KVM                         VMM
     |-- QueueSel=0 -------->|-- KVM_EXIT_MMIO ---------->|
     |                       |                            | queue_sel=0
     |                       |<-- KVM_RUN ----------------|
     |-- QueuePFN 読み ------>|-- KVM_EXIT_MMIO ---------->|
     |                       |                            | vqs[0].pfn=0 を返す
     |                       |<-- KVM_RUN ----------------|
     |-- QueueNumMax 読み -->|-- KVM_EXIT_MMIO ----------->|
     |                       |                            | 128 を返す
     |                       |<-- KVM_RUN ----------------|
     | vring 用メモリを確保    |                            |
     |-- Num / Align / PFN ->|-- KVM_EXIT_MMIO ---------->|
     |                       |                            | vqs[0] に設定を保存
     |                       |                            | 配置を計算してログ出力
     |                       |<-- KVM_RUN ----------------|
     | QueueSel=1 でも同じ設定手順を行い、vqs[1] に保存        |
     |-- Status=0x07 ------->|-- KVM_EXIT_MMIO ---------->|
     |                       |                            | DRIVER_OK を含む値を保存
     |                       |<-- KVM_RUN ----------------|
     |-- QueueNotify=0 ----->|-- KVM_EXIT_MMIO ---------->|
     |                       |                            | ログのみ。受信処理は未実装
```

Num・Align・PFN はそれぞれ別の MMIO 書き込み。ドライバは設定した receiveq に受信用バッファを登録し、QueueNotify で通知する。

## 実装

### virtio_mmio.h の追加

新規レジスタオフセットと per-queue 状態:

```c
/* Virtqueue setup */
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_ALIGN         0x03C
#define VIRTIO_MMIO_QUEUE_PFN           0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064

#define VIRTQ_NUM_QUEUES 2      /* receiveq (0) + transmitq (1) */
#define VIRTQ_MAX_SIZE   128    /* queue あたりの最大 descriptor 数 */

/* Per-virtqueue configuration (guest が setup 時に設定) */
struct virtqueue_state {
    uint32_t num;       /* queue サイズ (QueueNum から) */
    uint32_t align;     /* Used Ring アライメント (QueueAlign から) */
    uint32_t pfn;       /* vring のページフレーム番号 (QueuePFN から) */
};
```

デバイス構造体に per-queue 配列を追加:

```c
struct virtio_mmio_dev {
    ...
    struct virtqueue_state vqs[VIRTQ_NUM_QUEUES];
};
```

QueueSel は操作対象を選ぶレジスタで、キューの設定値そのものではない。`vqs[0]` と `vqs[1]` に分けて保存することで、送信キューへ切り替えても受信キューの設定を保持できる。

### virtio_mmio.c の追加

Read ハンドラ — QueueSel が 0 または 1 なら QueueNumMax は 128、それ以外は 0 を返す:

```c
case VIRTIO_MMIO_QUEUE_NUM_MAX:
    val = (dev->queue_sel < VIRTQ_NUM_QUEUES) ? 128 : 0;
    break;
case VIRTIO_MMIO_QUEUE_PFN:
    val = (dev->queue_sel < VIRTQ_NUM_QUEUES) ? dev->vqs[dev->queue_sel].pfn : 0;
    break;
```

Write ハンドラ — QueueNum と QueueAlign を保存し、QueuePFN の書き込み時に配置を計算する:

```c
case VIRTIO_MMIO_QUEUE_NUM:
    if (dev->queue_sel < VIRTQ_NUM_QUEUES)
        dev->vqs[dev->queue_sel].num = value;
    break;
case VIRTIO_MMIO_QUEUE_ALIGN:
    if (dev->queue_sel < VIRTQ_NUM_QUEUES)
        dev->vqs[dev->queue_sel].align = value;
    break;
```

```c
case VIRTIO_MMIO_QUEUE_PFN:
    if (dev->queue_sel < VIRTQ_NUM_QUEUES) {
        dev->vqs[dev->queue_sel].pfn = value;
        if (value) {
            uint64_t gpa = (uint64_t)value * dev->guest_page_size;
            uint32_t num = dev->vqs[dev->queue_sel].num;
            uint32_t align = dev->vqs[dev->queue_sel].align;
            uint64_t avail = gpa + num * 16;
            uint64_t used = (avail + 6 + 2 * num + align - 1) & ~((uint64_t)align - 1);
            fprintf(stderr, "[virtio-mmio] queue %d: desc=0x%lx avail=0x%lx used=0x%lx (num=%d)\n",
                dev->queue_sel, gpa, avail, used, num);
        }
    }
    break;
```

計算したアドレスはログに出すだけで、まだ Descriptor やリングの内容は読み書きしない。QueueNotify と InterruptACK への書き込みもログのみで、InterruptStatus は常に `0` を返す。

## 出力

```
[virtio-mmio] write offset=0x030 ← 0x0
[virtio-mmio] read  offset=0x040 → 0x0
[virtio-mmio] read  offset=0x034 → 0x80
[virtio-mmio] write offset=0x038 ← 0x80
[virtio-mmio] write offset=0x03c ← 0x1000
[virtio-mmio] write offset=0x040 ← 0x47a
[virtio-mmio] queue 0: desc=0x47a000 avail=0x47a800 used=0x47b000 (num=128)
[virtio-mmio] write offset=0x030 ← 0x1
[virtio-mmio] read  offset=0x040 → 0x0
[virtio-mmio] read  offset=0x034 → 0x80
[virtio-mmio] write offset=0x038 ← 0x80
[virtio-mmio] write offset=0x03c ← 0x1000
[virtio-mmio] write offset=0x040 ← 0x47c
[virtio-mmio] queue 1: desc=0x47c000 avail=0x47c800 used=0x47d000 (num=128)
[virtio-mmio] read  offset=0x070 → 0x3
[virtio-mmio] write offset=0x070 ← 0x7
[virtio-mmio] write offset=0x050 ← 0x0
...
[virtio-mmio] read  offset=0x070 → 0x7
```

2つの `queue` ログでキューの配置を、Status=`0x07` (ACKNOWLEDGE | DRIVER | DRIVER_OK) でドライバが初期化完了を通知したことを確認する。QueueNotify (`0x050`) への `0` の書き込みは receiveq への通知であり、VMM が受信データを転送したことを示すものではない。

## 重要な知見

vring のメモリを確保するのはゲストで、VMM はレジスタ経由でその配置とサイズを受け取る。送受信キューごとに設定を保持すれば、後続の処理は共有メモリを参照できる。DRIVER_OK への到達はドライバ側の準備完了を示すが、この段階では VMM のデータ転送処理はまだない。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| vring レイアウト | desc table + avail ring + used ring が連続メモリに配置 |
| Per-queue 状態 | `struct virtqueue_state` に num/align/pfn |
| QueueNumMax | デバイスがドライバに最大 queue 容量を通知 |
| QueuePFN | ドライバがデバイスに vring の配置場所を通知 |
| GPA 計算 | PFN × PageSize = vring の物理アドレス |
| DRIVER_OK | ドライバが初期化完了を通知する Status ビット |
| QueueNotify | kick メカニズムのプレースホルダー (Step 15 で実装) |

## 変わったこと

Step 13 からの変更:

- **virtio_mmio.h**: 6 個の新規レジスタオフセット + `VIRTQ_NUM_QUEUES`/`VIRTQ_MAX_SIZE` + `struct virtqueue_state` + `vqs[]` 配列
- **virtio_mmio.c read**: `QUEUE_NUM_MAX` が 128 を返す、`QUEUE_PFN` が保存値を返す、`INTERRUPT_STATUS` が 0 を返す
- **virtio_mmio.c write**: `QUEUE_NUM`, `QUEUE_ALIGN`, `QUEUE_PFN` (GPA ログ付き), `QUEUE_NOTIFY`, `INTERRUPT_ACK` の case 追加

## 次のステップ

[Step 15: virtio-console TX](step15_virtio-tx.md) — `/dev/hvc0` への書き込みを transmitq から読み取り、ホストの標準出力へ送る。
