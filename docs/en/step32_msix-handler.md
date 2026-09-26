# Step 32: MSI-X Interrupt Handler—Completion-Driven DMA

## Goal

Confirm DMA completion through MSI-X and a completion object before reading RESULT. The driver registers an MSI-X handler that wakes the waiting thread on completion, demonstrating end-to-end interrupt delivery from VMM `KVM_SIGNAL_MSI` through the guest LAPIC to the driver handler.

## Background

### From immediate RESULT reads to interrupt-driven completion

Step 31 read RESULT immediately after the doorbell. This works because microkvm DMA completes synchronously before the VMM returns from MMIO exit handling. Real devices complete asynchronously; MSI-X lets the device notify the driver when work finishes.

```
Step 31: doorbell → DMA → readl(RESULT)
Step 32: doorbell → DMA → MSI-X IRQ → complete() → confirm completion → readl(RESULT)
```

### MSI-X driver APIs

```c
pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);    /* Allocate one MSI-X vector */
irq = pci_irq_vector(pdev, 0);                      /* Get Linux IRQ number */
request_irq(irq, handler, 0, "name", data);         /* Register handler */
```

The 0 in `pci_irq_vector(pdev, 0)` is the allocated vector's index; the return value is a Linux IRQ number. Distinguish this from the CPU vector number in the MSI message.

Calling `pci_alloc_irq_vectors` makes Linux write the LAPIC address and vector number to the MSI-X table (BAR0 + 0x800). The VMM records them and uses them in `KVM_SIGNAL_MSI` after DMA completes.

### completion

```c
struct completion dma_done;
init_completion(&dma_done);

/* Process context: wait after kicking the doorbell */
wait_for_completion_timeout(&dma_done, HZ * 5);

/* Interrupt context: handler signals completion */
complete(&dma_done);
```

A `completion` is a standard synchronization mechanism from an interrupt handler (atomic context) to process context (sleepable), implemented with a spinlock and wait queue.

```
State transitions:
  init_completion() → incomplete
  wait while incomplete → wait for complete() or timeout
  complete()        → record completion; wake a waiter if present
  wait after completion → return without sleeping
```

Completion is recorded even if `complete()` runs before waiting begins. `wait_for_completion_timeout(..., HZ * 5)` waits up to five seconds; 0 means timeout, and a positive return means completion.

### Why an MP table is needed

In this guest environment, Linux could not allocate MSI-X vectors when booted with a PIC-only interrupt configuration. The VMM places an Intel MP table in guest RAM (GPA 0xF0000, an e820-reserved region) to describe an interrupt configuration including an IOAPIC. MSI-X delivery itself bypasses IOAPIC and reaches LAPIC directly.

MP table contents:
- 1 processor (BSP)
- 2 buses (ISA + PCI)
- 1 IOAPIC (addr 0xFEC00000)
- 16 ISA interrupt routing entries

### Correspondence with Phase E, Step 27

| Step 27 (VMM) | Step 32 (Driver) |
|---|---|
| Record MMIO writes to MSI-X table | pci_alloc_irq_vectors → Linux writes table |
| KVM_SIGNAL_MSI after DMA | request_irq → handler runs |
| "No irq handler" (Step 27) | Registered handler → IRQ_HANDLED |
| Manual setup with devmem | Automatic setup by Linux IRQ subsystem |

## Execution flow

```
Guest (driver)               KVM                VMM
──────────────               ───                ───
pci_alloc_irq_vectors()
  → Linux writes MSI-X table:
    addr=0xFEE00000, data=0x22 (example output)
                             KVM_EXIT_MMIO
                                                pci_msix_write(): record addr/data

request_irq(handler)

writel(1, DOORBELL)
                             KVM_EXIT_MMIO
                                                Execute DMA
                                                KVM_SIGNAL_MSI(addr, data)
                             Inject vector 0x22

microkvm_irq_handler():
  readl(STATUS)
  complete(&dma_done)

wait_for_completion_timeout() returns after confirming completion
  readl(RESULT) → 18 bytes
```

## Implementation

### Prerequisites and build

