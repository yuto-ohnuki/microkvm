# Step 13: virtio feature negotiation

## 目的

使用するオプション機能を選ぶ feature negotiation のレジスタ処理を明示し、ドライバからの書き込み値を保持する。提供する機能は引き続きゼロとし、次のキュー設定に使うページサイズとキュー番号も保存する。

## 背景

### Feature negotiation とは?

virtio デバイスがデータ転送を行う前に、ドライバとデバイスは「どのオプション機能を使うか」を合意する必要がある。これが **feature negotiation** — 以下のハンドシェイク:

1. デバイスがサポートする機能を広告 (HostFeatures)
2. ドライバが使いたい機能を選択 (GuestFeatures)
3. 選択した機能を前提に、その後の初期化とデータ転送を行う

デバイスが提供し、ドライバも理解できる機能だけを選ぶことで、未対応の機能を誤って使うのを防ぐ。

### Feature negotiation の流れ

Linux の virtio-mmio ドライバは、機能ビットを上位・下位の 32-bit に分けて読み書きする。この各部分を bank と呼び、Sel レジスタでアクセス対象を指定する:

```
64-bit feature bitmap:
  63.................32 31.................0
  +--------------------+--------------------+
  |       bank 1       |       bank 0       |
  +--------------------+--------------------+
  HostFeaturesSel=1    HostFeaturesSel=0
```

```
Driver                                    Device (VMM)
──────                                    ────────────
write HostFeaturesSel = 1                 (feature bank 1 を選択: bits 32-63)
read  HostFeatures    → 0                 (high features なし)
write HostFeaturesSel = 0                 (feature bank 0 を選択: bits 0-31)
read  HostFeatures    → 0                 (low features もなし)
write GuestFeaturesSel = 1
write GuestFeatures = 0                   (bank 1 から何も accept しない)
write GuestFeaturesSel = 0
write GuestFeatures = 0                   (bank 0 からも何も accept しない)
→ virtqueue setup に進む
```

microkvm は bank の指定にかかわらず HostFeatures に `0` を返す。Linux は機能を何も選ばず、GuestFeatures に `0` を書く。VMM は GuestFeaturesSel を保存せず、GuestFeatures の最後の 32-bit 書き込み値だけを保持する。複数 bank の機能を保持・検証する実装ではない。

### Legacy vs Modern

Legacy virtio-mmio (Version=1) では `FEATURES_OK` status ステップが存在しない。ドライバは ACKNOWLEDGE | DRIVER (`0x03`) を書いた後、feature read/write → queue setup に進む。Modern virtio-mmio (Version=2) ではドライバが `FEATURES_OK` を書いてデバイスが受理したことを確認する必要がある。

### レジスタ一覧

| Offset | 名前 | R/W | 役割 |
|--------|------|-----|---------|
| 0x010 | HostFeatures | R | デバイスが提供する feature bits (選択された bank) |
| 0x014 | HostFeaturesSel | W | HostFeatures のどの 32-bit bank を読むか選択 (0=low, 1=high) |
| 0x020 | GuestFeatures | W | ドライバが accept した feature bits |
| 0x024 | GuestFeaturesSel | W | GuestFeatures のどの bank に書くか選択 |
| 0x028 | GuestPageSize | W | ドライバがページサイズ (通常 4096) をデバイスに通知。Step 14 で vring 物理アドレス計算に使用: `GPA = QueuePFN × GuestPageSize` |
| 0x030 | QueueSel | W | 設定対象の virtqueue を選択 (virtio-console: 0=receive queue, 1=transmit queue) |
| 0x034 | QueueNumMax | R | 選択された queue がサポートする最大 descriptor 数 |

## 実行フロー

```text
Guest (Linux)               KVM                         VMM
     | Status=0x03 設定済み   |                           |
     |-- HostFeaturesSel --->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | 選択値を保存
     |                       |<-- KVM_RUN ---------------|
     |-- HostFeatures 読み -->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | 常に 0 を返す
     |                       |<-- KVM_RUN ---------------|
     | 機能を選択しない         |                           |
     |-- GuestFeaturesSel -->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | 選択値は保存しない
     |                       |<-- KVM_RUN ---------------|
     |-- GuestFeatures=0 --->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | guest_features=0
     |                       |<-- KVM_RUN ---------------|
     |-- QueueSel=0 -------->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | queue_sel=0 を保存
     |                       |<-- KVM_RUN ---------------|
     |-- QueueNumMax 読み --->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | 0 を返す
     |                       |<-- KVM_RUN ---------------|
     | キュー初期化に失敗       |                           |
```

機能の読み書きは bank 1、bank 0 の順に行う。キュー設定には進むが、QueueNumMax はまだ `0` のため Step 12 と同じ箇所で失敗する。

## 実装

### virtio_mmio.h の追加

新規レジスタオフセット定義:

