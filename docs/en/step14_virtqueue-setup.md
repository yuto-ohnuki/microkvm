# Step 14: virtqueue setup

## Goal

Return a non-zero QueueNumMax so the kernel can allocate and configure virtqueues — the shared memory ring buffers that will carry data between guest and VMM. After this step, the device reaches DRIVER_OK status.

## Background

### What is a virtqueue?

A virtqueue is a shared memory data structure (called a **vring**) that allows the guest and VMM to exchange data without per-byte VM exits. Instead of trapping every I/O operation, both sides read and write directly to guest RAM. Only a "kick" notification is needed to signal new work.

### vring memory layout

Each virtqueue consists of three contiguous regions in guest physical memory:

```
GPA = QueuePFN × GuestPageSize

+------------------------------------------+  ← GPA (desc base)
|  Descriptor Table (16 bytes × num)       |
|  Each entry: addr, len, flags, next      |
|  "Here is a buffer at this address"      |
+------------------------------------------+  ← GPA + num×16 (avail base)
|  Available Ring (6 + 2×num bytes)        |
|  Guest → Device: "process this desc"     |
|  [flags][idx][ring[0]..ring[num-1]]      |
+------------------------------------------+
|  (padding to QueueAlign boundary)        |
+------------------------------------------+  ← aligned (used base)
|  Used Ring (6 + 8×num bytes)             |
|  Device → Guest: "I finished this desc"  |
|  Each entry: descriptor ID + bytes used  |
|  [flags][idx][{id,len}×num]              |
+------------------------------------------+
```

### Address calculation

The kernel writes `QueuePFN` — a page frame number. This is the legacy virtio-mmio mechanism; modern devices use explicit descriptor addresses (QueueDescLow/High). The VMM calculates the actual GPA:
```
vring GPA       = QueuePFN × GuestPageSize
desc base       = vring GPA
avail base      = vring GPA + num × 16
used base       = align_up(avail base + 6 + 2×num, QueueAlign)

Example: QueuePFN = 0x4064, GuestPageSize = 4096
  → GPA = 0x4064 × 4096 = 0x4064000
```

### Setup sequence (per queue)

```
1. Driver writes QueueSel       → select which queue (0 or 1)
2. Driver reads  QueueNumMax    → "how many descriptors can this queue hold?"
3. Driver writes QueueNum       → "I will use this many" (≤ QueueNumMax)
4. Driver writes QueueAlign     → alignment for Used Ring (4096)
5. Driver writes QueuePFN       → "the vring is at this page frame number"
   → VMM now knows where the shared memory is
```

### virtio-console queues

| Queue | Index | Direction | Purpose |
|-------|-------|-----------|---------|
| receiveq | 0 | host → guest | VMM writes data for guest to read |
| transmitq | 1 | guest → host | Guest writes data for VMM to read |

## Execution flow

```
Linux kernel                              VMM (microkvm)
────────────                              ──────────────
write QueueSel (0x030) ← 0               dev->queue_sel = 0 (receiveq)
read  QueuePFN (0x040) → 0               "queue not in use, OK to set up"
read  QueueNumMax (0x034) → 128          "this queue supports 128 descriptors"
write QueueNum (0x038) ← 128             dev->vqs[0].num = 128
write QueueAlign (0x03C) ← 4096          dev->vqs[0].align = 4096
write QueuePFN (0x040) ← 0x4064          dev->vqs[0].pfn = 0x4064
                                          → GPA = 0x4064 × 4096 = 0x4064000
                                          → log: "queue 0: desc=0x4064000 ..."

write QueueSel (0x030) ← 1               dev->queue_sel = 1 (transmitq)
read  QueuePFN (0x040) → 0               "not in use"
read  QueueNumMax (0x034) → 128          "128 descriptors"
write QueueNum (0x038) ← 128             dev->vqs[1].num = 128
write QueueAlign (0x03C) ← 4096          dev->vqs[1].align = 4096
write QueuePFN (0x040) ← 0x406a          dev->vqs[1].pfn = 0x406a
                                          → GPA = 0x406a × 4096 = 0x406a000

write Status (0x070) ← 0x7               ACKNOWLEDGE | DRIVER | DRIVER_OK !!!
                                          Device initialization complete.

write QueueNotify (0x050) ← 0 × 128      receiveq kicks — the driver immediately
                                         supplies empty receive buffers so
                                         incoming data has somewhere to land
```

## Implementation

### virtio_mmio.h additions

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
#define VIRTQ_MAX_SIZE   128    /* max descriptors per queue */

