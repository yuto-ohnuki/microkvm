# Step 13: virtio feature negotiation

## Goal

Implement the feature negotiation registers so the kernel driver can read device capabilities and write back accepted features. This completes the second phase of the virtio initialization sequence.

## Background

### What is feature negotiation?

Before a virtio device can transfer data, the driver and device must agree on which optional capabilities to use. This is called **feature negotiation** — a handshake where:

1. The device advertises what it supports (HostFeatures)
2. The driver selects which features it wants to use (GuestFeatures)
3. Both sides commit to this agreed subset

This ensures forward/backward compatibility: a new driver can work with an old device (and vice versa) by only using features both understand.

### Feature negotiation flow

Features are represented as a 64-bit bitmap, split into two 32-bit banks:
```
64-bit feature bitmap:
  63................32  31................0
  +------------------+--------------------+
  |     bank 1       |      bank 0        |
  +------------------+--------------------+
  HostFeaturesSel=1    HostFeaturesSel=0
```

```
Driver                                    Device (VMM)
──────                                    ────────────
write HostFeaturesSel = 1                 (select feature bank 1: bits 32-63)
read  HostFeatures    → 0                 (no high features offered)
write HostFeaturesSel = 0                 (select feature bank 0: bits 0-31)
read  HostFeatures    → 0                 (no low features offered either)
write GuestFeaturesSel = 1
write GuestFeatures = 0                   (accept nothing from bank 1)
write GuestFeaturesSel = 0
write GuestFeatures = 0                   (accept nothing from bank 0)
→ proceed to virtqueue setup
```

In microkvm, HostFeatures returns 0 (no optional features). The kernel still goes through the full negotiation sequence, which is why we must handle these registers even though the values are all zero.

### Legacy vs Modern

In legacy virtio-mmio (Version=1), there is no `FEATURES_OK` status step. The driver writes DRIVER (0x03) and proceeds directly to feature read/write → queue setup. Modern (v2) would require the driver to write `FEATURES_OK` and verify the device accepted.

### Register summary

| Offset | Name | R/W | Purpose |
|--------|------|-----|---------|
| 0x010 | HostFeatures | R | Device-offered feature bits (selected bank) |
| 0x014 | HostFeaturesSel | W | Select which 32-bit bank of HostFeatures to read (0=low, 1=high) |
| 0x020 | GuestFeatures | W | Driver-accepted feature bits |
| 0x024 | GuestFeaturesSel | W | Select which bank of GuestFeatures to write |
| 0x028 | GuestPageSize | W | Driver informs device of its page size (typically 4096). Used in Step 14 to calculate vring physical address: `GPA = QueuePFN × GuestPageSize` |
| 0x030 | QueueSel | W | Select which virtqueue to configure (virtio-console: 0=receive queue, 1=transmit queue) |
| 0x034 | QueueNumMax | R | Maximum descriptors the selected queue supports |

## Execution flow

```
Linux kernel (virtio_console)              VMM (microkvm)
─────────────────────────────              ──────────────
Status = DRIVER (0x03) already set

write HostFeaturesSel (0x014) ← 1         dev->host_features_sel = 1
read  HostFeatures (0x010) → 0            (bank 1: no features)
write HostFeaturesSel (0x014) ← 0         dev->host_features_sel = 0
read  HostFeatures (0x010) → 0            (bank 0: no features)

write GuestFeaturesSel (0x024) ← 1
write GuestFeatures (0x020) ← 0           dev->guest_features = 0 (bank 1)
write GuestFeaturesSel (0x024) ← 0
write GuestFeatures (0x020) ← 0           dev->guest_features = 0 (bank 0)

write QueueSel (0x030) ← 0                dev->queue_sel = 0 (receiveq)
read  QueueNumMax (0x034) → 0             "no queue available" → fails
```

Feature negotiation succeeds (trivially — both sides agree on zero features). The failure occurs at QueueNumMax, which is resolved in Step 14.

