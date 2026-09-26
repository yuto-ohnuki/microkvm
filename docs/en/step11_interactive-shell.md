# Step 11: Interactive Shell

## Goal

Connect host terminal input to the UART **RX** (receive) handling prepared in Step 10. Use the existing busybox initramfs to run commands in the guest shell.

## Background

### TX vs RX

Step 10 implemented serial TX (transmit): the guest writes THR, and the VMM outputs to stdout.
Step 11 adds the reverse direction:

| Direction | UART role | Flow |
|------|------------|--------|
| TX (Step 10) | Guest → Host | Guest writes THR → VMM outputs to stdout |
| RX (Step 11) | Host → Guest | User types → VMM sets RBR → IRQ 4 → guest reads |

### Why a separate stdin thread?

The vCPU thread runs the guest in `KVM_RUN` and handles I/O on returning to userspace. Waiting for host input there would also stop guest execution, so a dedicated **stdin reader thread** waits for input:

```
[Host terminal] → [stdin_thread] → [UART RBR + IRQ 4] → [Guest kernel]
  Keystroke        read(stdin)      uart_rx()             Serial driver
```

### UART RX registers

| Register | Role |
|---------|------|
| RBR (0x3F8, read, DLAB=0) | Receive Buffer—guest reads characters here |
| LSR bit 0 (DR) | Data Ready—RBR contains unread data |
| IER bit 0 (RDI) | Enable Receive Data Interrupt |
| IIR = 0x04 | Interrupt identification: receive data caused the interrupt |

### RX sequence

```
1. User presses 'a'
   stdin_thread: read(STDIN_FILENO) returns 'a'

2. stdin_thread updates UART state:
   uart.rbr = 'a'
   uart.lsr |= DR (Data Ready)

3. stdin_thread injects IRQ 4 (if IER.RDI is enabled):
   KVM_IRQ_LINE {irq=4, level=1}  (assert)
   KVM_IRQ_LINE {irq=4, level=0}  (deassert)

4. Guest kernel receives IRQ 4:
   serial8250_interrupt() runs

5. Guest reads IIR (0x3FA) → VMM returns 0x04 (RDI)
   Driver identifies received data as the interrupt source

6. Guest reads RBR (0x3F8) → VMM returns 'a'
   VMM clears: lsr &= ~DR, pending_rdi = 0

7. Driver passes 'a' to the tty layer → shell input
   Guest terminal echo and shell output return to the host terminal through TX
```

### Host terminal input settings (retained from Step 10)

By default, the host terminal buffers input by line (canonical mode).
An interactive shell needs each keystroke delivered immediately:

```c
struct termios raw = orig_termios;
raw.c_lflag &= ~(ICANON | ECHO);    /* No line buffering or local echo */
raw.c_cc[VMIN] = 1;                 /* read returns after one byte */
raw.c_cc[VTIME] = 0;                /* No timeout */
tcsetattr(STDIN_FILENO, TCSANOW, &raw);
```

`ISIG` is not disabled, so with normal terminal settings, `Ctrl-C` terminates the host VMM instead of reaching the guest. `atexit(restore_terminal)` restores settings only on normal exit; if the terminal remains altered after signal termination, run `stty sane` on the host.

### ESC sequence filtering

When output contains a control sequence requesting the cursor position, the host terminal responds with that position. This response arrives on the VMM's standard input just like keystrokes. Passing it directly to `uart_rx()` would feed text the user never typed into the guest shell. The ESC filter removes these terminal responses.

The implementation simply discards everything from ESC (`0x1b`) through the next alphabetic character; it does not distinguish terminal responses from keystrokes. Arrow keys therefore do not pass through, and a standalone ESC also discards subsequent input through the next letter.

## Execution flow

```text
Guest (Linux + busybox)      KVM                         VMM
     |                       |                           | Start stdin_thread
     |                       |<-- KVM_RUN ---------------| vcpu_thread
     | Boot → /init → sh     |                           |
     |                       |                           | Type 'a' on host
     |                       |                           | read() → uart_rx()
     |                       |                           | RBR='a', LSR.DR=1
     |                       |<-- KVM_IRQ_LINE (IRQ 4) --| If IER.RDI enabled
     |<-- Deliver IRQ 4 -----|                           |
     |-- Read IIR / RBR ---->|-- KVM_EXIT_IO ----------->|
     |                       |                           | uart_in() responds
     |                       |<-- KVM_RUN ---------------|
     |<-- Receive input -----|                           |
     | tty → shell           |                           |
     |-- Echo/output (THR) ->|-- KVM_EXIT_IO ----------->|
     |                       |                           | uart_out() → stdout
```