`CONFIG_PCI_MSI`, required for MSI-X, was enabled at the [start of Phase F (Step 29)](step29_pci-driver.md#prerequisites). Here, build the VMM with its MP table and the driver with interrupt support.

```bash
# Build the VMM with MP table support and the driver
$ cd ~/microkvm
$ make
$ cd driver
$ make
```

Replace the `.ko` using [Step 29's initramfs procedure](step29_pci-driver.md#prerequisites), then run `./microkvm` in `~/microkvm`.

### VMM changes (platform.c + boot.c)

New files `platform.c` / `platform.h`—MP table setup:
```c
void setup_mp_table(void *mem) {
    /* MP Floating Pointer at 0xF0000 */
    /* MP Config Table at 0xF0010:
       - 1 processor, 2 buses, 1 IOAPIC, 16 IRQ entries */
}
```

Called from `load_initramfs()` in `boot.c`. The e820 reservation for 0x9F000–0x100000 is also required and already implemented in boot.c.

### Driver changes (microkvm_pci.c)

```c
#include <linux/interrupt.h>
#include <linux/completion.h>

struct microkvm_dev {
    ...
    struct completion dma_done;
};

static irqreturn_t microkvm_irq_handler(int irq, void *data)
{
    struct microkvm_dev *mdev = data;
    u32 status = readl(mdev->bar0 + REG_STATUS);

    dev_info(&mdev->pdev->dev, "IRQ: status=0x%x\n", status);
    complete(&mdev->dma_done);
    return IRQ_HANDLED;
}

/* Inside probe (after allocating mdev): */
    mdev->pdev = pdev;
    init_completion(&mdev->dma_done);

/* After BAR mapping and pci_set_master(): */
    /* Allocate MSI-X vector */
    nvec = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
    if (nvec < 0) {
        dev_err(&pdev->dev, "Failed to allocate MSI-X vector: %d\n", nvec);
        ret = nvec;
        goto err_iomap;
    }

    /* Get Linux IRQ number for MSI-X vector 0 */
    irq = pci_irq_vector(pdev, 0);
    ret = request_irq(irq, microkvm_irq_handler, 0, "microkvm_pci", mdev);
    if (ret) {
        dev_err(&pdev->dev, "Failed to request IRQ: %d\n", ret);
        goto err_irq_vec;
    }

/* After doorbell: */
    /* Wait for completion via MSI-X interrupt */
    if (!wait_for_completion_timeout(&mdev->dma_done, HZ * 5)) {
        dev_err(&pdev->dev, "DMA timeout!\n");
    } else {
        result = readl(mdev->bar0 + REG_RESULT);
        dev_info(&pdev->dev, "DMA done via MSI-X, transferred %u bytes\n", result);
    }

/* Inside remove: */
    free_irq(pci_irq_vector(pdev, 0), mdev);
    pci_free_irq_vectors(pdev);
```

IRQ allocation failure goes to `err_iomap`, handler registration failure to `err_irq_vec`, and DMA allocation failure to `err_irq`, releasing acquired resources. remove releases the handler and vector before the DMA buffer and BAR.

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

Full flow: Linux programs MSI-X table → driver kicks doorbell → VMM performs DMA → VMM injects MSI-X → handler runs → completion is confirmed → driver checks result.

## Key insight

The driver submits work through the doorbell and reads RESULT after the handler's `complete()` confirms completion. The completion object connects waiting and interrupt notification, handling notifications that arrive either before or after waiting begins. This environment also needs an MP table describing interrupt topology so Linux drivers can use MSI-X.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| pci_alloc_irq_vectors | Linux writes LAPIC address + vector to MSI-X table |
| request_irq | Register handler for MSI-X vector |
| completion | Synchronize interrupt handler → process context |
| wait_for_completion_timeout | Return immediately if complete; otherwise wait up to five seconds |
| MP table | Platform topology for IOAPIC discovery without BIOS/ACPI |
| e820 reserved | Protect MP table region from being overwritten by Linux |
| IRQ_HANDLED | Report that the interrupt was handled |
| End-to-end path | Driver → VMM DMA → KVM_SIGNAL_MSI → LAPIC → handler |

## What changed

Changes from Step 31:
- **New files**: `platform.c`, `platform.h` (MP table setup)
- **boot.c**: `#include "platform.h"`, `setup_mp_table(mem)` call; `MP_TABLE_ADDR` in boot.h
- **driver/microkvm_pci.c**: `<linux/interrupt.h>`, `<linux/completion.h>`, IRQ handler, `pci_alloc_irq_vectors`, `request_irq`, `wait_for_completion_timeout`, and remove cleanup
- **Makefile**: add `platform.c`

## Next step

Phase F is complete. The full driver lifecycle:

```
Step 29: Probe (ID match)
Step 30: BAR mapping (readl/writel)
Step 31: DMA (descriptor + doorbell)
Step 32: MSI-X (interrupt-driven completion)
```

microkvm implements **both sides** of a virtual PCI device—the VMM device model and guest kernel driver—demonstrating the complete data path from driver submission through DMA to interrupt-driven completion.
