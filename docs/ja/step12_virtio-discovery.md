# Step 12: virtio-mmio デバイス検出

> **Phase C: 高性能 I/O (virtio)**
>
> Phase B は UART でシリアル I/O を実現した — 動くが遅い（1バイトごとに 1 VM exit）。
> Phase C では標準的な準仮想化 I/O フレームワークである virtio を実装する: 共有メモリリング、
> バッチ通知、カーネル内イベント配送（ioeventfd/irqfd）。

## 目的

VMM に virtio-mmio レジスタ空間を実装し、Linux kernel の virtio-mmio ドライバがデバイスを発見・認識できるようにする。shared memory I/O への第一歩。

## 背景

### なぜ virtio か?

Step 10–11 では guest は 8250 UART (ポート 0x3F8) を通じて通信していた。1文字ごとに VM exit (PIO trap) が発生する。高スループット I/O にはこのモデルは高コストすぎる。

Virtio は **shared memory ring buffer** (virtqueue) にデータを配置して解決する。guest と VMM がリングバッファ経由でデータを交換し、バッチごとに1回の「kick」通知だけで済む — 1バイトごとの exit ではなく。

### virtio-mmio トランスポート

virtio 仕様は複数のトランスポート (PCI, MMIO, Channel I/O) を定義している。microkvm では最もシンプルな **virtio-mmio** を使用 — PCI バスのエミュレーションが不要。デバイスは固定 guest 物理アドレスのフラットな MMIO レジスタ領域として現れる。

Linux は kernel command line でデバイスを検出する:
```
virtio_mmio.device=0x200@0xd0000000:5
```
フォーマット: `<サイズ>@<ベースアドレス>:<IRQ番号>`。kernel 内部の `virtio_mmio_cmdline_devices()` がこれをパースし、platform device を登録して `virtio_mmio_probe()` でレジスタを読みに行く。

### なぜ GPA 0xD0000000 か?

guest RAM は 128 MB (0x0–0x07FFFFFF)。デバイスを 0xD0000000 に置くことで、guest RAM に背後づけされておらず、KVM に登録されたどのメモリスロットにも属さないことが保証される。guest がこのアドレスにアクセスすると EPT マッピングが存在しない → EPT violation → `KVM_EXIT_MMIO` — Step 5 と同じ仕組みを、実際のデバイスプロトコルに使う。

### レジスタレイアウト (識別)

| Offset | 名前 | 値 | 意味 |
|--------|------|-------|---------|
| 0x000 | MagicValue | 0x74726976 | ASCII "virt" (リトルエンディアン) — virtio デバイスであることを確認 |
| 0x004 | Version | 1 | Legacy インターフェース (v1)。microkvm は初期実装を小さく保つため v1 を使用。modern (v2) は FEATURES_OK ステップが追加され、将来対応可能 |
| 0x008 | DeviceID | 3 | デバイス種別: virtio-console (1=net, 2=block, 3=console)。Step 11 の UART の延長として選択 |
| 0x00C | VendorID | 0x4D4B564D | "MKVM" — 任意の識別値（Linux ドライバはこの値をチェックしない） |
| 0x070 | Status | (読み書き) | Virtio デバイス/ドライバ初期化状態マシン (ACKNOWLEDGE → DRIVER → DRIVER_OK) |

## 実行フロー

```
Linux kernel                              KVM                      VMM (microkvm)
────────────                              ───                      ──────────────
                                                                   CMDLINE に含む:
                                                                   "virtio_mmio.device=0x200@0xd0000000:5"

boot 完了後:
  virtio_mmio_cmdline_devices()
  CMDLINE をパース → platform_device 登録
  (base=0xD0000000)
  ※ kernel 内部関数

  virtio_mmio_probe():
    read [0xD0000000 + 0x000]
      ↓ EPT violation (RAM 外)
                                          KVM_EXIT_MMIO
                                          phys_addr=0xD0000000 ──→ offset=0x000
                                                                   virtio_mmio_read()
                                                                   → return 0x74726976 ("virt")
                                          KVM_RUN ←────────────────
    eax = 0x74726976
    "Magic OK, virtio デバイスだ!"

    read DeviceID (0x008) → 3
    "virtio-console だ"

    write Status (0x070) ← 0x0           → デバイスリセット
    write Status (0x070) ← 0x1           → ACKNOWLEDGE
    write Status (0x070) ← 0x3           → DRIVER

  virtio_console ドライバが probe:
    → virtqueue setup を試行
    → QueueNumMax = 0 → 失敗
    → Status = 0x83 (FAILED) を書き込む
```

## 実装

### 新規ファイル

**`virtio_mmio.h`** — レジスタオフセット定義とデバイス状態:
```c
#define VIRTIO_MMIO_BASE  0xD0000000
#define VIRTIO_MMIO_SIZE  0x200

struct virtio_mmio_dev {
    uint32_t status;
};
```

