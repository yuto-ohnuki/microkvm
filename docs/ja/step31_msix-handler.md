# Step 31: MSI-X 割り込みハンドラ — completion 駆動の DMA

## 目的

RESULT レジスタの polling を割り込み駆動の完了通知に置き換える。ドライバが MSI-X handler を登録し、DMA 完了時に待機スレッドを起床 — VMM の `KVM_SIGNAL_MSI` から guest LAPIC を経由してドライバの handler まで、end-to-end の割り込み配送を実証する。

## 背景

### Polling から割り込みへ

Step 30 は doorbell 直後に RESULT を読んだ — microkvm の DMA が同期的（VMM が MMIO exit から返る前に完了）なので動く。しかし実デバイスは非同期に完了する。MSI-X によりデバイスが「完了した」とドライバに通知できる。

```
Step 30: doorbell → DMA → readl(RESULT)         (polling)
Step 31: doorbell → DMA → MSI-X IRQ → handler   (interrupt-driven)
```

### MSI-X ドライバ API

```c
pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX)  → MSI-X vector 1本確保
pci_irq_vector(pdev, 0)                           → Linux IRQ 番号取得
request_irq(irq, handler, 0, "name", data)        → handler 登録
```

`pci_alloc_irq_vectors` を呼ぶと Linux が MSI-X テーブル (BAR0 + 0x800) に LAPIC アドレスと vector 番号を書く。VMM はこれを記録し、DMA 完了後に `KVM_SIGNAL_MSI` で使う。

### completion

```c
struct completion dma_done;
init_completion(&dma_done);

/* プロセスコンテキスト: doorbell kick 後に待機 */
wait_for_completion_timeout(&dma_done, HZ * 5);

/* 割り込みコンテキスト: handler が通知 */
complete(&dma_done);
```

`completion` は割り込みハンドラ（atomic context）からプロセスコンテキスト（sleepable）への同期の標準メカニズム。spinlock + wait queue で実装。

```
状態遷移:
  init_completion()  → IDLE
  wait_for_...()     → SLEEPING (プロセスがブロック)
  complete()         → RUNNING (プロセスが起床)
```

`wait_for_completion_timeout` は安全装置 — デバイスが完了を通知しない場合（ハードウェア障害、VMM バグ等）にドライバが永久にハングすることを防ぐ。

### なぜ MP table が必要か

`pci_alloc_irq_vectors(PCI_IRQ_MSIX)` は MSI IRQ domain を必要とする。これは IOAPIC 初期化時に作成される。BIOS/ACPI がないと Linux は IOAPIC を発見できないため、VMM が Intel MP table を guest RAM (GPA 0xF0000, e820 reserved 領域) に配置する。

MP table の内容:
- 1 processor (BSP)
- 2 buses (ISA + PCI)
- 1 IOAPIC (addr 0xFEC00000)
- 16 ISA interrupt routing entries

### Phase E Step 26 との対応

| Step 26 (VMM) | Step 31 (Driver) |
|---|---|
| MSI-X table の MMIO write を記録 | pci_alloc_irq_vectors → Linux が table に書く |
| DMA 完了後に KVM_SIGNAL_MSI | request_irq → handler が実行される |
| "No irq handler" (Step 26) | handler 登録済み → IRQ_HANDLED |
| devmem で手動設定 | Linux IRQ subsystem が自動設定 |

## 実行フロー

```
Guest (driver)               KVM                VMM
──────────────               ───                ───
pci_alloc_irq_vectors()
  → Linux が MSI-X table に書く:
    addr=0xFEE00000, data=0x22
                             KVM_EXIT_MMIO
                                                pci_msix_write(): addr/data を記録

request_irq(handler)

writel(1, DOORBELL)
                             KVM_EXIT_MMIO
                                                DMA 実行
                                                KVM_SIGNAL_MSI(addr, data)
                             vector 0x22 を注入

microkvm_irq_handler():
  readl(STATUS)
  complete(&dma_done)

wait_for_completion() が返る
  readl(RESULT) → 18 bytes
```

## 実装

### VMM 変更 (platform.c + boot.c)

新規ファイル `platform.c` / `platform.h` — MP table セットアップ:
```c
void setup_mp_table(void *mem) {
    /* MP Floating Pointer at 0xF0000 */
    /* MP Config Table at 0xF0010:
       - 1 processor, 2 buses, 1 IOAPIC, 16 IRQ entries */
}
```

`boot.c` の `load_initramfs()` から呼び出し。e820 で 0x9F000–0x100000 を reserved にする設定も必要（boot.c に実装済み）。

