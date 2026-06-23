# Step 17: ioeventfd — TX kick exit elimination

## Goal

Eliminate the VM exit caused by QueueNotify writes for the transmit queue. KVM handles the notification entirely in kernel space via eventfd, so the vCPU never stops for TX kicks.

## Background

### The problem with Step 15-16

In Steps 15-16, every time the guest writes to QueueNotify (to kick the transmitq), it causes:
```
Guest writes QueueNotify
  → EPT violation
  → VM exit (KVM_EXIT_MMIO)
  → vCPU stops
  → VMM processes TX
  → KVM_RUN to resume
  → vCPU restarts
```

For each `echo hello > /dev/hvc0`, the vCPU is completely halted during TX processing. This is unnecessary — the data is already in shared memory, so why should the vCPU wait?

### What is ioeventfd?

ioeventfd is a KVM feature that intercepts specific MMIO/PIO writes **inside the kernel** and converts them to an eventfd signal — without ever exiting to userspace:

```
Before (Step 15):                        After (Step 17):
Guest writes QueueNotify=1               Guest writes QueueNotify=1
  → EPT violation                          → EPT violation
  → VM exit to userspace                   → KVM checks ioeventfd list
  → vCPU stopped                           → addr + value match!
  → VMM handles TX                         → eventfd_signal() in kernel
  → KVM_RUN                                → vCPU resumes immediately
  → vCPU resumes                           → txkick_thread wakes and handles TX
```

The vCPU goes from "stopped for ~5μs" to "never stops."

### eventfd basics

An eventfd is a lightweight Linux notification mechanism:
- `write(fd, &val, 8)` → increments internal counter (signals)
- `read(fd, &val, 8)` → returns counter and resets to 0 (blocks until signaled)

Multiple kicks may be coalesced into a single wakeup because eventfd stores a counter rather than individual events. If the guest kicks 3 times before `txkick_thread` wakes, the thread sees `val=3` but only runs TX processing once — processing all pending buffers in one pass.

KVM's ioeventfd hooks this into the MMIO trap path: when the guest writes a matching address+value, KVM calls `eventfd_signal()` internally.

### datamatch

`KVM_IOEVENTFD_FLAG_DATAMATCH` with `.datamatch = 1` means:
- Guest writes QueueNotify = **1** (transmitq) → eventfd fires
- Guest writes QueueNotify = **0** (receiveq) → eventfd does NOT fire, normal MMIO exit occurs

This lets us selectively accelerate only the transmitq kick while leaving receiveq kicks as normal exits.

## Execution flow

```
Guest                          KVM (kernel)              txkick_thread
─────                          ────────────              ─────────────
echo hello > /dev/hvc0
  ↓
virtio-console driver:
  avail ring + descriptor ready
  writel(1, 0xD0000050)
    ↓ EPT violation
                               check ioeventfd list
                               addr=0xD0000050, val=1 → match!
                               eventfd_signal(txkick_fd)
                               vCPU resumes immediately
                               (no exit to userspace)
                                                        read(txkick_fd) returns
                                                        virtio_console_tx()
                                                        → "hello" on stdout
```

## Implementation

### microkvm.c additions

1. Create eventfd and register with KVM:
```c
txkick_fd = eventfd(0, EFD_CLOEXEC);

struct kvm_ioeventfd ioeventfd = {
    .addr = VIRTIO_MMIO_BASE + VIRTIO_MMIO_QUEUE_NOTIFY,
    .len = 4,
    .datamatch = 1,     /* transmitq only */
    .fd = txkick_fd,
    .flags = KVM_IOEVENTFD_FLAG_DATAMATCH,
};
ioctl(vmfd, KVM_IOEVENTFD, &ioeventfd);
```

2. Dedicated thread reads the eventfd and processes TX:
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

### virtio_mmio.c changes

- `virtio_console_tx` changes from `static` to public (called from txkick_thread)
- QueueNotify case removes direct TX call (ioeventfd handles it in kernel)

### virtio_mmio.h addition

```c
void virtio_console_tx(struct virtio_mmio_dev *dev, uint8_t *ram, size_t ram_size);
```

## Output

```
/ # echo hello > /dev/hvc0
hello
```

Same visible output as Step 15 — but internally, no VM exit occurs for the TX kick. The vCPU continues executing guest code while `txkick_thread` processes the data asynchronously.

## Key insight

ioeventfd decouples **notification** from **processing**. The guest's kick becomes a fire-and-forget signal that never stops the vCPU. Processing happens asynchronously in a separate thread. This is the same architecture used by:
- **QEMU**: virtio kick acceleration
- **vhost-net**: kernel-side networking with ioeventfd

The progression: per-byte exit (UART) → per-batch exit (virtio Step 15) → **zero exit** (ioeventfd Step 17).

In production, the same pattern powers vhost-net:
```
Guest kick → ioeventfd → vhost kernel thread → NIC hardware
```
microkvm's `txkick_thread` is the userspace equivalent of vhost's kernel thread.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| ioeventfd | KVM intercepts MMIO write in kernel, signals eventfd |
| eventfd | Lightweight inter-thread notification (write to signal, read to wait) |
| datamatch | Selectively trigger on specific write values |
| Async processing | TX handled in separate thread, vCPU never waits |
| Exit elimination | QueueNotify no longer causes KVM_EXIT_MMIO for transmitq |
| vhost pattern | Same architecture as production virtio acceleration |

## What changed

From Step 16:
- **microkvm.c**: `#include <sys/eventfd.h>`, `txkick_fd` + `txkick_thread`, `KVM_IOEVENTFD` registration, thread creation
- **virtio_mmio.c**: `virtio_console_tx` made public, QueueNotify case no longer calls TX directly
- **virtio_mmio.h**: `virtio_console_tx()` prototype added

## Next step

[Step 18: irqfd](step18_irqfd.md) — eliminate the ioctl overhead for IRQ injection. Writing to an eventfd injects IRQ 5 directly in kernel space.
