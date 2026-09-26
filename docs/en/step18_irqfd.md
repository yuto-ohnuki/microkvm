# Step 18: irqfd—Replace IRQ Injection with eventfd

## Goal

Replace the two `KVM_IRQ_LINE` ioctls used to notify IRQ 5 on RX completion with one eventfd write. KVM receives the eventfd notification and injects an interrupt into the guest.

## Background

### How the path changes from Step 17

Previously, every time `stdin_thread` wrote a character to receiveq, it asserted (level=1) and deasserted (level=0) IRQ 5 through `KVM_IRQ_LINE`.

irqfd connects an eventfd to a guest interrupt line. After registration through `KVM_IRQFD` at startup, an eventfd write on RX completion requests interrupt injection from KVM. **write is also a syscall, so IRQ notification drops from two syscalls to one.**

### Difference from ioeventfd

| | ioeventfd (Step 17) | irqfd (Step 18) |
|---|---|---|
| Direction | Guest → VMM (TX kick) | VMM → Guest (RX IRQ) |
| Trigger | Guest QueueNotify write | VMM eventfd write |
| Replaces | KVM_EXIT_MMIO for TX kicks | Two KVM_IRQ_LINE calls for IRQ 5 |
| Notification destination | TX thread | Guest interrupt controller |

This implementation registers with `.flags = 0`, and KVM asserts/deasserts IRQ 5. The VMM need not specify the two levels separately.

## Execution flow

```text
Guest (Linux)               KVM                         VMM (stdin_thread)
     |                       |                           | Receive one host character
     |                       |                           | virtio_console_rx()
     |                       |                           |   Copy into receive buffer
     |                       |                           |   Update Used Ring
     |                       |                           |   interrupt_status |= 1
     |                       |<-- Write eventfd ---------| Notify only on RX success
     |<-- Inject IRQ 5 ------|                           |
     | Read InterruptStatus (MMIO)                       |
     | Write InterruptACK (MMIO)                         |
     | Collect received data from Used Ring              |
     | Deliver to /dev/hvc0                              |
```

irqfd handles IRQ injection. The VMM still updates receive buffers and the Used Ring, and guest InterruptStatus / InterruptACK accesses are still handled in userspace.

## Implementation

### microkvm.c—Create and register an eventfd

Add `irq5_fd` for IRQ 5 notification:

```c
irq5_fd = eventfd(0, EFD_CLOEXEC);
if (irq5_fd < 0) {
    perror("eventfd irq5");
    return 1;
}
```

After creating the interrupt controller with `KVM_CREATE_IRQCHIP`, bind the eventfd to GSI (Global System Interrupt) 5:

```c
struct kvm_irqfd irqfd = {
    .fd = irq5_fd,
    .gsi = 5,   /* IRQ 5 = virtio-mmio interrupt line */
    .flags = 0,
};
if (ioctl(vmfd, KVM_IRQFD, &irqfd) < 0) {
    perror("KVM_IRQFD");
    return 1;
}
```

Registration uses an ioctl; subsequent RX notifications use eventfd.

### microkvm.c—Notify on RX completion

```c
if (virtio_mode) {
    if (virtio_console_rx(&virtio_dev, &c, 1) == 0) {
        /* Signal IRQ5 via irqfd (no ioctl needed) */
        uint64_t val = 1;
        write(irq5_fd, &val, sizeof(val));
    }
} else {
    uart_rx(&uart, c, g_vmfd);
}
```

Notify only if `virtio_console_rx()` successfully updates the receive buffer and Used Ring. If it fails, for example because no buffer is available, do not write to irqfd.

## Output

Start `cat /dev/hvc0 &` in the guest, switch input to virtio, and type `abc` followed by Enter.

```text
/ # cat /dev/hvc0 &
/ # < Ctrl-A v >
[monitor] input → hvc0 (virtio)
abc
abc

< Ctrl-A v >
[monitor] input → ttyS0 (UART)

/ #
```

With hvc0 terminal echo enabled, the first `abc` is echoed through hvc0 TX; the second is `cat` output through UART. This verifies RX and interrupt delivery, not latency improvement. See [benchmark.md](benchmark.md) for ioeventfd / irqfd measurement methods and results.

## Key insight

ioeventfd removes userspace returns for TX kicks; irqfd changes RX-completion IRQ notification from two ioctls to one write. These optimize notification paths; the VMM still processes data. They do not eliminate all VM exits or syscalls.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| irqfd | KVM converts eventfd notifications into guest IRQs |
| GSI (Global System Interrupt) | `.gsi = 5` binds eventfd to IRQ 5 |
| Fewer IRQ notification syscalls | Replace assert/deassert ioctls with one write |
| Split data handling and notification | VMM publishes received data; KVM delivers the interrupt |

## What changed

Changes from Step 17 are in **microkvm.c**:

- Add `irq5_fd`, create eventfd, and register `KVM_IRQFD`
- Change IRQ 5 notification in `stdin_thread` from two `KVM_IRQ_LINE` calls to one `write(irq5_fd)`
- Close `irq5_fd` during cleanup

## Next step

[Step 19: KVM MMU Stats](step19_mmu-stats.md)—Read KVM statistics and observe the MMU managing guest memory.
