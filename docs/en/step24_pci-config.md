# Step 24: PCI Config Space—Device Discovery through CF8/CFC

> **Phase E: PCI Device Model**
>
> Phases A–D built the hypervisor, I/O paths, and memory management.
> Phase E implements PCI, x86's standard device-discovery mechanism.
> Unlike Phase C's virtio-mmio (address specified on the kernel command line), PCI lets Linux scan the bus and discover devices itself.

## Goal

Implement PCI Configuration Mechanism #1 (CF8/CFC I/O ports) so Linux discovers a custom PCI device during standard boot-time bus enumeration.

## Background

### What is PCI?

PCI (Peripheral Component Interconnect) is a device interconnect standard on x86 systems. It provides the OS with:
1. **Discovery**: determine which devices exist (enumeration)
2. **Identification**: identify each device (vendor ID, device ID, class)
3. **Resource assignment**: memory regions (BARs) and interrupts

Many devices use PCI / PCI Express, including NICs, NVMe drives, GPUs, and USB controllers.

From the guest's perspective, PCI has two main parts:
- **Configuration space** (this step): a 256-byte region per device containing identification and settings, accessed through CF8/CFC
- **Device registers behind BARs** (Step 25): operational registers accessed through MMIO at addresses assigned during enumeration

### Phase C vs. Phase E

| | Phase C (virtio-mmio) | Phase E (PCI) |
|---|---|---|
| Device discovery | Explicit address on kernel command line | Automatic discovery through Linux bus scanning |
| Register access | Fixed MMIO address (0xD0000000) | Address dynamically assigned through a BAR |
| Data transfer | Virtqueue (shared-memory ring) | DMA descriptor (Step 26) |
| Interrupts | Fixed IRQ line (GSI 5) | MSI-X (dedicated vector, Step 27) |

### Configuration Mechanism #1

This step uses x86 PCI Configuration Mechanism #1, accessing config space through two I/O ports:

```
Port 0xCF8 (CONFIG_ADDRESS):
  bit 31     : Enable (1 enables access)
  bits 30:24 : Reserved (0)
  bits 23:16 : Bus
  bits 15:11 : Device
  bits 10:8  : Function
  bits 7:2   : Register (DWORD granularity)
  bits 1:0   : 0

Port 0xCFC (CONFIG_DATA):
  Read/write the config register selected through 0xCF8
```

Linux scans the bus and checks offset 0x00 (vendor ID) for device presence. A result of 0xFFFF means no device; otherwise, it reads the full config header to identify and configure the device.

### BAR (Base Address Register)

BARs describe the required MMIO region size and hold the address assigned by the OS:

```
OS writes 0xFFFFFFFF to BAR → device returns size mask
  Example: 0xFFFFF000 → ~0xFFFFF000 + 1 = 0x1000 = 4KB

OS writes final address → device uses that address
  Example: 0x08000000 → device registers start at GPA 0x08000000
```

Writing all ones and reading back a mask lets the OS discover resource requirements without hardcoding them.

Example:
```
Write: 0xFFFFFFFF
Read:  0xFFFFF000 (mask)
Size:  ~0xFFFFF000 + 1 = 0x00001000 = 4096 bytes
```

### Why pci=conf1 is needed

microkvm provides neither PCI BIOS services nor ACPI PCI configuration information. In this setup, `pci=conf1` tells Linux to use direct CF8/CFC access.

## Execution flow

```
Guest (Linux boot)           KVM                VMM (microkvm)
────────────────────         ───                ────────────────
                                                pci_init(): vendor=0x1234,
                                                  device=0x0001, class=0xFF

PCI: Using conf type 1
for each bus/dev/func:
  outl(0xCF8, addr)
                             KVM_EXIT_IO
                             port=0xCF8, OUT
                                                Store in config_address

  inl(0xCFC)
                             KVM_EXIT_IO
                             port=0xCFC, IN
                                                Decode BDF from config_address
                                                00:00.0 → pci_config_read()
                                                Otherwise → return 0xFFFFFFFF

  vendor != 0xFFFF → device found!
  BAR0 probe:
    Write 0xFFFFFFFF to BAR0
                                                pci_config_write(): store mask
    Read BAR0
                                                → Return 0xFFFFF000 (4KB)
    Write assigned address
                                                → Store 0x08000000
```

## Implementation

### Prerequisites

Configure PCI for Step 24 and `/dev/mem` for Step 25 onward together here:

```ini
CONFIG_PCI=y
CONFIG_PCI_DIRECT=y
CONFIG_DEVMEM=y
# CONFIG_STRICT_DEVMEM is not set
```

`CONFIG_PCI` / `CONFIG_PCI_DIRECT` enable PCI scanning and CF8/CFC access. `CONFIG_DEVMEM` enables `/dev/mem`; disabling `CONFIG_STRICT_DEVMEM` prepares for later guest physical-memory operations.

Add these to the existing `.config` in the host's Linux source directory. Adjust paths for your environment:

