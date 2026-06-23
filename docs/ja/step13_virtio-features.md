# Step 13: virtio feature negotiation

## 目的

feature negotiation レジスタを実装し、kernel ドライバがデバイスの capabilities を読み取り、受け入れる features を書き戻せるようにする。virtio 初期化シーケンスの第2フェーズを完了する。

## 背景

### Feature negotiation とは?

virtio デバイスがデータ転送を行う前に、ドライバとデバイスは「どのオプション機能を使うか」を合意する必要がある。これが **feature negotiation** — 以下のハンドシェイク:

1. デバイスがサポートする機能を広告 (HostFeatures)
2. ドライバが使いたい機能を選択 (GuestFeatures)
3. 両者がこの合意したサブセットにコミット

前方/後方互換性を保証: 新しいドライバが古いデバイスと（またその逆も）、両者が理解する機能だけを使うことで動作できる。

### Feature negotiation の流れ

Features は 64-bit ビットマップで表現され、2つの 32-bit bank に分割される:
```
64-bit feature bitmap:
  63................32  31................0
  +------------------+--------------------+
  |     bank 1       |      bank 0        |
  +------------------+--------------------+
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

microkvm では HostFeatures は 0 を返す（オプション機能なし）。kernel は値がゼロでも完全な negotiation シーケンスを実行するため、これらのレジスタは必ず実装する必要がある。

### Legacy vs Modern

Legacy virtio-mmio (Version=1) では `FEATURES_OK` status ステップが存在しない。ドライバは DRIVER (0x03) を書いた後、直接 feature read/write → queue setup に進む。Modern (v2) ではドライバが `FEATURES_OK` を書いてデバイスが受理したことを確認する必要がある。

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

```
Linux kernel (virtio_console)              VMM (microkvm)
─────────────────────────────              ──────────────
Status = DRIVER (0x03) 設定済み

write HostFeaturesSel (0x014) ← 1         dev->host_features_sel = 1
read  HostFeatures (0x010) → 0            (bank 1: features なし)
write HostFeaturesSel (0x014) ← 0         dev->host_features_sel = 0
read  HostFeatures (0x010) → 0            (bank 0: features なし)

write GuestFeaturesSel (0x024) ← 1
write GuestFeatures (0x020) ← 0           dev->guest_features = 0 (bank 1)
write GuestFeaturesSel (0x024) ← 0
write GuestFeatures (0x020) ← 0           dev->guest_features = 0 (bank 0)

write QueueSel (0x030) ← 0                dev->queue_sel = 0 (receiveq)
read  QueueNumMax (0x034) → 0             "queue が使えない" → 失敗
```

Feature negotiation は成功（自明に — 両者がゼロ features に合意）。失敗は QueueNumMax で発生し、Step 14 で解決する。

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
    uint32_t guest_features;      /* ドライバが accept した features */
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
    break;  /* 受理するが値は保存しない — microkvm は features を提供しないため
             * bank 選択は実質無意味。完全な実装では保存して
             * 複数の 32-bit write を 64-bit feature set に統合する */
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

## 出力

```
[virtio-mmio] write offset=0x070 ← 0x3          Status = DRIVER
[virtio-mmio] write offset=0x014 ← 0x1          HostFeaturesSel = 1
[virtio-mmio] read  offset=0x010 → 0x0          HostFeatures (bank 1) = 0
[virtio-mmio] write offset=0x014 ← 0x0          HostFeaturesSel = 0
[virtio-mmio] read  offset=0x010 → 0x0          HostFeatures (bank 0) = 0
[virtio-mmio] write offset=0x024 ← 0x1          GuestFeaturesSel = 1
[virtio-mmio] write offset=0x020 ← 0x0          GuestFeatures (bank 1) = 0
[virtio-mmio] write offset=0x024 ← 0x0          GuestFeaturesSel = 0
[virtio-mmio] write offset=0x020 ← 0x0          GuestFeatures (bank 0) = 0
[virtio-mmio] write offset=0x030 ← 0x0          QueueSel = 0
[virtio-mmio] read  offset=0x034 → 0x0          QueueNumMax = 0 → 失敗
```

kernel は feature negotiation を完了し queue setup フェーズに到達。QueueNumMax=0 により Step 12 と同じ失敗が発生 — Step 14 で解決する。

## 重要な知見

Feature negotiation は異なるバージョンのドライバとデバイス間の互換性を保証するために存在する。microkvm の最小実装では両者がゼロ features に合意するが、プロトコルは省略できない。kernel は値に関係なく HostFeatures を読む — このステップを飛ばすと virtio spec の状態遷移に違反する。重要な教訓: **「何もしない」デバイスでも完全なプロトコルハンドシェイクを実装する必要がある**。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| Feature negotiation | デバイスが advertise、ドライバが select、両者がコミット |
| Bank 選択 | HostFeaturesSel で読む 32-bit half を切り替え |
| GuestPageSize | ドライバがページサイズを通知 (Step 14 の vring アドレス計算で使用) |
| QueueSel | 操作対象の virtqueue を選択 (receiveq=0, transmitq=1) |
| プロトコル準拠 | ゼロを返す場合でも全レジスタを実装する必要がある |

## 変わったこと

Step 12 からの変更:
- **virtio_mmio.h**: 7 個の新規レジスタオフセット定義 + 4 個の新規構造体フィールド
- **virtio_mmio.c read**: `HOST_FEATURES` と `QUEUE_NUM_MAX` の case 追加 (共に 0 を返す)
- **virtio_mmio.c write**: `HOST_FEATURES_SEL`, `GUEST_FEATURES_SEL`, `GUEST_FEATURES`, `GUEST_PAGE_SIZE`, `QUEUE_SEL` の case 追加

`microkvm.c` や `Makefile` の変更なし。

## 次のステップ

[Step 14: virtqueue setup](step14_virtqueue-setup.md) — QueueNumMax に非ゼロ値を返し、kernel が virtqueue を確立できるようにする。ここで shared memory ring buffer が確立される。
