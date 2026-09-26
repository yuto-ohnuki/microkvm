# Step 27: MSI-X Emulation—KVM_SIGNAL_MSI

## Goal

Notify the guest through MSI-X (Message Signaled Interrupts - Extended) after DMA completion. Instead of polling RESULT, the VMM passes the MSI-X table's address/data to `KVM_SIGNAL_MSI`, injecting an interrupt into the guest LAPIC.

## Background

### Why MSI-X is needed

In Step 26, the guest had to read RESULT to check DMA completion (polling). MSI-X lets the device actively announce completion.

### PCI INTx vs MSI-X

| | PCI INTx | MSI-X |
|---|---|---|
| Mechanism | Physical pins (shared) | Memory writes to LAPIC (dedicated) |
| Interrupt specification | INTA#–INTD# pins (shareable) | Up to 2048 table entries per device |
| Routing | IOAPIC → LAPIC | Direct to LAPIC |
| Sharing issues | Yes (check every ISR) | None (dedicated vector) |
| KVM API | `ioctl(KVM_IRQ_LINE)` | `ioctl(KVM_SIGNAL_MSI)` |

Phase C's virtio-mmio uses fixed GSI 5, not PCI INTx. The table above compares PCI interrupt mechanisms.

> **Note:** INTx signals through pins; MSI signals through memory writes and supports up to 32 consecutive vectors. MSI-X uses a table within a BAR to configure destinations and vectors independently for up to 2048 entries. microkvm implements one entry.

### How MSI-X works

An MSI-X interrupt is a *memory write*:
1. Guest OS writes `address` and `data` into the device's MSI-X table
2. Here, `address` = `0xFEE00000` (destination APIC ID 0)
3. Here, `data` = `0x21` (vector 33, Fixed delivery)
4. When the device wants to signal an interrupt, it writes data to address → LAPIC accepts it → CPU receives interrupt

From the VMM's perspective: record the address/data written by the guest to the MSI-X table, then inject through `KVM_SIGNAL_MSI` on DMA completion.

### LAPIC

The LAPIC (Local Advanced Programmable Interrupt Controller) is the interrupt controller for each logical CPU. An x86 MSI message encodes the destination in its address and the vector and other attributes in its data, bypassing PIC/IOAPIC. Here, `0xFEE00000` and `0x21` deliver vector 33 to APIC ID 0.

### KVM_SIGNAL_MSI

```c
struct kvm_msi msi = {
    .address_lo = 0xFEE00000,
    .address_hi = 0,
    .data = 0x21,
};
ioctl(vmfd, KVM_SIGNAL_MSI, &msi);
```

KVM identifies the target APIC from address, extracts the vector from data, and injects it into the guest LAPIC. No PIC/IOAPIC routing is needed.

### MSI-X table

The MSI-X table resides within BAR0 MMIO space (offset 0x800 in microkvm). Each entry is 16 bytes:

```
BAR0 layout:
  0x000–0x013: Device registers (STATUS, DOORBELL, RESULT, DESC_LO/HI)
  0x800–0x80F: MSI-X table (1 entry × 16 bytes)
  0xC00:      PBA (Pending Bit Array) location advertised by capability
```

Each MSI-X table entry:
```
Offset 0x00: addr_lo   (LAPIC address)
Offset 0x04: addr_hi   (usually 0)
Offset 0x08: data      (vector number)
Offset 0x0C: ctrl      (bit 0: masked)
```

A capability in PCI config space advertises the table's location. This step implements reads/writes for one MSI-X table entry.

### PCI Capability chain

PCI capabilities form a linked list in config space, starting at config[0x34] (capabilities pointer). Each capability contains `[cap_id][next_ptr][cap-specific data]`. The MSI-X capability (ID=0x11) tells the OS the table location (BAR and offset).

## Execution flow

```
Guest (driver/devmem)        KVM                VMM (microkvm)
─────────────────────        ───                ────────────────

Boot: Linux reads capability chain
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
                                                Execute DMA...
                                                check: ctrl not masked && addr_lo != 0
                                                KVM_SIGNAL_MSI(addr=0xFEE00000, data=0x21)
                             Inject vector 33
  IRQ handler runs (or "No irq handler"—no driver registered)
```

## Implementation

### Additions to pci.h

```c
#define PCI_MSIX_TABLE_OFFSET   0x800
#define PCI_MSIX_TABLE_ENTRIES  1

struct msix_entry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t data;
    uint32_t ctrl;      /* bit 0: masked */
};

/* Inside struct pci_device: */
    struct msix_entry msix_table[PCI_MSIX_TABLE_ENTRIES];
    int vmfd;   /* For KVM_SIGNAL_MSI */
```

### pci.c—MSI-X capability in pci_init()