## Implementation

### virtio_mmio.h additions

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

New fields in device struct:
```c
struct virtio_mmio_dev {
    uint32_t status;
    uint32_t host_features_sel;   /* which feature bank to read */
    uint32_t guest_features;      /* features accepted by driver */
    uint32_t guest_page_size;     /* page size reported by driver (4096) */
    uint32_t queue_sel;           /* which virtqueue is being configured */
};
```

### virtio_mmio.c additions

Read handler:
```c
case VIRTIO_MMIO_HOST_FEATURES:
    val = 0;  /* no features offered for now */
    break;
case VIRTIO_MMIO_QUEUE_NUM_MAX:
    val = 0;  /* will be non-zero in Step 14 */
    break;
```

Write handler:
```c
case VIRTIO_MMIO_HOST_FEATURES_SEL:
    dev->host_features_sel = value;
    break;
case VIRTIO_MMIO_GUEST_FEATURES_SEL:
    break;  /* acknowledged but value not stored — microkvm exposes no features,
             * so bank selection has no effect. A full implementation would store
             * this and combine multiple 32-bit writes into a 64-bit feature set. */
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

## Output

```
[virtio-mmio] write offset=0x070 ← 0x3          Status = DRIVER
[virtio-mmio] write offset=0x014 ← 0x1          HostFeaturesSel = 1
[virtio-mmio] read  offset=0x010 → 0x0          HostFeatures (bank 1) = 0
[virtio-mmio] write offset=0x014 ← 0x0          HostFeaturesSel = 0
[virtio-mmio] read  offset=0x010 → 0x0          HostFeatures (bank 0) = 0
[virtio-mmio] write offset=0x024 ← 0x1          GuestFeaturesSel = 1
[virtio-mmio] write offset=0x020 ← 0x0          GuestFeatures (bank 1) = 0
[virtio-mmio] write offset=0x024 ← 0x0          GuestFeaturesSel = 0
[virtio-mmio] write offset=0x020 ← 0x0          GuestFeatures (bank 0) = 0
[virtio-mmio] write offset=0x030 ← 0x0          QueueSel = 0
[virtio-mmio] read  offset=0x034 → 0x0          QueueNumMax = 0 → fails
```

The kernel completes feature negotiation and reaches the queue setup phase. QueueNumMax=0 causes the same failure as Step 12 — this is resolved in Step 14.

## Key insight

Feature negotiation exists to ensure compatibility between different versions of drivers and devices. In microkvm's minimal implementation, both sides agree on zero features — but the protocol still must be followed. The kernel reads HostFeatures regardless of the values, because skipping this step would violate the virtio spec's state machine. This teaches an important lesson: **even a "do-nothing" device must implement the full protocol handshake**.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| Feature negotiation | Device advertises, driver selects, both commit |
| Bank selection | HostFeaturesSel selects which 32-bit half to read |
| GuestPageSize | Driver informs device of page size (used in Step 14 for vring address calculation) |
| QueueSel | Selects which virtqueue to operate on (receiveq=0, transmitq=1) |
| Protocol compliance | Must handle all registers even when returning zero |

## What changed

From Step 12:
- **virtio_mmio.h**: 7 new register offset defines + 4 new struct fields
- **virtio_mmio.c read**: `HOST_FEATURES` and `QUEUE_NUM_MAX` cases (both return 0)
- **virtio_mmio.c write**: `HOST_FEATURES_SEL`, `GUEST_FEATURES_SEL`, `GUEST_FEATURES`, `GUEST_PAGE_SIZE`, `QUEUE_SEL` cases

No changes to `microkvm.c` or `Makefile`.

## Next step

[Step 14: virtqueue setup](step14_virtqueue-setup.md) — return a non-zero QueueNumMax so the kernel can allocate and configure virtqueues. This is where shared memory ring buffers are established.
