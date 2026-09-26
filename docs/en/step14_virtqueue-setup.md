# Step 14: virtqueue setup

## Goal

Configure two virtqueues for transmission and reception, allowing the Linux driver to set DRIVER_OK. The VMM stores queue sizes and layouts; data transfer is implemented in subsequent steps.

## Background

### What is a virtqueue?

The virtqueues used here exchange data buffers through a shared-memory management structure called a **vring**. The guest submits buffers for processing, and the VMM records results. Writing QueueNotify (a kick) tells the VMM that the queue has work.

### vring memory layout

This legacy layout allocates the following three regions together in guest RAM. Actual data resides in separate buffers pointed to by descriptors.

```text
GPA = QueuePFN × GuestPageSize

GPA
  Descriptor Table: 16 × num bytes
    [addr, len, flags, next] × num
    Buffer address and length, and next descriptor index

GPA + 16 × num
  Available Ring: 6 + 2 × num bytes
    [flags][idx][descriptor indices × num][used_event]
    Guest → VMM: submit work

  (Padding to the QueueAlign boundary)

Start of Used Ring
  Used Ring: 6 + 8 × num bytes
    [flags][idx][{id, len} × num][avail_event]
    VMM → guest: report completed descriptor chains
```

`num` is the descriptor count. In the Used Ring, `id` is the chain's head index and `len` is the number of bytes the device wrote to the buffer. The size formulas above include the two-byte event fields at the ends; event-based notification suppression is not used at this stage.

### Address calculation

The kernel writes `QueuePFN` (page frame number). This mechanism is specific to legacy virtio-mmio; the modern MMIO interface specifies each of the three region addresses separately. Here the VMM calculates the layout as follows:

```
vring GPA       = QueuePFN × GuestPageSize
desc base       = vring GPA
avail base      = vring GPA + num × 16
used base       = align_up(avail base + 6 + 2×num, QueueAlign)

Example: QueuePFN = 0x47a, GuestPageSize = 4096
  → GPA = 0x47a × 4096 = 0x47a000
```

### Setup sequence (per queue)

```
1. Driver writes QueueSel     → Select queue (0 or 1)
2. Driver reads QueuePFN      → 0 means unused
3. Driver reads QueueNumMax   → Maximum descriptors supported by this queue
4. Driver writes QueueNum     → Number to use (≤ QueueNumMax)
5. Driver writes QueueAlign   → Used Ring alignment (4096)
6. Driver writes QueuePFN     → Page frame number containing the vring
   → VMM now knows the shared-memory location
```

### virtio-console queue configuration

| Queue | Index | Direction | Purpose |
|-------|-------|-----------|---------|
| receiveq | 0 | host → guest | VMM writes data for the guest to read |
| transmitq | 1 | guest → host | Guest writes data for the VMM to read |

## Execution flow

```text
Guest (Linux)               KVM                         VMM
     |-- QueueSel=0 -------->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | queue_sel=0
     |                       |<-- KVM_RUN ---------------|
     |-- Read QueuePFN ----->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | Return vqs[0].pfn=0
     |                       |<-- KVM_RUN ---------------|
     |-- Read QueueNumMax -->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | Return 128
     |                       |<-- KVM_RUN ---------------|
     | Allocate vring memory |                           |
     |-- Num / Align / PFN ->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | Store settings in vqs[0]
     |                       |                           | Calculate and log layout
     |                       |<-- KVM_RUN ---------------|
     | Repeat setup with QueueSel=1, storing in vqs[1]   |
     |-- Status=0x07 ------->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | Store value including DRIVER_OK
     |                       |<-- KVM_RUN ---------------|
     |-- QueueNotify=0 ---->|-- KVM_EXIT_MMIO ---------->|
     |                       |                           | Log only; RX not implemented
```

Num, Align, and PFN each use a separate MMIO write. The driver submits receive buffers to the configured receiveq and notifies through QueueNotify.

## Implementation

### Additions to virtio_mmio.h

New register offsets and per-queue state:

```c
/* Virtqueue setup */
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_ALIGN         0x03C
#define VIRTIO_MMIO_QUEUE_PFN           0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064

#define VIRTQ_NUM_QUEUES 2      /* receiveq (0) + transmitq (1) */
#define VIRTQ_MAX_SIZE   128    /* Maximum descriptors per queue */

/* Per-virtqueue configuration (set by guest during setup) */
struct virtqueue_state {
    uint32_t num;       /* Queue size (from QueueNum) */
    uint32_t align;     /* Used Ring alignment (from QueueAlign) */
    uint32_t pfn;       /* vring page frame number (from QueuePFN) */
};
```

Add a per-queue array to the device structure:

```c
struct virtio_mmio_dev {
    ...
    struct virtqueue_state vqs[VIRTQ_NUM_QUEUES];
};
```

QueueSel selects the target queue; it is not a queue configuration value itself. Storing settings separately in `vqs[0]` and `vqs[1]` preserves the receive queue configuration when switching to the transmit queue.

### Additions to virtio_mmio.c

