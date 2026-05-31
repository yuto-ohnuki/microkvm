# Step 7: Interrupt injection (IRQ)

## Goal

Deliver an interrupt from the **host to the guest** using `KVM_INTERRUPT`.
This introduces the **IDT** (Interrupt Descriptor Table), interrupt gates, and `iretq` - completing the third communication direction between host and guest.

## Background

### A new direction of communication

Until now, every VM exit was caused by an instruction the guest executed: `out`, `mov [addr]`, `hlt`, `wrmsr`. The guest decided when to communicate.

Interrupts are fundamentally different. The host can notify the guest at any time - even when the guest is not actively communicating. The guest may be interrupted in the middle of unrelated computation. This is how real hardware works: a NIC doesn't wait for the CPU to poll; it raises an interrupt when a packet arrives.

### Communication directions so far

| Step | Direction | Mechanism |
|------|-----------|-----------|
| 2 | Guest → Host | PIO (`out` → `KVM_EXIT_IO`) |
| 5 | Guest → Host | MMIO write (`mov [addr], val` → `KVM_EXIT_MMIO`) |
| 6 | Guest ↔ Host | MMIO read (guest requests, VMM responds with value) |
| **7** | **Host → Guest** | **Interrupt injection (`KVM_INTERRUPT`)** |

MMIO read lets the guest *poll* for data. Interrupts let the host *notify* the guest asynchronously - "something happened, handle it now." This is how real devices signal events: a NIC has a packet ready, a disk completed a transfer, a timer fired.

### IDT (Interrupt Descriptor Table)

The IDT maps interrupt vector numbers (0–255) to handler addresses.
When an interrupt arrives, the CPU:
1. Looks up `IDT[vector]`
2. Pushes RIP, CS, RFLAGS (and RSP, SS in 64-bit mode) onto the stack
3. Jumps to the handler address
4. Clears IF (for interrupt gates) to prevent nested interrupts

The handler runs, then executes `iretq` to restore the saved state and return to the interrupted code.

### 64-bit interrupt gate (16 bytes)

```
┌──────────────────────────────────────────────────────────────┐
│ offset_low (15:0) │ selector │ IST │ type_attr │ offset_mid  │
├──────────────────────────────────────────────────────────────┤
│ offset_high (63:32)              │ reserved                  │
└──────────────────────────────────────────────────────────────┘

type_attr = 0x8E:
  present=1, DPL=0, type=interrupt gate (0xE)
```

The handler address is split across three fields (offset_low, offset_mid, offset_high) for historical reasons.

### KVM_INTERRUPT

```c
struct kvm_interrupt irq = { .irq = 32 };
ioctl(vcpufd, KVM_INTERRUPT, &irq);
```

This requests delivery of interrupt vector 32 to the guest. The interrupt will be injected when the guest is in a state where interrupt delivery is permitted (IF=1 and not in an interrupt shadow).

KVM_INTERRUPT is a simple mechanism suitable for learning. Production VMMs typically deliver interrupts through virtual LAPICs, IOAPICs, irqfd, or MSI/MSI-X - topics covered in later steps.

### Why the guest needs a stack

When an interrupt fires, the CPU pushes an interrupt frame onto the stack.
The guest must have a valid RSP before enabling interrupts. In 64-bit mode, the interrupt frame includes SS, RSP, RFLAGS, CS, and RIP. Without a valid stack pointer, this push would fault. In this step, we set `RSP = 0x60000` before enabling interrupts.

### Why `sti; hlt`?

After `sti`, x86 temporarily delays interrupt delivery for one instruction (the **interrupt shadow**).
The sequence:

```asm
sti
hlt
```

allows the CPU to enter the halted state atomically before any pending interrupt is delivered. Without this guarantee, an interrupt could arrive between `sti` and `hlt`, causing the `hlt` to sleep forever (the interrupt was already handled). This pattern is commonly used in operating systems - Linux's idle loop uses the same technique.

## Execution flow

```
VMM (microkvm.c)                         Guest (guest.S)
────────────────                         ───────────────
                                         [long mode]
                                           mov rsp, 0x60000
                                           MMIO write 'M'
                                           MMIO read → '0'
                                           lidt [idt_desc]
                                           sti          (IF=1)
                                           hlt          (wait for interrupt)
                                                │
KVM_EXIT_HLT                                    ▼
  irq_injected == 0, so:
  ioctl(KVM_INTERRUPT, {.irq=32})
  irq_injected = 1
  ioctl(KVM_RUN)
                                                │
                                         CPU delivers vector 32:
                                           push interrupt frame
                                           IDT[32] → irq_handler
                                           mov al, 'I'
                                           out 0x10, al
                                                │
KVM_EXIT_IO                                     ▼
  printf("[PIO out] I")
ioctl(KVM_RUN)
                                           iretq
                                           (pop RIP, CS, RFLAGS, RSP, SS)
                                           ← returns to instruction after hlt
                                           hlt (second time)
                                                │
KVM_EXIT_HLT                                    ▼
  irq_injected == 1 → done
  printf("Guest halted.")
```

