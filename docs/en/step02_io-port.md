# Step 2: Character Output through I/O Ports

## Goal

Use x86 port I/O (the `out` instruction) to output characters from the guest to the host terminal.
This introduces the **exit handler loop**—the basic structure of every VMM.

## Background

### Port I/O (PIO)

x86 has a separate 16-bit I/O address space (ports 0x0000–0xFFFF), accessed through the `in` and `out` instructions.
In this step, each port I/O instruction causes `KVM_RUN` to return with `KVM_EXIT_IO`.

```
out 0x10, al    →  Write AL to port 0x10  →  KVM_EXIT_IO (direction=OUT)
in  al, 0x10    →  Read port 0x10 into AL →  KVM_EXIT_IO (direction=IN)
```

On physical machines, legacy devices (serial ports, PIC, PIT) use PIO. Our hypervisor defines port `0x10` as a simple character output device.

### Exit handler loop

A VMM runs `KVM_RUN` in a loop rather than calling it just once:

```c
for (;;) {
    ioctl(vcpufd, KVM_RUN, NULL);
    switch (run->exit_reason) {
        case KVM_EXIT_IO:   /* Handle I/O */  break;
        case KVM_EXIT_HLT:  /* Stop */        goto done;
    }
}
```

Step 1 called `KVM_RUN` only once. Here, we call it again after handling each I/O operation to continue guest execution. The loop ends when it receives `KVM_EXIT_HLT`.

### Fields in run->io

When `exit_reason == KVM_EXIT_IO`, the shared kvm_run page contains:

| Field | Meaning |
|-----------|------|
| `run->io.port` | Port accessed |
| `run->io.direction` | `KVM_EXIT_IO_OUT` (1) or `KVM_EXIT_IO_IN` (0) |
| `run->io.size` | Data width (1, 2, or 4 bytes) |
| `run->io.count` | Number of data items (1 in this guest) |
| `run->io.data_offset` | Offset to the data from the start of kvm_run |

## Execution flow

```
VMM (microkvm)                                  Guest
──────────────                                  ─────
                                                mov al, 'H'
                                                out 0x10, al
KVM_EXIT_IO
  port=0x10, direction=OUT
  data at run + data_offset = 'H'
  putchar('H')
KVM_RUN                                         (complete I/O and resume)
                                                mov al, 'i'
                                                out 0x10, al
KVM_EXIT_IO: putchar('i')
KVM_RUN                                         (resume)
                                                mov al, '\n'
                                                out 0x10, al
KVM_EXIT_IO: putchar('\n')
KVM_RUN                                         (resume)
                                                hlt
KVM_EXIT_HLT: end loop
```

For these guest instructions, the VMM receives three `KVM_EXIT_IO` returns and one `KVM_EXIT_HLT` return.

## Implementation

Update the guest code and execution loop in `microkvm.c`, and add the character output port `PIO_PORT` (`0x10`) to `microkvm.h`. The excerpts below omit error handling and other details.

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

- `0xB0` = `mov al, imm8` (load an immediate byte into AL)
- `0xE6` = `out imm8, al` (write AL to the specified port)
- AL is the low 8 bits of RAX—x86 I/O instructions use AL/AX/EAX for data

We still use a raw byte array here to focus on KVM exits.
In the next step, the guest code moves to a separate assembly file.

### Exit handler loop

```c
for (;;) {
    ioctl(vcpufd, KVM_RUN, NULL);

    switch (run->exit_reason) {
    case KVM_EXIT_IO:
        if (run->io.port == PIO_PORT && run->io.direction == KVM_EXIT_IO_OUT) {
            putchar(*(char *)((char *)run + run->io.data_offset));
        }
        break;
    case KVM_EXIT_HLT:
        printf("Guest halted.\n");
        goto done;
    }
}
```

The data address is `(char *)run + run->io.data_offset`. The offset is in bytes, so cast to `char *` before adding it. Since this guest's `out` has `size = 1` and `count = 1`, we read one byte and pass it to `putchar`. Reading the data from shared memory requires no additional syscall.

## Output

```
$ ./microkvm
Starting guest...
Hi
Guest halted.
```

`Starting guest...` appears before execution starts. `Hi` and its newline come from the guest's three `out` instructions, rendered by the VMM. `Guest halted.` is printed by the VMM after receiving `KVM_EXIT_HLT`.

### Why start with port I/O?

Port I/O in this step only requires checking the port number and output direction, then printing one character. KVM passes the port number and data to the VMM through `KVM_EXIT_IO` and shared memory.

MMIO (memory-mapped I/O), which uses the memory address space, is introduced in Step 5.

### Performance note

Each `out` instruction involves:

```
Guest execution
  ↓
VM exit
  ↓
Userspace device emulation (putchar)
  ↓
VM entry (KVM_RUN)
```

This configuration makes a round trip to userspace for every character. Steps 12–18 introduce virtio shared queues, ioeventfd, and irqfd to explore reducing these round trips.

## Key insight

Ordinary guest instructions execute directly on the CPU. A VM exit first transfers control to KVM; if KVM can handle it internally, execution continues without returning to userspace. In this step, the VMM handles port output, so `KVM_RUN` returns with `KVM_EXIT_IO`. The VMM outputs the character and calls `KVM_RUN` again.

## What this step teaches

| Concept | How it appears here |
|------|--------------|
| Exit handler loop | Call `KVM_RUN` again after handling I/O; stop on HLT |
| Port I/O interception | `out` triggers `KVM_EXIT_IO` in this configuration |
| kvm_run data access | Read output data at `(char *)run + run->io.data_offset` |
| Guest → host communication | PIO as the simplest channel |

## What changed

- Replace the guest's lone `hlt` with three `mov` / `out` pairs that output `Hi\n`, followed by `hlt`.
- Define `PIO_PORT` (`0x10`) and add character output handling for `KVM_EXIT_IO`.
- Replace the single `KVM_RUN` call with a loop that ends on HLT.

## Next step

[Step 3: Real Mode → Protected Mode](step03_protected-mode.md)—Move guest code into an assembly file, set up a GDT, and transition the CPU from 16-bit real mode to 32-bit protected mode.
