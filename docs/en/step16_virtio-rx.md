# Step 16: virtio-console RX (host → guest)

## Goal

Deliver host stdin to the guest via the receive queue and IRQ injection. When the user types in virtio mode, data flows from host → guest RAM → IRQ → driver → `/dev/hvc0`. This completes the bidirectional virtio-console.

## Background

### RX is the reverse of TX

In Step 15 (TX), the guest placed data in a buffer and kicked the VMM. In RX, the roles reverse:
- The **guest** provides empty buffers (the virtio-console driver allocates receive buffers during initialization and places them into receiveq — the 128 QueueNotify kicks seen at the end of Step 14)
- The **VMM** writes data into those buffers
- The **VMM** notifies the guest via IRQ that data is available (IRQ 5 — the same line configured in the kernel command line during device setup)

### Why IRQ is needed for RX

The guest cannot know when host input arrives. Unlike TX where the guest initiates the transfer, RX is asynchronous — the VMM must actively notify the guest. Without an interrupt, the guest would never check the receive queue.

```
Without IRQ: VMM writes data → guest never notices → /dev/hvc0 stays empty
With IRQ:    VMM writes data → IRQ 5 → driver wakes → reads used ring → delivers to /dev/hvc0
```

Polling would also work (guest repeatedly checks the used ring), but would waste CPU time. Interrupts allow the guest to sleep until data arrives — this is the fundamental reason all I/O devices (NIC, block, serial, NVMe) use interrupts.

### RX flow overview

```
Host stdin → stdin_thread → virtio_console_rx():
  1. Find empty buffer in receiveq (VRING_DESC_F_WRITE flag)
  2. Write data into guest buffer (memcpy to guest RAM)
  3. Post descriptor to used ring (with actual byte count)
  4. Set interrupt_status |= 0x1
  5. Inject IRQ 5 via KVM_IRQ_LINE

Guest IRQ handler:
  6. Read InterruptStatus → 0x1 (used buffer notification)
  7. Write InterruptACK ← 0x1 (clear pending)
  8. Check used ring → find completed descriptor
  9. Read data from buffer → deliver to /dev/hvc0
```

### Ctrl-A v mode switching

microkvm's stdin is shared between UART (ttyS0) and virtio (hvc0). `Ctrl-A v` toggles which device receives input:
- Default: UART mode → `uart_rx()` delivers to ttyS0 (shell)
- After `Ctrl-A v`: Virtio mode → `virtio_console_rx()` delivers to hvc0

### InterruptStatus / InterruptACK

| Register | Direction | Purpose |
|----------|-----------|---------|
| InterruptStatus (0x060) | Read | VMM tells guest which interrupts are pending (bit 0 = used buffer) |
| InterruptACK (0x064) | Write | Guest clears pending interrupt after handling |

The guest's IRQ handler reads InterruptStatus to identify the interrupt source, then writes InterruptACK to clear it. If not cleared, the interrupt remains pending.

## Execution flow

```
Host (stdin_thread)              KVM                Guest (virtio-console driver)
───────────────────              ───                ────────────────────────────
user types 'a' in virtio mode
read(stdin) → 'a'

virtio_console_rx(&dev, 'a', 1):
  avail ring: find empty buffer
  descriptor: addr=X, flags=WRITE
  memcpy(ram + X, "a", 1)
  used ring: post {id, len=1}
  interrupt_status |= 0x1

ioctl(KVM_IRQ_LINE, irq=5, 1)
ioctl(KVM_IRQ_LINE, irq=5, 0)
                                 PIC delivers IRQ 5
                                 to guest CPU
                                                    IRQ handler fires
                                                    MMIO read InterruptStatus → 0x1
                                                      (KVM_EXIT_MMIO → VMM returns value)
                                                    MMIO write InterruptACK ← 0x1
                                                      (KVM_EXIT_MMIO → VMM clears bit)
                                                    check used ring → descriptor ready
                                                    read buffer at addr X → 'a'
                                                    deliver to /dev/hvc0
                                                    cat /dev/hvc0 prints 'a'
```