```bash
$ cd ~/linux-src

$ scripts/config --enable CONFIG_PCI
$ scripts/config --enable CONFIG_PCI_DIRECT
$ scripts/config --enable CONFIG_DEVMEM
$ scripts/config --disable CONFIG_STRICT_DEVMEM

$ make olddefconfig
$ grep -E '^(CONFIG_(PCI|PCI_DIRECT|DEVMEM)=|# CONFIG_STRICT_DEVMEM is not set)' .config

$ make -j"$(nproc)" bzImage
$ cp arch/x86/boot/bzImage ~/microkvm/bzImage
```

After `olddefconfig`, verify the four settings above before building. `pci=conf1` is already set in this step's `microkvm.c` CMDLINE.

### pci.h—Device structure and constants

```c
#define PCI_CONFIG_ADDR_PORT  0x0CF8
#define PCI_CONFIG_DATA_PORT  0x0CFC
#define PCI_VENDOR_ID         0x1234
#define PCI_DEVICE_ID         0x0001
#define PCI_BAR0_SIZE         4096

struct pci_device {
    uint8_t config[256];       /* Type 0 config header */
    uint32_t bar0_mask;        /* Size mask for BAR probing */
    uint32_t config_address;   /* Last value written to 0xCF8 */
};
```

### pci.c—Config space initialization and access

```c
/* Excerpt: identification and BAR0 setup */
void pci_init(struct pci_device *dev) {
    memset(dev->config, 0, sizeof(dev->config));
    *(uint16_t *)&dev->config[0x00] = PCI_VENDOR_ID;   /* 0x1234 */
    *(uint16_t *)&dev->config[0x02] = PCI_DEVICE_ID;   /* 0x0001 */
    dev->config[0x0B] = 0xFF;   /* class: unassigned */
    dev->config[0x0E] = 0x00;   /* Header type: 0 = endpoint (not a PCI bridge) */
    dev->bar0_mask = ~(PCI_BAR0_SIZE - 1);  /* 0xFFFFF000 */
}
```

Class 0xFF (unassigned) leaves this educational device outside existing standard classes. Driver matching depends on vendor/device IDs, class, and other criteria—not class alone.

BAR0 write handling excerpt: update four bytes of config space, storing the mask for probing or an address aligned to 4 KiB otherwise:

```c
if (offset == 0x10) {
    if (value == 0xFFFFFFFF) {
        *(uint32_t *)&dev->config[0x10] = dev->bar0_mask;
    } else {
        *(uint32_t *)&dev->config[0x10] = value & dev->bar0_mask;
    }
    return;
}
```

`pci_config_read()` / `pci_config_write()` handle 1-, 2-, and 4-byte accesses and check config-space bounds.

### microkvm.c—I/O exit routing

```c
} else if (port == PCI_CONFIG_ADDR_PORT) {
    /* Store/return the 32-bit address register */
} else if (port >= PCI_CONFIG_DATA_PORT && port <= PCI_CONFIG_DATA_PORT + 3) {
    /* Decode BDF from config_address and route to pci_config_read/write */
    if (bus == 0 && device == 0 && func == 0)
        → Our device
    else
        → Return all ones (0xFFFFFFFF)
}
```

If the Enable bit is 0 or the BDF is not `00:00.0`, reads return all ones. The offset is `(config_address & 0xFC) + (port - 0xCFC)`, accounting for byte positions across CFC–CFF.

## Output

Check vendor/device IDs in the guest:

```
/ # cat /sys/bus/pci/devices/0000:00:00.0/vendor
0x1234
/ # cat /sys/bus/pci/devices/0000:00:00.0/device
0x0001
```

Linux discovered the device through standard PCI enumeration, without a device-specific kernel command-line declaration as used by virtio-mmio.

## Key insight

PCI config space tells the OS the device's identity and resource requirements. BAR probing writes all ones, reads the mask, and calculates the region size. This step implements discovery and BAR0 configuration; Step 25 implements MMIO registers at the assigned address.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| PCI Configuration Mechanism #1 | CF8 (address) + CFC (data) I/O port pair |
| BDF addressing | Identify each slot by bus:device.function |
| Type 0 config header | Header at the start of config space containing vendor/device/class/BAR |
| BAR probing | Write 0xFFFFFFFF → read mask → calculate size |
| pci=conf1 | Force CF8/CFC when BIOS/ACPI information is absent |
| 0xFFFF = no device | Standard PCI indication of an empty slot |

## What changed

Changes from Step 23:
- **New files**: `pci.h` (constants, structures), `pci.c` (init, config_read, config_write)
- **microkvm.c**: `#include "pci.h"`, global `pci_dev`, CF8/CFC handling in I/O exits, `pci_init()` call, CMDLINE += `pci=conf1`
- **Makefile**: add `pci.c`

## Next step

[Step 25: PCI MMIO Device Registers](step25_pci-mmio.md) implements actual device registers behind the address Linux assigns to BAR0. Guest accesses BAR0 + offset → KVM_EXIT_MMIO → VMM responds with device state.
