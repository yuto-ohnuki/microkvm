# Step 15: virtio-console TX (guest → host)

## Goal

Follow descriptors in transmitq (queue 1) to read data the guest writes to `/dev/hvc0`, then send it to host standard output. After processing, update the Used Ring to return buffers to the guest.

## Background

### How virtio TX works

The guest driver places data in guest memory, describes its location in a descriptor, adds the descriptor index to the available ring, and kicks through QueueNotify. The VMM then reads the data directly from guest RAM.

```
Guest writes "hello" to /dev/hvc0:

  avail ring               descriptor table           guest memory
  +----------+             +------------------+       +---------+
  | idx: 1   |             | [5] addr=0x1234  |  ──→  | "hello" |
  | ring[0]=5| ──────────→ |     len=5        |       +---------+
  +----------+             |     flags=0      |
                           +------------------+
```

The MMIO write to QueueNotify causes `KVM_EXIT_MMIO`. The notification contains no string; the VMM reads "hello" directly from `ram + 0x1234`, pointed to by the descriptor. QueueNotify offset `0x050` follows the MMIO register definition in the [virtio 1.2 specification](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html); the [Linux definition](https://github.com/torvalds/linux/blob/master/include/uapi/linux/virtio_mmio.h) uses the same value.

### Descriptor chain

One I/O operation may span multiple buffers linked through `flags & VRING_DESC_F_NEXT`. The VMM follows `next` fields until a descriptor has no NEXT flag:

```
descriptor[5]          descriptor[7]          descriptor[2]
addr=0x1000            addr=0x2000            addr=0x3000
len=100                len=200                len=50
flags=NEXT             flags=NEXT             flags=0
next=7         ──→     next=2         ──→     (end of chain)
```

The first descriptor in a chain (the **head descriptor**, index 5 here) goes into `avail->ring[]`. The used ring also receives this head index, not an intermediate descriptor.

### TX vs. RX descriptor flags

| Direction | Flag | Meaning |
|-----------|------|---------|
| TX (guest → host) | No WRITE bit | Device reads the buffer; NEXT may be set for a chain |
| RX (host → guest) | WRITE bit set | Device writes to the buffer |

## Execution flow

```text
Guest (Linux)               KVM                         VMM
     | Write to /dev/hvc0    |                           |
     | Set up buffers and descriptors                    |
     | Add head to Available Ring and advance idx        |
     |-- QueueNotify=1 ----->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | virtio_console_tx()
     |                       |                           | Get unprocessed head
     |                       |                           | Follow descriptor chain
     |                       |                           | Output buffers to stdout
     |                       |                           | Return head in Used Ring
     |                       |                           | Advance used->idx
     |                       |                           | Advance last_avail_idx
     |                       |<-- KVM_RUN ---------------|
     |<-- Resume ------------|                           |
```

This TX handling updates the Used Ring but does not inject a completion interrupt.

## Implementation

### Additions to virtio_mmio.h

```c
#define VRING_DESC_F_NEXT   1   /* Descriptor is chained */
#define VRING_DESC_F_WRITE  2   /* Device writes (RX) */

struct virtqueue_state {
    ...
    uint16_t last_avail_idx;    /* VMM's shadow avail index */
};

struct virtio_mmio_dev {
    ...
    uint8_t *ram;       /* Host address of allocated guest RAM */
    size_t  ram_size;   /* Guest RAM size for bounds checks */
};

/* vring structures */
struct vring_desc {
    uint64_t addr;      /* Buffer GPA */
    uint32_t len;       /* Buffer length */
    uint16_t flags;     /* NEXT, WRITE */
    uint16_t next;      /* Next descriptor (when NEXT is set) */
};

struct vring_used_elem {
    uint32_t id;        /* descriptor head index */
    uint32_t len;       /* Bytes written by the device */
};
```

### virtio_mmio.c—Core TX handling

Call TX handling when the queue number written to QueueNotify is `1`. The notification value, not QueueSel, determines this.

```c
case VIRTIO_MMIO_QUEUE_NOTIFY:
    if (value == 1) {   /* transmitq */
        virtio_console_tx(dev, dev->ram, dev->ram_size);
    }
    break;
```

`virtio_console_tx()` uses helper functions to calculate the Step 14 layout. The following excerpts show the subsequent processing.

```c
/* Read avail->idx (guest increments this after adding buffers) */
uint16_t avail_idx;
memcpy(&avail_idx, ram + avail_base + 2, sizeof(uint16_t));

while (vq->last_avail_idx != avail_idx) {
    /* Get descriptor head index from avail ring */
    uint16_t ring_slot = vq->last_avail_idx % vq->num;
    uint16_t desc_idx;
    memcpy(&desc_idx, ram + avail_base + 4 + ring_slot * 2, sizeof(uint16_t));
    /* Process the chain and update the Used Ring below */
}
```

`last_avail_idx` is a 16-bit counter tracking how far the VMM has processed. For example, `last_avail_idx=1` and `avail_idx=3` mean Available Ring positions 1 and 2 are pending, not necessarily descriptor indices 1 and 2. Actual head indices are read from the ring entries.

Follow the chain from the retrieved head and output buffers without the WRITE bit:

```c
struct vring_desc desc;
memcpy(&desc, ram + desc_base + cur * 16, sizeof(desc));

/* TX: device reads from buffer (flags should NOT have WRITE) */
if (!(desc.flags & VRING_DESC_F_WRITE)) {
    if (desc.addr < ram_size && desc.len <= ram_size - desc.addr) {
        write(STDOUT_FILENO, ram + desc.addr, desc.len);
    }
}

if (desc.flags & VRING_DESC_F_NEXT)
    cur = desc.next;
else
    break;
```

For each chain, return the first descriptor index in the Used Ring:

```c
/* Post to used ring */
uint16_t used_idx;
memcpy(&used_idx, ram + used_base + 2, sizeof(uint16_t));
uint16_t used_slot = used_idx % vq->num;

struct vring_used_elem elem = { .id = desc_idx, .len = 0 };
memcpy(ram + used_base + 4 + used_slot * 8, &elem, sizeof(elem));

used_idx++;
memcpy(ram + used_base + 2, &used_idx, sizeof(uint16_t));

vq->last_avail_idx++;
```

`len=0` because TX does not write into the guest's data buffers. This field does not report how many bytes were output to the host.

Bounds checks cover ring starts, descriptor indices, and data-buffer ranges. They do not validate entire ring boundaries or detect cycles in chains.

### Additions to microkvm.c

```c
/* Pointer for direct guest-memory access by the virtio device */
virtio_dev.ram = (uint8_t *)mem;
virtio_dev.ram_size = GUEST_MEM_SIZE;
```

Pass the VMM's mmap'ed guest RAM pointer to the virtio device. `virtio_console_tx()` uses it to read guest buffers directly.

## Output

Boot logs confirm both queue configurations and the transition to DRIVER_OK:

```text
[virtio-mmio] queue 0: desc=0x494000 avail=0x494800 used=0x495000 (num=128)
...
[virtio-mmio] queue 1: desc=0x496000 avail=0x496800 used=0x497000 (num=128)
[virtio-mmio] read  offset=0x070 → 0x3
[virtio-mmio] write offset=0x070 ← 0x7
[virtio-mmio] read  offset=0x070 → 0x7
```

To test transmission, start `./microkvm` without piping it through `grep`, then run the command in the guest shell. `grep virtio` would hide the `hello` output you want to check.

| Guest command | Output path | What it verifies |
|------------------------|----------|----------------|
| `echo hello` | ttyS0 → UART → PIO | UART output |
| `echo hello > /dev/hvc0` | transmitq → QueueNotify → VMM reads RAM | virtio transmission |

Expected transmission output:

```text
/ # echo hello > /dev/hvc0
hello
```

QueueNotify logs are suppressed regardless of queue number, so neither boot-time receiveq notifications nor transmitq notifications appear. Missing logs do not mean notifications or MMIO exits have disappeared.

## Key insight

UART transfers characters through PIO; virtio notifies the VMM of shared-memory buffers that it can read in batches. However, one command does not necessarily produce one QueueNotify: the count depends on how the driver splits and notifies data.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| Shared-memory I/O | VMM reads guest buffers directly at `ram + desc.addr`, bypassing kvm_run |
| Descriptor chain walk | Follow `next` until the NEXT flag is clear |
| Available ring | Guest submits head indices and advances idx to publish work |
| Used ring | VMM returns head indices and advances used->idx |
| last_avail_idx | VMM shadow counter tracking progress |
| Bounds checking | Check ring starts, descriptor indices, and data-buffer ranges |
| QueueNotify | Value 1 processes pending chains up to the sampled avail_idx |

## What changed

Changes from Step 14:

- **virtio_mmio.h**: per-queue `last_avail_idx`, device `ram`/`ram_size`, vring structure definitions, and `VRING_DESC_F_*` flags
- **virtio_mmio.c**: `virtio_console_tx()`, vring address helpers, TX call from QueueNotify handling, and suppression of QueueNotify logs
- **microkvm.c**: `virtio_dev.ram = mem` + `virtio_dev.ram_size = GUEST_MEM_SIZE`

## Next step

[Step 16: virtio-console RX](step16_virtio-rx.md)—Deliver host stdin to the guest through the receive queue and IRQ injection, reversing Step 15's direction.