## Implementation

### VMM: interrupt injection on HLT

```c
int irq_injected = 0;

case KVM_EXIT_HLT:
    if (!irq_injected) {
        struct kvm_interrupt irq = { .irq = 32 };
        ioctl(vcpufd, KVM_INTERRUPT, &irq);
        irq_injected = 1;
    } else {
        printf("Guest halted.\n");
        goto done;
    }
    break;
```

The first `hlt` puts the guest CPU into a halted state. The injected interrupt wakes the CPU and transfers control to the interrupt handler. The second `hlt` (after the handler returns via `iretq`) means "I'm done."

### Guest: IDT setup and interrupt wait

```asm
    /* Set up stack (required for interrupt delivery) */
    .byte 0x48, 0xC7, 0xC4, 0x00, 0x00, 0x06, 0x00  /* mov rsp, 0x60000 */

    /* Load IDT */
    .byte 0x48, 0xC7, 0xC1                            /* mov rcx, imm32 */
    .long idt_desc
    .byte 0x0F, 0x01, 0x19                            /* lidt [rcx] */

    /* Enable interrupts and wait */
    .byte 0xFB                                         /* sti */
    .byte 0xF4                                         /* hlt */

    /* After iretq returns here */
    .byte 0xF4                                         /* hlt (done) */
```

The guest sets up a stack (required for the interrupt frame push), loads the IDT with `lidt`, enables interrupt delivery with `sti`, and halts. The `hlt` instruction suspends the CPU until an interrupt arrives.

### Guest: interrupt handler

```asm
.align 16
irq_handler:
    .byte 0xB0, 'I'       /* mov al, 'I' */
    .byte 0xE6, 0x10      /* out 0x10, al */
    .byte 0xB0, '\n'      /* mov al, '\n' */
    .byte 0xE6, 0x10      /* out 0x10, al */
    .byte 0x48, 0xCF      /* iretq */
```

The handler prints 'I' via PIO and returns with `iretq`. `iretq` consumes the interrupt frame that the CPU previously pushed and restores execution state from it, resuming at the instruction after `hlt`.

### Guest: IDT entry for vector 32

```asm
idt:
    .fill 64, 8, 0                /* vectors 0-31: null (512 bytes) */
    /* Vector 32: interrupt gate → irq_handler */
    .word irq_handler             /* offset_low */
    .word 0x18                    /* selector: 64-bit code segment */
    .byte 0x00                    /* IST = 0 (use current stack) */
    .byte 0x8E                    /* present=1, DPL=0, interrupt gate */
    .word 0x0000                  /* offset_mid = 0 */
    .long 0x00000000              /* offset_high = 0 */
    .long 0x00000000              /* reserved */
```

The IDT position determines the vector number. We fill 32 null entries (vectors 0–31) and place our handler at position 32.

For simplicity, the handler is located in low memory (below 64KB) so that the upper offset fields (`offset_mid`, `offset_high`) are zero. A general implementation must split the full 64-bit handler address across all three offset fields.

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
[PIO out port 0x10] I
Guest halted.
```

The 'I' confirms the interrupt was delivered and the handler executed.

## Key insight

Interrupt injection is the mechanism that makes device emulation **asynchronous**. Without interrupts, the guest would have to continuously poll MMIO registers to check for events. With interrupts, the VMM can notify the guest exactly when something happens.

### Polling (Step 6) vs interrupts (Step 7)

```
Polling:                          Interrupts:
  while (status == 0)               hlt  (CPU sleeps, zero power)
      read_mmio();                  ← interrupt arrives
  // wastes CPU cycles              handler runs immediately
```

Interrupts exist because polling scales poorly. A guest polling a device wastes CPU time and causes repeated VM exits. With interrupts, the guest sleeps (or does useful work) and is woken only when there is something to handle. This tradeoff — latency vs CPU efficiency — is central to I/O virtualization.

The VMM chooses the interrupt vector to inject. The guest decides what that vector means by populating its IDT. KVM is just the delivery mechanism — it has no knowledge of the IDT contents or handler code.

### The complete device model

With Steps 5–7, we now have all three components of a device model:

| Component | Step | Mechanism |
|-----------|------|-----------|
| Command/data to device | 5 | MMIO write |
| Status/data from device | 6 | MMIO read |
| Asynchronous notification | 7 | Interrupt injection |

This is the pattern every real device follows: the driver writes commands, reads status, and receives interrupts when events occur.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| KVM_INTERRUPT | Host → guest notification via vector number |
| IDT | Maps vector numbers to handler addresses |
| Interrupt gate | Saves state, jumps to handler, clears IF |
| iretq | Restores RIP/CS/RFLAGS/RSP/SS from stack |
| Stack requirement | CPU pushes return state - RSP must be valid |
| Asynchronous notification | VMM decides when to interrupt the guest |

## Next step

[Step 8: MSR handling](step8_msr.md) - trap and emulate Model-Specific Register accesses using `KVM_EXIT_X86_WRMSR` / `KVM_EXIT_X86_RDMSR`, completing the set of VM exit types.