## Implementation

### virtio_mmio.h additions

```c
uint32_t interrupt_status;  /* pending interrupt bits */

/* RX: write data into receiveq and raise interrupt */
int virtio_console_rx(struct virtio_mmio_dev *dev, const uint8_t *data, size_t len);
```

### virtio_mmio.c — virtio_console_rx()

```c
int virtio_console_rx(struct virtio_mmio_dev *dev, const uint8_t *data, size_t len)
{
    int qidx = 0;  /* receiveq */

    /* Find next empty buffer from avail ring */
    /* Verify VRING_DESC_F_WRITE flag (device writes into this buffer) */
    /* memcpy data into guest buffer */
    /* Post to used ring with actual bytes written */
    /* Set interrupt_status |= 0x1 */
    return 0;  /* success; caller injects IRQ */
}
```

Key difference from TX: `used_elem.len` returns the actual byte count written, so the guest knows how much data is in the buffer.

### virtio_mmio.c — InterruptStatus / InterruptACK

```c
case VIRTIO_MMIO_INTERRUPT_STATUS:
    val = dev->interrupt_status;    /* guest reads to identify interrupt */
    break;

case VIRTIO_MMIO_INTERRUPT_ACK:
    dev->interrupt_status &= ~value;  /* guest clears handled bits */
    break;
```

### microkvm.c — stdin_thread changes

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

IRQ injection uses edge trigger (0→1→0) — same pattern as UART's IRQ 4 in Step 11.

## Output

```
/ # cat /dev/hvc0 &
/ #
[monitor] input → hvc0 (virtio)
abc
abc

[monitor] input → ttyS0 (UART)

/ #
```

Characters typed in virtio mode are delivered to `/dev/hvc0` and displayed by `cat`.

## Key insight

RX completes the virtio I/O loop. TX (Step 15) showed guest → host via shared memory. RX shows host → guest via the same mechanism — but with an added IRQ notification because the guest cannot poll.

| | TX (Step 15) | RX (Step 16) |
|---|---|---|
| Who initiates | Guest (writes + kicks) | Host (writes + IRQ) |
| Queue | transmitq (1) | receiveq (0) |
| Descriptor flag | 0 (device reads) | VRING_DESC_F_WRITE (device writes) |
| used_elem.len | 0 (not checked) | actual bytes written |
| Notification | guest → VMM (QueueNotify) | VMM → guest (IRQ 5) |

The asymmetry exists because **I/O initiation** differs: TX is guest-driven (guest knows when it has data), RX is host-driven (guest doesn't know when input arrives). IRQ bridges this gap.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| RX via shared memory | VMM writes directly into guest-provided buffer |
| VRING_DESC_F_WRITE | Marks buffer as "device may write here" |
| IRQ injection | KVM_IRQ_LINE triggers guest interrupt handler |
| InterruptStatus/ACK | Guest identifies and clears interrupt source |
| Edge-triggered IRQ | Assert (level=1) then deassert (level=0) |
| Ctrl-A v mode switch | Single stdin shared between UART and virtio |
| Bidirectional virtio | TX + RX = complete console device |

## What changed

From Step 15:
- **virtio_mmio.h**: `interrupt_status` field, `virtio_console_rx()` prototype
- **virtio_mmio.c**: `virtio_console_rx()` implementation, `INTERRUPT_STATUS` returns live value, `INTERRUPT_ACK` clears bits, log suppression for InterruptStatus/ACK
- **microkvm.c**: `Ctrl-A v` mode toggle in stdin_thread, virtio mode calls `virtio_console_rx()` + IRQ 5 injection

## Next step

[Step 17: ioeventfd](step17_ioeventfd.md) — eliminate the QueueNotify VM exit for TX kicks. KVM handles the notification entirely in kernel space via eventfd.
