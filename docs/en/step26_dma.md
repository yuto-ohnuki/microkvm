# Step 26: DMA Simulation—Descriptors and Doorbells

## Goal

Implement DMA (Direct Memory Access), where the PCI device directly reads/writes guest RAM. Instead of transferring each byte through an MMIO exit, the guest places a descriptor in RAM and rings the doorbell; the VMM performs the transfer in bulk.

## Background

### The problem with MMIO-only data transfer

Transferring 4 MiB through 4-byte MMIO registers, as in Step 25, would require 1,048,576 KVM_EXIT_MMIO returns. Describing a buffer with a descriptor avoids MMIO accesses proportional to data size.

```
MMIO transfer: 4 MiB / 4 bytes = 1,048,576 MMIO exits
DMA transfer: two here, DESC_LO + DOORBELL (plus one separate RESULT read)
```

### How DMA works

DMA reverses control: instead of the CPU manipulating registers byte by byte, it tells the device where data is, and the device accesses memory directly:

```
1. Driver writes a descriptor in guest RAM (addr + len + direction)
2. Driver writes the descriptor's GPA to device registers (DESC_LO/HI)
3. Driver kicks DOORBELL → one MMIO exit
4. Device (VMM) reads descriptor from guest RAM
5. Device (VMM) directly reads/writes the data buffer
```

In this simulation, the VMM directly accesses the mmap region backing guest RAM. TX sends the buffer to host standard output with `write()`; RX writes into the buffer with `memcpy()`. Descriptor and buffer bounds checks first validate the address, then compare size against `ram_size - addr` to avoid addition overflow.

### Descriptor structure

```c
struct dma_desc {
    uint64_t addr;      /* Data buffer GPA */
    uint32_t len;       /* Transfer size (bytes) */
    uint32_t flags;     /* 0=device reads guest (TX), nonzero=device writes guest (RX) */
};
```

This is a common pattern in high-performance device I/O:

| microkvm | virtio | NVMe | AHCI |
|----------|--------|------|------|
| dma_desc | vring_desc | SQ entry | PRDT |
| doorbell write | QueueNotify | doorbell | CI register |

All use the same idea: shared-memory descriptors + doorbell kicks.

### Extended device registers

```
BAR0 + 0x00: STATUS    R    (0x01 = ready)
BAR0 + 0x04: DOORBELL  W    (write → execute DMA)
BAR0 + 0x08: RESULT    R    (len of the last processed descriptor)
BAR0 + 0x0C: DESC_LO   W    (descriptor GPA, low 32 bits)
BAR0 + 0x10: DESC_HI   W    (descriptor GPA, high 32 bits)
```

DESC_LO/HI are separate because the MMIO registers are 32-bit while GPAs may be 64-bit.

## Execution flow

```
Guest                        KVM                VMM (microkvm)
─────                        ───                ────────────────
Place descriptor in RAM
  (GPA 0x07F00000)
Place data "hello" in RAM
  (GPA 0x07F00100)

devmem BAR0+0x0C ← 0x07F00000
                             KVM_EXIT_MMIO
                                                desc_addr = 0x07F00000

devmem BAR0+0x04 ← 1
                             KVM_EXIT_MMIO
                                                DOORBELL:
                                                1. Read desc from RAM[0x07F00000]
                                                2. desc.addr=0x07F00100, len=5, flags=0
                                                3. write(STDOUT_FILENO, RAM+0x07F00100, 5)
                                                → Output "hello"
                                                last_dma_len = 5

devmem BAR0+0x08
                             KVM_EXIT_MMIO
                                                → Return 5 (RESULT)
```

## Implementation

### pci.h—New registers and descriptor structure

```c
#define PCI_DEV_REG_DESC_LO  0x0C
#define PCI_DEV_REG_DESC_HI  0x10

struct dma_desc {
    uint64_t addr;      /* Data buffer GPA */
    uint32_t len;       /* Transfer size */
    uint32_t flags;     /* 0=device reads from guest (TX) */
};
```

