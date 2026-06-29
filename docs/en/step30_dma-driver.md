# Step 30: DMA-capable driver — dma_alloc_coherent and doorbell

## Goal

Extend the driver to perform DMA: allocate a coherent buffer, build a descriptor, write the descriptor address to the device, and kick the doorbell. The VMM reads the descriptor from guest RAM and transfers data — all in a single doorbell exit.

## Background

### Why DMA from the driver?

Step 29 used `readl` to read one register at a time (1 VM exit per access). For bulk data transfer, DMA avoids per-byte exits:

```
MMIO only:  N bytes = N/4 VM exits
DMA:        N bytes = 3 VM exits (DESC_LO + DESC_HI + DOORBELL)
```

### dma_alloc_coherent

```c
void *dma_alloc_coherent(struct device *dev, size_t size,
                         dma_addr_t *dma_handle, gfp_t flag);
```

Returns two addresses for the same physical memory:
- `void *` — kernel virtual address (CPU uses this)
- `dma_addr_t` — bus/physical address (device uses this)

"Coherent" means hardware ensures cache consistency — no manual flush needed. The driver writes data via the kernel VA; the VMM (device) reads it via the bus address (= GPA in microkvm).

> **Note:** In this educational implementation without an IOMMU, `dma_addr_t` is effectively the guest physical address. On real hardware with an IOMMU, the DMA address would be an I/O virtual address translated by the IOMMU.

### Buffer layout

```
dma_buf (4096 bytes):
┌──────────────────────────┐  offset 0x00
│ struct microkvm_dma_desc │
│   .addr = dma_addr + 16  │  → points to payload below
│   .len  = 18             │
│   .flags = 0 (TX)        │
├──────────────────────────┤  offset 0x10 (sizeof desc)
│ payload:                 │
│ "hello from driver\n"    │
└──────────────────────────┘
```

Descriptor and payload share one allocation — the descriptor's `.addr` points into the same buffer.

### pci_set_master

```c
pci_set_master(pdev);  /* Command register bit 2 = Bus Master Enable */
```

Required before DMA. Without this, the PCI bus blocks DMA transactions from the device — the device has no permission to initiate memory reads/writes.

### dma_alloc_coherent vs kmalloc

| API | Who can access | Use case |
|-----|---------------|----------|
| `kmalloc()` | CPU only | Normal kernel data structures |
| `dma_alloc_coherent()` | CPU + Device | Shared buffers for DMA (descriptors, data) |

A `kmalloc`'d buffer's physical address is not guaranteed to be usable by a device. `dma_alloc_coherent` provides a buffer that both CPU and device can safely access simultaneously.

### Phase E Step 25 correspondence

| Step 25 (VMM) | Step 30 (Driver) |
|---|---|
| Reads descriptor from guest RAM | Builds descriptor in coherent buffer |
| Uses desc.addr to find payload | Sets desc.addr = dma_addr + sizeof(desc) |
| Doorbell triggers DMA execution | writel(1, bar0 + REG_DOORBELL) |
| Stores result in last_dma_len | readl(bar0 + REG_RESULT) to check |

## Execution flow

```
Guest (driver)               KVM                VMM
──────────────               ───                ───
dma_alloc_coherent()
  → 4096 bytes at GPA X

Build descriptor at GPA X:
  addr = X + 16, len = 18, flags = 0
Write "hello from driver\n" at X + 16

writel(X, DESC_LO)
                             KVM_EXIT_MMIO
                                                desc_addr = X

writel(1, DOORBELL)
                             KVM_EXIT_MMIO
                                                read desc from RAM[X]
                                                read payload from RAM[X+16]
                                                write(stdout, "hello from driver\n")
                                                last_dma_len = 18

readl(RESULT)
                             KVM_EXIT_MMIO
                                                → return 18
  → result = 18
```

## Implementation

### Key additions to driver/microkvm_pci.c

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

In probe:
```c
    pci_set_master(pdev);

    mdev->dma_buf = dma_alloc_coherent(&pdev->dev, 4096,
        &mdev->dma_addr, GFP_KERNEL);

    /* Build descriptor */
    desc = mdev->dma_buf;
    payload = (char *)mdev->dma_buf + sizeof(*desc);
    memcpy(payload, "hello from driver\n", 18);
    desc->addr = mdev->dma_addr + sizeof(*desc);
    desc->len = 18;
    desc->flags = 0;

    /* Submit */
    writel(lower_32_bits(mdev->dma_addr), mdev->bar0 + REG_DESC_LO);
    writel(upper_32_bits(mdev->dma_addr), mdev->bar0 + REG_DESC_HI);
    writel(1, mdev->bar0 + REG_DOORBELL);

    result = readl(mdev->bar0 + REG_RESULT);
```

In remove:
```c
    dma_free_coherent(&pdev->dev, 4096, mdev->dma_buf, mdev->dma_addr);
```

## Output

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

The full data path: driver builds descriptor in coherent memory → tells device the address → kicks doorbell → VMM reads descriptor + payload → outputs "hello from driver" → driver confirms via RESULT register.

## Key insight

From the driver's perspective, DMA is: allocate a buffer, fill in a descriptor, tell the device where it is, kick. The device (VMM) does the rest. The driver never copies data byte-by-byte through MMIO registers. This is exactly how production NVMe drivers submit I/O commands and how network drivers transmit packets — the descriptor + doorbell pattern is universal.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| dma_alloc_coherent | Allocates buffer with both kernel VA and bus address |
| dma_addr_t | Address the device uses (= GPA in microkvm, no IOMMU) |
| pci_set_master | Enables Bus Master bit (permission for DMA) |
| Descriptor submission | Build desc → write addr to device → kick doorbell |
| lower/upper_32_bits | Split 64-bit DMA address into two 32-bit register writes |
| Coherent vs streaming | Coherent = no cache management needed |
| O(1) exits | Data size doesn't affect number of VM exits |

## What changed

From Step 29:
- **driver/microkvm_pci.c**: `#include <linux/dma-mapping.h>`, all register defines, `struct microkvm_dma_desc`, DMA fields in `microkvm_dev`, `pci_set_master`, `dma_alloc_coherent`, descriptor build + submit, `dma_free_coherent` in remove

No VMM changes.

## Next step

[Step 31: MSI-X interrupt handler](step31_msix-handler.md) replaces polling the RESULT register with interrupt-driven completion. The driver registers an MSI-X handler that wakes the waiting thread when DMA finishes.
