# Step 16: virtio-console RX (host → guest)

## Goal

Write host terminal input to receiveq (queue 0) buffers and notify Linux with IRQ 5. Add a monitor operation to switch the input destination so characters can be read from `/dev/hvc0`.

## Background

### RX reverses TX

In Step 15 (TX), the guest placed data in buffers and kicked the VMM. RX reverses the roles:

- The **guest** supplies empty buffers (the virtio-console driver allocates receive buffers during initialization and submits them to receiveq—the 128 QueueNotify kicks at the end of Step 14)
- The **VMM** writes data into those buffers
- The **VMM** notifies the guest through an IRQ that data has arrived (IRQ 5, the interrupt line declared in the kernel command line during device setup)

### Why RX needs an IRQ

Host input arrives independently of guest execution. After updating the Used Ring, this implementation injects IRQ 5 to prompt Linux's virtio-mmio driver to check completed buffers. Shared memory carries the data; the interrupt announces its arrival.

### RX flow overview

```
Host stdin → stdin_thread → virtio_console_rx():
  1. Find an empty buffer in receiveq (VRING_DESC_F_WRITE flag)
  2. Write data into the guest buffer (memcpy into guest RAM)
  3. Record the descriptor in the used ring (with actual byte count)
  4. Set interrupt_status |= 0x1

stdin_thread (if virtio_console_rx() succeeds):
  5. Inject IRQ 5 through KVM_IRQ_LINE

Guest IRQ handler:
  6. Read InterruptStatus → 0x1 (used buffer notification)
  7. Write 0x1 to InterruptACK (clear pending state)
  8. Check used ring → find completed descriptor
  9. Read data from buffer → deliver to /dev/hvc0
```

### Switch modes with Ctrl-A v

microkvm shares stdin between UART (ttyS0) and virtio (hvc0). **Press and release Ctrl-A, then press `v`** to switch the input destination.

- Default: UART mode → `uart_rx()` delivers to ttyS0 (shell)
- After `Ctrl-A v`: virtio mode → `virtio_console_rx()` delivers to hvc0

Repeat to return to UART. The VMM consumes the switching keystrokes instead of passing them to the guest. When returning to UART, it sends one newline to redisplay the shell prompt.

### InterruptStatus / InterruptACK

| Register | Direction | Role |
|----------|-----------|---------|
| InterruptStatus (0x060) | Read | VMM reports pending interrupts to guest (bit 0 = used buffer) |
| InterruptACK (0x064) | Write | Guest clears handled interrupts |

Linux's IRQ handler reads InterruptStatus, then writes InterruptACK to clear the specified bits. This updates pending state inside the device, separately from raising/lowering the IRQ line through `KVM_IRQ_LINE`. InterruptStatus reads and InterruptACK writes are not logged.

## Execution flow

```text
Guest (Linux)               KVM                         VMM
     | Submit empty buffers to receiveq                  |
     |                       |                           | Type in virtio mode
     |                       |                           | virtio_console_rx()
     |                       |                           | Copy into WRITE buffer
     |                       |                           | Update Used Ring
     |                       |                           | interrupt_status |= 1
     |                       |<-- KVM_IRQ_LINE (5, 1→0) --| stdin_thread
     |<-- Deliver IRQ 5 -----|                           |
     |-- Read InterruptStatus>|-- KVM_EXIT_MMIO ---------->|
     |                       |                           | Return pending bits
     |                       |<-- KVM_RUN ---------------|
     |-- Write InterruptACK ->|-- KVM_EXIT_MMIO ---------->|
     |                       |                           | Clear specified bits
     |                       |<-- KVM_RUN ---------------|
     | Check Used Ring and receive buffer                |
     | Deliver to /dev/hvc0  |                           |
```

The IRQ does not carry input data. Linux uses Used Ring `id` and `len` to determine which buffer contains data and how many bytes arrived.

## Implementation

### Additions to virtio_mmio.h

```c
uint32_t interrupt_status;  /* Pending interrupt bits */

/* RX: update buffer and Used Ring, then set the interrupt-pending bit */
int virtio_console_rx(struct virtio_mmio_dev *dev, const uint8_t *data, size_t len);
```

### virtio_mmio.c — virtio_console_rx()

Get the next descriptor from receiveq's Available Ring. This excerpt covers descriptor validation through recording receive completion:

```c
/* RX descriptors must have WRITE flag (device writes into guest buffer) */
if (!(desc.flags & VRING_DESC_F_WRITE))
    return -1;

/* Overflow-safe bounds check */
size_t copy_len = len < desc.len ? len : desc.len;
if (desc.addr >= ram_size || copy_len > ram_size - desc.addr)
    return -1;

memcpy(ram + desc.addr, data, copy_len);

/* Post to used ring with actual bytes written */
uint16_t used_idx;
memcpy(&used_idx, ram + used_base + 2, sizeof(uint16_t));
uint16_t used_slot = used_idx % vq->num;

struct vring_used_elem elem = {
    .id = desc_idx,
    .len = (uint32_t)copy_len
};
memcpy(ram + used_base + 4 + used_slot * 8, &elem, sizeof(elem));

used_idx++;
memcpy(ram + used_base + 2, &used_idx, sizeof(uint16_t));

vq->last_avail_idx++;

/* Set interrupt pending (bit 0 = used buffer notification) */
dev->interrupt_status |= 0x1;

return 0;
```

Unlike TX, `used_elem.len` reports the bytes actually written into the guest buffer. RX uses only the first descriptor and does not follow a chain.

The stdin thread passes one character at a time. If no buffer is available or another error occurs, the function returns `-1` and discards the character without retrying. The caller injects an IRQ on success.

### virtio_mmio.c — InterruptStatus / InterruptACK

```c
case VIRTIO_MMIO_INTERRUPT_STATUS:
    val = dev->interrupt_status;        /* Guest reads to identify interrupt source */
    break;

case VIRTIO_MMIO_INTERRUPT_ACK:
    dev->interrupt_status &= ~value;    /* Guest clears handled bits */
    break;
```

### microkvm.c—Changes to stdin_thread

```c
if (virtio_mode) {
    if (virtio_console_rx(&virtio_dev, &c, 1) == 0) {
        /* Notify guest: data available in receiveq */
        struct kvm_irq_level irq = { .irq = 5, .level = 1 };
        ioctl(g_vmfd, KVM_IRQ_LINE, &irq);
        irq.level = 0;
        ioctl(g_vmfd, KVM_IRQ_LINE, &irq);
    }
} else {
    uart_rx(&uart, c, g_vmfd);
}
```

IRQ injection uses an edge (0→1→0), the same pattern as UART IRQ 4 in Step 11.

## Output

Start `cat /dev/hvc0 &` in the guest, switch input to virtio, and type `abc` followed by Enter. Finally, switch back to UART.

```
/ # cat /dev/hvc0 &
/ # < Ctrl-A v >
[monitor] input → hvc0 (virtio)
abc
abc

< Ctrl-A v >
[monitor] input → ttyS0 (UART)

/ #
```

With hvc0 terminal echo enabled, the first `abc` is echoed through hvc0 TX; the second is written by `cat` to ttyS0 (UART). `cat` retains standard output inherited from the shell. The monitor operation switches only the destination of host input.

## Key insight

For RX, the VMM writes into guest-provided buffers, publishes completion in the Used Ring, then signals an interrupt. Shared memory carries data; the IRQ prompts the guest to check it. InterruptACK clears the notification's pending bit.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| Shared-memory RX | VMM writes directly into guest-provided buffers |
| VRING_DESC_F_WRITE | Flag granting the device permission to write here |
| IRQ injection | KVM_IRQ_LINE invokes the guest interrupt handler |
| InterruptStatus/ACK | Guest identifies and clears the interrupt source |
| Edge-triggered IRQ | assert (level=1) → deassert (level=0) |
| Ctrl-A v mode switch | Share one stdin between UART and virtio |
| Bidirectional virtio | transmitq handles sending; receiveq handles receiving |

## What changed

Changes from Step 15:

- **virtio_mmio.h**: `interrupt_status` field and `virtio_console_rx()` prototype
- **virtio_mmio.c**: implement `virtio_console_rx()`, return live `INTERRUPT_STATUS`, clear bits through `INTERRUPT_ACK`, and suppress InterruptStatus/ACK logs
- **microkvm.c**: add `Ctrl-A v` switching to stdin_thread; call `virtio_console_rx()` and inject IRQ 5 in virtio mode

## Next step

[Step 17: ioeventfd](step17_ioeventfd.md)—Let KVM notify an eventfd for TX QueueNotify, eliminating the path that returns `KVM_EXIT_MMIO` to the vCPU thread.
