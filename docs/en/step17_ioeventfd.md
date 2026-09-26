# Step 17: ioeventfd—Avoid Userspace Exits for TX Kicks

## Goal

Have KVM notify an eventfd for transmitq QueueNotify writes, and move TX processing to a dedicated `txkick_thread`. This removes the path where the vCPU thread receives `KVM_EXIT_MMIO` and processes TX inline.

## Background

### How the path changes from Step 16

Previously, writing QueueNotify returned `KVM_RUN` to userspace, where the vCPU thread processed TX before resuming the guest.

ioeventfd converts matching MMIO/PIO writes into eventfd notifications inside KVM. A separate thread processes TX when notified, so the vCPU thread no longer needs to finish TX handling before calling `KVM_RUN` again.

Hardware VM exits transferring control from guest to KVM still occur. What is removed is **KVM returning `KVM_EXIT_MMIO` to userspace**, not every pause in vCPU execution.

### eventfd basics

Here, `eventfd(0, EFD_CLOEXEC)` creates a notification fd with a counter initialized to zero:

- `write(fd, &val, 8)` → add val to the internal counter
- `read(fd, &val, 8)` → read the counter and reset it to 0 (blocks while it is 0)

Multiple kicks may be combined into one read. For example, even if three notifications arrive as `val=3`, the thread calls the TX function once. Work is determined by the Available Ring range from `last_avail_idx` to the `avail_idx` sampled by that call, not by the notification count.

KVM's ioeventfd integrates this into the MMIO trap path: when the guest writes a matching address and value, KVM calls `eventfd_signal()` internally.

### datamatch

The registration matches a 4-byte write of value `1` to GPA `0xD0000050`. `KVM_IOEVENTFD_FLAG_DATAMATCH` also compares the written value:

- Guest writes QueueNotify = **1** (transmitq) → signal eventfd
- Guest writes QueueNotify = **0** (receiveq) → no eventfd signal; ordinary MMIO exit

Only transmitq kicks are accelerated; receiveq kicks retain the ordinary exit path.

## Execution flow

```text
Guest (Linux)               KVM                         VMM
     | Prepare descriptors and Available Ring            | txkick_thread waits in read()
     |-- QueueNotify=1 ----->|                           |
     |                       | Address, width, value match
     |                       |-- Signal eventfd -------->| read() returns
     |<-- Resume guest ------|                           | virtio_console_tx()
     |                       |                           | Read data from RAM
     |                       |                           | Output to stdout
     |                       |                           | Update Used Ring
```

Guest resumption and TX thread execution proceed according to host scheduling. The TX thread runs in userspace; data processing does not move into KVM.

## Implementation

### Additions to microkvm.c

1. Create an eventfd and register it with KVM:

```c
txkick_fd = eventfd(0, EFD_CLOEXEC);
if (txkick_fd < 0) {
    perror("eventfd");
    return 1;
}

struct kvm_ioeventfd ioeventfd = {
    .addr = VIRTIO_MMIO_BASE + VIRTIO_MMIO_QUEUE_NOTIFY,
    .len = 4,
    .datamatch = 1,     /* transmitq only */
    .fd = txkick_fd,
    .flags = KVM_IOEVENTFD_FLAG_DATAMATCH,
};
if (ioctl(vmfd, KVM_IOEVENTFD, &ioeventfd) < 0) {
    perror("KVM_IOEVENTFD");
    return 1;
}
```

2. A dedicated thread reads the eventfd and processes TX:

```c
static void *txkick_thread(void *arg) {
    (void)arg;
    uint64_t val;
    while (read(txkick_fd, &val, sizeof(val)) == sizeof(val)) {
        virtio_console_tx(&virtio_dev, virtio_dev.ram, virtio_dev.ram_size);
    }
    return NULL;
}
```

Before starting the vCPU thread, `main()` starts `txkick_thread` with `pthread_create()`. Failure to create or register the eventfd terminates the program; there is no fallback to the previous MMIO handling.

### Changes to virtio_mmio.c

- Make `virtio_console_tx` public instead of `static` so txkick_thread can call it
- Remove the direct TX call from the QueueNotify case; matching TX notifications reach eventfd and are handled by `txkick_thread`

### Additions to virtio_mmio.h

```c
void virtio_console_tx(struct virtio_mmio_dev *dev, uint8_t *ram, size_t ram_size);
```

## Output

Write to `/dev/hvc0` in the guest to verify TX:

```
/ # echo hello > /dev/hvc0
hello
```

This output verifies transmission, not exit counts. QueueNotify logs were already suppressed, so their absence alone cannot demonstrate ioeventfd's effect.

## Key insight

ioeventfd separates guest notification from VMM data processing. KVM signals eventfd for matching writes, and a dedicated userspace thread processes shared-memory queues. It reduces userspace round trips for TX kicks, not hardware VM exits overall.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| ioeventfd | KVM intercepts MMIO writes in the kernel and signals eventfd |
| eventfd | Lightweight inter-thread notification (signal with write, wait with read) |
| datamatch | Trigger selectively on a specific written value |
| Asynchronous processing | Separate TX handling from the vCPU thread |
| Fewer userspace exits | Matching TX kicks do not return KVM_EXIT_MMIO |

## What changed

Changes from Step 16:

- **microkvm.c**: `#include <sys/eventfd.h>`, `txkick_fd` + `txkick_thread`, `KVM_IOEVENTFD` registration, and thread creation
- **virtio_mmio.c**: expose `virtio_console_tx`; QueueNotify no longer calls TX directly
- **virtio_mmio.h**: add the `virtio_console_tx()` prototype

## Next step

[Step 18: irqfd](step18_irqfd.md)—Replace `KVM_IRQ_LINE` calls for RX IRQ 5 notification with an eventfd write.
