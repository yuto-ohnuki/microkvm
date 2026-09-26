# Step 31: DMA-Capable Driver—dma_alloc_coherent and Doorbells

## Goal

Extend the driver to perform DMA: allocate a coherent buffer, build a descriptor, provide its address to the device, and kick the doorbell. During doorbell MMIO exit handling, the VMM reads the descriptor and payload from guest RAM and records the result.

## Background

### Why DMA from the driver?

Step 30 read one register at a time with `readl` (one access = one VM exit). For large transfers, DMA avoids per-byte exits:

```
Transfer payload through MMIO: MMIO exit for each 32-bit access
Submit this DMA transfer:      3 MMIO exits (DESC_LO + DESC_HI + DOORBELL)
```

This probe also reads STATUS and RESULT once each, totaling five MMIO exits including submission. The payload itself is read from guest RAM, so there are no per-byte MMIO exits.

### dma_alloc_coherent

```c
void *dma_alloc_coherent(struct device *dev, size_t size,
                         dma_addr_t *dma_handle, gfp_t flag);
```

It returns two addresses for the same physical memory:
- `void *`—kernel virtual address (used by CPU)
- `dma_addr_t`—DMA address (used by device)

"Coherent" means the CPU and device can share the buffer without explicit cache synchronization. The driver writes through the kernel VA; the VMM (device) reads through the bus address (GPA in microkvm).

> **Note:** This educational implementation has no IOMMU, so `dma_addr_t` is effectively a guest physical address. With an IOMMU on real hardware, the DMA address is an I/O virtual address translated by the IOMMU.

### Buffer layout

```
dma_buf (4096 bytes):
┌──────────────────────────┐  offset 0x00
│ struct microkvm_dma_desc │
│   .addr = dma_addr + 16  │  → Points to the payload below
│   .len  = 18             │
│   .flags = 0 (TX)        │
├──────────────────────────┤  offset 0x10 (sizeof desc)
│ payload:                 │
│ "hello from driver\n"    │
└──────────────────────────┘
```

Descriptor and payload share one allocation; the descriptor's `.addr` points inside the same buffer.

### pci_set_master

```c
pci_set_master(pdev);  /* Command register bit 2 = Bus Master Enable */
```

Set Bus Master Enable so the PCI device can initiate DMA. Command register `0x6` in the output means Memory Space Enable (bit 1) and Bus Master Enable (bit 2) are set.

### dma_alloc_coherent vs kmalloc

| API | Who can access it? | Purpose |
|-----|---------------------|------|
| `kmalloc()` | CPU (separate DMA mapping required for DMA use) | Ordinary kernel data structures |
| `dma_alloc_coherent()` | CPU + device | Shared DMA buffers (descriptors, data) |

A buffer's physical address from `kmalloc` is not necessarily usable by a device. `dma_alloc_coherent` also returns a DMA address. Cache coherence and access ordering are separate: write the descriptor before notifying through `writel()` to the doorbell.

### Correspondence with Phase E, Step 26

| Step 26 (VMM) | Step 31 (Driver) |
|---|---|
| Read descriptor from guest RAM | Build descriptor in coherent buffer |
| Locate payload through desc.addr | desc.addr = dma_addr + sizeof(desc) |
| Execute DMA on doorbell | writel(1, bar0 + REG_DOORBELL) |
| Record result in last_dma_len | Check with readl(bar0 + REG_RESULT) |

## Execution flow

```
Guest (driver)               KVM                VMM
──────────────               ───                ───
dma_alloc_coherent()
  → 4096 bytes at GPA X

Build descriptor at GPA X:
  addr = X + 16, len = 18, flags = 0
Write "hello from driver\n" at X + 16

writel(lower_32_bits(X), DESC_LO)
                             KVM_EXIT_MMIO
                                                Set low 32 bits of desc_addr
writel(upper_32_bits(X), DESC_HI)
                             KVM_EXIT_MMIO
                                                Set high 32 bits of desc_addr

readl(STATUS)
                             KVM_EXIT_MMIO
                                                → Return 0x1

writel(1, DOORBELL)
                             KVM_EXIT_MMIO
                                                Read desc from RAM[X]
                                                Read payload from RAM[X+16]
                                                write(stdout, "hello from driver\n")
                                                last_dma_len = 18

readl(RESULT)
                             KVM_EXIT_MMIO
                                                → Return 18
  → result = 18
```

## Implementation

### Main additions to driver/microkvm_pci.c

```c
#include <linux/dma-mapping.h>

#define REG_DOORBELL  0x04
#define REG_RESULT    0x08
#define REG_DESC_LO   0x0C
#define REG_DESC_HI   0x10

struct microkvm_dma_desc {
    u64 addr;
    u32 len;
    u32 flags;  /* 0=device reads from guest (TX) */
};

struct microkvm_dev {
    ...
    void *dma_buf;
    dma_addr_t dma_addr;
};
```

