# Step 26: MSI-X emulation with KVM_SIGNAL_MSI

## Goal

Add interrupt notification after DMA completion using MSI-X (Message Signaled Interrupts - Extended). Instead of polling the RESULT register, the device signals the guest CPU directly via a memory write to the LAPIC.

## Background

### Why MSI-X?

In Step 25, after DMA completes, the guest must poll the RESULT register to know when data is ready. This wastes CPU cycles. MSI-X lets the device proactively notify the guest — "DMA is done" — via an interrupt.

### Legacy IRQ (Phase C) vs MSI-X (Phase E)

| | Phase C (IRQ line) | Phase E (MSI-X) |
|---|---|---|
| Mechanism | Physical pin (shared) | Memory write to LAPIC (dedicated) |
| Vectors | 4 shared (INTA#–INTD#) | Up to 2048 per device |
| Routing | IOAPIC → LAPIC | Direct to LAPIC |
| Sharing problem | Yes (must check all ISRs) | No (dedicated vector) |
| KVM API | `ioctl(KVM_IRQ_LINE)` | `ioctl(KVM_SIGNAL_MSI)` |

> **Note:** There is also MSI (without the X), which supports up to 32 contiguous vectors. MSI-X extends this to 2048 arbitrary vectors with a programmable table in BAR space. microkvm implements MSI-X directly as it is the modern standard for high-performance devices.

### How MSI-X works

MSI-X interrupts are *memory writes*:
1. Guest OS programs the device's MSI-X table with `address` and `data`
2. `address` = 0xFEE00000 (LAPIC base) + destination APIC ID
3. `data` = interrupt vector number + delivery mode
4. When the device wants to interrupt, it "writes" data to address → LAPIC receives it → CPU interrupted

In a VMM: the guest writes addr/data to the MSI-X table (MMIO), the VMM records them, and on DMA completion calls `KVM_SIGNAL_MSI` to inject the interrupt.

### LAPIC

The LAPIC (Local Advanced Programmable Interrupt Controller) is the per-CPU interrupt controller in x86. Its memory-mapped base address is 0xFEE00000. MSI-X writes directly to this address to deliver interrupts without any intermediate controller (PIC/IOAPIC).

### KVM_SIGNAL_MSI

```c
struct kvm_msi {
    __u32 address_lo;   /* LAPIC address */
    __u32 address_hi;   /* usually 0 */
    __u32 data;         /* vector + delivery mode */
};
ioctl(vmfd, KVM_SIGNAL_MSI, &msi);
```

KVM decodes the address to find the target APIC, extracts the vector from data, and injects the interrupt into the guest LAPIC — no PIC/IOAPIC routing needed.

### MSI-X table

The MSI-X table lives in the BAR0 MMIO region (offset 0x800 in microkvm). Each entry is 16 bytes:

```
BAR0 layout:
  0x000–0x7FF: Device registers (STATUS, DOORBELL, RESULT, DESC_LO/HI)
  0x800–0x80F: MSI-X table (1 entry × 16 bytes)
  0xC00–...:   MSI-X PBA (Pending Bit Array)
```

Each MSI-X table entry:
```
Offset 0x00: addr_lo   (LAPIC address)
Offset 0x04: addr_hi   (usually 0)
Offset 0x08: data      (vector number)
Offset 0x0C: ctrl      (bit 0: masked)
```

The table location is advertised via a PCI capability in config space.

### PCI Capability chain

PCI capabilities are a linked list in config space starting at offset 0x34 (capabilities pointer). Each capability has: `[cap_id][next_ptr][cap-specific data]`. MSI-X capability (ID=0x11) tells the OS where the MSI-X table is (which BAR, at what offset).

## Execution flow

```
Guest (driver/devmem)        KVM                VMM (microkvm)
─────────────────────        ───                ────────────────

Boot: Linux reads cap chain
  config[0x34] → 0x40
  config[0x40] → cap_id=0x11 (MSI-X)
  config[0x44] → table at BAR0+0x800

Program MSI-X table:
  write BAR0+0x800 ← 0xFEE00000
                             KVM_EXIT_MMIO
                                                msix_table[0].addr_lo = 0xFEE00000
  write BAR0+0x808 ← 0x21
                                                msix_table[0].data = 0x21 (vector 33)
  write BAR0+0x80C ← 0x0
                                                msix_table[0].ctrl = 0 (unmasked)

DMA + interrupt:
  write BAR0+0x04 ← 1 (DOORBELL)
                             KVM_EXIT_MMIO
                                                DMA executes...
                                                check: ctrl not masked && addr_lo != 0
                                                KVM_SIGNAL_MSI(addr=0xFEE00000, data=0x21)
                             inject vector 33
  IRQ handler fires (or "No irq handler" if no driver)
```

## Implementation

### pci.h additions

```c
#define PCI_MSIX_TABLE_OFFSET   0x800
#define PCI_MSIX_TABLE_ENTRIES  1

struct msix_entry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t data;
    uint32_t ctrl;      /* bit 0: masked */
};

/* In struct pci_device: */
    struct msix_entry msix_table[PCI_MSIX_TABLE_ENTRIES];
    int vmfd;   /* for KVM_SIGNAL_MSI */
```

### pci.c — MSI-X capability in pci_init()

```c
    dev->config[0x34] = 0x40;       /* capabilities pointer */
    dev->config[0x40] = 0x11;       /* cap ID = MSI-X */
    dev->config[0x41] = 0x00;       /* next = NULL */
    *(uint16_t *)&dev->config[0x42] = 0x0000;  /* 1 entry, not masked */
    *(uint32_t *)&dev->config[0x44] = PCI_MSIX_TABLE_OFFSET;  /* table BIR=0 */
    *(uint32_t *)&dev->config[0x48] = 0x00000C00;  /* PBA BIR=0 */
```

### pci.c — injection after DMA

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

Conditions: not masked (ctrl bit 0 == 0) AND addr_lo != 0 (table has been programmed).

### pci.c — msix_read/write

```c
uint32_t pci_msix_read(struct pci_device *dev, uint64_t offset) {
    uint32_t *table = (uint32_t *)dev->msix_table;
    return table[(offset - PCI_MSIX_TABLE_OFFSET) / 4];
}

void pci_msix_write(struct pci_device *dev, uint64_t offset, uint32_t value) {
    uint32_t *table = (uint32_t *)dev->msix_table;
    table[(offset - PCI_MSIX_TABLE_OFFSET) / 4] = value;
}
```

### microkvm.c — BAR0 MMIO routing split

```c
if (offset >= PCI_MSIX_TABLE_OFFSET && ...) {
    /* MSI-X table access */
    pci_msix_read/write(...)
} else {
    /* Device registers */
    pci_dev_mmio_read/write(...)
}
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

Steps:
1. Set up DMA descriptor + data buffer (same as Step 25)
2. Program MSI-X table: addr_lo=0xFEE00000 (LAPIC), data=0x21 (vector 33), ctrl=0 (unmasked)
3. Kick doorbell → DMA executes → MSI-X interrupt injected
4. Linux receives vector 33 ("No irq handler" — no driver registered, but interrupt delivery confirmed)

Without MSI-X table programmed, doorbell still works but skips injection (Step 25 behavior).

## Key insight

MSI-X converts interrupts from "shared physical pins routed through controllers" to "targeted memory writes directly to the CPU's LAPIC." The guest OS programs the destination (which CPU, which vector) by writing to the MSI-X table — the device just replays that address/data pair via `KVM_SIGNAL_MSI`. This eliminates interrupt sharing, reduces routing latency, and enables per-queue interrupt vectors (critical for multi-queue NVMe/network devices). This becomes especially important once a device exposes multiple DMA queues, each with its own completion interrupt.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| MSI-X | Interrupt via memory write to LAPIC (0xFEE00000) |
| MSI-X table | BAR0+0x800, 16 bytes/entry: addr_lo, addr_hi, data, ctrl |
| PCI capability chain | config[0x34]→0x40, cap_id=0x11 tells OS where table lives |
| KVM_SIGNAL_MSI | Inject interrupt to guest LAPIC without PIC/IOAPIC routing |
| Masked vs unmasked | ctrl bit 0: guest can temporarily suppress interrupts |
| Legacy IRQ vs MSI-X | Shared pins vs dedicated vectors, IOAPIC vs direct LAPIC |

## What changed

From Step 25:
- **pci.h**: `<linux/kvm.h>`, `PCI_MSIX_TABLE_OFFSET/ENTRIES`, `struct msix_entry`, `msix_table[]` + `vmfd` in pci_device, `pci_msix_read/write` declarations
- **pci.c**: `<sys/ioctl.h>`, MSI-X capability in `pci_init()`, injection in doorbell handler, `pci_msix_read/write` implementations
- **microkvm.c**: MSI-X table MMIO routing, `pci_dev.vmfd = vmfd`

## Next step

[Step 27: PCI hotplug](step27_hotplug.md) adds runtime device add/remove via `Ctrl-A h`. A second PCI device starts absent (vendor=0xFFFF) and becomes visible when toggled — Linux discovers it through sysfs rescan.
