# Step 14: virtqueue setup

## 目的

QueueNumMax に非ゼロ値を返し、kernel が virtqueue（guest と VMM 間のデータ転送に使う shared memory ring buffer）を確立できるようにする。このステップ完了後、デバイスは DRIVER_OK に到達する。

## 背景

### Virtqueue とは?

Virtqueue は **vring** と呼ばれる shared memory データ構造で、guest と VMM が 1バイトごとの VM exit なしにデータを交換できる。両者が guest RAM に直接読み書きし、新しいワークを通知する「kick」だけが必要。

### vring メモリレイアウト

各 virtqueue は guest 物理メモリ上の3つの連続領域で構成される:

```
GPA = QueuePFN × GuestPageSize

+------------------------------------------+  ← GPA (desc base)
|  Descriptor Table (16 bytes × num)       |
|  各エントリ: addr, len, flags, next      　|
|  「このアドレスにバッファがある」        　　  |
+------------------------------------------+  ← GPA + num×16 (avail base)
|  Available Ring (6 + 2×num bytes)        |
|  Guest → Device: 「この desc を処理して」   |
|  [flags][idx][ring[0]..ring[num-1]]      |
+------------------------------------------+
|  (QueueAlign 境界までのパディング)          |
+------------------------------------------+  ← aligned (used base)
|  Used Ring (6 + 8×num bytes)             |
|  Device → Guest: 「この desc は処理済み」   |
|  各エントリ: descriptor ID + 消費バイト数  　|
|  [flags][idx][{id,len}×num]              |
+------------------------------------------+
```

### アドレス計算

kernel が `QueuePFN` (ページフレーム番号) を書き込む。これは legacy virtio-mmio 固有の仕組み; modern デバイスでは明示的な descriptor アドレス (QueueDescLow/High) を使用する。VMM は実際の GPA を計算:
```
vring GPA       = QueuePFN × GuestPageSize
desc base       = vring GPA
avail base      = vring GPA + num × 16
used base       = align_up(avail base + 6 + 2×num, QueueAlign)

例: QueuePFN = 0x4064, GuestPageSize = 4096
  → GPA = 0x4064 × 4096 = 0x4064000
```

### Setup シーケンス (queue ごと)

```
1. ドライバが QueueSel を書く     → 操作対象の queue を選択 (0 or 1)
2. ドライバが QueueNumMax を読む  → 「この queue は最大何個の descriptor を保持できるか?」
3. ドライバが QueueNum を書く     → 「この数だけ使う」(≤ QueueNumMax)
4. ドライバが QueueAlign を書く   → Used Ring のアライメント (4096)
5. ドライバが QueuePFN を書く     → 「vring はこのページフレーム番号にある」
   → VMM は shared memory の場所を知る
```

### virtio-console の queue 構成

| Queue | Index | 方向 | 用途 |
|-------|-------|-----------|---------|
| receiveq | 0 | host → guest | VMM が guest に読ませるデータを書き込む |
| transmitq | 1 | guest → host | Guest が VMM に読ませるデータを書き込む |

## 実行フロー

```
Linux kernel                              VMM (microkvm)
────────────                              ──────────────
write QueueSel (0x030) ← 0               dev->queue_sel = 0 (receiveq)
read  QueuePFN (0x040) → 0               "queue 未使用、setup OK"
read  QueueNumMax (0x034) → 128          "128 descriptors サポート"
write QueueNum (0x038) ← 128             dev->vqs[0].num = 128
write QueueAlign (0x03C) ← 4096          dev->vqs[0].align = 4096
write QueuePFN (0x040) ← 0x4064          dev->vqs[0].pfn = 0x4064
                                          → GPA = 0x4064 × 4096 = 0x4064000
                                          → log: "queue 0: desc=0x4064000 ..."

write QueueSel (0x030) ← 1               dev->queue_sel = 1 (transmitq)
read  QueuePFN (0x040) → 0               "未使用"
read  QueueNumMax (0x034) → 128          "128 descriptors"
write QueueNum (0x038) ← 128             dev->vqs[1].num = 128
write QueueAlign (0x03C) ← 4096          dev->vqs[1].align = 4096
write QueuePFN (0x040) ← 0x406a          dev->vqs[1].pfn = 0x406a
                                          → GPA = 0x406a × 4096 = 0x406a000

write Status (0x070) ← 0x7               ACKNOWLEDGE | DRIVER | DRIVER_OK !!!
                                         デバイス初期化完了。

write QueueNotify (0x050) ← 0 × 128      receiveq kicks — ドライバが直ちに
                                         空の receive バッファを投入し、
                                         受信データの配置先を用意する
```

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

