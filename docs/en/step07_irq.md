# Step 7: Interrupt Injection (IRQ)

## Goal

Deliver an interrupt **from host to guest** using `KVM_INTERRUPT`.
Use the **IDT** (Interrupt Descriptor Table), an interrupt gate, and `iretq` to learn how the guest runs an interrupt handler and returns to its interrupted code.

## Background

### A new communication direction

PIO and MMIO in Steps 2–6 were mechanisms where the guest initiated communication by executing an instruction.

Interrupts provide notification without waiting for the guest to read the device. Instead of waiting for an external event, this step injects one interrupt from the VMM on the first `KVM_EXIT_HLT`.

### Communication directions so far

| Step | Direction | Mechanism |
|------|------|-----------|
| 2 | Guest → Host | PIO (`out` → `KVM_EXIT_IO`) |
| 5 | Guest → Host | MMIO write (`mov [addr], val` → `KVM_EXIT_MMIO`) |
| 6 | Guest ↔ Host | MMIO read (guest requests, VMM responds with a value) |
| **7** | **Host → Guest** | **Interrupt injection (`KVM_INTERRUPT`)** |

Repeatedly reading MMIO to check for a state change is called polling. An interrupt is a host-initiated notification, delivered when the guest is ready to accept it.

### IDT (Interrupt Descriptor Table)

The IDT maps interrupt vector numbers (0–255) to handler addresses.
When an interrupt arrives, the CPU:
1. Looks up `IDT[vector]`
2. In 64-bit mode, saves SS, RSP, RFLAGS, CS, and RIP to the stack in that order
3. For an interrupt gate, clears IF to inhibit further delivery of maskable external interrupts
4. Jumps to the handler address

The handler runs and uses `iretq` to restore the saved state and return to the interrupted code.

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

For historical reasons, the handler address is split across three fields: offset_low, offset_mid, and offset_high.

### KVM_INTERRUPT

```c
struct kvm_interrupt irq = { .irq = 32 };
ioctl(vcpufd, KVM_INTERRUPT, &irq);
```

`irq=32` is an IDT vector number, not an interrupt-line number. This step does not create an in-kernel interrupt controller; it uses `KVM_INTERRUPT` to make vector 32 pending. On the next `KVM_RUN`, it is delivered when IF (the interrupt-enable flag in RFLAGS) is 1 and the guest is outside an interrupt shadow.

KVM_INTERRUPT is a simple mechanism suited to learning. Production VMMs normally deliver interrupts through virtual LAPICs, IOAPICs, irqfd, or MSI/MSI-X—topics covered in later steps.

### Why the guest needs a stack

When an interrupt is delivered, the CPU pushes an interrupt frame onto the stack.
The guest must have a valid RSP before enabling interrupts. In 64-bit mode, the interrupt frame contains SS, RSP, RFLAGS, CS, and RIP. Without a valid stack pointer, the push faults. This step sets `RSP = 0x60000` before enabling interrupts.

### Why `sti; hlt`?

When `sti` changes IF from 0 to 1, as in this guest, delivery of maskable external interrupts is inhibited until the next instruction completes (the **interrupt shadow**).
The sequence:

```asm
sti
hlt
```

avoids the race where the guest handles an interrupt just before executing `hlt`, then halts waiting for another notification. In this step, after `hlt` returns `KVM_EXIT_HLT`, the VMM requests an interrupt and runs the guest again.

## Execution flow

```
Guest                         KVM                         VMM (microkvm)
─────                         ───                         ──────────────
Long mode:
  RSP = 0x60000
  MMIO write 'M' and newline, read → 2 (as in Step 6)
  lidt [idt_desc]
  sti
  hlt ──────────────────────→ KVM_EXIT_HLT ─────────────→ KVM_INTERRUPT (vector=32)
                                                          irq_injected = 1
Accept interrupt ←─────────── Deliver vector 32 ←──────── KVM_RUN
  Save return state on the stack
  IF=0, IDT[32] → irq_handler
  out 'I' and newline ───────→ KVM_EXIT_IO (one each) ───→ Print 'I' and resume
  iretq
  Return to the instruction after the first hlt
  hlt ──────────────────────→ KVM_EXIT_HLT ─────────────→ irq_injected == 1
                                                          Print Guest halted. and stop
```

## Implementation

Update HLT handling in `microkvm.c` and modify `guest.S`. PIO and MMIO handling remain as in Step 6.

### VMM: Inject an interrupt on HLT

The following is an excerpt from `main` (error handling for `KVM_INTERRUPT` omitted).

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

On the first `KVM_EXIT_HLT`, request an interrupt and run the guest again through `KVM_RUN` at the top of the loop. Treating the second HLT as completion is a convention of this VMM. `irq_injected` records whether injection has been requested.

