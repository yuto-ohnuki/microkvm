# Step 6: MMIO read + device state

## Goal

Add MMIO **read** support so the guest can query device state.
This completes the bidirectional device model: writes send commands to the device, reads retrieve status or data back.

## Background

### Bidirectional device communication

In Step 5, the guest could only write to the MMIO address - a one-way channel. Real devices need both directions:

| Direction | Guest operation | Device role | Example |
|-----------|----------------|-------------|---------|
| Write | `mov [addr], val` | Receive command/data | Send a packet, set config |
| Read | `mov val, [addr]` | Return status/data | Read interrupt status, get counter |

A single MMIO address can behave differently on read vs write. This is common in hardware: for example, a UART's address 0x3F8 is the Transmit Holding Register on write and the Receive Buffer Register on read - two completely different functions at the same address.

### How MMIO read works

When the guest reads from an unregistered GPA, KVM exits with `KVM_EXIT_MMIO` and `is_write = 0`. The VMM must:
1. Fill `run->mmio.data[]` with the value to return
2. Call `KVM_RUN` to resume the guest

KVM completes the memory access and places the returned value into the destination register specified by the original instruction.

```
Guest: mov al, [0xD0000]
         │
         ▼
KVM_EXIT_MMIO (is_write=0, len=1)
         │
         ▼
VMM: run->mmio.data[0] = counter_value
VMM: ioctl(KVM_RUN)
         │
         ▼
KVM completes the load → al = counter_value
```

### Device state

In this step, the VMM maintains a simple `device_counter` variable. Each MMIO read returns the current counter value and increments it. This demonstrates that the device model can hold **state** that persists across multiple guest accesses - the foundation of any device emulator.

## Execution flow

```
VMM (microkvm.c)                         Guest (guest.S)
────────────────                         ───────────────
                                         [long mode]
                                           mov rbx, 0xD0000
                                           mov [rbx], al  ('M')
                                                │
KVM_EXIT_MMIO (write)                           ▼
  data[0] = 'M'
  printf("[MMIO write] M")
ioctl(KVM_RUN)
                                           mov al, [rbx]  (read)
                                                │
KVM_EXIT_MMIO (read)                            ▼
  run->mmio.data[0] = 0
  device_counter++ → 1
  printf("[MMIO read] returning 0")
ioctl(KVM_RUN)
                                           al = 0
                                           add al, '0'  → '0'
                                           out 0x10, al
                                                │
KVM_EXIT_IO                                     ▼
  printf("[PIO out] 0")
ioctl(KVM_RUN)
                                           hlt
```

## Implementation

### VMM: MMIO handler with read support

```c
static uint8_t device_counter = 0;

/* ... inside the exit handler loop: */
case KVM_EXIT_MMIO:
    if (run->mmio.phys_addr == 0xD0000) {
        if (run->mmio.is_write) {
            char c = run->mmio.data[0];
            if (c != '\n')
                printf("[MMIO write @ 0x%llx] %c\n",
                       run->mmio.phys_addr, c);
        } else {
            run->mmio.data[0] = device_counter++;
            printf("[MMIO read  @ 0x%llx] returning %d\n",
                   run->mmio.phys_addr, device_counter - 1);
        }
    }
    break;
```

For reads, the VMM writes the return value into `run->mmio.data[]` before calling `KVM_RUN`. KVM picks up this value and completes the load instruction, placing it into the guest's destination register.

The post-increment operator returns the current value and then increments the counter, so the first read returns 0, the second returns 1, and so on.

### Guest: reading from the MMIO address

```asm
    /* MMIO read: load from 0xD0000 → KVM_EXIT_MMIO (is_write=0) */
    .byte 0x8A, 0x03       /* mov al, [rbx]  — rbx still = 0xD0000 */
    .byte 0x04, 0x30       /* add al, '0'    — convert to ASCII digit */
    .byte 0xE6, 0x10       /* out 0x10, al   — print via PIO */
```

The guest reads from the same address it wrote to. The `mov al, [rbx]` instruction triggers `KVM_EXIT_MMIO` with `is_write=0`. After the VMM fills `data[0]`, the guest receives the value in AL, converts it to ASCII, and outputs it via PIO.

## Output

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
[PIO out port 0x10] R
[PIO out port 0x10] P
[MMIO write @ 0xd0000] M
[MMIO read  @ 0xd0000] returning 0
[PIO out port 0x10] 0
Guest halted.
```

## Key insight

A device model is not just a passive receiver of writes. It holds state and returns different values on each read. The same MMIO address can mean completely different things depending on the direction of access.

This is exactly how real hardware works:
- A UART's data register: write = transmit, read = receive
- A NIC's status register: write = clear flags, read = get status
- A timer's counter register: write = set value, read = get current count

The address 0xD0000 does not represent a stored byte. Instead, it represents a device register whose behavior depends on the access direction and the current device state. This pattern recurs throughout virtualization: virtio queue notifications, PCI config space, and MSI-X tables all work this way.

The VMM is the "hardware" - it decides what each address means and how reads and writes behave. This is the essence of device emulation.

```
Guest
  read GPA 0xD0000
        │
        ▼
MMIO dispatcher (address match)
        │
        ▼
Device state machine
  counter = 0
        │
        ▼
Return value: 0
  counter becomes 1
```

### Contrast with RAM

| | RAM | MMIO device |
|---|---|---|
| Write then read | Returns the value written | May return something completely different |
| Side effects | None | May trigger actions (send packet, fire IRQ) |
| State | Just stores bytes | Maintains complex internal state |

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| MMIO read | VMM supplies the value; KVM completes the guest load |
| Device state | `device_counter` persists across accesses |
| Bidirectional model | Same address, different behavior on read vs write |
| Device emulation pattern | Address → dispatch → state machine → response |

## Next step

[Step 7: Interrupt injection](step07_irq.md) — so far, every interaction has been initiated by the guest. In the next step, the host will asynchronously notify the guest using an interrupt, introducing the opposite direction of communication: host → guest.
