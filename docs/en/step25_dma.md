# Step 25: DMA simulation via descriptor and doorbell

## Goal

Implement DMA (Direct Memory Access) so the PCI device can read/write guest RAM directly. Instead of transferring data one word at a time via MMIO exits, the guest places a descriptor in RAM describing the transfer, then kicks a doorbell — the VMM performs the entire transfer in one shot.

## Background

### The problem with MMIO-only data transfer

In Step 24, each `devmem` access causes one KVM_EXIT_MMIO. To transfer 4MB of data via MMIO registers, the guest would need 1 million exits (4 bytes per exit). This is impractical for real I/O.

```
Without DMA: 4MB transfer = 1M exits (one per 4-byte read/write)
With DMA:    4MB transfer = 2 exits (DESC_LO + DOORBELL)
```

### How DMA works

DMA inverts the control: instead of the CPU pushing data byte-by-byte through registers, the CPU tells the device *where* the data is, and the device accesses memory directly:

```
1. Driver writes descriptor to guest RAM (addr + len + direction)
2. Driver writes descriptor GPA to device register (DESC_LO/HI)
3. Driver kicks DOORBELL → single MMIO exit
4. Device (VMM) reads descriptor from guest RAM
5. Device (VMM) reads/writes data buffer directly (memcpy)
```

The key insight: data transfer happens entirely in VMM userspace memory (the guest RAM mmap). No VM exits per byte. A real device must also validate that descriptors point within guest RAM — our implementation checks `desc.addr + desc.len` against `ram_size` before accessing.

### Descriptor structure

```c
struct dma_desc {
    uint64_t addr;      /* GPA of data buffer */
    uint32_t len;       /* transfer length in bytes */
    uint32_t flags;     /* 0=device reads from guest (TX), 1=device writes to guest (RX) */
};
```

This is the universal pattern for high-performance device I/O:

| microkvm | virtio | NVMe | AHCI |
|----------|--------|------|------|
| dma_desc | vring_desc | SQ entry | PRDT |
| doorbell write | QueueNotify | doorbell | CI register |

All use: descriptor in shared memory + doorbell kick.

### Device registers (extended)

```
BAR0 + 0x00: STATUS    R    (0x01 = ready)
BAR0 + 0x04: DOORBELL  W    (write → DMA execution)
BAR0 + 0x08: RESULT    R    (bytes transferred in last DMA)
BAR0 + 0x0C: DESC_LO   W    (descriptor GPA, lower 32 bits)
BAR0 + 0x10: DESC_HI   W    (descriptor GPA, upper 32 bits)
```

DESC_LO/HI are split because MMIO registers are 32-bit, but GPAs can be 64-bit.

## Execution flow

```
Guest                        KVM                VMM (microkvm)
─────                        ───                ────────────────
write descriptor to RAM
  (GPA 0x07F00000)
write data "hello" to RAM
  (GPA 0x07F00100)

devmem BAR0+0x0C ← 0x07F00000
                             KVM_EXIT_MMIO
                                                desc_addr = 0x07F00000

devmem BAR0+0x04 ← 1
                             KVM_EXIT_MMIO
                                                DOORBELL:
                                                1. read desc from RAM[0x07F00000]
                                                2. desc.addr=0x07F00100, len=5, flags=0
                                                3. memcpy(stdout, RAM+0x07F00100, 5)
                                                → "hello" printed
                                                last_dma_len = 5

devmem BAR0+0x08
                             KVM_EXIT_MMIO
                                                → return 5 (RESULT)
```

## Implementation

### pci.h — new registers and descriptor struct

```c
#define PCI_DEV_REG_DESC_LO  0x0C
#define PCI_DEV_REG_DESC_HI  0x10

struct dma_desc {
    uint64_t addr;      /* GPA of data buffer */
    uint32_t len;       /* transfer length */
    uint32_t flags;     /* 0=device reads from guest (TX) */
};
```

`struct pci_device` gains DMA state:
```c
    uint64_t desc_addr;     /* GPA of descriptor */
    uint32_t last_dma_len;  /* result of last DMA */
    uint8_t *ram;           /* pointer to guest RAM */
    size_t ram_size;
```

