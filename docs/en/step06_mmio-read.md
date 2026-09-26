# Step 6: MMIO Reads + Device State

## Goal

Add MMIO **read** support so the guest can query device state.
This completes the bidirectional device model: writes send commands to the device, and reads return status or data.

## Background

### Bidirectional device communication

In Step 5, the guest could only write to the MMIO address—a one-way channel. Real devices need both directions:

| Direction | Guest operation | Device role | Example |
|------|-----------|--------------|-----|
| Write | `mov [addr], val` | Receive commands/data | Send a packet, change settings |
| Read | `mov val, [addr]` | Return status/data | Read interrupt status, retrieve a counter |

At the same MMIO address, 0xD0000, this device receives characters and increments a counter on writes, and returns the current counter value on reads.

### How MMIO reads work

When the guest reads from an unregistered GPA, KVM exits with `KVM_EXIT_MMIO` and `is_write = 0`. The VMM:
1. Writes the value to return into `run->mmio.data[]`
2. Resumes the guest with `KVM_RUN`

KVM completes the memory access and places the returned value in the destination register specified by the original instruction.

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

The VMM maintains a simple `device_counter` variable. Each MMIO write to 0xD0000 increments it; a read returns its current value without changing it. This demonstrates a device model maintaining **state** across guest accesses—the foundation of any device emulator.

## Execution flow

The following flow starts with the MMIO accesses in long mode.

```
VMM (microkvm)                                  Guest
──────────────                                  ─────
                                                RBX = 0xD0000
                                                mov al, 'M'
                                                mov [rbx], al
KVM_EXIT_MMIO (write)
  device_counter: 0 → 1
  Log 'M'
KVM_RUN                                         (complete write and resume)
                                                mov al, '\n'
                                                mov [rbx], al
KVM_EXIT_MMIO (write)
  device_counter: 1 → 2
  Omit newline log
KVM_RUN                                         (resume)
                                                mov al, [rbx]
KVM_EXIT_MMIO (read)
  is_write=0, len=1
  run->mmio.data[0] = 2
  Print returning 2
KVM_RUN                                         (complete read: AL = 2)
                                                add al, '0' → '2'
                                                out 0x10, al
KVM_EXIT_IO: log '2'
KVM_RUN                                         (resume)
                                                out newline
KVM_EXIT_IO: omit log
KVM_RUN                                         (resume)
                                                hlt
KVM_EXIT_HLT: stop
```

## Implementation

The changed files are `microkvm.c` and `guest.S`. Memory slots and page tables remain as in Step 5.

### VMM: MMIO handler with read support

```c
/* Maintained at file scope in microkvm.c */
static uint8_t device_counter = 0;

/* ... inside the exit handler loop: */
case KVM_EXIT_MMIO:
    if (run->mmio.phys_addr == MEM_GAP_START) {
        if (run->mmio.is_write) {
            char c = run->mmio.data[0];
            device_counter++;
            if (c != '\n')
                printf("[MMIO write @ 0x%llx] %c\n",
                       run->mmio.phys_addr, c);
        } else {
            run->mmio.data[0] = device_counter;
            printf("[MMIO read  @ 0x%llx] returning %d\n",
                   run->mmio.phys_addr, device_counter);
        }
    }
    break;
```

For reads, the VMM writes the return value to `run->mmio.data[]` before calling `KVM_RUN`. KVM takes this value, completes the load instruction, and places it in the guest's destination register.

The counter increments on each write. A read after two writes ('M' and '\n') returns 2.

### Guest: Read from the MMIO address

```asm
    /* MMIO read: load from 0xD0000 → KVM_EXIT_MMIO (is_write=0) */
    .byte 0x8A, 0x03       /* mov al, [rbx]  — rbx is still 0xD0000 */
    .byte 0x04, 0x30       /* add al, '0'    — convert to an ASCII digit */
    .byte 0xE6, 0x10       /* out 0x10, al   — output through PIO */
    .byte 0xB0, '\n'       /* mov al, '\n' */
    .byte 0xE6, 0x10       /* out 0x10, al */
```

The guest reads from the same address it wrote to. `mov al, [rbx]` triggers `KVM_EXIT_MMIO` with `is_write=0`. The VMM fills `data[0]`; when KVM completes the read on the next `KVM_RUN`, the guest receives the value in AL. `add al, '0'` converts a value from 0–9 into one ASCII digit. Here it converts 2 to the character `2`, which is output through PIO.

## Output

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
[PIO out port 0x10] R
[PIO out port 0x10] P
[PIO out port 0x10] L
[MMIO write @ 0xd0000] M
[MMIO read  @ 0xd0000] returning 2
[PIO out port 0x10] 2
Guest halted.
```

`returning 2` is the value supplied by the VMM. The following PIO `2` is the guest's conversion of that value to a character. The counter is 2 because the newline write counts even though it is not logged.

## Key insight

Reading this MMIO address returns the number of writes, not the last character written. By maintaining state across accesses and deciding what reads return, the VMM implements behavior different from ordinary RAM. Reads do not change the counter, so repeated reads return the same value until another write occurs.

### Comparison with RAM

| | RAM | MMIO device |
|---|---|---|
| Write → Read | Returns the written value | May return a completely different value |
| Side effects | None | May trigger actions (send a packet, raise an IRQ) |
| State | Simply stores bytes | Maintains complex internal state |

## What this step teaches

| Concept | How it appears here |
|------|--------------|
| MMIO read | VMM supplies a value; KVM completes the guest load |
| Device state | `device_counter` persists across accesses |
| Bidirectional model | Same address, different behavior for reads and writes |
| Device emulation pattern | Address → dispatch → state machine → response |

## What changed

- `microkvm.c`: Add `device_counter` at file scope; increment it on MMIO writes and return its current value on reads.
- `guest.S`: Add an MMIO read and PIO output of the returned value as a digit.

## Next step

[Step 7: Interrupt Injection](step07_irq.md)—All interactions so far have been initiated by the guest. Next, the host uses interrupts to notify the guest asynchronously, introducing communication in the opposite direction (host → guest).