### virtio_mmio.c の追加

Read ハンドラ — QueueNumMax が 128 を返すように:
```c
case VIRTIO_MMIO_QUEUE_NUM_MAX:
    val = (dev->queue_sel < VIRTQ_NUM_QUEUES) ? 128 : 0;
    break;
case VIRTIO_MMIO_QUEUE_PFN:
    val = (dev->queue_sel < VIRTQ_NUM_QUEUES) ? dev->vqs[dev->queue_sel].pfn : 0;
    break;
```

Write ハンドラ — per-queue 状態を保存し vring レイアウトをログ:
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

## 出力

```
[virtio-mmio] write offset=0x030 ← 0x0          QueueSel = 0
[virtio-mmio] read  offset=0x040 → 0x0          QueuePFN = 0 (未使用)
[virtio-mmio] read  offset=0x034 → 0x80         QueueNumMax = 128
[virtio-mmio] write offset=0x038 ← 0x80         QueueNum = 128
[virtio-mmio] write offset=0x03c ← 0x1000       QueueAlign = 4096
[virtio-mmio] write offset=0x040 ← 0x4064       QueuePFN
[virtio-mmio] queue 0: desc=0x4064000 avail=0x4064800 used=0x4065000 (num=128)
[virtio-mmio] write offset=0x030 ← 0x1          QueueSel = 1
[virtio-mmio] read  offset=0x040 → 0x0          QueuePFN = 0
[virtio-mmio] read  offset=0x034 → 0x80         QueueNumMax = 128
[virtio-mmio] write offset=0x038 ← 0x80         QueueNum = 128
[virtio-mmio] write offset=0x03c ← 0x1000       QueueAlign = 4096
[virtio-mmio] write offset=0x040 ← 0x406a       QueuePFN
[virtio-mmio] queue 1: desc=0x406a000 avail=0x406a800 used=0x406b000 (num=128)
[virtio-mmio] read  offset=0x070 → 0x3
[virtio-mmio] write offset=0x070 ← 0x7          Status = DRIVER_OK !!!
```

Status = 0x07 = ACKNOWLEDGE (1) | DRIVER (2) | DRIVER_OK (4)。デバイス初期化が完了。その後大量の QueueNotify (offset 0x050 ← 0x0) は virtio-console ドライバが receive queue に 128 個の空バッファを投入している。

## 重要な知見

Virtqueue は virtio の核心的イノベーション: **shared memory によるデータ転送、通知はシグナリングのみ**。kernel が guest RAM の一領域を確保し、QueuePFN で VMM にその場所を伝え、両者がそのメモリに直接読み書きする。実際のデータに VM exit は不要 — 「kick」(QueueNotify) だけが trap を引き起こす。UART では1バイトごとに PIO exit が必要だったのとは根本的に異なる。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| vring レイアウト | desc table + avail ring + used ring が連続メモリに配置 |
| Per-queue 状態 | `struct virtqueue_state` に num/align/pfn |
| QueueNumMax | デバイスがドライバに最大 queue 容量を通知 |
| QueuePFN | ドライバがデバイスに vring の配置場所を通知 |
| GPA 計算 | PFN × PageSize = vring の物理アドレス |
| DRIVER_OK | デバイスが完全に動作可能であることを示す最終 status |
| QueueNotify | kick メカニズムのプレースホルダー (Step 15 で実装) |

## 変わったこと

Step 13 からの変更:
- **virtio_mmio.h**: 6 個の新規レジスタオフセット + `VIRTQ_NUM_QUEUES`/`VIRTQ_MAX_SIZE` + `struct virtqueue_state` + `vqs[]` 配列
- **virtio_mmio.c read**: `QUEUE_NUM_MAX` が 128 を返す、`QUEUE_PFN` が保存値を返す、`INTERRUPT_STATUS` が 0 を返す
- **virtio_mmio.c write**: `QUEUE_NUM`, `QUEUE_ALIGN`, `QUEUE_PFN` (GPA ログ付き), `QUEUE_NOTIFY`, `INTERRUPT_ACK` の case 追加

`microkvm.c` の変更なし — 既存の MMIO ディスパッチが新しいレジスタを自動的に処理。

## 次のステップ

[Step 15: virtio-console TX](step15_virtio-tx.md) — guest が `/dev/hvc0` に書き込むと、transmit queue の descriptor chain を walk して host stdout に出力する。shared memory I/O が実際に動く瞬間。