```c
    dev->config[0x34] = 0x40;       /* capabilities pointer */
    dev->config[0x40] = 0x11;       /* cap ID = MSI-X */
    dev->config[0x41] = 0x00;       /* next = NULL */
    *(uint16_t *)&dev->config[0x42] = 0x0000;  /* 1 entry, not masked */
    *(uint32_t *)&dev->config[0x44] = PCI_MSIX_TABLE_OFFSET;  /* table BIR=0 */
    *(uint32_t *)&dev->config[0x48] = 0x00000C00;  /* PBA BIR=0 */
```

### pci.c—Injection after DMA completion

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

Injection requires the entry to be unmasked (ctrl bit 0 == 0) and addr_lo != 0 (table programmed). The output example uses `devmem` to meet these conditions.

### pci.c — msix_read/write

```c
/* Write MSI-X table entry — guest driver programs addr/data/ctrl for each vector */
void pci_msix_write(struct pci_device *dev, uint64_t offset, uint32_t value) {
    if (offset < PCI_MSIX_TABLE_OFFSET)
        return;
    uint64_t rel = offset - PCI_MSIX_TABLE_OFFSET;
    if ((rel & 3) != 0 || rel + sizeof(uint32_t) > sizeof(dev->msix_table))
        return;
    uint32_t *table = (uint32_t *)dev->msix_table;
    table[rel / sizeof(uint32_t)] = value;
    fprintf(stderr, "[pci-msix] table write offset=0x%lx val=0x%x\n",
            (unsigned long)offset, value);
}

/* Read MSI-X table entry — guest driver reads to check vector configuration */
uint32_t pci_msix_read(struct pci_device *dev, uint64_t offset) {
    if (offset < PCI_MSIX_TABLE_OFFSET)
        return 0xFFFFFFFF;
    uint64_t rel = offset - PCI_MSIX_TABLE_OFFSET;
    if ((rel & 3) != 0 || rel + sizeof(uint32_t) > sizeof(dev->msix_table))
        return 0xFFFFFFFF;
    uint32_t *table = (uint32_t *)dev->msix_table;
    return table[rel / sizeof(uint32_t)];
}
```

### microkvm.c—BAR0 MMIO routing branch

```c
/* Routing outline */
if (offset >= PCI_MSIX_TABLE_OFFSET &&
    offset < PCI_MSIX_TABLE_OFFSET + PCI_MSIX_TABLE_ENTRIES * 16) {
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

Procedure:
1. Set up DMA descriptor and data buffer (as in Step 26)
2. Program MSI-X table: addr_lo=0xFEE00000 (LAPIC), data=0x21 (vector 33), ctrl=0 (unmasked)
3. Kick doorbell → execute DMA → inject MSI-X interrupt
4. Linux receives vector 33 ("No irq handler"—no driver registered, but delivery succeeded)

If the MSI-X table is unprogrammed, the doorbell still works but injection is skipped (same behavior as Step 26).

## Key insight

MSI-X replaces shared physical pins and controller routing with direct memory writes to the CPU's LAPIC. The guest OS writes the destination CPU/vector into the MSI-X table, and the device replays that address/data pair through `KVM_SIGNAL_MSI`. This removes the need to identify the source of a shared IRQ and allows dedicated vectors per queue, as used by multi-queue NVMe/NIC devices. This is especially useful when multiple DMA queues each need completion interrupts.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| MSI-X | Interrupt through a memory write to LAPIC (0xFEE00000) |
| MSI-X table | BAR0+0x800, 16 bytes/entry: addr_lo, addr_hi, data, ctrl |
| PCI capability chain | config[0x34]→0x40, cap_id=0x11 advertises table location |
| KVM_SIGNAL_MSI | Inject into guest LAPIC without PIC/IOAPIC routing |
| Masked vs. unmasked | ctrl bit 0 lets guest temporarily suppress interrupts |
| Legacy IRQ vs. MSI-X | Shared pins vs. dedicated vectors; IOAPIC vs. direct LAPIC |

## What changed

Changes from Step 26:
- **pci.h**: `<linux/kvm.h>`, `PCI_MSIX_TABLE_OFFSET/ENTRIES`, `struct msix_entry`, `msix_table[]` + `vmfd` in pci_device, and `pci_msix_read/write` declarations
- **pci.c**: `<sys/ioctl.h>`, MSI-X capability in `pci_init()`, injection in doorbell handler, and `pci_msix_read/write` implementations
- **microkvm.c**: MSI-X table MMIO routing, `pci_dev.vmfd = vmfd`

## Next step

[Step 28: PCI Hotplug](step28_hotplug.md) adds/removes a device at runtime with `Ctrl-A h`. A second PCI device starts absent (vendor=0xFFFF) and is discovered by sysfs rescan after toggling.