**`virtio_mmio.c`** — Read/Write ハンドラ:
```c
uint32_t virtio_mmio_read(struct virtio_mmio_dev *dev, uint64_t offset, int len)
{
    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE: return 0x74726976;
    case VIRTIO_MMIO_VERSION:     return 1;
    case VIRTIO_MMIO_DEVICE_ID:   return 3;
    case VIRTIO_MMIO_VENDOR_ID:   return 0x4D4B564D;
    case VIRTIO_MMIO_STATUS:      return dev->status;
    default:                      return 0;
    }
}
```

### microkvm.c の変更

1. kernel command line を更新:
```c
#define CMDLINE "console=ttyS0 earlyprintk=serial rdinit=/init virtio_mmio.device=0x200@0xd0000000:5"
```

2. `main()` 内でデバイス初期化:
```c
virtio_mmio_init(&virtio_dev);
```

3. MMIO exit ハンドラが virtio レジスタに振り分け:
```c
case KVM_EXIT_MMIO: {
    uint64_t addr = run->mmio.phys_addr;
    if (addr >= VIRTIO_MMIO_BASE && addr < VIRTIO_MMIO_BASE + VIRTIO_MMIO_SIZE) {
        uint64_t offset = addr - VIRTIO_MMIO_BASE;
        if (run->mmio.is_write) {
            uint32_t val = 0;
            memcpy(&val, run->mmio.data, run->mmio.len);
            virtio_mmio_write(&virtio_dev, offset, val, run->mmio.len);
        } else {
            uint32_t val = virtio_mmio_read(&virtio_dev, offset, run->mmio.len);
            memcpy(run->mmio.data, &val, run->mmio.len);
        }
    }
    break;
}
```

### 前提条件

Phase C では以下の kernel config オプションを追加して bzImage を再ビルド:
```
CONFIG_VIRTIO_MENU=y
CONFIG_VIRTIO=y
CONFIG_VIRTIO_RING=y
CONFIG_VIRTIO_MMIO=y
CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_HVC_DRIVER=y
```

## 出力

```
virtio-mmio: Registering device virtio-mmio.0 at 0xd0000000-0xd00001ff, IRQ 5.
[virtio-mmio] read  offset=0x000 → 0x74726976
[virtio-mmio] read  offset=0x004 → 0x1
[virtio-mmio] read  offset=0x008 → 0x3
[virtio-mmio] read  offset=0x00c → 0x4d4b564d
[virtio-mmio] write offset=0x028 ← 0x1000
[virtio-mmio] write offset=0x070 ← 0x0
[virtio-mmio] device reset
[virtio-mmio] write offset=0x070 ← 0x1
[virtio-mmio] write offset=0x070 ← 0x3
...
virtio_console virtio0: Error -2 initializing vqs
```

probe は成功（デバイス認識、Status が DRIVER に到達）するが、`QueueNumMax` が 0 を返すため virtqueue setup が失敗する。QueueNumMax はドライバに「この virtqueue には最大何個の descriptor を入れられるか」を伝えるレジスタ — 0 は「queue が使えない」を意味する。これは想定通りで、Step 14 で解決する。

## 重要な知見

virtio-mmio のデバイス検出は Step 5 の MMIO デバイスと全く同じ仕組みを再利用している — EPT に登録されていない GPA へのアクセスが VM exit を引き起こし、VMM がレジスタ応答を返す。違いは「自作の1レジスタ toy デバイス」ではなく「virtio spec に準拠した標準プロトコル」を実装している点。これにより Linux の既存 virtio ドライバがそのまま動作する — guest 側にカスタムコードは一切不要。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| virtio-mmio トランスポート | 固定 GPA のレジスタ領域、PCI バス不要 |
| デバイス識別 | Magic/Version/DeviceID/VendorID の読み取り |
| MMIO ディスパッチ | Step 5 の KVM_EXIT_MMIO を実プロトコルに活用 |
| Status 状態遷移 | ACKNOWLEDGE → DRIVER (virtio spec の初期化シーケンス) |
| kernel command line | `virtio_mmio.device=size@base:irq` でデバイス位置を宣言 |
| EPT ベースの MMIO trap | 0xD0000000 は RAM 外 → 自動的に EPT violation |

## 変わったこと

Step 11 からの変更:
- **新規ファイル**: `virtio_mmio.c`, `virtio_mmio.h`
- **CMDLINE**: `virtio_mmio.device=0x200@0xd0000000:5` を追加
- **KVM_EXIT_MMIO ハンドラ**: アドレスを判定して virtio レジスタの read/write にディスパッチ
- **Kernel**: CONFIG_VIRTIO_* オプションを追加して再ビルド
- **Makefile**: `virtio_mmio.c` をビルド対象に追加

## 次のステップ

[Step 13: virtio feature negotiation](step13_virtio-features.md) — HostFeatures/GuestFeatures レジスタを実装し、kernel が feature negotiation を完了できるようにする。

**Step 13–15 の予告:** デバイス検出の後、ドライバは機能のネゴシエーション (Step 13)、*virtqueue* と呼ばれる共有メモリ ring buffer の確立 (Step 14)、そしてこれらのリングを通じた実データ転送 (Step 15) を行う。これらのステップが答える核心的な問いは「guest と VMM は、CPU を1バイトごとに止めずに、どうやってメモリを効率的に共有するか？」。