Inside probe:
```c
    pci_set_master(pdev);

    /* Allocate coherent DMA buffer: descriptor at start, payload after it */
    mdev->dma_buf = dma_alloc_coherent(&pdev->dev, 4096,
        &mdev->dma_addr, GFP_KERNEL);
    if (!mdev->dma_buf) {
        ret = -ENOMEM;
        goto err_iomap;
    }

    pci_set_drvdata(pdev, mdev);

    /* Build DMA descriptor at start of buffer */
    desc = mdev->dma_buf;
    payload = (char *)mdev->dma_buf + sizeof(*desc);
    memcpy(payload, "hello from driver\n", 18);

    desc->addr = mdev->dma_addr + sizeof(*desc);    /* points to payload */
    desc->len = 18;
    desc->flags = 0;    /* device reads from guest (TX) */

    /* Tell device where descriptor is */
    writel(lower_32_bits(mdev->dma_addr), mdev->bar0 + REG_DESC_LO);
    writel(upper_32_bits(mdev->dma_addr), mdev->bar0 + REG_DESC_HI);

    /* Confirm device is ready */
    status = readl(mdev->bar0 + REG_STATUS);
    dev_info(&pdev->dev, "STATUS = 0x%x\n", status);

    /* Kick doorbell — triggers DMA on VMM side */
    writel(1, mdev->bar0 + REG_DOORBELL);

    /* Read result — number of bytes transferred */
    result = readl(mdev->bar0 + REG_RESULT);
    dev_info(&pdev->dev, "DMA complete, transferred %u bytes\n", result);
```

If DMA buffer allocation fails, `err_iomap` unmaps the BAR and continues through Step 30's cleanup.

```c
err_iomap:
    pci_iounmap(pdev, mdev->bar0);
err_regions:
    pci_release_regions(pdev);
err_disable:
    pci_disable_device(pdev);
    return ret;
```

Inside remove (before unmapping BAR):
```c
    dma_free_coherent(&pdev->dev, 4096, mdev->dma_buf, mdev->dma_addr);
```

## Output

Use [Step 29's procedure](step29_pci-driver.md#prerequisites) to rebuild the driver, replace the `.ko` in initramfs, and boot.

```
/ # insmod /lib/modules/microkvm_pci.ko
[pci] config write offset=0x04 ← 0x6 (len=2)
[pci-dev] MMIO write offset=0x0c ← 0x4137000
[pci-dev] MMIO write offset=0x10 ← 0x0
[pci-dev] MMIO read  offset=0x00 → 0x1
microkvm_pci 0000:00:00.0: STATUS = 0x1
[pci-dev] MMIO write offset=0x04 ← 0x1
[pci-dma] DMA read: 18 bytes from GPA 0x4137010
hello from driver
[pci-dev] MMIO read  offset=0x08 → 0x12
microkvm_pci 0000:00:00.0: DMA complete, transferred 18 bytes
/ # rmmod microkvm_pci
microkvm_pci 0000:00:00.0: remove called
```

This implementation finishes the transfer and updates RESULT inside doorbell handling, so the driver verifies it with one immediate RESULT read.

Full data path: driver builds descriptor in coherent memory → provides address to device → kicks doorbell → VMM reads descriptor + payload → outputs "hello from driver" → driver checks RESULT.

## Key insight

From the driver, DMA means allocate a buffer → fill a descriptor → provide its address → kick. The device (VMM) does the rest. The driver does not copy each byte through MMIO registers. This descriptor-and-doorbell pattern also appears in NVMe and NIC drivers.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| dma_alloc_coherent | Allocate buffer returning both kernel VA and bus address |
| dma_addr_t | Device address (GPA in microkvm, without an IOMMU) |
| pci_set_master | Enable Bus Master bit (DMA permission) |
| Descriptor submission | Build desc → provide address → kick doorbell |
| lower/upper_32_bits | Split a 64-bit DMA address into two 32-bit register writes |
| Coherent vs. streaming | Coherent requires no cache management |
| MMIO exit count | Three submission accesses and two checks, independent of payload size here |

## What changed

Changes from Step 30:
- **driver/microkvm_pci.c**: `#include <linux/dma-mapping.h>`, all register definitions, `struct microkvm_dma_desc`, DMA fields in `microkvm_dev`, `pci_set_master`, `dma_alloc_coherent`, descriptor build/submission, and `dma_free_coherent` in remove

No VMM changes.

## Next step

[Step 32: MSI-X Interrupt Handler](step32_msix-handler.md) adds completion waiting through MSI-X. The driver registers a handler that wakes the waiting thread when DMA completes.
