# Step 15: virtio-console TX (guest → host)

## Goal

When the guest writes to `/dev/hvc0`, walk the transmit queue's descriptor chain and output the data to host stdout. This is where shared memory I/O becomes real — data flows through the vring without per-byte VM exits.

## Background

### How TX works in virtio

The guest driver places data in guest memory, describes its location in a descriptor, adds the descriptor index to the available ring, and "kicks" the device via QueueNotify. The VMM then reads the data directly from guest RAM.

```
Guest writes "hello" to /dev/hvc0:

  avail ring               descriptor table           guest memory
  +----------+             +------------------+       +---------+
  | idx: 1   |             | [5] addr=0x1234  |  ──→  | "hello" |
  | ring[0]=5| ──────────→ |     len=5        |       +---------+
  +----------+             |     flags=0      |
                           +------------------+
```

The key insight: **the data itself never passes through a VM exit**. Only the kick (QueueNotify MMIO write) causes a trap. The VMM reads "hello" directly from `guest_ram + 0x1234`.

### Descriptor chain

A single I/O operation may span multiple buffers linked via `flags & VRING_DESC_F_NEXT`. The VMM must follow the `next` field until a descriptor without NEXT is found:

```
descriptor[5]          descriptor[7]          descriptor[2]
addr=0x1000            addr=0x2000            addr=0x3000
len=100                len=200                len=50
flags=NEXT             flags=NEXT             flags=0
next=7         ──→     next=2         ──→     (end of chain)
```

The first descriptor in the chain (the **head descriptor**, index 5 in this example) is what appears in `avail->ring[]`. When posting to the used ring, the VMM returns this head index — not the intermediate ones.

### TX vs RX descriptor flags

| Direction | Flag | Meaning |
|-----------|------|---------|
| TX (guest → host) | flags = 0 | Device reads from buffer |
| RX (host → guest) | flags = VRING_DESC_F_WRITE | Device writes into buffer |

## Execution flow

```
Guest                                    VMM (microkvm)
─────                                    ──────────────
echo hello > /dev/hvc0
  ↓
virtio-console driver:
  writes "hello\n" to guest buffer
  fills descriptor: addr=X, len=6
  adds desc index to avail ring
  bumps avail->idx
  writes 1 to QueueNotify (0xD0000050)
    ↓ EPT violation → KVM_EXIT_MMIO
                                         virtio_mmio_write():
                                           QueueNotify value=1 → transmitq
                                           → virtio_console_tx()

                                         virtio_console_tx():
                                           read avail->idx
                                           while (last_avail_idx != avail_idx):
                                             desc_idx = avail->ring[slot]
                                             desc = descriptor_table[desc_idx]
                                             write(stdout, ram + desc.addr, desc.len)
                                             post to used ring
                                             last_avail_idx++

                                         "hello\n" appears on host terminal
```

## Implementation

### virtio_mmio.h additions

```c
#define VRING_DESC_F_NEXT   1   /* descriptor is chained */
#define VRING_DESC_F_WRITE  2   /* device writes (RX side) */

struct virtqueue_state {
    ...
    uint16_t last_avail_idx;    /* VMM's shadow of avail idx */
};

struct virtio_mmio_dev {
    ...
    uint8_t *ram;       /* pointer to guest physical memory */
    size_t  ram_size;   /* guest RAM size for bounds checking */
};

/* vring structures */
struct vring_desc {
    uint64_t addr;      /* GPA of buffer */
    uint32_t len;       /* buffer length */
    uint16_t flags;     /* NEXT, WRITE */
    uint16_t next;      /* next descriptor (if NEXT flag set) */
};

struct vring_used_elem {
    uint32_t id;        /* descriptor head index */
    uint32_t len;       /* bytes written by device */
};
```

### virtio_mmio.c — core TX function

```c
static void virtio_console_tx(struct virtio_mmio_dev *dev,
    uint8_t *ram, size_t ram_size)
{
    int qidx = 1;  /* transmitq */
    struct virtqueue_state *vq = &dev->vqs[qidx];

    /* Read avail->idx */
    uint16_t avail_idx;
    memcpy(&avail_idx, ram + avail_base + 2, sizeof(uint16_t));

    /* Process all buffers between last_avail_idx and avail_idx:
     *   avail_idx = 3, last_avail_idx = 1 → descriptors #1, #2 are pending */
    while (vq->last_avail_idx != avail_idx) {
        /* Get descriptor head from avail ring */
        uint16_t desc_idx = avail->ring[last_avail_idx % num];

        /* Walk descriptor chain, output each buffer */
        while (descriptor is valid) {
            if (!(desc.flags & VRING_DESC_F_WRITE))
                write(STDOUT_FILENO, ram + desc.addr, desc.len);
            if (desc.flags & VRING_DESC_F_NEXT)
                follow next;
            else break;
        }

        /* Post to used ring */
        used->ring[used_idx % num] = { .id = desc_idx, .len = 0 };
        used_idx++;  /* device must publish updated used->idx so guest knows */
        last_avail_idx++;
    }
}
```

**Why `len = 0`?** For TX, the console driver does not check the returned length — the data was already consumed by the host's `write()`. RX (Step 16) will return the actual byte count so the guest knows how much data was written into its buffer.

### microkvm.c addition

```c
/* Give virtio device access to guest memory */
virtio_dev.ram = (uint8_t *)mem;
virtio_dev.ram_size = GUEST_MEM_SIZE;
```

The VMM passes its mmap'd guest RAM pointer to the virtio device so `virtio_console_tx()` can read guest buffers directly.

## Output

```
/ # echo hello > /dev/hvc0
hello
```

The `hello` appears on the host terminal — written directly from guest RAM via the vring, with only a single QueueNotify VM exit for the entire string.

> **Note:** QueueNotify logs are suppressed in v2 (they are noisy during boot). With logging enabled, `[virtio-mmio] write offset=0x050 ← 0x1` would appear before the output.

## Key insight

This is the moment where virtio's performance advantage becomes concrete. Compare with UART:

| | UART (Step 11) | virtio TX (Step 15) |
|---|---|---|
| "hello" (5 chars) | 5 PIO exits + 5 IRQs | **1 MMIO exit** (QueueNotify) |
| Data path | kvm_run → VMM → putchar per byte | VMM reads directly from guest RAM |
| Latency per byte | ~5 μs (exit + entry) | ~0 (no exit for data) |

The exit count drops from O(n) to O(1) for n bytes. This is why every modern hypervisor uses virtio for I/O.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| Shared memory I/O | VMM reads guest buffer via `ram + desc.addr` — no copying through kvm_run |
| Descriptor chain walk | Follow `next` field until no NEXT flag |
| Available ring | Guest signals "new work" by incrementing avail->idx |
| Used ring | VMM signals "work done" by posting descriptor ID |
| last_avail_idx | VMM's shadow counter — tracks how far it has processed |
| Bounds checking | All GPA accesses validated against ram_size |
| QueueNotify | Single MMIO exit triggers processing of all pending buffers |

## What changed

From Step 14:
- **virtio_mmio.h**: `last_avail_idx` in per-queue state, `ram`/`ram_size` in device struct, vring structure definitions, `VRING_DESC_F_*` flags
- **virtio_mmio.c**: `virtio_console_tx()` function + vring address helpers, QueueNotify handler calls TX, log suppression for QueueNotify
- **microkvm.c**: `virtio_dev.ram = mem` + `virtio_dev.ram_size = GUEST_MEM_SIZE`

## Next step

[Step 16: virtio-console RX](step16_virtio-rx.md) — deliver host stdin to the guest via the receive queue and IRQ injection. The reverse direction of Step 15.
