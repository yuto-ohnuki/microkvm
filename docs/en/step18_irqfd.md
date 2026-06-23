# Step 18: irqfd — IRQ injection exit elimination

## Goal

Replace `ioctl(KVM_IRQ_LINE)` with an eventfd write for IRQ 5 injection. KVM handles the interrupt delivery entirely in kernel space — no syscall needed from the VMM.

## Background

### The problem remaining after Step 17

Step 17 eliminated the TX kick exit. But RX still has overhead:

```
Step 16-17 RX path:
  stdin_thread receives 'a'
  → virtio_console_rx() writes to guest buffer
  → ioctl(KVM_IRQ_LINE, irq=5, level=1)    ← syscall #1
  → ioctl(KVM_IRQ_LINE, irq=5, level=0)    ← syscall #2
```

Each RX character requires 2 syscalls for edge-triggered IRQ injection. For high-throughput input, this ioctl overhead becomes significant.

### What is irqfd?

irqfd is the IRQ-direction counterpart of ioeventfd:

| | ioeventfd (Step 17) | irqfd (Step 18) |
|---|---|---|
| Direction | Guest → Host (kick) | Host → Guest (IRQ) |
| Trigger | Guest MMIO write | VMM eventfd write |
| What it replaces | KVM_EXIT_MMIO | ioctl(KVM_IRQ_LINE) |
| Effect | vCPU doesn't stop | VMM doesn't syscall |

With irqfd, writing to an eventfd causes KVM to inject the interrupt directly in kernel space — edge-triggered, no level management needed.

### Before vs After

```
Step 16 (ioctl):
  stdin_thread:
    virtio_console_rx()
    ioctl(KVM_IRQ_LINE, level=1)    ← user→kernel→user
    ioctl(KVM_IRQ_LINE, level=0)    ← user→kernel→user
  Total: 2 syscalls per character

Step 18 (irqfd):
  stdin_thread:
    virtio_console_rx()
    write(irq5_fd, &1, 8)           ← 1 syscall, KVM auto-injects
  Total: 1 syscall per character, auto edge-trigger
```

## Execution flow

```
Host (stdin_thread)              KVM (kernel)              Guest
───────────────────              ────────────              ─────
virtio_console_rx() completes
write(irq5_fd, &1, 8)
                                 eventfd signaled
                                 → KVM injects IRQ 5
                                   (auto edge-trigger:
                                    assert + deassert)
                                                          IRQ handler fires
                                                          reads InterruptStatus
                                                          writes InterruptACK
                                                          processes used ring
                                                          delivers to /dev/hvc0
```

No ioctl. No manual level=1/level=0. KVM handles the full edge-trigger sequence internally.

> How does KVM "see" the eventfd? KVM registers a poll handler on the eventfd at `KVM_IRQFD` time. When the counter becomes non-zero (i.e., someone writes to it), the IRQ is injected from kernel context — no userspace involvement.

**Note:** irqfd only replaces the *notification path*. The guest still validates InterruptStatus and acknowledges the interrupt through the normal virtio protocol (Step 16). The guest-side behavior is unchanged.

## Implementation

### microkvm.c changes

1. Create eventfd and register as irqfd:
```c
irq5_fd = eventfd(0, EFD_CLOEXEC);

struct kvm_irqfd irqfd = {
    .fd = irq5_fd,
    .gsi = 5,       /* IRQ 5 = virtio-mmio interrupt line */
    .flags = 0,     /* edge-triggered by default */
};
ioctl(vmfd, KVM_IRQFD, &irqfd);
```

2. Replace ioctl calls in stdin_thread:
```c
/* Before (Step 16): 2 ioctls for edge trigger */
struct kvm_irq_level irq = { .irq = 5, .level = 1 };
ioctl(g_vmfd, KVM_IRQ_LINE, &irq);
irq.level = 0;
ioctl(g_vmfd, KVM_IRQ_LINE, &irq);

/* After (Step 18): single write, KVM handles edge trigger */
uint64_t val = 1;
write(irq5_fd, &val, sizeof(val));
```

### Dependency

`KVM_IRQFD` must be called after `KVM_CREATE_IRQCHIP`. The irqchip must exist before an IRQ line can be wired to an eventfd.

## Output

```
/ # cat /dev/hvc0 &
/ #
[monitor] input → hvc0 (virtio)
abc
abc
```

Same behavior as Step 16 — but IRQ injection no longer requires any ioctl. The improvement is invisible in output but measurable in latency. See [benchmark.md](benchmark.md) for measured results comparing ioctl vs irqfd.

## Key insight

With ioeventfd (Step 17) + irqfd (Step 18), the entire virtio notification path is kernel-resident:

```
TX notification:  Guest MMIO write → ioeventfd → kernel signals → thread processes
RX notification:  VMM eventfd write → irqfd → kernel injects IRQ → guest handles
```

Neither the TX kick nor the RX IRQ requires a full VM exit or ioctl round-trip. The vCPU runs uninterrupted, and the VMM avoids unnecessary syscalls. Notification handling moves entirely out of the vCPU path — the same principle that production hypervisors like vhost-net achieve with kernel threads and hardware offload.

### The complete optimization journey

| Step | Mechanism | Exits/syscalls per operation |
|------|-----------|------------------------------|
| 11 | UART PIO | 1 VM exit per byte |
| 15 | virtio QueueNotify | 1 VM exit per batch |
| 17 | ioeventfd | **0 VM exits** (kernel eventfd) |
| 16 | ioctl KVM_IRQ_LINE | 2 syscalls per IRQ |
| 18 | irqfd | **1 write** (kernel auto-injects) |

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| irqfd | eventfd write → KVM kernel injects IRQ (no ioctl) |
| Symmetry with ioeventfd | ioeventfd = guest→host acceleration, irqfd = host→guest acceleration |
| Edge-trigger automation | KVM handles assert+deassert internally |
| GSI (Global System Interrupt) | `.gsi = 5` maps eventfd to IRQ line 5 |
| Notification path optimization | Both directions now kernel-resident |

## What changed

From Step 17:
- **microkvm.c only**: `irq5_fd` eventfd creation, `KVM_IRQFD` registration, stdin_thread replaces `ioctl(KVM_IRQ_LINE)` × 2 with `write(irq5_fd)` × 1

No changes to `virtio_mmio.c` or `virtio_mmio.h`.

## Next step

Phase C is complete. The virtio I/O path is fully functional with optimized notifications.

[Step 19: KVM MMU stats](../step19_mmu-stats.md) begins Phase D (Memory State Management) — observing EPT internals, dirty page tracking, snapshots, and live migration.