### Guest: Set up the IDT and wait for an interrupt

```asm
    /* Set up the stack (required for interrupt delivery) */
    .byte 0x48, 0xC7, 0xC4, 0x00, 0x00, 0x06, 0x00  /* mov rsp, 0x60000 */

    /* Load the IDT */
    .byte 0x48, 0xC7, 0xC1                          /* mov rcx, imm32 */
    .long idt_desc
    .byte 0x0F, 0x01, 0x19                          /* lidt [rcx] */

    /* Enable interrupts and wait */
    .byte 0xFB                                      /* sti */
    .byte 0xF4                                      /* hlt */

    /* After iretq returns here */
    .byte 0xF4                                      /* hlt (done) */
```

The actual code sets up the stack immediately after entering long mode, then executes `lidt` and `sti; hlt` after PIO and MMIO processing. The excerpt above shows only the interrupt-related parts.

### Guest: Interrupt handler

```asm
.align 16
irq_handler:
    .byte 0xB0, 'I'       /* mov al, 'I' */
    .byte 0xE6, 0x10      /* out 0x10, al */
    .byte 0xB0, '\n'      /* mov al, '\n' */
    .byte 0xE6, 0x10      /* out 0x10, al */
    .byte 0x48, 0xCF      /* iretq */
```

The handler outputs 'I' through PIO and returns with `iretq`. `iretq` consumes the interrupt frame previously pushed by the CPU, restores execution state from it, and resumes at the instruction after `hlt`.

`iretq` does not restore general-purpose registers such as RAX. This handler changes AL but omits saving it because only HLT follows the return. A handler must save and restore registers if the interrupted code needs their previous values.

### Guest: IDT entry for vector 32

```asm
.align 16
idt:
    .fill 64, 8, 0                /* Vectors 0-31: null (512 bytes) */
    /* Vector 32: interrupt gate → irq_handler */
    .word irq_handler             /* offset_low */
    .word 0x18                    /* selector: 64-bit code segment */
    .byte 0x00                    /* IST = 0 (use the current stack) */
    .byte 0x8E                    /* present=1, DPL=0, interrupt gate */
    .word 0x0000                  /* offset_mid = 0 */
    .long 0x00000000              /* offset_high = 0 */
    .long 0x00000000              /* reserved */

idt_desc:
    .word idt_desc - idt - 1      /* IDT size - 1 */
    .long idt                     /* Low 32 bits of the base address */
    .long 0                       /* High 32 bits of the base address */
```

An entry's position in the IDT determines its vector number. `.fill 64, 8, 0` fills 512 bytes, or 32 entries of 16 bytes each, so the next entry is vector 32. `lidt` reads the IDT limit and base address from the 10-byte `idt_desc`.

For simplicity, the handler is placed in low memory (below 64KB), so the upper offset fields (`offset_mid`, `offset_high`) are zero. A general implementation must split the full 64-bit handler address across all three offset fields.

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
[PIO out port 0x10] I
Guest halted.
```

`I` shows that the interrupt handler ran. The following `Guest halted.` shows that execution returned from the handler and reached the second HLT.

## Key insight

The VMM selects the vector to notify, and the guest defines its handler through the IDT. This step uses the first HLT as the trigger, verifying execution of the IDT[32] handler and return through `iretq`. This forms the basis of asynchronous notification for external events, though this step does not implement a timer or notifications from another thread.

### Difference from polling

Polling repeatedly reads state; interrupts trigger processing when a notification arrives. Interrupts reduce repeated work while waiting, but delivery and handler execution still have costs. HLT in this configuration returns to the VMM; it does not mean the host CPU sleeps or consumes no power.

## What this step teaches

| Concept | How it appears here |
|------|--------------|
| KVM_INTERRUPT | Host → guest notification by vector number |
| IDT | Maps vector numbers to handler addresses |
| Interrupt gate | Save state, jump to handler, clear IF |
| iretq | Restore RIP/CS/RFLAGS/RSP/SS from the stack |
| Stack requirement | CPU pushes return state—RSP must be valid |
| Notification timing | VMM requests injection on the first HLT; guest accepts it |

## What changed

- `microkvm.c`: Inject vector 32 on the first HLT and stop on the second.
- `guest.S`: Add a stack, IDT, `sti; hlt`, and a handler that outputs `I` and returns with `iretq`.

## Next step

[Step 8: MSR Handling](step08_msr.md)—Trap and emulate Model-Specific Register accesses with `KVM_EXIT_X86_WRMSR` / `KVM_EXIT_X86_RDMSR` to learn how MSR accesses are handled.
