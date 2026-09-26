# Step 24: PCI config space — CF8/CFC によるデバイス発見

> **Phase E: PCI Device Model**
>
> Phase A–D で hypervisor、I/O パス、メモリ管理を構築した。
> Phase E では x86 標準のデバイス発見メカニズムである PCI を実装する。
> Phase C の virtio-mmio（kernel cmdline でアドレス指定）とは異なり、PCI では Linux が自らバスを走査してデバイスを発見する。

## 目的

PCI Configuration Mechanism #1（CF8/CFC I/O ポート）を実装し、Linux が boot 時の標準的な bus enumeration でカスタム PCI デバイスを発見するようにする。

## 背景

### PCI とは

PCI (Peripheral Component Interconnect) は x86 システムのデバイス接続標準。OS に以下の仕組みを提供する:
1. **発見**: どのデバイスが存在するか（enumeration）
2. **識別**: 各デバイスの種類（vendor ID, device ID, class）
3. **リソース割り当て**: メモリ領域（BAR）、割り込み

NIC、NVMe、GPU、USB コントローラなど、多くのデバイスが PCI / PCI Express を利用する。

Guest の視点から PCI は2つの主要部分で構成される:
- **Configuration space**（このステップ）: 各デバイスの識別情報や設定を持つ256 byteの領域、CF8/CFC 経由でアクセス
- **BAR の背後のデバイスレジスタ**（Step 25）: 実際の操作用レジスタ、enumeration で割り当てたアドレスに MMIO でアクセス

### Phase C と Phase E の違い

| | Phase C (virtio-mmio) | Phase E (PCI) |
|---|---|---|
| デバイスの見つけ方 | kernel cmdline でアドレスを明示指定 | Linux がバスを走査して自動発見 |
| レジスタアクセス | 固定 MMIO アドレス (0xD0000000) | BAR で動的に割り当てられたアドレス |
| データ転送 | Virtqueue (共有メモリリング) | DMA descriptor (Step 26) |
| 割り込み | 固定 IRQ line (GSI 5) | MSI-X (専用 vector, Step 27) |

### Configuration Mechanism #1

このステップでは、x86 の PCI Configuration Mechanism #1 を使い、2つの I/O ポートで config space にアクセスする:

```
Port 0xCF8 (CONFIG_ADDRESS):
  bit 31     : Enable（1でアクセス有効）
  bits 30:24 : Reserved（0）
  bits 23:16 : Bus
  bits 15:11 : Device
  bits 10:8  : Function
  bits 7:2   : Register（DWORD 単位）
  bits 1:0   : 0

Port 0xCFC (CONFIG_DATA):
  0xCF8 で選択した config register を read/write する
```

Linux はバスを走査し、デバイスの有無を offset 0x00 (vendor ID) で確認する。結果が 0xFFFF なら「デバイスなし」、それ以外なら config header 全体を読んでデバイスを識別・設定する。

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

microkvm は PCI BIOS サービスや ACPI の PCI 設定情報を提供しない。この構成では `pci=conf1` を指定し、Linux に CF8/CFC による直接アクセスを使わせる。

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

Step 24 の PCI 設定と、Step 25 以降で使う `/dev/mem` の設定をここでまとめて行う:

```ini
CONFIG_PCI=y
CONFIG_PCI_DIRECT=y
CONFIG_DEVMEM=y
# CONFIG_STRICT_DEVMEM is not set
```

`CONFIG_PCI` / `CONFIG_PCI_DIRECT` は PCI の走査と CF8/CFC アクセス用。`CONFIG_DEVMEM` は `/dev/mem` を有効にし、`CONFIG_STRICT_DEVMEM` の無効化は後続の guest 物理メモリ操作に備えた設定。

ホストの Linux ソースディレクトリで、既存の `.config` に追加する。パスは使用中の環境に合わせる:

```bash
$ cd ~/linux-src

$ scripts/config --enable CONFIG_PCI
$ scripts/config --enable CONFIG_PCI_DIRECT
$ scripts/config --enable CONFIG_DEVMEM
$ scripts/config --disable CONFIG_STRICT_DEVMEM

$ make olddefconfig
$ grep -E '^(CONFIG_(PCI|PCI_DIRECT|DEVMEM)=|# CONFIG_STRICT_DEVMEM is not set)' .config

$ make -j"$(nproc)" bzImage
$ cp arch/x86/boot/bzImage ~/microkvm/bzImage
```

