# Step 27: PCI hotplug — runtime device add/remove

## Goal

Simulate PCI hotplug by toggling a device's presence at runtime. `Ctrl-A h` makes a second PCI device appear or disappear; the guest uses standard sysfs interfaces (`rescan`/`remove`) to discover or detach it.

## Background

### What is PCI hotplug?

PCI hotplug is the ability to add or remove devices while the system is running. Real-world use cases:
- Hot-swapping NVMe drives in servers
- Thunderbolt device plug/unplug
- Cloud block storage attach/detach (appears as PCI device to guest)
- QEMU/libvirt `virsh attach-device` / `virsh detach-device`

### How absence is represented in PCI

When Linux scans a bus slot and reads vendor ID = 0xFFFF, it means "no device present." This is the PCI standard signal for an empty slot — on a physical bus, an empty slot returns all-ones because no device drives the data lines.

```
present=1: config read → Vendor=0x1234 → device exists
present=0: config read → Vendor=0xFFFF → empty slot
```

### Native PCIe hotplug vs sysfs rescan

| | Native PCIe hotplug | sysfs rescan (microkvm) |
|---|---|---|
| Mechanism | Root port + slot control + interrupt notification | Guest explicitly rescans bus |
| Complexity | High (state machine, attention button, power control) | Low (toggle flag + rescan) |
| Guest behavior | Automatic detection | Manual `echo 1 > /sys/bus/pci/rescan` |

microkvm uses the simpler sysfs approach. The educational value is equivalent — the guest-side behavior (enumeration, BAR allocation, removal) is identical. Production systems additionally notify the OS through a Hot-Plug Controller interrupt, eliminating the need for manual rescans.

## Execution flow

```
VMM (Ctrl-A h)               Guest
──────────────               ─────
                             / # ls /sys/bus/pci/devices/
                             0000:00:00.0

pci_hotplug_dev.present = 1
"[monitor] ADDED"
                             / # echo 1 > /sys/bus/pci/rescan
                             Linux probes bus:
                               device=1: vendor=0x1234 → found!
                               BAR0 probing → assigned 0x08002000
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

### pci.h — present flag

```c
struct pci_device {
    ...
    /* hotplug */
    int present;    /* 0=absent (returns 0xFFFF), 1=present */
};
```

### pci.c — presence guards

```c
void pci_config_write(struct pci_device *dev, ...) {
    if (!dev->present)
        return;   /* ignore writes to absent device */
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
    *(uint16_t *)&dev->config[0x02] = 0x0002;   /* different device ID */
    dev->config[0x0B] = 0xFF;   /* class: unassigned */
    dev->bar0_mask = ~(PCI_BAR0_SIZE - 1);
    dev->present = 0;   /* starts absent */
}
```

### microkvm.c — config routing generalized

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

### microkvm.c — Ctrl-A h monitor command

```c
if (c == 'h') {
    pci_hotplug_dev.present = !pci_hotplug_dev.present;
    fprintf(stderr, "\n[monitor] PCI device 0000:00:01.0 %s\n",
        pci_hotplug_dev.present ? "ADDED ..." : "REMOVED ...");
}
```

### microkvm.c — hotplug device BAR0 MMIO routing

```c
} else {
    uint32_t bar0_hp = pci_bar0_addr(&pci_hotplug_dev);
    if (bar0_hp && addr >= bar0_hp && addr < bar0_hp + PCI_BAR0_SIZE) {
        /* route to pci_hotplug_dev */
    }
}
```

## Output

```
/ # ls /sys/bus/pci/devices/
0000:00:00.0

(Ctrl-A h)
[monitor] PCI device 0000:00:01.0 ADDED (run: echo 1 > /sys/bus/pci/rescan)

/ # echo 1 > /sys/bus/pci/rescan
pci 0000:00:01.0: [1234:0002] type 00 class 0xff0000 conventional PCI endpoint
pci 0000:00:01.0: BAR 0 [mem 0x08002000-0x08002fff]: assigned

/ # ls /sys/bus/pci/devices/
0000:00:00.0  0000:00:01.0

/ # cat /sys/bus/pci/devices/0000:00:01.0/device
0x0002

(Ctrl-A h)
[monitor] PCI device 0000:00:01.0 REMOVED (run: echo 1 > /sys/bus/pci/devices/0000:00:01.0/remove)

/ # echo 1 > /sys/bus/pci/devices/0000:00:01.0/remove
/ # ls /sys/bus/pci/devices/
0000:00:00.0
```

The hotplug device uses device ID 0x0002 (vs 0x0001 for the main device) so they are distinguishable. The BAR address (0x08002000) is assigned during each enumeration pass by Linux's resource allocator and is not guaranteed to remain constant across rescans.

> **Note:** `rescan` asks the PCI core to discover new devices. `remove` detaches an already-enumerated device from the kernel. They are not inverse operations — `remove` does not make the device invisible to future rescans (that requires toggling `present` via `Ctrl-A h`).

## Key insight

PCI hotplug at its core is simple: control whether config reads return a valid vendor ID or 0xFFFF. When 0xFFFF, the slot appears empty. When valid, Linux performs the full enumeration sequence (config read → BAR probe → resource assignment) — exactly the same as boot-time discovery. The complexity in production (native PCIe hotplug) comes from *notifying* the guest automatically; microkvm sidesteps this with manual sysfs rescan, which exercises the same kernel code paths.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| PCI hotplug | Runtime device add/remove by toggling config space visibility |
| Vendor 0xFFFF | PCI standard "no device present" signal |
| sysfs rescan | `echo 1 > /sys/bus/pci/rescan` triggers Linux bus re-enumeration |
| sysfs remove | `echo 1 > .../remove` detaches device from kernel |
| Config routing generalization | `target` pointer selects device by BDF number |
| BAR re-probing on hotplug | Same probe sequence as boot-time |

## What changed

From Step 26:
- **pci.h**: `int present` field, `pci_init_hotplug()` declaration
- **pci.c**: `dev->present = 1` in pci_init, `pci_init_hotplug()`, presence guards in config_read/write
- **microkvm.c**: `pci_hotplug_dev` global, `Ctrl-A h` handler, generalized config routing with `target`, hotplug BAR0 MMIO routing, init + RAM setup

## Next step

Phase E is complete. The full PCI device lifecycle:

```
Step 23: Discover (config space enumeration)
Step 24: Access (device registers via BAR)
Step 25: Transfer (DMA)
Step 26: Notify (MSI-X interrupts)
Step 27: Lifecycle (hotplug add/remove)
```