Read handler—QueueNumMax returns 128 for QueueSel 0 or 1, and 0 otherwise:

```c
case VIRTIO_MMIO_QUEUE_NUM_MAX:
    val = (dev->queue_sel < VIRTQ_NUM_QUEUES) ? 128 : 0;
    break;
case VIRTIO_MMIO_QUEUE_PFN:
    val = (dev->queue_sel < VIRTQ_NUM_QUEUES) ? dev->vqs[dev->queue_sel].pfn : 0;
    break;
```

Write handler—store QueueNum and QueueAlign, and calculate the layout when QueuePFN is written:

```c
case VIRTIO_MMIO_QUEUE_NUM:
    if (dev->queue_sel < VIRTQ_NUM_QUEUES)
        dev->vqs[dev->queue_sel].num = value;
    break;
case VIRTIO_MMIO_QUEUE_ALIGN:
    if (dev->queue_sel < VIRTQ_NUM_QUEUES)
        dev->vqs[dev->queue_sel].align = value;
    break;
```

```c
case VIRTIO_MMIO_QUEUE_PFN:
    if (dev->queue_sel < VIRTQ_NUM_QUEUES) {
        dev->vqs[dev->queue_sel].pfn = value;
        if (value) {
            uint64_t gpa = (uint64_t)value * dev->guest_page_size;
            uint32_t num = dev->vqs[dev->queue_sel].num;
            uint32_t align = dev->vqs[dev->queue_sel].align;
            uint64_t avail = gpa + num * 16;
            uint64_t used = (avail + 6 + 2 * num + align - 1) & ~((uint64_t)align - 1);
            fprintf(stderr, "[virtio-mmio] queue %d: desc=0x%lx avail=0x%lx used=0x%lx (num=%d)\n",
                dev->queue_sel, gpa, avail, used, num);
        }
    }
    break;
```

The calculated addresses are only logged; descriptor and ring contents are not yet accessed. Writes to QueueNotify and InterruptACK are also only logged, and InterruptStatus always returns `0`.

## Output

```
[virtio-mmio] write offset=0x030 ← 0x0
[virtio-mmio] read  offset=0x040 → 0x0
[virtio-mmio] read  offset=0x034 → 0x80
[virtio-mmio] write offset=0x038 ← 0x80
[virtio-mmio] write offset=0x03c ← 0x1000
[virtio-mmio] write offset=0x040 ← 0x47a
[virtio-mmio] queue 0: desc=0x47a000 avail=0x47a800 used=0x47b000 (num=128)
[virtio-mmio] write offset=0x030 ← 0x1
[virtio-mmio] read  offset=0x040 → 0x0
[virtio-mmio] read  offset=0x034 → 0x80
[virtio-mmio] write offset=0x038 ← 0x80
[virtio-mmio] write offset=0x03c ← 0x1000
[virtio-mmio] write offset=0x040 ← 0x47c
[virtio-mmio] queue 1: desc=0x47c000 avail=0x47c800 used=0x47d000 (num=128)
[virtio-mmio] read  offset=0x070 → 0x3
[virtio-mmio] write offset=0x070 ← 0x7
[virtio-mmio] write offset=0x050 ← 0x0
...
[virtio-mmio] read  offset=0x070 → 0x7
```

The two `queue` logs show the queue layouts. Status=`0x07` (ACKNOWLEDGE | DRIVER | DRIVER_OK) confirms that the driver reported initialization complete. Writing `0` to QueueNotify (`0x050`) notifies receiveq; it does not show that the VMM transferred received data.

## Key insight

The guest allocates vring memory, and the VMM receives its layout and size through registers. Retaining settings per transmit/receive queue allows later processing to access shared memory. DRIVER_OK indicates that the driver is ready, but VMM data-transfer handling is not implemented yet.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| vring layout | Descriptor table + avail ring + used ring in contiguous memory |
| Per-queue state | num/align/pfn in `struct virtqueue_state` |
| QueueNumMax | Device tells driver the maximum queue capacity |
| QueuePFN | Driver tells device where the vring is located |
| GPA calculation | PFN × PageSize = vring physical address |
| DRIVER_OK | Status bit indicating driver initialization is complete |
| QueueNotify | Placeholder for the kick mechanism (implemented in Step 15) |

## What changed

Changes from Step 13:

- **virtio_mmio.h**: six new register offsets, `VIRTQ_NUM_QUEUES`/`VIRTQ_MAX_SIZE`, `struct virtqueue_state`, and the `vqs[]` array
- **virtio_mmio.c read**: `QUEUE_NUM_MAX` returns 128, `QUEUE_PFN` returns the stored value, `INTERRUPT_STATUS` returns 0
- **virtio_mmio.c write**: add `QUEUE_NUM`, `QUEUE_ALIGN`, `QUEUE_PFN` (with GPA logging), `QUEUE_NOTIFY`, and `INTERRUPT_ACK` cases

## Next step

[Step 15: virtio-console TX](step15_virtio-tx.md)—Read writes to `/dev/hvc0` from transmitq and send them to host standard output.
