# Step 23: PCI config space — device discovery via CF8/CFC

> **Phase E: PCI Device Model**
>
> Phases A–D built the hypervisor, I/O path, and memory management.
> Phase E implements the x86 standard device discovery mechanism: PCI.
> Unlike Phase C's virtio-mmio (address specified on kernel cmdline), PCI lets
> Linux discover devices on its own through bus enumeration.

## Goal

Implement PCI Configuration Mechanism #1 (CF8/CFC I/O ports) so that Linux discovers a custom PCI device during boot via standard bus enumeration.

## Background

### What is PCI?

PCI (Peripheral Component Interconnect) is the standard device interconnect for x86 systems. It provides a uniform mechanism for the OS to:
1. **Discover** what devices are present (enumeration)
2. **Identify** each device (vendor ID, device ID, class)
3. **Allocate resources** (memory regions via BARs, interrupts)

Every NIC, NVMe drive, GPU, and USB controller on a modern x86 system is a PCI device.

PCI has two major pieces from the guest's perspective:
- **Configuration space** (this step): the 256-byte "directory entry" for each device, accessed via CF8/CFC
- **Device registers behind BARs** (Step 24): the actual operational registers, accessed via MMIO at an address assigned during enumeration

### Phase C vs Phase E

| | Phase C (virtio-mmio) | Phase E (PCI) |
|---|---|---|
| How guest finds device | Kernel cmdline specifies exact address | Linux scans bus and discovers device |
| Register access | Fixed MMIO address (0xD0000000) | BAR-assigned MMIO address (dynamic) |
| Data transfer | Virtqueue (shared memory rings) | DMA descriptors (Step 25) |
| Interrupt | Fixed IRQ line (GSI 5) | MSI-X (dedicated vector, Step 26) |

### Configuration Mechanism #1

x86 PCI config space access uses two I/O ports. This mechanism predates PCI Express and remains supported on all x86 systems for compatibility:

```
Port 0xCF8 (CONFIG_ADDRESS):
  ┌───┬────────┬────────┬──────────┬───────────────┬──┐
  │31 │ 23:16  │ 15:11  │  10:8    │    7:2        │1:0│
  │ 1 │  bus   │ device │ function │ register(dword)│ 0 │
  └───┴────────┴────────┴──────────┴───────────────┴──┘

Port 0xCFC (CONFIG_DATA):
  Read/write the config register selected by 0xCF8
```

Linux probes every bus/device/function by reading vendor ID at offset 0x00. If the result is 0xFFFF, no device exists at that slot. Otherwise, Linux reads the full config header to identify and configure the device.

### BAR (Base Address Register)

A BAR tells the OS how much MMIO space a device needs and where the OS assigned it:

```
OS writes 0xFFFFFFFF to BAR → device returns size mask
  e.g., 0xFFFFF000 → ~0xFFFFF000 + 1 = 0x1000 = 4KB

OS writes final address → device uses that base address
  e.g., 0x08000000 → device registers start at GPA 0x08000000
```

This "write all-ones, read back mask" protocol lets the OS discover each device's resource requirements without hardcoded addresses.

Example:
```
Write: 0xFFFFFFFF
Read:  0xFFFFF000 (mask)
Size:  ~0xFFFFF000 + 1 = 0x00001000 = 4096 bytes
```

### Why pci=conf1?

microkvm provides no BIOS or ACPI tables. Linux normally auto-detects the PCI access method via:
1. ACPI MCFG table → PCIe ECAM (memory-mapped)
2. BIOS PCI service → `int 0x1a`
3. Direct CF8/CFC probe

Without BIOS/ACPI, auto-detection fails. `pci=conf1` forces Linux to use Configuration Mechanism #1 directly.

## Execution flow

```
Guest (Linux boot)           KVM                VMM (microkvm)
────────────────────         ───                ────────────────
                                                pci_init(): set vendor=0x1234,
                                                  device=0x0001, class=0xFF

PCI: Using conf type 1
for each bus/dev/func:
  outl(0xCF8, addr)
                             KVM_EXIT_IO
                             port=0xCF8, OUT
                                                store config_address

  inl(0xCFC)
                             KVM_EXIT_IO
                             port=0xCFC, IN
                                                decode BDF from config_address
                                                if 00:00.0 → pci_config_read()
                                                else → return 0xFFFFFFFF

  vendor != 0xFFFF → found!
  probe BAR0:
    write 0xFFFFFFFF to BAR0
                                                pci_config_write(): store mask
    read BAR0
                                                → return 0xFFFFF000 (4KB)
    write assigned address
                                                → store 0x08000000
```

