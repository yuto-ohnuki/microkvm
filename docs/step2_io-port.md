# Step 2: I/O port character output

## Goal

Make the guest print characters to the host terminal using x86 Port I/O (`out` instruction).
This introduces the **exit handler loop** - the fundamental structure of every VMM.

## Background

### Port I/O (PIO)

x86 has a separate 16-bit I/O address space (ports 0x0000–0xFFFF) accessed via `in` and `out` instructions.
In this step, each port I/O instruction causes `KVM_RUN` to return with `KVM_EXIT_IO`. These instructions are intercepted by the hypervisor and exposed to userspace as `KVM_EXIT_IO` exits.

```
out 0x10, al    →  write AL to port 0x10  →  KVM_EXIT_IO (direction=OUT)
in  al, 0x10    →  read port 0x10 into AL →  KVM_EXIT_IO (direction=IN)
```

Real hardware uses PIO for legacy devices (serial ports, PIC, PIT). In our hypervisor, we define port `0x10` as a simple character output device.

### The exit handler loop

A real VMM doesn't call `KVM_RUN` once - it runs in a loop:

```c
for (;;) {
    ioctl(vcpufd, KVM_RUN, NULL);
    switch (run->exit_reason) {
        case KVM_EXIT_IO:   /* handle I/O */  break;
        case KVM_EXIT_HLT:  /* stop */        goto done;
    }
}
```

This is the same structure as QEMU's `kvm_cpu_exec()`. The guest runs natively on the CPU; the VMM only intervenes when the hardware traps.

### run->io fields

When `exit_reason == KVM_EXIT_IO`, the shared kvm_run page contains:

| Field | Meaning |
|-------|---------|
| `run->io.port` | Which port was accessed |
| `run->io.direction` | `KVM_EXIT_IO_OUT` (1) or `KVM_EXIT_IO_IN` (0) |
| `run->io.size` | Data width (1, 2, or 4 bytes) |
| `run->io.data_offset` | Offset from start of kvm_run to the data |

## Execution flow

```
Guest (CPU)                   Host (microkvm)
───────────                   ───────────────
mov al, 'H'
out 0x10, al  ── VM exit ──→  run->exit_reason = KVM_EXIT_IO
                              run->io.port = 0x10
                              run->io.direction = OUT
                              data at run + data_offset = 'H'
                              putchar('H')
              ←─ KVM_RUN ───
mov al, 'i'
out 0x10, al  ── VM exit ──→  putchar('i')
              ←─ KVM_RUN ───
mov al, '\n'
out 0x10, al  ── VM exit ──→  putchar('\n')
              ←─ KVM_RUN ───
hlt           ── VM exit ──→  KVM_EXIT_HLT → break loop
```

Total: 4 VM exits (3× IO + 1× HLT).

## Implementation

### Guest code (inline byte array)

```c
static const unsigned char guest_code[] = {
    0xB0, 'H',     /* mov al, 'H' */
    0xE6, 0x10,    /* out 0x10, al */
    0xB0, 'i',     /* mov al, 'i' */
    0xE6, 0x10,    /* out 0x10, al */
    0xB0, '\n',    /* mov al, '\n' */
    0xE6, 0x10,    /* out 0x10, al */
    0xf4           /* hlt */
};
```

- `0xB0` = `mov al, imm8` (load immediate byte into AL)
- `0xE6` = `out imm8, al` (write AL to the specified port)
- AL is the lower 8 bits of RAX - x86 I/O instructions use AL/AX/EAX for data

We still use a raw byte array here to keep the focus on KVM exits.
In the next step, guest code moves into a standalone assembly file.

### Exit handler loop

```c
for (;;) {
    ioctl(vcpufd, KVM_RUN, NULL);

    switch (run->exit_reason) {
    case KVM_EXIT_IO:
        if (run->io.port == IO_PORT && run->io.direction == KVM_EXIT_IO_OUT) {
            putchar(*(char *)((char *)run + run->io.data_offset));
        }
        break;
    case KVM_EXIT_HLT:
        printf("Guest halted.\n");
        goto done;
    }
}
```

The data lives inside the kvm_run shared page at `run + data_offset`. No extra syscall needed - this is why kvm_run is mmap'd rather than copied.

## Output

```
$ ./microkvm
Starting guest...
Hi
Guest halted.
```

## Why use Port I/O first?

Port I/O is simpler than MMIO because the hardware directly reports the port number and data through `KVM_EXIT_IO`.

MMIO requires address decoding and device emulation based on guest physical addresses, which we will introduce in Step 5.

## Performance note

Each `out` instruction causes:

```
Guest execution
  ↓
VM exit
  ↓
Userspace device emulation (putchar)
  ↓
VM entry (KVM_RUN)
```

This round-trip is expensive. Modern virtualization stacks reduce exits using shared memory (virtio), kernel-side handling (ioeventfd), and paravirtualized devices - topics covered in Steps 12-14.

## Key insight

The hypervisor does not continuously inspect guest execution.
Most instructions execute directly on the CPU without involving userspace. The VMM only regains control when the hardware triggers a VM exit.

```
guest   ← runs natively on CPU
guest
guest
guest
guest
VM exit ← hardware trap, control returns to host
host    ← VMM handles the exit
VM entry
guest   ← back to native execution
guest
...
```

This execution model remains unchanged throughout the rest of the project.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| Exit handler loop | `for(;;) { KVM_RUN; switch(exit_reason) }` - same as QEMU |
| Port I/O interception | `out` triggers `KVM_EXIT_IO` in this configuration |
| kvm_run data access | `run + run->io.data_offset` for zero-copy I/O data |
| Guest → host communication | PIO as the simplest channel |

## Next step

[Step 3: Real → protected mode](step3_protected-mode.md) - separate guest code into an assembly file, set up a GDT, and transition the CPU from 16-bit real mode to 32-bit protected mode.
