# Step 13: virtio feature negotiation

## Goal

Explicitly handle feature-negotiation registers used to select optional features, and retain values written by the driver. Continue offering zero features, while also storing the page size and queue number for the upcoming queue setup.

## Background

### What is feature negotiation?

Before a virtio device transfers data, the driver and device must agree on which optional features to use. This is **feature negotiation**, the following handshake:

1. Device advertises supported features (HostFeatures)
2. Driver selects the features it wants to use (GuestFeatures)
3. Subsequent initialization and data transfer use the selected features

Selecting only features offered by the device and understood by the driver prevents accidental use of unsupported features.

### Feature negotiation flow

Linux's virtio-mmio driver reads and writes feature bits in upper and lower 32-bit portions. Each portion is called a bank, selected through the Sel registers:

```
64-bit feature bitmap:
  63.................32 31.................0
  +--------------------+--------------------+
  |       bank 1       |       bank 0       |
  +--------------------+--------------------+
  HostFeaturesSel=1    HostFeaturesSel=0
```

```
Driver                                    Device (VMM)
──────                                    ────────────
write HostFeaturesSel = 1                 (select feature bank 1: bits 32-63)
read  HostFeatures    → 0                 (no high features)
write HostFeaturesSel = 0                 (select feature bank 0: bits 0-31)
read  HostFeatures    → 0                 (no low features either)
write GuestFeaturesSel = 1
write GuestFeatures = 0                   (accept nothing from bank 1)
write GuestFeaturesSel = 0
write GuestFeatures = 0                   (accept nothing from bank 0 either)
→ Proceed to virtqueue setup
```

microkvm returns `0` for HostFeatures regardless of the selected bank. Linux selects no features and writes `0` to GuestFeatures. The VMM does not store GuestFeaturesSel; it retains only the last 32-bit write to GuestFeatures. This is not an implementation that stores and validates features across multiple banks.

### Legacy vs Modern

Legacy virtio-mmio (Version=1) has no `FEATURES_OK` status step. After writing ACKNOWLEDGE | DRIVER (`0x03`), the driver proceeds through feature reads/writes to queue setup. With modern virtio-mmio (Version=2), the driver must write `FEATURES_OK` and confirm the device accepted it.

### Registers

| Offset | Name | R/W | Role |
|--------|------|-----|---------|
| 0x010 | HostFeatures | R | Feature bits offered by the device (selected bank) |
| 0x014 | HostFeaturesSel | W | Select the 32-bit HostFeatures bank to read (0=low, 1=high) |
| 0x020 | GuestFeatures | W | Feature bits accepted by the driver |
| 0x024 | GuestFeaturesSel | W | Select the GuestFeatures bank to write |
| 0x028 | GuestPageSize | W | Driver reports page size (usually 4096); used in Step 14 to calculate vring physical addresses: `GPA = QueuePFN × GuestPageSize` |
| 0x030 | QueueSel | W | Select the virtqueue to configure (virtio-console: 0=receive queue, 1=transmit queue) |
| 0x034 | QueueNumMax | R | Maximum descriptor count supported by the selected queue |

## Execution flow

```text
Guest (Linux)               KVM                         VMM
     | Status=0x03 already set                           |
     |-- HostFeaturesSel --->|-- KVM_EXIT_MMIO ---------->|
     |                       |                           | Store selection
     |                       |<-- KVM_RUN ---------------|
     |-- Read HostFeatures ->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | Always return 0
     |                       |<-- KVM_RUN ---------------|
     | Select no features    |                           |
     |-- GuestFeaturesSel -->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | Do not store selection
     |                       |<-- KVM_RUN ---------------|
     |-- GuestFeatures=0 --->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | guest_features=0
     |                       |<-- KVM_RUN ---------------|
     |-- QueueSel=0 -------->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | Store queue_sel=0
     |                       |<-- KVM_RUN ---------------|
     |-- Read QueueNumMax -->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | Return 0
     |                       |<-- KVM_RUN ---------------|
     | Queue init fails      |                           |
```

Features are read and written in bank 1, then bank 0 order. Initialization proceeds to queue setup, but QueueNumMax is still `0`, so it fails at the same point as in Step 12.

## Implementation

### Additions to virtio_mmio.h

New register offset definitions:

```c
/* Feature negotiation */
#define VIRTIO_MMIO_HOST_FEATURES       0x010
#define VIRTIO_MMIO_HOST_FEATURES_SEL   0x014
#define VIRTIO_MMIO_GUEST_FEATURES      0x020
#define VIRTIO_MMIO_GUEST_FEATURES_SEL  0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE     0x028
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
```

New fields in the device structure:

```c
struct virtio_mmio_dev {
    uint32_t status;
    uint32_t host_features_sel;   /* Feature bank to read */
    uint32_t guest_features;      /* Last 32-bit value written */
    uint32_t guest_page_size;     /* Page size reported by driver (4096) */
    uint32_t queue_sel;           /* Virtqueue number to configure */
};
```

### Additions to virtio_mmio.c

Read handler:

```c
case VIRTIO_MMIO_HOST_FEATURES:
    val = 0;  /* No features offered yet */
    break;
case VIRTIO_MMIO_QUEUE_NUM_MAX:
    val = 0;  /* Made nonzero in Step 14 */
    break;
```

Write handler:

```c
case VIRTIO_MMIO_HOST_FEATURES_SEL:
    dev->host_features_sel = value;
    break;
case VIRTIO_MMIO_GUEST_FEATURES_SEL:
    break;  /* Do not store the selection */
case VIRTIO_MMIO_GUEST_FEATURES:
    dev->guest_features = value;
    break;
case VIRTIO_MMIO_GUEST_PAGE_SIZE:
    dev->guest_page_size = value;
    break;
case VIRTIO_MMIO_QUEUE_SEL:
    dev->queue_sel = value;
    break;
```

`host_features_sel` is stored but is not used to select the read value. Since all offered features are `0`, reading either bank returns the same value at this stage.

## Output

```
[virtio-mmio] write offset=0x070 ← 0x3
[virtio-mmio] write offset=0x014 ← 0x1
[virtio-mmio] read  offset=0x010 → 0x0
[virtio-mmio] write offset=0x014 ← 0x0
[virtio-mmio] read  offset=0x010 → 0x0
[virtio-mmio] write offset=0x024 ← 0x1
[virtio-mmio] write offset=0x020 ← 0x0
[virtio-mmio] write offset=0x024 ← 0x0
[virtio-mmio] write offset=0x020 ← 0x0
[virtio-mmio] write offset=0x030 ← 0x0
[virtio-mmio] read  offset=0x040 → 0x0
[virtio-mmio] read  offset=0x034 → 0x0
[virtio-mmio] write offset=0x040 ← 0x0
[virtio-mmio] read  offset=0x070 → 0x3
[virtio-mmio] write offset=0x070 ← 0x83
virtio_console virtio0: Error -2 initializing vqs
virtio_console virtio0: probe with driver virtio_console failed with error -2
```

`0x014` / `0x024` select banks, `0x010` / `0x020` read/write features, and `0x030` / `0x034` select a queue and query its maximum size. Initialization failure due to QueueNumMax=`0` is expected; Status becomes `0x83` with FAILED added. Press `Ctrl-C` in the host terminal to exit.

## Key insight

Even in Step 12, unimplemented registers returned `0`, allowing Linux to finish feature reads/writes and reach queue setup. Step 13 differs in its explicit register handling and storage of values such as GuestFeatures, GuestPageSize, and QueueSel, not in its logged values.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| Feature negotiation | Device offers 0 features, so driver selects none |
| Bank selection | Linux accesses upper/lower banks; VMM returns 0 for both |
| GuestPageSize | Driver reports page size (used for vring addresses in Step 14) |
| QueueSel | Select the virtqueue to operate on (receiveq=0, transmitq=1) |
| Retaining register values | Move from ignoring writes to storing state for later use |

## What changed

Changes from Step 12:

- **virtio_mmio.h**: seven new register offset definitions and four new structure fields
- **virtio_mmio.c read**: add `HOST_FEATURES` and `QUEUE_NUM_MAX` cases (both return 0)
- **virtio_mmio.c write**: add `HOST_FEATURES_SEL`, `GUEST_FEATURES_SEL`, `GUEST_FEATURES`, `GUEST_PAGE_SIZE`, and `QUEUE_SEL` cases

No changes to `microkvm.c` or the `Makefile`.

## Next step

[Step 14: Virtqueue Setup](step14_virtqueue-setup.md)—Return a nonzero QueueNumMax so the kernel can establish virtqueues, creating the shared-memory ring buffers.