## Implementation

### Prerequisites

Kernel config (bzImage rebuild required):
```
CONFIG_PCI=y
CONFIG_PCI_DIRECT=y
```

Kernel cmdline addition: `pci=conf1`

### pci.h — device structure and constants

```c
#define PCI_CONFIG_ADDR_PORT  0x0CF8
#define PCI_CONFIG_DATA_PORT  0x0CFC
#define PCI_VENDOR_ID         0x1234
#define PCI_DEVICE_ID         0x0001
#define PCI_BAR0_SIZE         4096

struct pci_device {
    uint8_t config[256];       /* Type 0 config header */
    uint32_t bar0_mask;        /* size mask for BAR probing */
    uint32_t config_address;   /* last value written to 0xCF8 */
};
```

### pci.c — config space initialization and access

```c
void pci_init(struct pci_device *dev) {
    *(uint16_t *)&dev->config[0x00] = PCI_VENDOR_ID;   /* 0x1234 */
    *(uint16_t *)&dev->config[0x02] = PCI_DEVICE_ID;   /* 0x0001 */
    dev->config[0x0B] = 0xFF;   /* class: unassigned — no standard driver binds */
    dev->config[0x0E] = 0x00;   /* header type: 0 = endpoint (not a PCI bridge) */
    dev->bar0_mask = ~(PCI_BAR0_SIZE - 1);  /* 0xFFFFF000 */
}
```

We intentionally use class 0xFF (unassigned) so Linux enumerates the device without binding a standard driver (e.g., network class 0x02 would trigger the networking subsystem).

void pci_config_write(dev, offset, value, len) {
    if (offset == 0x10) {
        /* BAR0: probe returns mask, assignment stores aligned address */
        if (value == 0xFFFFFFFF)
            config[0x10] = bar0_mask;
        else
            config[0x10] = value & bar0_mask;
    }
}
```

### microkvm.c — IO exit handler routing

```c
} else if (port == PCI_CONFIG_ADDR_PORT) {
    /* Store/return the 32-bit address register */
} else if (port >= PCI_CONFIG_DATA_PORT && port <= PCI_CONFIG_DATA_PORT + 3) {
    /* Decode BDF from config_address, route to pci_config_read/write */
    if (bus == 0 && device == 0 && func == 0)
        → our device
    else
        → return 0xFF (no device)
}
```

## Output

```
/ # cat /sys/bus/pci/devices/0000:00:00.0/vendor
0x1234
/ # cat /sys/bus/pci/devices/0000:00:00.0/device
0x0001
```

Linux discovered the device through standard PCI enumeration — no kernel cmdline device specification needed (unlike virtio-mmio).

## Key insight

PCI config space is the x86 standard "device directory." The OS reads vendor ID at each bus/device/function slot; 0xFFFF means empty, anything else means a device exists. BAR probing uses a write-all-ones/read-back protocol to discover resource sizes without hardcoded addresses. This is how every x86 OS has discovered devices since 1992 — from a BIOS POST to a cloud VM boot.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| PCI Configuration Mechanism #1 | CF8 (address) + CFC (data) I/O port pair |
| BDF addressing | bus:device.function identifies each slot |
| Type 0 config header | 256-byte "business card" with vendor/device/class/BARs |
| BAR probing | Write 0xFFFFFFFF, read back mask, compute size |
| pci=conf1 | Force direct CF8/CFC when no BIOS/ACPI available |
| 0xFFFF = no device | PCI standard signal for empty slot |

## What changed

From Step 22:
- **New files**: `pci.h` (constants, struct), `pci.c` (init, config_read, config_write)
- **microkvm.c**: `#include "pci.h"`, `pci_dev` global, CF8/CFC handling in IO exit, `pci_init()` call, CMDLINE += `pci=conf1`
- **Makefile**: add `pci.c`

## Next step

[Step 24: PCI MMIO device registers](step24_pci-mmio.md) implements actual device registers behind the BAR0 address that Linux assigned. Guest accesses BAR0 + offset → KVM_EXIT_MMIO → VMM responds with device state.