### ドライバ変更 (microkvm_pci.c)

```c
#include <linux/interrupt.h>
#include <linux/completion.h>

struct microkvm_dev {
    ...
    struct completion dma_done;
};

/* MSI-X interrupt handler — VMM が KVM_SIGNAL_MSI で注入後に呼ばれる */
static irqreturn_t microkvm_irq_handler(int irq, void *data) {
    struct microkvm_dev *mdev = data;
    complete(&mdev->dma_done);
    return IRQ_HANDLED;
}

/* probe 内: */
    init_completion(&mdev->dma_done);
    pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
    irq = pci_irq_vector(pdev, 0);
    request_irq(irq, microkvm_irq_handler, 0, "microkvm_pci", mdev);

    /* doorbell 後: */
    wait_for_completion_timeout(&mdev->dma_done, HZ * 5);

/* remove 内: */
    free_irq(pci_irq_vector(pdev, 0), mdev);
    pci_free_irq_vectors(pdev);
```

## 出力

```
/ # insmod /lib/modules/microkvm_pci.ko
[pci-msix] table write offset=0x800 val=0xfee00000
[pci-msix] table write offset=0x808 val=0x22
[pci-dev] MMIO read  offset=0x00 → 0x1
microkvm_pci 0000:00:00.0: STATUS = 0x1
[pci-dev] MMIO write offset=0x0c ← 0x4136000
[pci-dev] MMIO write offset=0x10 ← 0x0
[pci-dev] MMIO write offset=0x04 ← 0x1
[pci-dma] DMA read: 18 bytes from GPA 0x4136010
hello from driver
[pci-msix] IRQ injected: addr=0xfee00000 data=0x22
[pci-dev] MMIO read  offset=0x00 → 0x1
microkvm_pci 0000:00:00.0: IRQ: status=0x1
[pci-dev] MMIO read  offset=0x08 → 0x12
microkvm_pci 0000:00:00.0: DMA done via MSI-X, transferred 18 bytes
/ # rmmod microkvm_pci
microkvm_pci 0000:00:00.0: remove called
```

全フロー: Linux が MSI-X table をプログラム → ドライバが doorbell kick → VMM が DMA 実行 → VMM が MSI-X 注入 → handler 起動 → completion が thread を wake → ドライバが result 確認。

## 重要な知見

MSI-X 割り込み配送が非同期デバイス I/O のループを閉じる。ドライバは作業を投入（doorbell）して sleep。デバイスが作業完了して MSI-X で通知。Handler が thread を起床。この submit → sleep → interrupt → wake パターンは全ての現代ドライバが非同期 I/O を処理する方法 — NVMe completion queue から network NAPI まで。microkvm での追加課題は、実 BIOS が提供する platform topology（MP table）を VMM 側で用意すること。これがないと Linux が MSI IRQ domain を初期化できない。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| pci_alloc_irq_vectors | Linux が MSI-X table に LAPIC addr + vector を書く |
| request_irq | MSI-X vector の handler 登録 |
| completion | 割り込みハンドラ → プロセスコンテキストの同期 |
| wait_for_completion_timeout | handler が signal するか timeout まで sleep |
| MP table | IOAPIC 発見のための platform topology（BIOS/ACPI 非提供時） |
| e820 reserved | MP table 領域を Linux に上書きされないよう保護 |
| IRQ_HANDLED | 割り込み処理完了の報告 |
| End-to-end path | ドライバ → VMM DMA → KVM_SIGNAL_MSI → LAPIC → handler |

## 変わったこと

Step 30 からの変更:
- **新規ファイル**: `platform.c`, `platform.h`（MP table セットアップ）
- **boot.c**: `#include "platform.h"`, `setup_mp_table(mem)` 呼び出し, boot.h に `MP_TABLE_ADDR`
- **driver/microkvm_pci.c**: `<linux/interrupt.h>`, `<linux/completion.h>`, IRQ handler, `pci_alloc_irq_vectors`, `request_irq`, `wait_for_completion_timeout`, remove にクリーンアップ
- **Makefile**: `platform.c` 追加

## 次のステップ

Phase F 完了。ドライバの全ライフサイクル:

```
Step 28: Probe (ID match)
Step 29: BAR mapping (readl/writel)
Step 30: DMA (descriptor + doorbell)
Step 31: MSI-X (interrupt-driven completion)
```

microkvm は仮想 PCI デバイスの**両側** — VMM デバイスモデルと guest kernel ドライバ — を実装し、ドライバ投入から DMA 経由の割り込み駆動完了までの完全なデータパスを実証する。
