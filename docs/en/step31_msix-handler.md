# Step 31: MSI-X interrupt handler — completion-driven DMA

## Goal

Replace polling the RESULT register with interrupt-driven completion. The driver registers an MSI-X handler that wakes the waiting thread when DMA finishes — proving end-to-end interrupt delivery from VMM (`KVM_SIGNAL_MSI`) through the guest LAPIC to the driver's handler.

## Background

### From polling to interrupts

Step 30 read RESULT immediately after doorbell — this works because microkvm's DMA is synchronous (VMM completes before returning from the MMIO exit). But real devices complete asynchronously. MSI-X lets the device notify the driver when work is done.

```
Step 30: doorbell → DMA → readl(RESULT)         (polling)
Step 31: doorbell → DMA → MSI-X IRQ → handler   (interrupt-driven)
```

### MSI-X driver API

```c
pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX)  → allocate 1 MSI-X vector
pci_irq_vector(pdev, 0)                           → get Linux IRQ number
request_irq(irq, handler, 0, "name", data)        → register handler
```

When `pci_alloc_irq_vectors` is called, Linux writes the LAPIC address and vector number into the device's MSI-X table (BAR0 + 0x800). The VMM records these values and uses them in `KVM_SIGNAL_MSI` after DMA completion.

### completion

```c
struct completion dma_done;
init_completion(&dma_done);

/* Process context: wait after doorbell kick */
wait_for_completion_timeout(&dma_done, HZ * 5);

/* Interrupt context: handler signals completion */
complete(&dma_done);
```

`completion` is the standard mechanism for synchronizing between interrupt handlers (atomic context) and process context (sleepable). Built on spinlock + wait queue.

```
State transitions:
  init_completion()  → IDLE
  wait_for_...()     → SLEEPING (process blocks)
  complete()         → RUNNING (process wakes)
```

`wait_for_completion_timeout` adds a safety bound — protects the driver from hanging forever if the device never signals completion (hardware failure, VMM bug, etc.).

### Why MP table is needed

`pci_alloc_irq_vectors(PCI_IRQ_MSIX)` requires an MSI IRQ domain, which is created during IOAPIC initialization. Without BIOS/ACPI, Linux needs an Intel MP table to discover the IOAPIC.

The VMM places the MP table at GPA 0xF0000 (in the e820 reserved region) describing:
- 1 processor (BSP)
- 2 buses (ISA + PCI)
- 1 IOAPIC (addr 0xFEC00000)
- 16 ISA interrupt routing entries

### Phase E Step 26 correspondence

| Step 26 (VMM) | Step 31 (Driver) |
|---|---|
| MSI-X table MMIO write recorded | pci_alloc_irq_vectors → Linux writes table |
| KVM_SIGNAL_MSI after DMA | request_irq → handler fires |
| "No irq handler" (Step 26) | Handler registered → IRQ_HANDLED |
| devmem manual setup | Linux IRQ subsystem programs automatically |

## Execution flow

```
Guest (driver)               KVM                VMM
──────────────               ───                ───
pci_alloc_irq_vectors()
  → Linux writes MSI-X table:
    addr=0xFEE00000, data=0x22
                             KVM_EXIT_MMIO
                                                pci_msix_write(): record addr/data

request_irq(handler)

writel(1, DOORBELL)
                             KVM_EXIT_MMIO
                                                DMA executes
                                                KVM_SIGNAL_MSI(addr, data)
                             inject vector 0x22

microkvm_irq_handler():
  readl(STATUS)
  complete(&dma_done)

wait_for_completion() returns
  readl(RESULT) → 18 bytes
```

## Implementation

### VMM changes (platform.c + boot.c)

New files `platform.c` / `platform.h` — MP table setup:
```c
void setup_mp_table(void *mem) {
    /* MP Floating Pointer at 0xF0000 */
    /* MP Config Table at 0xF0010:
       - 1 processor, 2 buses, 1 IOAPIC, 16 IRQ entries */
}
```

Called from `load_initramfs()` in `boot.c`. Also requires e820 entry marking 0x9F000–0x100000 as reserved (already done in boot.c).

### Driver changes (microkvm_pci.c)

```c
#include <linux/interrupt.h>
#include <linux/completion.h>

struct microkvm_dev {
    ...
    struct completion dma_done;
};

static irqreturn_t microkvm_irq_handler(int irq, void *data) {
    struct microkvm_dev *mdev = data;
    complete(&mdev->dma_done);
    return IRQ_HANDLED;
}

/* In probe: */
    init_completion(&mdev->dma_done);
    pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
    irq = pci_irq_vector(pdev, 0);
    request_irq(irq, microkvm_irq_handler, 0, "microkvm_pci", mdev);

    /* After doorbell: */
    wait_for_completion_timeout(&mdev->dma_done, HZ * 5);

/* In remove: */
    free_irq(pci_irq_vector(pdev, 0), mdev);
    pci_free_irq_vectors(pdev);
```

## Output

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

The complete flow: Linux programs MSI-X table → driver kicks doorbell → VMM does DMA → VMM injects MSI-X → handler fires → completion wakes thread → driver reads result.

## Key insight

MSI-X interrupt delivery closes the loop on asynchronous device I/O. The driver submits work (doorbell) and sleeps. The device completes work and signals via MSI-X. The handler wakes the thread. This submit → sleep → interrupt → wake pattern is how every modern driver handles asynchronous I/O — from NVMe completion queues to network NAPI. The additional challenge in microkvm was providing platform topology (MP table) that a real BIOS would supply, since without it Linux cannot initialize the MSI IRQ domain.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| pci_alloc_irq_vectors | Linux programs MSI-X table with LAPIC addr + vector |
| request_irq | Register handler for MSI-X vector |
| completion | Synchronize interrupt handler → process context |
| wait_for_completion_timeout | Sleep until handler signals or timeout |
| MP table | Platform topology for IOAPIC discovery (no BIOS/ACPI) |
| e820 reserved | Protect MP table region from Linux overwriting |
| IRQ_HANDLED | Confirms interrupt was processed |
| End-to-end path | Driver → VMM DMA → KVM_SIGNAL_MSI → LAPIC → handler |

## What changed

From Step 30:
- **New files**: `platform.c`, `platform.h` (MP table setup)
- **boot.c**: `#include "platform.h"`, call `setup_mp_table(mem)`, `MP_TABLE_ADDR` in boot.h
- **driver/microkvm_pci.c**: `<linux/interrupt.h>`, `<linux/completion.h>`, IRQ handler, `pci_alloc_irq_vectors`, `request_irq`, `wait_for_completion_timeout`, cleanup in remove
- **Makefile**: add `platform.c`

## Next step

Phase F is complete. The full driver lifecycle:

```
Step 28: Probe (ID match)
Step 29: BAR mapping (readl/writel)
Step 30: DMA (descriptor + doorbell)
Step 31: MSI-X (interrupt-driven completion)
```

microkvm now implements **both sides** of a virtual PCI device — the VMM device model and the guest kernel driver — demonstrating the complete data path from driver submission through DMA to interrupt-driven completion.
