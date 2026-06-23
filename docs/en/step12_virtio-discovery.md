# Step 12: virtio-mmio device discovery

## Goal

Implement a virtio-mmio register space in the VMM so that the Linux kernel's virtio-mmio driver can discover and identify the device. This is the first step toward shared-memory I/O.

## Background

### Why virtio?

In Steps 10–11, the guest communicates through the 8250 UART at port 0x3F8. Every character causes a VM exit (PIO trap). For high-throughput I/O, this per-character exit model is too expensive.

Virtio solves this by placing data in **shared memory ring buffers** (virtqueues). The guest and VMM exchange data through these rings, and only a single "kick" notification is needed per batch — rather than one exit per byte.

### virtio-mmio transport

The virtio specification defines multiple transports (PCI, MMIO, Channel I/O). microkvm uses **virtio-mmio** because it is the simplest — no PCI bus emulation needed. The device appears as a flat MMIO register region at a fixed guest physical address.

Linux discovers virtio-mmio devices via the kernel command line:
```
virtio_mmio.device=0x200@0xd0000000:5
```
Format: `<size>@<base_address>:<irq>`. The kernel's internal function `virtio_mmio_cmdline_devices()` parses this and registers a platform device, then calls `virtio_mmio_probe()` to read the registers.

### Why GPA 0xD0000000?

Guest RAM is 128 MB (0x0–0x07FFFFFF). Placing the device at 0xD0000000 ensures it is not backed by guest RAM and is outside any memory slot registered with KVM. When the guest accesses this address, there is no EPT mapping → EPT violation → `KVM_EXIT_MMIO` — the same mechanism from Step 5, but now used for a real device protocol.

### Register layout (identification)

