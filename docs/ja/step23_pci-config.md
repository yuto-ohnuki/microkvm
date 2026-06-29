# Step 23: PCI config space — CF8/CFC によるデバイス発見

> **Phase E: PCI Device Model**
>
> Phase A–D で hypervisor、I/O パス、メモリ管理を構築した。
> Phase E では x86 標準のデバイス発見メカニズムである PCI を実装する。
> Phase C の virtio-mmio（kernel cmdline でアドレス指定）とは異なり、
> PCI では Linux が自らバスを走査してデバイスを発見する。

## 目的

PCI Configuration Mechanism #1（CF8/CFC I/O ポート）を実装し、Linux が boot 時の標準的な bus enumeration でカスタム PCI デバイスを発見するようにする。

## 背景

### PCI とは

PCI (Peripheral Component Interconnect) は x86 システムのデバイス接続標準。OS に以下の仕組みを提供する:
1. **発見**: どのデバイスが存在するか（enumeration）
2. **識別**: 各デバイスの種類（vendor ID, device ID, class）
3. **リソース割り当て**: メモリ領域（BAR）、割り込み

NIC、NVMe、GPU、USB コントローラ — 現代 x86 の全デバイスは PCI デバイス。

Guest の視点から PCI は2つの主要部分で構成される:
- **Configuration space**（このステップ）: 各デバイスの 256 byte「名刺」、CF8/CFC 経由でアクセス
- **BAR の背後のデバイスレジスタ**（Step 24）: 実際の操作用レジスタ、enumeration で割り当てたアドレスに MMIO でアクセス

### Phase C と Phase E の違い

| | Phase C (virtio-mmio) | Phase E (PCI) |
|---|---|---|
| デバイスの見つけ方 | kernel cmdline でアドレスを明示指定 | Linux がバスを走査して自動発見 |
| レジスタアクセス | 固定 MMIO アドレス (0xD0000000) | BAR で動的に割り当てられたアドレス |
| データ転送 | Virtqueue (共有メモリリング) | DMA descriptor (Step 25) |
| 割り込み | 固定 IRQ line (GSI 5) | MSI-X (専用 vector, Step 26) |

### Configuration Mechanism #1

x86 の PCI config space アクセスは2つの I/O ポートを使う。この仕組みは PCI Express より前に策定され、互換性のため全ての x86 システムでサポートが続いている:

```
Port 0xCF8 (CONFIG_ADDRESS):
  ┌───┬────────┬────────┬──────────┬───────────────┬──┐
  │31 │ 23:16  │ 15:11  │  10:8    │    7:2        │1:0│
  │ 1 │  bus   │ device │ function │ register(dword)│ 0 │
  └───┴────────┴────────┴──────────┴───────────────┴──┘

Port 0xCFC (CONFIG_DATA):
  0xCF8 で選択した config register を read/write する
```

Linux は全ての bus/device/function に対して offset 0x00 (vendor ID) を読む。結果が 0xFFFF なら「デバイスなし」、それ以外なら config header 全体を読んでデバイスを識別・設定する。

### BAR (Base Address Register)

BAR はデバイスが必要とする MMIO 領域のサイズと、OS が割り当てたアドレスを保持する:

```
OS が BAR に 0xFFFFFFFF を書く → デバイスが size mask を返す
  例: 0xFFFFF000 → ~0xFFFFF000 + 1 = 0x1000 = 4KB

OS が最終アドレスを書く → デバイスがそのアドレスを使用
  例: 0x08000000 → デバイスレジスタが GPA 0x08000000 から始まる
```

「全ビット1を書いて mask を読み返す」プロトコルにより、OS はハードコードなしで各デバイスの必要リソースを知る。

例:
```
Write: 0xFFFFFFFF
Read:  0xFFFFF000 (mask)
Size:  ~0xFFFFF000 + 1 = 0x00001000 = 4096 bytes
```

### なぜ pci=conf1 が必要か

microkvm は BIOS も ACPI テーブルも提供しない。Linux は通常:
1. ACPI MCFG テーブル → PCIe ECAM（memory-mapped）
2. BIOS PCI サービス → `int 0x1a`
3. CF8/CFC の直接 probe

の順で自動検出する。BIOS/ACPI がないと自動検出が失敗する。`pci=conf1` は「CF8/CFC を直接使え」と強制する。

## 実行フロー

```
Guest (Linux boot)           KVM                VMM (microkvm)
────────────────────         ───                ────────────────
                                                pci_init(): vendor=0x1234,
                                                  device=0x0001, class=0xFF

PCI: Using conf type 1
for each bus/dev/func:
  outl(0xCF8, addr)
                             KVM_EXIT_IO
                             port=0xCF8, OUT
                                                config_address に保存

  inl(0xCFC)
                             KVM_EXIT_IO
                             port=0xCFC, IN
                                                config_address から BDF decode
                                                00:00.0 → pci_config_read()
                                                他 → 0xFFFFFFFF 返却

  vendor != 0xFFFF → 発見!
  BAR0 probe:
    BAR0 に 0xFFFFFFFF 書き込み
                                                pci_config_write(): mask 格納
    BAR0 読み出し
                                                → 0xFFFFF000 (4KB) を返却
    割り当てアドレス書き込み
                                                → 0x08000000 を格納
```