```c
/* Feature negotiation */
#define VIRTIO_MMIO_HOST_FEATURES       0x010
#define VIRTIO_MMIO_HOST_FEATURES_SEL   0x014
#define VIRTIO_MMIO_GUEST_FEATURES      0x020
#define VIRTIO_MMIO_GUEST_FEATURES_SEL  0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE     0x028
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
```

デバイス構造体に新規フィールド:

```c
struct virtio_mmio_dev {
    uint32_t status;
    uint32_t host_features_sel;   /* どの feature bank を読むか */
    uint32_t guest_features;      /* 最後に書き込まれた 32-bit 値 */
    uint32_t guest_page_size;     /* ドライバが報告するページサイズ (4096) */
    uint32_t queue_sel;           /* 設定対象の virtqueue 番号 */
};
```

### virtio_mmio.c の追加

Read ハンドラ:

```c
case VIRTIO_MMIO_HOST_FEATURES:
    val = 0;  /* 現時点では features を提供しない */
    break;
case VIRTIO_MMIO_QUEUE_NUM_MAX:
    val = 0;  /* Step 14 で非ゼロにする */
    break;
```

Write ハンドラ:

```c
case VIRTIO_MMIO_HOST_FEATURES_SEL:
    dev->host_features_sel = value;
    break;
case VIRTIO_MMIO_GUEST_FEATURES_SEL:
    break;  /* 選択値は保存しない */
case VIRTIO_MMIO_GUEST_FEATURES:
    dev->guest_features = value;
    break;
case VIRTIO_MMIO_GUEST_PAGE_SIZE:
    dev->guest_page_size = value;
    break;
case VIRTIO_MMIO_QUEUE_SEL:
    dev->queue_sel = value;
    break;
```

`host_features_sel` は保存するが、読み取り値の切り替えには使っていない。提供する機能がすべて `0` なので、この段階ではどちらの bank を読んでも同じ値になる。

## 出力

```
[virtio-mmio] write offset=0x070 ← 0x3
[virtio-mmio] write offset=0x014 ← 0x1
[virtio-mmio] read  offset=0x010 → 0x0
[virtio-mmio] write offset=0x014 ← 0x0
[virtio-mmio] read  offset=0x010 → 0x0
[virtio-mmio] write offset=0x024 ← 0x1
[virtio-mmio] write offset=0x020 ← 0x0
[virtio-mmio] write offset=0x024 ← 0x0
[virtio-mmio] write offset=0x020 ← 0x0
[virtio-mmio] write offset=0x030 ← 0x0
[virtio-mmio] read  offset=0x040 → 0x0
[virtio-mmio] read  offset=0x034 → 0x0
[virtio-mmio] write offset=0x040 ← 0x0
[virtio-mmio] read  offset=0x070 → 0x3
[virtio-mmio] write offset=0x070 ← 0x83
virtio_console virtio0: Error -2 initializing vqs
virtio_console virtio0: probe with driver virtio_console failed with error -2
```

`0x014` / `0x024` は bank 選択、`0x010` / `0x020` は機能の読み書き、`0x030` / `0x034` はキュー選択と最大サイズの確認に対応する。QueueNumMax=`0` による初期化失敗は想定通りで、Status は FAILED が加わった `0x83` になる。終了するにはホスト端末で `Ctrl-C` を押す。

## 重要な知見

Step 12 でも未実装レジスタが `0` を返すため、Linux は機能の読み書きを終えてキュー設定まで進んでいた。Step 13 の違いはログの値ではなく、レジスタの処理を明示し、GuestFeatures・GuestPageSize・QueueSel などの値を保存する点にある。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| Feature negotiation | 提供機能が 0 なので、ドライバも機能を選択しない |
| Bank 選択 | Linux は上位・下位を指定してアクセス。VMM はどちらにも 0 を返す |
| GuestPageSize | ドライバがページサイズを通知 (Step 14 の vring アドレス計算で使用) |
| QueueSel | 操作対象の virtqueue を選択 (receiveq=0, transmitq=1) |
| レジスタ値の保持 | 書き込みを無視する処理から、後続処理に使う状態の保存へ |

## 変わったこと

Step 12 からの変更:

- **virtio_mmio.h**: 7 個の新規レジスタオフセット定義 + 4 個の新規構造体フィールド
- **virtio_mmio.c read**: `HOST_FEATURES` と `QUEUE_NUM_MAX` の case 追加 (共に 0 を返す)
- **virtio_mmio.c write**: `HOST_FEATURES_SEL`, `GUEST_FEATURES_SEL`, `GUEST_FEATURES`, `GUEST_PAGE_SIZE`, `QUEUE_SEL` の case 追加

`microkvm.c` や `Makefile` の変更なし。

## 次のステップ

[Step 14: virtqueue setup](step14_virtqueue-setup.md) — QueueNumMax に非ゼロ値を返し、kernel が virtqueue を確立できるようにする。ここで shared memory ring buffer が確立される。