### pci.c — doorbell handler with DMA

```c
case PCI_DEV_REG_DOORBELL: {
    /* Read descriptor from guest RAM */
    struct dma_desc desc;
    memcpy(&desc, dev->ram + dev->desc_addr, sizeof(desc));

    if (desc.flags == 0) {
        /* TX: device reads from guest → print to stdout */
        write(STDOUT_FILENO, dev->ram + desc.addr, desc.len);
    } else {
        /* RX: device writes to guest */
        memcpy(dev->ram + desc.addr, "DMA-WRITE-OK\n", msg_len);
    }
    dev->last_dma_len = desc.len;
    break;
}
case PCI_DEV_REG_DESC_LO:
    dev->desc_addr = (dev->desc_addr & 0xFFFFFFFF00000000ULL) | value;
    break;
case PCI_DEV_REG_DESC_HI:
    dev->desc_addr = (dev->desc_addr & 0xFFFFFFFF) | ((uint64_t)value << 32);
    break;
```

### microkvm.c — give PCI device access to guest RAM

```c
pci_dev.ram = (uint8_t *)mem;
pci_dev.ram_size = GUEST_MEM_SIZE;
```

## Output

```
/ # devmem 0x07F00000 32 0x07F00100
/ # devmem 0x07F00004 32 0x00000000
/ # devmem 0x07F00008 32 0x00000005
/ # devmem 0x07F0000C 32 0x00000000
/ # devmem 0x07F00100 32 0x6C6C6568
/ # devmem 0x07F00104 8 0x6F
/ # devmem 0x0800000C 32 0x07F00000
[pci-dev] MMIO write offset=0x0c ← 0x7f00000
/ # devmem 0x08000004 32 0x1
[pci-dev] MMIO write offset=0x04 ← 0x1
[pci-dma] DMA read: 5 bytes from GPA 0x7f00100
hello
/ # devmem 0x08000008
[pci-dev] MMIO read  offset=0x08 → 0x5
0x00000005
```

Steps explained:
1. Write DMA descriptor to guest RAM at 0x07F00000 (addr=0x07F00100, len=5, flags=0)
2. Write "hello" to data buffer at 0x07F00100
3. Set DESC_LO = 0x07F00000 (tell device where descriptor is)
4. Kick DOORBELL → VMM reads descriptor, performs DMA, prints "hello"
5. Read RESULT → 5 (bytes transferred)

## Key insight

DMA reduces VM exits from O(N/4) to O(1) regardless of transfer size. More precisely: for a fixed descriptor count, exits remain constant regardless of payload size. The descriptor + doorbell pattern is the universal abstraction for high-performance device I/O — NVMe, virtio, GPU command buffers, and network DMA engines all use this exact model. The data itself never crosses the VM exit boundary; only the 4-byte doorbell write does.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| DMA | Device accesses guest RAM directly (no per-byte MMIO exits) |
| Descriptor | addr + len + flags structure placed in shared memory by driver |
| Doorbell | Single MMIO write triggers entire transfer |
| O(1) exits | Transfer size doesn't affect exit count |
| DESC_LO/HI split | 32-bit registers encoding 64-bit address |
| Virtio equivalence | dma_desc ≈ vring_desc, doorbell ≈ QueueNotify |
| Bounds checking | Validate desc.addr + desc.len against ram_size |

## What changed

From Step 24:
- **pci.h**: `PCI_DEV_REG_DESC_LO/HI`, `struct dma_desc`, DMA state in `struct pci_device`
- **pci.c**: doorbell handler reads descriptor + performs DMA, DESC_LO/HI handlers, RESULT returns `last_dma_len`, `#include <unistd.h>` for write()
- **microkvm.c**: `pci_dev.ram` / `pci_dev.ram_size` assignment

## Next step

[Step 26: MSI-X emulation](step26_msix.md) adds interrupt notification after DMA completion. Instead of polling RESULT, the device signals the guest via MSI-X — a PCI write-to-LAPIC mechanism that bypasses the legacy interrupt controller entirely. The RX path (flags=1) will also become more meaningful once the device can notify the guest that new data has arrived.
