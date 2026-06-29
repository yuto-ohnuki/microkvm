# Step 24: PCI MMIO device registers via BAR0

## Goal

Implement device registers behind the BAR0 address that Linux assigned in Step 23. When the guest accesses BAR0 + offset, a KVM_EXIT_MMIO is delivered to the VMM, which responds with device state.

## Background

### Config space vs Device registers

Step 23 implemented PCI *config space* — the 256-byte "business card" accessed via I/O ports CF8/CFC. Now we implement the device's actual *operational registers*, accessed via MMIO at the address Linux wrote into BAR0.

Two access paths to a PCI device:
```
Config space (CF8/CFC):  discovery, identification, resource allocation → KVM_EXIT_IO
Device registers (BAR):  actual device operation                       → KVM_EXIT_MMIO
```

### BAR address → MMIO

In Step 23, Linux probed BAR0 (4KB) and assigned address 0x08000000. From that point:
- Guest access to GPA 0x08000000–0x08000FFF → no EPT mapping → EPT violation → KVM_EXIT_MMIO → VMM handles

This is the same mechanism as Step 5 (MMIO hole) and Phase C (virtio-mmio). The difference is *who decides the address*:
- Step 5: VMM hardcoded 0xD0000
- Phase C: VMM specified via kernel cmdline
- Phase E: **Linux chose dynamically** via BAR probing

### Device register layout

A minimal PCI device exposes a few registers at fixed offsets from BAR0:

```
BAR0 + 0x00: STATUS    (R)   device ready flag
BAR0 + 0x04: DOORBELL  (W)   kick device to start processing
BAR0 + 0x08: RESULT    (R)   result of last operation
```

This STATUS/DOORBELL/RESULT pattern is common across real devices (NVMe, virtio-pci, etc.). A real device would expose multiple status bits; here a single "ready" bit keeps the example minimal.

### Doorbell

A doorbell register is a "trigger" — writing to it signals the device to begin work. The written value itself is often irrelevant; what matters is that the write occurred. This is analogous to Phase C's `QueueNotify` for virtio.

In this step, the doorbell only prints a log message. In Step 25, it will read a DMA descriptor from guest memory and perform a bulk data transfer.

### devmem

`devmem` is a busybox utility for direct physical address read/write via `/dev/mem`:
```
devmem 0x08000000        → 32-bit read at that GPA
devmem 0x08000004 32 0x1 → 32-bit write of 0x1
```

Requires `CONFIG_DEVMEM=y` and `CONFIG_STRICT_DEVMEM=n` (strict mode blocks access to non-device RAM regions).

## Execution flow

```
Guest (userspace)            KVM                VMM (microkvm)
─────────────────            ───                ────────────────
devmem 0x08000000
  → read GPA 0x08000000
                             EPT violation
                             KVM_EXIT_MMIO
                             addr=0x08000000
                             is_write=0
                                                bar0 = pci_bar0_addr()
                                                offset = addr - bar0 = 0x00
                                                val = pci_dev_mmio_read(0x00)
                                                → STATUS = 0x01
                             return to guest
  → receives 0x01

devmem 0x08000004 32 0x1
  → write 0x1 to GPA 0x08000004
                             KVM_EXIT_MMIO
                             is_write=1
                                                offset = 0x04
                                                pci_dev_mmio_write(0x04, 1)
                                                → DOORBELL kicked
```

## Implementation

### Prerequisites

Additional kernel config (from Step 23):
```
CONFIG_DEVMEM=y
CONFIG_STRICT_DEVMEM=n
```

### pci.h — register offset definitions

```c
/* Device registers within BAR0 MMIO region (offsets from BAR0 base) */
#define PCI_DEV_REG_STATUS      0x00
#define PCI_DEV_REG_DOORBELL    0x04
#define PCI_DEV_REG_RESULT      0x08
```

### pci.c — BAR0 address helper and MMIO handlers

```c
/* Extract BAR0 base address (mask out type bits in lower 4 bits) */
uint32_t pci_bar0_addr(struct pci_device *dev) {
    return *(uint32_t *)&dev->config[0x10] & 0xFFFFF000;
}

uint32_t pci_dev_mmio_read(struct pci_device *dev, uint64_t offset) {
    switch (offset) {
    case PCI_DEV_REG_STATUS:  return 0x01;  /* ready */
    case PCI_DEV_REG_RESULT:  return 0x42;  /* placeholder */
    }
}

void pci_dev_mmio_write(struct pci_device *dev, uint64_t offset, uint32_t value) {
    switch (offset) {
    case PCI_DEV_REG_DOORBELL:
        /* Step 25 will add DMA processing here */
        break;
    }
}
```

### microkvm.c — BAR0 MMIO routing

Added as `else` branch after the virtio-mmio region check:

```c
} else {
    /* PCI BAR0 MMIO — device registers accessed via BAR0 address */
    uint32_t bar0 = pci_bar0_addr(&pci_dev);
    if (bar0 && addr >= bar0 && addr < bar0 + PCI_BAR0_SIZE) {
        uint64_t offset = addr - bar0;
        if (run->mmio.is_write)
            pci_dev_mmio_write(&pci_dev, offset, val);
        else
            val = pci_dev_mmio_read(&pci_dev, offset);
    }
}
```

The BAR0 address is read from config space at runtime — not hardcoded. This is why PCI is dynamic: the VMM doesn't know the address until Linux assigns it.

## Output

```
/ # devmem 0x08000000
[pci-dev] MMIO read  offset=0x00 → 0x1
0x00000001

/ # devmem 0x08000004 32 0x1
[pci-dev] MMIO write offset=0x04 ← 0x1
[pci-dev] doorbell kicked!

/ # devmem 0x08000008
[pci-dev] MMIO read  offset=0x08 → 0x42
0x00000042
```

Each access:
- `0x08000000` = BAR0 + 0x00 = STATUS read → 0x01 (device ready)
- `0x08000004` = BAR0 + 0x04 = DOORBELL write → triggers processing
- `0x08000008` = BAR0 + 0x08 = RESULT read → 0x42 (placeholder, becomes DMA result in Step 25)

## Key insight

PCI device registers live in the MMIO address space behind a BAR. The guest doesn't know (or care) that these are emulated — it simply reads/writes physical addresses. The VMM intercepts via KVM_EXIT_MMIO and provides the illusion of hardware. This is the same trap-and-emulate pattern as Step 5, but now the address was chosen by the guest OS itself through BAR allocation.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| BAR → MMIO mapping | Linux's BAR assignment becomes the device register base address |
| Device register pattern | STATUS / DOORBELL / RESULT — common across real devices |
| MMIO exit routing | VMM checks GPA against BAR0 value to route to correct device |
| Doorbell | Write-only trigger register (like virtio QueueNotify) |
| devmem | Direct physical address access for driver-less testing |
| CONFIG_STRICT_DEVMEM | Must be disabled for RAM-region devmem access (Step 25 DMA test) |

## What changed

From Step 23:
- **pci.h**: device register offset defines, `pci_bar0_addr()`, `pci_dev_mmio_read/write` declarations
- **pci.c**: `pci_bar0_addr()`, `pci_dev_mmio_read()`, `pci_dev_mmio_write()` implementations
- **microkvm.c**: BAR0 MMIO routing in KVM_EXIT_MMIO handler

## Next step

[Step 25: DMA simulation](step25_dma.md) extends the doorbell handler to read a descriptor from guest RAM and perform direct memory access — transferring data without per-byte MMIO exits.
