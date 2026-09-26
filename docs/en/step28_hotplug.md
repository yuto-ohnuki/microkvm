# Step 28: PCI Hotplug—Add and Remove Devices at Runtime

## Goal

Simulate PCI hotplug by toggling device presence at runtime. `Ctrl-A h` makes a second PCI device appear/disappear; the guest discovers/removes it through standard sysfs interfaces (`rescan`/`remove`).

## Background

### What is PCI hotplug?

PCI hotplug adds/removes devices while a system is running. Real-world uses include:
- Hot-swapping server NVMe drives
- Connecting/disconnecting Thunderbolt devices
- QEMU/libvirt `virsh attach-device` / `virsh detach-device`

### Representing absence in PCI

When Linux scans a bus slot, vendor ID 0xFFFF means no device. microkvm represents this by returning all ones, sized to the access width, for config reads to an absent device.

```
present=1: config read → Vendor=0x1234 → device present
present=0: config read → Vendor=0xFFFF → empty slot
```

### Native PCIe hotplug vs sysfs rescan

| | Native PCIe hotplug | sysfs rescan (microkvm) |
|---|---|---|
| Mechanism | Root port + slot control + interrupt notification | Guest explicitly rescans the bus |
| Complexity | High (state machine, attention button, power control) | Low (flag toggle + rescan) |
| Guest behavior | Automatic detection | Manual `echo 1 > /sys/bus/pci/rescan` |

microkvm toggles config-space visibility through `present`, using manual `rescan` / `remove` to observe Linux device discovery, BAR assignment, and deregistration.

## Execution flow

```
VMM (Ctrl-A h)               Guest
──────────────               ─────
                             / # ls /sys/bus/pci/devices/
                             0000:00:00.0

pci_hotplug_dev.present = 1
"[monitor] ADDED"
                             / # echo 1 > /sys/bus/pci/rescan
                             Linux probes the bus:
                               device=1: vendor=0x1234 → found!
                               BAR0 probing → assign 0x08002000
                             / # ls /sys/bus/pci/devices/
                             0000:00:00.0  0000:00:01.0

pci_hotplug_dev.present = 0
"[monitor] REMOVED"
                             / # echo 1 > .../0000:00:01.0/remove
                             Linux removes device from sysfs
                             / # ls /sys/bus/pci/devices/
                             0000:00:00.0
```

## Implementation

### pci.h—present flag

```c
struct pci_device {
    ...
    /* hotplug */
    int present;    /* 0=absent (returns 0xFFFF), 1=present */
};
```

### pci.c—Presence guards

`pci_init()` initializes the normal device with `present = 1`; `pci_init_hotplug()` initializes the second device with `present = 0`. The following additions guard config reads/writes.

```c
void pci_config_write(struct pci_device *dev, ...) {
    if (!dev->present)
        return;   /* Ignore writes to an absent device */
    ...
}

uint32_t pci_config_read(struct pci_device *dev, ...) {
    /* Absent device: return all-ones (0xFFFF vendor = no device) */
    if (!dev->present) {
        if (len == 4) return 0xFFFFFFFF;
        else if (len == 2) return 0xFFFF;
        else return 0xFF;
    }
    ...
}
```

### pci.c — pci_init_hotplug()

```c
/* Initialize hotplug PCI device (device=1). Starts absent (present=0) */
void pci_init_hotplug(struct pci_device *dev) {
    memset(dev, 0, sizeof(*dev));
    *(uint16_t *)&dev->config[0x00] = 0x1234;
    *(uint16_t *)&dev->config[0x02] = 0x0002;   /* Different device ID */
    dev->config[0x08] = 0x01;   /* revision ID */
    dev->config[0x0B] = 0xFF;   /* class: unassigned */
    dev->config[0x0E] = 0x00;   /* header type: endpoint */
    dev->bar0_mask = ~(PCI_BAR0_SIZE - 1);
    dev->present = 0;   /* Absent at startup */
}
```

### microkvm.c—Generalize config routing

Select the target for config reads/writes by BDF (bus/device/function). The routing outline follows.