Add DMA state to `struct pci_device`:
```c
    uint64_t desc_addr;     /* Descriptor GPA */
    uint32_t last_dma_len;  /* Last DMA result */
    uint8_t *ram;           /* Guest RAM pointer */
    size_t ram_size;
```

### pci.c—Implement DMA in the doorbell handler

```c
case PCI_DEV_REG_DOORBELL: {
    /* Read descriptor from guest RAM */
    if (dev->desc_addr >= dev->ram_size ||
        sizeof(struct dma_desc) > dev->ram_size - dev->desc_addr)
        break;
    struct dma_desc desc;
    memcpy(&desc, dev->ram + dev->desc_addr, sizeof(desc));
    if (desc.addr >= dev->ram_size || desc.len > dev->ram_size - desc.addr)
        break;

    if (desc.flags == 0) {
        /* DMA: Device reads from guest (TX path) */
        fprintf(stderr, "[pci-dma] DMA read: %u bytes from GPA 0x%lx\n",
            desc.len, (unsigned long)desc.addr);
        write(STDOUT_FILENO, dev->ram + desc.addr, desc.len);
    } else {
        /* DMA: Device writes to guest (RX path) */
        const char *msg = "DMA-WRITE-OK\n";
        size_t msg_len = strlen(msg);
        if (msg_len > desc.len)
            msg_len = desc.len;
        memcpy(dev->ram + desc.addr, msg, msg_len);
        fprintf(stderr, "[pci-dma] DMA write: %zu bytes to GPA 0x%lx\n",
            msg_len, (unsigned long)desc.addr);
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

RX writes the smaller of `desc.len` and the 12 bytes of `DMA-WRITE-OK\n`. RESULT returns `desc.len` for both TX and RX, so for RX it may differ from the actual bytes written.

### microkvm.c—Give the PCI device access to guest RAM

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

Procedure:
1. Place a DMA descriptor in guest RAM (0x07F00000): addr=0x07F00100, len=5, flags=0
2. Write "hello" into the data buffer (0x07F00100)
3. Set DESC_LO = 0x07F00000 to tell the device where the descriptor is; this example uses DESC_HI's initial value of 0
4. Kick DOORBELL → VMM reads descriptor, executes DMA, outputs "hello"
5. Read RESULT → 5 (descriptor len; this TX transfers five bytes)

## Key insight

With a fixed descriptor count and fixed register operations, increasing data size does not increase MMIO exits needed to request DMA. MMIO carries descriptor addresses and doorbell notifications; the VMM processes the data itself by directly accessing guest RAM.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| DMA | Device directly accesses guest RAM (no per-byte MMIO exits) |
| Descriptor | Shared-memory addr + len + flags structure placed by the driver |
| Doorbell | One MMIO write triggers the entire transfer |
| O(1) exits | Transfer size does not affect exit count |
| DESC_LO/HI split | Convey a 64-bit address through 32-bit registers |
| Equivalence to virtio | dma_desc ≈ vring_desc, doorbell ≈ QueueNotify |
| Bounds checks | Compare descriptor/buffer sizes with remaining RAM after their addresses |

## What changed

Changes from Step 25:
- **pci.h**: `PCI_DEV_REG_DESC_LO/HI`, `struct dma_desc`, and DMA state in `struct pci_device`
- **pci.c**: descriptor reading and DMA in doorbell handler, DESC_LO/HI handlers, RESULT returns `last_dma_len`, and `#include <unistd.h>` for write()
- **microkvm.c**: set `pci_dev.ram` / `pci_dev.ram_size`

## Next step

[Step 27: MSI-X Emulation](step27_msix.md) adds interrupt notification on DMA completion. Instead of polling RESULT, the device notifies the guest through MSI-X. The RX path (flags=1) also becomes more useful when the device can announce new data.