/* Per-virtqueue configuration (set by guest during queue setup) */
struct virtqueue_state {
    uint32_t num;       /* queue size (from QueueNum) */
    uint32_t align;     /* Used Ring alignment (from QueueAlign) */
    uint32_t pfn;       /* vring page frame number (from QueuePFN) */
};
```

Device struct gains a per-queue array:
```c
struct virtio_mmio_dev {
    ...
    struct virtqueue_state vqs[VIRTQ_NUM_QUEUES];
};
```

### virtio_mmio.c additions

Read handler — QueueNumMax now returns 128:
```c
case VIRTIO_MMIO_QUEUE_NUM_MAX:
    val = (dev->queue_sel < VIRTQ_NUM_QUEUES) ? 128 : 0;
    break;
case VIRTIO_MMIO_QUEUE_PFN:
    val = (dev->queue_sel < VIRTQ_NUM_QUEUES) ? dev->vqs[dev->queue_sel].pfn : 0;
    break;
```

Write handler — stores per-queue state and logs vring layout:
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

## Output

```
[virtio-mmio] write offset=0x030 ← 0x0          QueueSel = 0
[virtio-mmio] read  offset=0x040 → 0x0          QueuePFN = 0 (not in use)
[virtio-mmio] read  offset=0x034 → 0x80         QueueNumMax = 128
[virtio-mmio] write offset=0x038 ← 0x80         QueueNum = 128
[virtio-mmio] write offset=0x03c ← 0x1000       QueueAlign = 4096
[virtio-mmio] write offset=0x040 ← 0x4064       QueuePFN
[virtio-mmio] queue 0: desc=0x4064000 avail=0x4064800 used=0x4065000 (num=128)
[virtio-mmio] write offset=0x030 ← 0x1          QueueSel = 1
[virtio-mmio] read  offset=0x040 → 0x0          QueuePFN = 0
[virtio-mmio] read  offset=0x034 → 0x80         QueueNumMax = 128
[virtio-mmio] write offset=0x038 ← 0x80         QueueNum = 128
[virtio-mmio] write offset=0x03c ← 0x1000       QueueAlign = 4096
[virtio-mmio] write offset=0x040 ← 0x406a       QueuePFN
[virtio-mmio] queue 1: desc=0x406a000 avail=0x406a800 used=0x406b000 (num=128)
[virtio-mmio] read  offset=0x070 → 0x3
[virtio-mmio] write offset=0x070 ← 0x7          Status = DRIVER_OK !!!
```

Status = 0x07 = ACKNOWLEDGE (1) | DRIVER (2) | DRIVER_OK (4). Device initialization is complete. The subsequent flood of QueueNotify writes (offset 0x050 ← 0x0) is the virtio-console driver populating the receive queue with 128 empty buffers.

## Key insight

The virtqueue is the core innovation of virtio: **data transfer through shared memory, with notifications only for signaling**. The kernel allocates a region of guest RAM, tells the VMM its location via QueuePFN, and both sides then read/write directly to that memory. No VM exit is needed for the actual data — only the "kick" (QueueNotify) causes a trap. This is fundamentally different from UART where every byte requires a PIO exit.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| vring layout | desc table + avail ring + used ring in contiguous memory |
| Per-queue state | `struct virtqueue_state` with num/align/pfn |
| QueueNumMax | Device tells driver maximum queue capacity |
| QueuePFN | Driver tells device where it placed the vring |
| GPA calculation | PFN × PageSize = physical address of vring |
| DRIVER_OK | Final status indicating device is fully operational |
| QueueNotify | Placeholder for kick mechanism (implemented in Step 15) |

## What changed

From Step 13:
- **virtio_mmio.h**: 6 new register offsets + `VIRTQ_NUM_QUEUES`/`VIRTQ_MAX_SIZE` + `struct virtqueue_state` + `vqs[]` array in device struct
- **virtio_mmio.c read**: `QUEUE_NUM_MAX` returns 128, `QUEUE_PFN` returns stored value, `INTERRUPT_STATUS` returns 0
- **virtio_mmio.c write**: `QUEUE_NUM`, `QUEUE_ALIGN`, `QUEUE_PFN` (with GPA logging), `QUEUE_NOTIFY`, `INTERRUPT_ACK` cases

No changes to `microkvm.c` — the existing MMIO dispatch handles the new registers automatically.

## Next step

[Step 15: virtio-console TX](step15_virtio-tx.md) — when the guest writes to `/dev/hvc0`, walk the transmit queue's descriptor chain and output the data to host stdout. This is where shared memory I/O becomes real.
