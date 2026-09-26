# Step 12: virtio-mmio Device Discovery

> **Phase C: High-Performance I/O (virtio)**
>
> Phase B implemented serial I/O through a UART. Sending and receiving characters required PIO handling by the VMM.
> Phase C implements virtio, the standard paravirtualized I/O framework: shared-memory rings, batched notifications, and in-kernel event delivery (ioeventfd/irqfd).

## Goal

Implement virtio-mmio identification registers and Status so Linux recognizes a virtio-console device. Queues are not provided yet, so virtio-console initialization fails partway through.

## Background

### Why virtio?

In Steps 10–11, every guest access to data or status registers of the 8250 UART (port 0x3F8) returns handling to the VMM through `KVM_EXIT_IO`. Per-character handling becomes costly for large amounts of data.

virtio uses shared-memory queues called **virtqueues** to exchange data-buffer locations and lengths. It can process data in batches and reduce notifications (kicks). This step covers device discovery, the prerequisite for using those queues.

### The virtio-mmio transport

The virtio specification defines several transports (PCI, MMIO, Channel I/O). microkvm uses the simplest, **virtio-mmio**, which requires no PCI bus emulation. The device appears as a flat MMIO register region at a fixed guest physical address.

The VMM tells Linux the device location through the kernel command line:

```
virtio_mmio.device=0x200@0xd0000000:5
```
Format: `<size>@<base address>:<IRQ number>`. Linux parses this, registers a platform device, and reads the identification registers in `virtio_mmio_probe()`. IRQ 5 is declared here, but this tag does not yet implement virtio interrupt notification.

### Why GPA 0xD0000000?

Guest memory ends at 128MB (`0x08000000`). `0xD0000000` lies outside the registered memory slots, so KVM forwards accesses there to the VMM as MMIO. The VMM checks the address in `KVM_EXIT_MMIO` and responds as a virtio register. This uses the same mechanism as Step 5, but a different region from the existing MMIO hole at `0xD0000`.

### Register layout (identification)

| Offset | Name | Value | Meaning |
|--------|------|-------|---------|
| 0x000 | MagicValue | 0x74726976 | ASCII "virt" (little-endian)—identifies a virtio device |
| 0x004 | Version | 1 | Legacy MMIO interface (Version=1), not the version of the virtio specification as a whole |
| 0x008 | DeviceID | 3 | Device type: virtio-console (1=net, 2=block, 3=console), chosen as an extension of Step 11's UART |
| 0x00C | VendorID | 0x4D4B564D | microkvm's custom vendor identifier |
| 0x070 | Status | (read/write) | Driver-set status bits; this tag only stores and reads back the value |

## Execution flow

```text
Guest (Linux)               KVM                         VMM
     |                       |                           | Initialize Status=0
     |                       |                           | Specify location/IRQ in CMDLINE
     | Register during boot  |                           |
     |-- Read MagicValue --->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | virtio_mmio_read()
     |                       |                           | Store 0x74726976 in data
     |                       |<-- KVM_RUN ---------------|
     |<-- Complete read -----|                           |
     | Read Version / DeviceID / VendorID through the same path
     |-- Write Status ------>|-- KVM_EXIT_MMIO --------->|
     |                       |                           | Store in dev->status
     |                       |<-- KVM_RUN ---------------|
     |<-- Continue ----------|                           |
     | Initialize virtio-console                         |
     |-- Read QueueNumMax -->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | Return 0 (not implemented)
     | Queue init fails      |                           |
     | Write Status with FAILED added                    |
```

Status progresses from `0` (reset) to `1` (ACKNOWLEDGE) to `3` (ACKNOWLEDGE | DRIVER), but does not reach DRIVER_OK in this step.

## Implementation

### New files

**`virtio_mmio.h`**—register offset definitions and device state:

```c
#define VIRTIO_MMIO_BASE  0xD0000000
#define VIRTIO_MMIO_SIZE  0x200

struct virtio_mmio_dev {
    uint32_t status;
};
```

**`virtio_mmio.c`**—read handler:

```c
uint32_t virtio_mmio_read(struct virtio_mmio_dev *dev, uint64_t offset, int len)
{
    uint32_t val = 0;

    switch (offset) {
    case VIRTIO_MMIO_STATUS:
        val = dev->status;
        break;
    case VIRTIO_MMIO_MAGIC_VALUE:
        val = VIRTIO_MMIO_MAGIC;
        break;
    case VIRTIO_MMIO_VERSION:
        val = 1;
        break;
    case VIRTIO_MMIO_DEVICE_ID:
        val = VIRTIO_ID_CONSOLE;
        break;
    case VIRTIO_MMIO_VENDOR_ID:
        val = VIRTIO_VENDOR_MKVM;
        break;
    default:
        break;
    }

    fprintf(stderr, "[virtio-mmio] read  offset=0x%03lx → 0x%x\n",
        (unsigned long)offset, val);
    return val;
}
```