`olddefconfig` 後に上記4項目を確認してからビルドする。`pci=conf1` はこのステップの `microkvm.c` の CMDLINE に設定済み。

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
/* 識別情報と BAR0 の設定を抜粋 */
void pci_init(struct pci_device *dev) {
    memset(dev->config, 0, sizeof(dev->config));
    *(uint16_t *)&dev->config[0x00] = PCI_VENDOR_ID;   /* 0x1234 */
    *(uint16_t *)&dev->config[0x02] = PCI_DEVICE_ID;   /* 0x0001 */
    dev->config[0x0B] = 0xFF;   /* class: unassigned */
    dev->config[0x0E] = 0x00;   /* header type: 0 = endpoint（PCI bridge ではない）*/
    dev->bar0_mask = ~(PCI_BAR0_SIZE - 1);  /* 0xFFFFF000 */
}
```

class 0xFF (unassigned) は、この学習用デバイスを既存の標準クラスに分類しないために使う。ドライバとの対応は vendor/device ID や class などの照合で決まり、class だけでは決まらない。

BAR0 の書き込み処理を抜粋する。config space の4バイトを更新し、probe 時は mask、それ以外は4 KiB境界に揃えたアドレスを格納する:

```c
if (offset == 0x10) {
    if (value == 0xFFFFFFFF) {
        *(uint32_t *)&dev->config[0x10] = dev->bar0_mask;
    } else {
        *(uint32_t *)&dev->config[0x10] = value & dev->bar0_mask;
    }
    return;
}
```

`pci_config_read()` / `pci_config_write()` は1・2・4バイトのアクセスを扱い、config space の範囲を確認する。

### microkvm.c — IO exit handler の routing

```c
} else if (port == PCI_CONFIG_ADDR_PORT) {
    /* 32-bit address register の保存/返却 */
} else if (port >= PCI_CONFIG_DATA_PORT && port <= PCI_CONFIG_DATA_PORT + 3) {
    /* config_address から BDF を decode し、pci_config_read/write に routing */
    if (bus == 0 && device == 0 && func == 0)
        → 自分のデバイス
    else
        → 全ビット 1 (0xFFFFFFFF) 返却
}
```

Enable bit が0、または BDF が `00:00.0` 以外なら、読み取りには全ビット1を返す。アクセス先は `(config_address & 0xFC) + (port - 0xCFC)` で求め、CFC〜CFF のバイト位置を反映する。

## 出力

ゲストで vendor / device ID を確認する:

```
/ # cat /sys/bus/pci/devices/0000:00:00.0/vendor
0x1234
/ # cat /sys/bus/pci/devices/0000:00:00.0/device
0x0001
```

Linux が標準的な PCI enumeration でデバイスを発見した — virtio-mmio のような kernel cmdline でのデバイス指定は不要。

## 重要な知見

PCI config space から、OS はデバイスの識別情報と必要なリソースを読み取る。BAR probing では全ビット1を書いて mask を読み返し、領域のサイズを求める。このステップでは発見と BAR0 の設定までを実装し、割り当て先の MMIO レジスタは Step 25 で扱う。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| PCI Configuration Mechanism #1 | CF8 (address) + CFC (data) I/O ポートペア |
| BDF アドレッシング | bus:device.function で各スロットを識別 |
| Type 0 config header | config space 先頭の vendor/device/class/BAR を含むヘッダ |
| BAR probing | 0xFFFFFFFF 書き込み → mask 読み返し → サイズ計算 |
| pci=conf1 | BIOS/ACPI 非提供時に CF8/CFC を強制使用 |
| 0xFFFF = デバイスなし | 空スロットの PCI 標準シグナル |

## 変わったこと

Step 23 からの変更:
- **新規ファイル**: `pci.h`（定数、構造体）、`pci.c`（init, config_read, config_write）
- **microkvm.c**: `#include "pci.h"`, `pci_dev` グローバル, CF8/CFC handling in IO exit, `pci_init()` 呼び出し, CMDLINE += `pci=conf1`
- **Makefile**: `pci.c` 追加

## 次のステップ

[Step 25: PCI MMIO device registers](step25_pci-mmio.md) では Linux が BAR0 に割り当てたアドレスの先に実際のデバイスレジスタを実装する。Guest が BAR0 + offset にアクセス → KVM_EXIT_MMIO → VMM がデバイス状態で応答する。
