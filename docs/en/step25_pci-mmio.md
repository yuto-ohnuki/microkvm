# Step 25: PCI MMIO Device Registers through BAR0

## Goal

Implement actual device registers behind the address Linux assigned to BAR0 in Step 24. Guest accesses to BAR0 + offset reach the VMM through KVM_EXIT_MMIO, which responds with device state.

## Background

### Config space and device registers

Step 24 implemented PCI *config space*: a 256-byte identification/configuration region accessed through CF8/CFC I/O ports. Here we implement actual *operational registers*, accessed through MMIO at the address Linux wrote to BAR0.

Two access paths to a PCI device:
```
Config space (CF8/CFC): discovery, identification, resource assignment → KVM_EXIT_IO
Device registers (BAR): actual device operations                       → KVM_EXIT_MMIO
```

### BAR address → MMIO

In this run, Linux assigns `0x08000000` to BAR0 (4 KiB). This region is outside guest RAM memory slots, so:
- Access to GPA 0x08000000–0x08000FFF → EPT violation / EPT misconfiguration → KVM MMIO handling → KVM_EXIT_MMIO → VMM handling

This is the same mechanism as Step 5's MMIO hole and Phase C's virtio-mmio. What differs is *who chooses the address*:
- Step 5: VMM hardcodes 0xD0000
- Phase C: VMM specifies it on the kernel command line
- Phase E: **Linux determines size through BAR probing and chooses the address during resource assignment**

### Device register layout

The minimal PCI device exposes a few registers at fixed offsets from BAR0:

```
BAR0 + 0x00: STATUS    (R)   Device-ready flag
BAR0 + 0x04: DOORBELL  (W)   Notify device to start work
BAR0 + 0x08: RESULT    (R)   Result register (fixed value in this step)
```

Three registers represent status checking, work notification, and result retrieval. STATUS always returns ready (`0x01`), and RESULT always returns `0x42`.

### Doorbell

A doorbell register is a trigger: writing it tells the device to start work. The written value often has no significance; the write itself is the trigger. This is the same concept as virtio `QueueNotify` in Phase C.

Here, the doorbell only prints a log message; STATUS and RESULT do not change. Step 26 adds reading a DMA descriptor from guest RAM and transferring data in bulk.

### devmem

`devmem` is a busybox utility that directly reads/writes physical addresses through `/dev/mem`:
```
devmem 0x08000000        → 32-bit GPA read
devmem 0x08000004 32 0x1 → 32-bit write of 0x1
```

## Execution flow

```
Guest (userspace)            KVM                VMM (microkvm)
─────────────────            ───                ────────────────
devmem 0x08000000
  → Read GPA 0x08000000
                             VM exit due to EPT
                             KVM_EXIT_MMIO
                             addr=0x08000000
                             is_write=0
                                                bar0 = pci_bar0_addr()
                                                offset = addr - bar0 = 0x00
                                                val = pci_dev_mmio_read(0x00)
                                                → STATUS = 0x01
                             Return to guest
  → Receive 0x01

devmem 0x08000004 32 0x1
  → Write 0x1 to GPA 0x08000004
                             KVM_EXIT_MMIO
                             is_write=1
                                                offset = 0x04
                                                pci_dev_mmio_write(0x04, 1)
                                                → DOORBELL kicked
```

## Implementation

### pci.h—Register offset definitions

```c
/* Device registers within BAR0 MMIO region (offsets from BAR0 base) */
#define PCI_DEV_REG_STATUS      0x00
#define PCI_DEV_REG_DOORBELL    0x04
#define PCI_DEV_REG_RESULT      0x08
```

### pci.c—BAR0 address helper and MMIO handlers

```c
/* Align BAR0 to 4 KiB, removing the low 12 bits */
uint32_t pci_bar0_addr(struct pci_device *dev) {
    return *(uint32_t *)&dev->config[0x10] & 0xFFFFF000;
}

/* Device MMIO register write — BAR0 region */
void pci_dev_mmio_write(struct pci_device *dev, uint64_t offset, uint32_t value)
{
    fprintf(stderr, "[pci-dev] MMIO write offset=0x%02lx ← 0x%x\n",
        (unsigned long)offset, value);
    switch (offset) {
    case PCI_DEV_REG_DOORBELL:
        fprintf(stderr, "[pci-dev] doorbell kicked!\n");
        break;
    default:
        break;
    }
}

/* Device MMIO register read — BAR0 region */
uint32_t pci_dev_mmio_read(struct pci_device *dev, uint64_t offset)
{
    uint32_t val = 0;
    switch (offset) {
    case PCI_DEV_REG_STATUS:
        val = 0x01;
        break;  /* ready */
    case PCI_DEV_REG_RESULT:
        val = 0x42;
        break;  /* result */
    default:
        break;
    }
    fprintf(stderr, "[pci-dev] MMIO read  offset=0x%02lx → 0x%x\n",
        (unsigned long)offset, val);
    return val;
}
```

### microkvm.c — BAR0 MMIO routing

Add as the `else` branch of the virtio-mmio region check:

```c
} else {
    /* PCI BAR0 MMIO — device registers accessed via BAR0 address */
    uint32_t bar0 = pci_bar0_addr(&pci_dev);
    if (bar0 && addr >= bar0 && addr < bar0 + PCI_BAR0_SIZE) {
        uint64_t offset = addr - bar0;
        if (run->mmio.is_write) {
            uint32_t val = 0;
            memcpy(&val, run->mmio.data, run->mmio.len);
            pci_dev_mmio_write(&pci_dev, offset, val);
        } else {
            uint32_t val = pci_dev_mmio_read(&pci_dev, offset);
            memcpy(run->mmio.data, &val, run->mmio.len);
        }
    }
}
```

Read the BAR0 address from config space at runtime rather than hardcoding it. The VMM does not know the address until Linux assigns it. This is PCI's dynamic nature.

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

Meaning of each access:
- `0x08000000` = BAR0 + 0x00 = STATUS read → 0x01 (device ready)
- `0x08000004` = BAR0 + 0x04 = DOORBELL write → trigger work
- `0x08000008` = BAR0 + 0x08 = RESULT read → 0x42 (placeholder, replaced with DMA results in Step 26)

## Key insight

PCI device registers reside in MMIO address space behind a BAR. The guest neither knows nor needs to care that they are emulated; it simply reads/writes physical addresses. The VMM intercepts accesses through KVM_EXIT_MMIO to provide the hardware behavior. This is Step 5's trap-and-emulate pattern, but the guest OS itself chose the address through BAR assignment.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| BAR → MMIO mapping | Linux's BAR assignment becomes the device-register base address |
| Device register pattern | Separate status, notification, and result registers |
| MMIO exit routing | VMM compares GPA with BAR0 to route to the correct device |
| Doorbell | Write-only trigger register (same concept as virtio QueueNotify) |
| devmem | Direct physical-address access for testing without a driver |

## What changed

Changes from Step 24:
- **pci.h**: device register offset definitions, `pci_bar0_addr()`, and `pci_dev_mmio_read/write` declarations
- **pci.c**: implement `pci_bar0_addr()`, `pci_dev_mmio_read()`, and `pci_dev_mmio_write()`
- **microkvm.c**: add BAR0 MMIO routing to the KVM_EXIT_MMIO handler

## Next step

[Step 26: DMA Simulation](step26_dma.md) extends the doorbell handler to read descriptors from guest RAM and access memory directly, transferring data without per-byte MMIO exits.