IIR and RBR reads are each handled as PIO. When Enter submits a command, the shell outputs its result through the same TX path.

## Implementation

### stdin_thread

```c
static void *stdin_thread(void *arg) {
    (void)arg;
    uint8_t c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        /* ESC sequence filter (terminal responses) */
        if (c == 0x1b) {
            while (read(STDIN_FILENO, &c, 1) == 1) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                    break;
            }
            continue;
        }
        uart_rx(&uart, c, g_vmfd);
    }
    return NULL;
}
```

In `main()`, create the stdin thread before starting the vCPU thread:

```c
pthread_t stdin_tid;
int ret = pthread_create(&stdin_tid, NULL, stdin_thread, NULL);
if (ret != 0) {
    fprintf(stderr, "pthread_create(stdin): %s\n", strerror(ret));
    exit(1);
}
```

### uart_rx (retained from Step 10)

```c
void uart_rx(struct uart8250 *u, uint8_t c, int vmfd) {
    u->rbr = c;
    u->lsr |= UART_LSR_DR;          /* Data Ready */
    if (u->ier & UART_IER_RDI) {    /* RX interrupt enabled? */
        u->pending_rdi = 1;
        inject_irq4(vmfd);
    }
}
```

`uart_rx()` shares UART state with `uart_in()` / `uart_out()` in the vCPU thread, but this tag has no locking. RBR also holds only one byte and is overwritten without waiting for unread data to be consumed, so rapid input or pasting can lose characters.

### initramfs init script

```sh
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
echo "=== Hello from microkvm guest! ==="
exec /bin/sh
```

`/init` mounts proc, sysfs, and devtmpfs, then starts busybox sh. With `/dev/console` from Step 10 and `console=ttyS0`, the shell inherits standard input/output connected to the serial console. `CONFIG_DEVTMPFS_MOUNT` does not automatically mount devtmpfs in initramfs.

## About initramfs

The initramfs from Step 10 already contains busybox and an interactive shell (`exec /bin/sh` in init). No initramfs change is needed here: adding `stdin_thread` to the VMM lets host input reach the guest.

## Output

Use the same `bzImage` and `initramfs.gz` as in Step 10.

```bash
$ make clean
$ make
$ ./microkvm
```

When the prompt appears, enter these commands one at a time:

```
/ # uname -r
7.3.0-rc4+
/ # ls /
bin   dev   init  lib   proc  root  sys
/ # echo hello
hello
/ # cat /proc/cmdline
console=ttyS0 earlyprintk=serial rdinit=/init
```

Seeing the commands and their results verifies the round trip: host input → UART RX → guest execution → UART TX. Press `Ctrl-C` in the host terminal to exit.

## Key insight

Received data goes into UART RBR; IRQ 4 announces its arrival. Separating the stdin thread from vCPU execution lets host input reach the guest while it runs. This minimal implementation, however, lacks shared-state synchronization and a receive queue.

## What this step teaches

| Concept | How it appears here |
|------|--------------|
| UART RX | RBR, LSR.DR, IER.RDI, IIR=0x04 |
| Separate I/O thread | stdin_thread reads host input independently of the vCPU |
| IRQ as notification | IRQ 4 wakes the guest driver when data arrives |
| Host terminal settings | Disable ICANON/ECHO, retain ISIG |
| ESC filtering | Discard from ESC through the next alphabetic character |
| initramfs + init | Shell inherits /init's standard input/output |
| Bidirectional I/O | Deliver RX data and interrupts in addition to TX |

## What changed

- Add `stdin_thread()` and its startup code to `microkvm.c`.
- Discard ESC sequences in the stdin thread and pass other input to the existing `uart_rx()`.
- `uart.c`, terminal settings, and initramfs are unchanged from Step 10.

## Next step

[Step 12: virtio-mmio Device Discovery](step12_virtio-discovery.md)—Implement virtio-mmio identification registers so Linux recognizes the device.