## 実装

### 前提条件

kernel config（bzImage 再ビルド必要）:
```
CONFIG_PCI=y
CONFIG_PCI_DIRECT=y
```

kernel cmdline に `pci=conf1` を追加。

### pci.h — デバイス構造体と定数

```c
#define PCI_CONFIG_ADDR_PORT  0x0CF8
#define PCI_CONFIG_DATA_PORT  0x0CFC
#define PCI_VENDOR_ID         0x1234
#define PCI_DEVICE_ID         0x0001
#define PCI_BAR0_SIZE         4096

struct pci_device {
    uint8_t config[256];       /* Type 0 config header */
    uint32_t bar0_mask;        /* BAR probing 用の size mask */
    uint32_t config_address;   /* 0xCF8 に最後に書かれた値 */
};
```

### pci.c — config space 初期化とアクセス

```c
void pci_init(struct pci_device *dev) {
    *(uint16_t *)&dev->config[0x00] = PCI_VENDOR_ID;   /* 0x1234 */
    *(uint16_t *)&dev->config[0x02] = PCI_DEVICE_ID;   /* 0x0001 */
    dev->config[0x0B] = 0xFF;   /* class: unassigned — 標準ドライバが bind しない */
    dev->config[0x0E] = 0x00;   /* header type: 0 = endpoint（PCI bridge ではない）*/
    dev->bar0_mask = ~(PCI_BAR0_SIZE - 1);  /* 0xFFFFF000 */
}
```

意図的に class 0xFF (unassigned) を使用。Linux がデバイスを enumerate するが標準ドライバを bind しない（例: network class 0x02 だとネットワークサブシステムが起動する）。

void pci_config_write(dev, offset, value, len) {
    if (offset == 0x10) {
        /* BAR0: probe は mask を返し、assignment は aligned address を格納 */
        if (value == 0xFFFFFFFF)
            config[0x10] = bar0_mask;
        else
            config[0x10] = value & bar0_mask;
    }
}
```

### microkvm.c — IO exit handler の routing

```c
} else if (port == PCI_CONFIG_ADDR_PORT) {
    /* 32-bit address register の保存/返却 */
} else if (port >= PCI_CONFIG_DATA_PORT && port <= PCI_CONFIG_DATA_PORT + 3) {
    /* config_address から BDF を decode し、pci_config_read/write に routing */
    if (bus == 0 && device == 0 && func == 0)
        → 自分のデバイス
    else
        → 0xFF 返却 (デバイスなし)
}
```

## 出力

```
/ # cat /sys/bus/pci/devices/0000:00:00.0/vendor
0x1234
/ # cat /sys/bus/pci/devices/0000:00:00.0/device
0x0001
```

Linux が標準的な PCI enumeration でデバイスを発見した — virtio-mmio のような kernel cmdline でのデバイス指定は不要。

## 重要な知見

PCI config space は x86 の標準「デバイスディレクトリ」。OS は各 bus/device/function スロットで vendor ID を読み、0xFFFF なら空、それ以外ならデバイスが存在。BAR probing は「全ビット1書き込み→読み返し」プロトコルでハードコードなしにリソースサイズを知る。これが 1992 年から全ての x86 OS がデバイスを発見してきた仕組み — BIOS POST からクラウド VM の boot まで。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| PCI Configuration Mechanism #1 | CF8 (address) + CFC (data) I/O ポートペア |
| BDF アドレッシング | bus:device.function で各スロットを識別 |
| Type 0 config header | vendor/device/class/BAR を含む 256 byte の「名刺」 |
| BAR probing | 0xFFFFFFFF 書き込み → mask 読み返し → サイズ計算 |
| pci=conf1 | BIOS/ACPI 非提供時に CF8/CFC を強制使用 |
| 0xFFFF = デバイスなし | 空スロットの PCI 標準シグナル |

## 変わったこと

Step 22 からの変更:
- **新規ファイル**: `pci.h`（定数、構造体）、`pci.c`（init, config_read, config_write）
- **microkvm.c**: `#include "pci.h"`, `pci_dev` グローバル, CF8/CFC handling in IO exit, `pci_init()` 呼び出し, CMDLINE += `pci=conf1`
- **Makefile**: `pci.c` 追加

## 次のステップ

[Step 24: PCI MMIO device registers](step24_pci-mmio.md) では Linux が BAR0 に割り当てたアドレスの先に実際のデバイスレジスタを実装する。Guest が BAR0 + offset にアクセス → KVM_EXIT_MMIO → VMM がデバイス状態で応答する。