```c
struct pci_device *target = NULL;
if (bus == 0 && func == 0) {
    if (device == 0) target = &pci_dev;          /* 0000:00:00.0 */
    else if (device == 1) target = &pci_hotplug_dev;  /* 0000:00:01.0 */
}
if (target) {
    pci_config_read/write(target, ...)
}
```

### microkvm.c—Ctrl-A h monitor command

```c
if (c == 'h') {
    pci_hotplug_dev.present = !pci_hotplug_dev.present;
    fprintf(stderr, "\n[monitor] PCI device 0000:00:01.0 %s\n",
        pci_hotplug_dev.present ? "ADDED (run: echo 1 > /sys/bus/pci/rescan)" :
        "REMOVED (run: echo 1 > /sys/bus/pci/devices/0000:00:01.0/remove)");
    continue;
}
```

### microkvm.c—BAR0 MMIO routing for the hotplug device

If the address does not match the first device's BAR0 range, check the second device's assigned BAR0 and dispatch to the existing `pci_dev_mmio_read/write()` handlers.

```c
} else {
    uint32_t bar0_hp = pci_bar0_addr(&pci_hotplug_dev);
    if (bar0_hp && addr >= bar0_hp && addr < bar0_hp + PCI_BAR0_SIZE) {
        /* Route to pci_hotplug_dev */
    }
}
```

## Output

```
/ # ls /sys/bus/pci/devices/
0000:00:00.0

< Ctrl-A h >
[monitor] PCI device 0000:00:01.0 ADDED (run: echo 1 > /sys/bus/pci/rescan)

/ # echo 1 > /sys/bus/pci/rescan
pci 0000:00:01.0: [1234:0002] type 00 class 0xff0000 conventional PCI endpoint
pci 0000:00:01.0: BAR 0 [mem 0x08002000-0x08002fff]: assigned

/ # ls /sys/bus/pci/devices/
0000:00:00.0  0000:00:01.0

/ # cat /sys/bus/pci/devices/0000:00:01.0/device
0x0002

< Ctrl-A h >
[monitor] PCI device 0000:00:01.0 REMOVED (run: echo 1 > /sys/bus/pci/devices/0000:00:01.0/remove)

/ # echo 1 > /sys/bus/pci/devices/0000:00:01.0/remove
/ # ls /sys/bus/pci/devices/
0000:00:00.0
```

The hotplug device has device ID 0x0002 (the main device uses 0x0001). Linux's resource allocator assigns the BAR address (0x08002000) during enumeration, so rescans do not guarantee the same address.

> **Note:** `Ctrl-A h` toggles the VMM's `present`. `rescan` asks Linux to scan the bus again, while `remove` deregisters the device in Linux. `remove` does not change `present`; if it remains 1, another `rescan` discovers the device again.

## Key insight

The key is connecting config-space responses from the VMM to Linux's device registration. With `present = 1`, `rescan` repeats vendor-ID checking, BAR probing, and address assignment as at boot. To remove it, `present = 0` makes config space absent, and `remove` deregisters it in Linux.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| PCI hotplug | Toggle config-space visibility to add/remove devices at runtime |
| Vendor 0xFFFF | Standard PCI indication of device absence |
| sysfs rescan | `echo 1 > /sys/bus/pci/rescan` makes Linux rescan the bus |
| sysfs remove | `echo 1 > .../remove` detaches the device from the kernel |
| Generalized config routing | A `target` pointer selects the device by BDF |
| BAR re-probing on hotplug | Same probe sequence as at boot |

## What changed

Changes from Step 27:
- **pci.h**: `int present` field and `pci_init_hotplug()` declaration
- **pci.c**: `dev->present = 1` in `pci_init()`, `pci_init_hotplug()`, and presence guards in config_read/write
- **microkvm.c**: global `pci_hotplug_dev`, `Ctrl-A h` handler, generalized config routing through `target`, hotplug BAR0 MMIO routing, initialization and RAM setup

## Next step

Phase E is complete. The full PCI device lifecycle:

```
Step 24: Discover (config-space enumeration)
Step 25: Access (device registers through BAR)
Step 26: Transfer (DMA)
Step 27: Notify (MSI-X interrupts)
Step 28: Lifecycle (hotplug add/remove)
```