Unimplemented registers return `0`. The write handler stores only Status; other writes are logged without updating state. For example, writes to `GuestPageSize` (`0x028`) are not stored in this tag.

### Changes to microkvm.c

1. Update the kernel command line:

```c
#define CMDLINE "console=ttyS0 earlyprintk=serial rdinit=/init virtio_mmio.device=0x200@0xd0000000:5"
```

2. Initialize the device in `main()`:

```c
virtio_mmio_init(&virtio_dev);
```

3. Dispatch MMIO exits to virtio registers:

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

## Prerequisites

### Add kernel configuration and rebuild

On the x86_64 Linux host, reuse the kernel source and `.config` from Step 10. Add virtio settings without rerunning `make tinyconfig`. These instructions assume kernel sources in `~/linux-src` and the VMM in `~/microkvm`.

```bash
$ cd ~/linux-src

$ scripts/config --enable CONFIG_VIRTIO_MENU
$ scripts/config --enable CONFIG_VIRTIO_MMIO
$ scripts/config --enable CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES
$ scripts/config --enable CONFIG_VIRTIO_CONSOLE

# Resolve dependencies and use defaults for new options
$ make olddefconfig

# Confirm that all six options below are =y
$ grep -E '^CONFIG_(VIRTIO_MENU|VIRTIO|VIRTIO_MMIO|VIRTIO_MMIO_CMDLINE_DEVICES|VIRTIO_CONSOLE|HVC_DRIVER)=' .config

$ make -j$(nproc) bzImage
$ cp arch/x86/boot/bzImage ~/microkvm/bzImage
```

Settings to verify:

```text
CONFIG_HVC_DRIVER=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_VIRTIO=y
CONFIG_VIRTIO_MENU=y
CONFIG_VIRTIO_MMIO=y
CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y
```

`VIRTIO_MMIO_CMDLINE_DEVICES` enables device registration from the command line; `VIRTIO_CONSOLE` provides the driver for DeviceID=3. Dependencies enable `VIRTIO` and `HVC_DRIVER`. Ring handling is built with `CONFIG_VIRTIO`, so no separate `CONFIG_VIRTIO_RING` setting is needed.

### Build and start the VMM

Use the same initramfs as in Steps 10–11.

```bash
$ cd ~/microkvm
$ make clean
$ make
$ ./microkvm
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
[virtio-mmio] read  offset=0x070 → 0x0
[virtio-mmio] write offset=0x070 ← 0x1
[virtio-mmio] read  offset=0x070 → 0x1
[virtio-mmio] write offset=0x070 ← 0x3
...
[virtio-mmio] write offset=0x030 ← 0x0
[virtio-mmio] read  offset=0x040 → 0x0
[virtio-mmio] read  offset=0x034 → 0x0
[virtio-mmio] write offset=0x040 ← 0x0
[virtio-mmio] read  offset=0x070 → 0x3
[virtio-mmio] write offset=0x070 ← 0x83
virtio_console virtio0: Error -2 initializing vqs
virtio_console virtio0: probe with driver virtio_console failed with error -2
```

Identification through the registers succeeds, but the virtio-console driver fails to initialize. `QueueNumMax` (`0x034`) reports the maximum queue size; it is unimplemented in this tag and returns `0` (queue unavailable). Status `0x83` combines `0x03` (ACKNOWLEDGE | DRIVER) with `0x80` (FAILED). `Error -2 initializing vqs` is expected at this stage; queue setup is implemented in Step 14.

Shell I/O still uses the UART. Press `Ctrl-C` in the host terminal to exit.

## Key insight

The command line tells Linux where the device is, and MMIO register responses identify its type. Recognizing a device is separate from completing driver initialization and transferring data. This step implements only recognition.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| virtio-mmio transport | Register region at a fixed GPA; no PCI bus required |
| Device identification | Read Magic/Version/DeviceID/VendorID |
| MMIO dispatch | Apply Step 5's KVM_EXIT_MMIO to a real protocol |
| Status transitions | ACKNOWLEDGE → DRIVER (virtio initialization sequence) |
| Kernel command line | Declare device location with `virtio_mmio.device=size@base:irq` |
| MMIO outside memory slots | KVM forwards accesses to the VMM, which responds with register values |

## What changed

Changes from Step 11:

- **New files**: `virtio_mmio.c`, `virtio_mmio.h`
- **CMDLINE**: add `virtio_mmio.device=0x200@0xd0000000:5`
- **KVM_EXIT_MMIO handler**: check the address and dispatch virtio register reads/writes
- **Kernel**: add CONFIG_VIRTIO_* options and rebuild
- **Makefile**: add `virtio_mmio.c` to the build

## Next step

[Step 13: virtio Feature Negotiation](step13_virtio-features.md)—Implement HostFeatures/GuestFeatures registers so the kernel can complete feature negotiation.

Then implement queue setup in Step 14 and transmission in Step 15.