| Offset | Name | Value | Meaning |
|--------|------|-------|---------|
| 0x000 | MagicValue | 0x74726976 | ASCII "virt" (little-endian) — confirms virtio device |
| 0x004 | Version | 1 | Legacy interface (v1). microkvm uses v1 because it keeps the initial implementation small. Modern v2 adds a FEATURES_OK step and can be added later |
| 0x008 | DeviceID | 3 | Device type: virtio-console (1=net, 2=block, 3=console). Chosen as natural extension of UART |
| 0x00C | VendorID | 0x4D4B564D | "MKVM" — arbitrary identification value (Linux drivers don't check this) |
| 0x070 | Status | (read/write) | Virtio device/driver initialization state machine (ACKNOWLEDGE → DRIVER → DRIVER_OK) |

## Execution flow

```
Linux kernel                              KVM                      VMM (microkvm)
────────────                              ───                      ──────────────
                                                                   CMDLINE includes:
                                                                   "virtio_mmio.device=0x200@0xd0000000:5"

boot complete:
  virtio_mmio_cmdline_devices()
  parses CMDLINE → registers
  platform_device (base=0xD0000000)
  (kernel internal function)

  virtio_mmio_probe():
    read [0xD0000000 + 0x000]
      ↓ EPT violation (not in RAM)
                                          KVM_EXIT_MMIO
                                          phys_addr=0xD0000000 ──→ offset=0x000
                                                                   virtio_mmio_read()
                                                                   → return 0x74726976 ("virt")
                                          KVM_RUN ←────────────────
    eax = 0x74726976
    "Magic OK, this is a virtio device!"

    read DeviceID (0x008) → 3
    "This is a virtio-console"

    write Status (0x070) ← 0x0           → device reset
    write Status (0x070) ← 0x1           → ACKNOWLEDGE
    write Status (0x070) ← 0x3           → DRIVER

  virtio_console driver probes:
    → attempts virtqueue setup
    → QueueNumMax = 0 → fails
    → writes Status = 0x83 (FAILED)
```

## Implementation

### New files

**`virtio_mmio.h`** — Register offset definitions and device state:
```c
#define VIRTIO_MMIO_BASE  0xD0000000
#define VIRTIO_MMIO_SIZE  0x200

struct virtio_mmio_dev {
    uint32_t status;
};
```

**`virtio_mmio.c`** — Read/write handlers:
```c
uint32_t virtio_mmio_read(struct virtio_mmio_dev *dev, uint64_t offset, int len)
{
    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE: return 0x74726976;
    case VIRTIO_MMIO_VERSION:     return 1;
    case VIRTIO_MMIO_DEVICE_ID:   return 3;
    case VIRTIO_MMIO_VENDOR_ID:   return 0x4D4B564D;
    case VIRTIO_MMIO_STATUS:      return dev->status;
    default:                      return 0;
    }
}
```

### microkvm.c changes

1. Kernel command line updated:
```c
#define CMDLINE "console=ttyS0 earlyprintk=serial rdinit=/init virtio_mmio.device=0x200@0xd0000000:5"
```

2. Device initialization in `main()`:
```c
virtio_mmio_init(&virtio_dev);
```

3. MMIO exit handler dispatches to virtio registers:
```c
case KVM_EXIT_MMIO: {
    uint64_t addr = run->mmio.phys_addr;
    if (addr >= VIRTIO_MMIO_BASE && addr < VIRTIO_MMIO_BASE + VIRTIO_MMIO_SIZE) {
        uint64_t offset = addr - VIRTIO_MMIO_BASE;
        if (run->mmio.is_write) {
            uint32_t val = 0;
            memcpy(&val, run->mmio.data, run->mmio.len);
            virtio_mmio_write(&virtio_dev, offset, val, run->mmio.len);
        } else {
            uint32_t val = virtio_mmio_read(&virtio_dev, offset, run->mmio.len);
            memcpy(run->mmio.data, &val, run->mmio.len);
        }
    }
    break;
}
```

### Prerequisites

Phase C requires these kernel config options (rebuild bzImage after adding):
```
CONFIG_VIRTIO_MENU=y
CONFIG_VIRTIO=y
CONFIG_VIRTIO_RING=y
CONFIG_VIRTIO_MMIO=y
CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_HVC_DRIVER=y
```

## Output

```
virtio-mmio: Registering device virtio-mmio.0 at 0xd0000000-0xd00001ff, IRQ 5.
[virtio-mmio] read  offset=0x000 → 0x74726976
[virtio-mmio] read  offset=0x004 → 0x1
[virtio-mmio] read  offset=0x008 → 0x3
[virtio-mmio] read  offset=0x00c → 0x4d4b564d
[virtio-mmio] write offset=0x028 ← 0x1000
[virtio-mmio] write offset=0x070 ← 0x0
[virtio-mmio] device reset
[virtio-mmio] write offset=0x070 ← 0x1
[virtio-mmio] write offset=0x070 ← 0x3
...
virtio_console virtio0: Error -2 initializing vqs
```

The probe succeeds (device recognized, status reaches DRIVER) but virtqueue setup fails because `QueueNumMax` returns 0. QueueNumMax is the register that tells the driver how many descriptors a virtqueue can hold — returning 0 means "no queue available." This is expected — resolved in Step 14.

## Key insight

virtio-mmio device discovery reuses the exact same mechanism as Step 5's MMIO device — an EPT hole causes VM exits that the VMM handles. The difference is that instead of a custom single-register toy device, we now implement the **virtio specification's standard register protocol**. This means Linux's built-in virtio drivers work out of the box — no custom guest-side code needed.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| virtio-mmio transport | Fixed GPA register region, no PCI bus needed |
| Device identification | Magic/Version/DeviceID/VendorID reads |
| MMIO dispatch | Reusing KVM_EXIT_MMIO from Step 5 for a real protocol |
| Status state machine | ACKNOWLEDGE → DRIVER (virtio spec initialization sequence) |
| Kernel command line | `virtio_mmio.device=size@base:irq` declares device location |
| EPT-based MMIO trap | Device at 0xD0000000 is outside RAM → automatic EPT violation |

## What changed

From Step 11:
- **New files**: `virtio_mmio.c`, `virtio_mmio.h`
- **CMDLINE**: added `virtio_mmio.device=0x200@0xd0000000:5`
- **KVM_EXIT_MMIO handler**: address dispatch to virtio register read/write
- **Kernel**: rebuilt with CONFIG_VIRTIO_* options
- **Makefile**: added `virtio_mmio.c` to build

## Next step

[Step 13: virtio feature negotiation](step13_virtio-features.md) — handle HostFeatures/GuestFeatures registers so the kernel can complete feature negotiation.

**Preview of Steps 13–15:** After discovery, the driver needs to negotiate capabilities (Step 13), set up shared memory ring buffers called *virtqueues* where data will flow without per-byte exits (Step 14), and finally transfer actual data through these rings (Step 15). The key question these steps answer: *how do guest and VMM share memory efficiently without stopping the CPU for every byte?*
