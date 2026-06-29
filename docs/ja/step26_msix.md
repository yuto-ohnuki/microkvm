# Step 26: MSI-X エミュレーション — KVM_SIGNAL_MSI

## 目的

DMA 完了後に MSI-X (Message Signaled Interrupts - Extended) でデバイスから guest に割り込み通知する。RESULT レジスタを polling する代わりに、デバイスが LAPIC に直接メモリ書き込みして CPU に割り込みを発火する。

## 背景

### なぜ MSI-X が必要か

Step 25 では DMA 完了後、guest が RESULT レジスタを読んで完了を確認する必要があった（polling）。MSI-X を使えばデバイスが「DMA 完了」を能動的に通知できる。

### Legacy IRQ (Phase C) vs MSI-X (Phase E)

| | Phase C (IRQ line) | Phase E (MSI-X) |
|---|---|---|
| 仕組み | 物理ピン（共有） | LAPIC へのメモリ書き込み（専用） |
| ベクタ数 | 4本共有 (INTA#–INTD#) | デバイスごとに最大 2048 |
| ルーティング | IOAPIC → LAPIC | 直接 LAPIC |
| 共有問題 | あり（全 ISR を確認） | なし（専用 vector） |
| KVM API | `ioctl(KVM_IRQ_LINE)` | `ioctl(KVM_SIGNAL_MSI)` |

> **Note:** MSI（X なし）もあり、最大 32 の連続 vector をサポートする。MSI-X はこれを拡張し、BAR 空間内のプログラマブルテーブルで最大 2048 の任意 vector を使える。microkvm は高性能デバイスの現代標準である MSI-X を直接実装。

### MSI-X の仕組み

MSI-X 割り込みは*メモリ書き込み*:
1. Guest OS がデバイスの MSI-X テーブルに `address` と `data` を書く
2. `address` = 0xFEE00000 (LAPIC base) + destination APIC ID
3. `data` = 割り込み vector 番号 + delivery mode
4. デバイスが割り込みを発火したい時、address に data を書く → LAPIC が受理 → CPU に割り込み

VMM の観点: guest が MSI-X テーブルに書いた addr/data を記録し、DMA 完了時に `KVM_SIGNAL_MSI` で割り込みを注入する。

### LAPIC

LAPIC (Local Advanced Programmable Interrupt Controller) は各 CPU コアの割り込みコントローラ。memory-mapped base address は 0xFEE00000。MSI-X はこのアドレスに直接書き込んで、中間コントローラ（PIC/IOAPIC）なしで割り込みを配送する。

### KVM_SIGNAL_MSI

```c
struct kvm_msi {
    __u32 address_lo;   /* LAPIC アドレス */
    __u32 address_hi;   /* 通常 0 */
    __u32 data;         /* vector + delivery mode */
};
ioctl(vmfd, KVM_SIGNAL_MSI, &msi);
```

KVM は address から対象 APIC を特定し、data から vector を取り出して guest LAPIC に割り込みを注入する。PIC/IOAPIC routing 不要。

### MSI-X テーブル

MSI-X テーブルは BAR0 MMIO 領域内（microkvm では offset 0x800）。各エントリは 16 bytes:

```
BAR0 レイアウト:
  0x000–0x7FF: デバイスレジスタ (STATUS, DOORBELL, RESULT, DESC_LO/HI)
  0x800–0x80F: MSI-X テーブル (1 entry × 16 bytes)
  0xC00–...:   MSI-X PBA (Pending Bit Array)
```

各 MSI-X テーブルエントリ:
```
Offset 0x00: addr_lo   (LAPIC アドレス)
Offset 0x04: addr_hi   (通常 0)
Offset 0x08: data      (vector 番号)
Offset 0x0C: ctrl      (bit 0: masked)
```

テーブルの場所は PCI config space 内の capability で告知される。

### PCI Capability chain

PCI capabilities は config space 内の linked list。config[0x34] (capabilities pointer) から開始。各 capability は `[cap_id][next_ptr][cap-specific data]`。MSI-X capability (ID=0x11) は MSI-X テーブルの場所（どの BAR、どの offset）を OS に伝える。

## 実行フロー

```
Guest (driver/devmem)        KVM                VMM (microkvm)
─────────────────────        ───                ────────────────

Boot: Linux が cap chain を読む
  config[0x34] → 0x40
  config[0x40] → cap_id=0x11 (MSI-X)
  config[0x44] → table at BAR0+0x800

MSI-X テーブルをプログラム:
  write BAR0+0x800 ← 0xFEE00000
                             KVM_EXIT_MMIO
                                                msix_table[0].addr_lo = 0xFEE00000
  write BAR0+0x808 ← 0x21
                                                msix_table[0].data = 0x21 (vector 33)
  write BAR0+0x80C ← 0x0
                                                msix_table[0].ctrl = 0 (unmasked)

DMA + 割り込み:
  write BAR0+0x04 ← 1 (DOORBELL)
                             KVM_EXIT_MMIO
                                                DMA 実行...
                                                check: ctrl not masked && addr_lo != 0
                                                KVM_SIGNAL_MSI(addr=0xFEE00000, data=0x21)
                             vector 33 を注入
  IRQ handler 起動 (or "No irq handler" — ドライバ未登録)
```

## 実装

### pci.h 追加

```c
#define PCI_MSIX_TABLE_OFFSET   0x800
#define PCI_MSIX_TABLE_ENTRIES  1

struct msix_entry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t data;
    uint32_t ctrl;      /* bit 0: masked */
};

/* struct pci_device 内: */
    struct msix_entry msix_table[PCI_MSIX_TABLE_ENTRIES];
    int vmfd;   /* KVM_SIGNAL_MSI 用 */
```

### pci.c — pci_init() に MSI-X capability

```c
    dev->config[0x34] = 0x40;       /* capabilities pointer */
    dev->config[0x40] = 0x11;       /* cap ID = MSI-X */
    dev->config[0x41] = 0x00;       /* next = NULL */
    *(uint16_t *)&dev->config[0x42] = 0x0000;  /* 1 entry, not masked */
    *(uint32_t *)&dev->config[0x44] = PCI_MSIX_TABLE_OFFSET;  /* table BIR=0 */
    *(uint32_t *)&dev->config[0x48] = 0x00000C00;  /* PBA BIR=0 */
```

### pci.c — DMA 完了後の injection

```c
        /* Inject MSI-X after DMA completion */
        if (!(dev->msix_table[0].ctrl & 1) && dev->msix_table[0].addr_lo) {
            struct kvm_msi msi = {
                .address_lo = dev->msix_table[0].addr_lo,
                .address_hi = dev->msix_table[0].addr_hi,
                .data = dev->msix_table[0].data,
            };
            ioctl(dev->vmfd, KVM_SIGNAL_MSI, &msi);
        }
```

条件: masked でない (ctrl bit 0 == 0) かつ addr_lo != 0（テーブルがプログラム済み）。

### pci.c — msix_read/write

```c
/* Read MSI-X table entry — guest driver reads to check vector configuration */
uint32_t pci_msix_read(struct pci_device *dev, uint64_t offset) {
    uint32_t *table = (uint32_t *)dev->msix_table;
    return table[(offset - PCI_MSIX_TABLE_OFFSET) / 4];
}

/* Write MSI-X table entry — guest driver programs addr/data/ctrl for each vector */
void pci_msix_write(struct pci_device *dev, uint64_t offset, uint32_t value) {
    uint32_t *table = (uint32_t *)dev->msix_table;
    table[(offset - PCI_MSIX_TABLE_OFFSET) / 4] = value;
}
```

### microkvm.c — BAR0 MMIO routing 分岐

```c
if (offset >= PCI_MSIX_TABLE_OFFSET && ...) {
    /* MSI-X table アクセス */
    pci_msix_read/write(...)
} else {
    /* Device registers */
    pci_dev_mmio_read/write(...)
}
```

## 出力

```
/ # devmem 0x07F00000 32 0x07F00100
/ # devmem 0x07F00004 32 0x00000000
/ # devmem 0x07F00008 32 0x00000005
/ # devmem 0x07F0000C 32 0x00000000
/ # devmem 0x07F00100 32 0x6C6C6568
/ # devmem 0x07F00104 8 0x6F
/ # devmem 0x0800000C 32 0x07F00000
[pci-dev] MMIO write offset=0x0c ← 0x7f00000
/ # devmem 0x08000800 32 0xFEE00000
[pci-msix] table write offset=0x800 val=0xfee00000
/ # devmem 0x08000804 32 0x0
[pci-msix] table write offset=0x804 val=0x0
/ # devmem 0x08000808 32 0x21
[pci-msix] table write offset=0x808 val=0x21
/ # devmem 0x0800080C 32 0x0
[pci-msix] table write offset=0x80c val=0x0
/ # devmem 0x08000004 32 0x1
[pci-dev] MMIO write offset=0x04 ← 0x1
[pci-dma] DMA read: 5 bytes from GPA 0x7f00100
hello[pci-msix] IRQ injected: addr=0xfee00000 data=0x21
No irq handler for 0.33
```

手順:
1. DMA descriptor + data buffer 設定（Step 25 と同じ）
2. MSI-X テーブルをプログラム: addr_lo=0xFEE00000 (LAPIC), data=0x21 (vector 33), ctrl=0 (unmasked)
3. Doorbell kick → DMA 実行 → MSI-X 割り込み注入
4. Linux が vector 33 を受信（"No irq handler" — ドライバ未登録だが割り込み配送は成功）

MSI-X テーブル未プログラム時は doorbell 動作するが injection はスキップ（Step 25 と同じ動作）。

## 重要な知見

MSI-X は割り込みを「共有物理ピン + コントローラ経由のルーティング」から「CPU の LAPIC への直接メモリ書き込み」に変える。Guest OS が宛先（どの CPU、どの vector）を MSI-X テーブルに書き、デバイスはその address/data ペアを `KVM_SIGNAL_MSI` で再生するだけ。これにより割り込み共有が消え、ルーティングレイテンシが減り、キューごとの専用 vector が可能になる（multi-queue NVMe/NIC に必須）。デバイスが複数の DMA キューを持ち、それぞれに完了割り込みが必要な場合に特に重要。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| MSI-X | LAPIC (0xFEE00000) へのメモリ書き込みで割り込み |
| MSI-X テーブル | BAR0+0x800, 16 bytes/entry: addr_lo, addr_hi, data, ctrl |
| PCI capability chain | config[0x34]→0x40, cap_id=0x11 でテーブル位置を告知 |
| KVM_SIGNAL_MSI | PIC/IOAPIC routing なしで guest LAPIC に割り込み注入 |
| Masked vs unmasked | ctrl bit 0: guest が一時的に割り込みを抑制可能 |
| Legacy IRQ vs MSI-X | 共有ピン vs 専用 vector, IOAPIC vs 直接 LAPIC |

## 変わったこと

Step 25 からの変更:
- **pci.h**: `<linux/kvm.h>`, `PCI_MSIX_TABLE_OFFSET/ENTRIES`, `struct msix_entry`, pci_device に `msix_table[]` + `vmfd`, `pci_msix_read/write` 宣言
- **pci.c**: `<sys/ioctl.h>`, `pci_init()` に MSI-X capability, doorbell handler に injection, `pci_msix_read/write` 実装
- **microkvm.c**: MSI-X table MMIO routing, `pci_dev.vmfd = vmfd`

## 次のステップ

[Step 27: PCI hotplug](step27_hotplug.md) では `Ctrl-A h` で runtime にデバイスを追加/削除する。2つ目の PCI デバイスが absent (vendor=0xFFFF) で起動し、toggle 後に sysfs rescan で発見される。
