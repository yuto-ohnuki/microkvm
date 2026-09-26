# Step 3: Real Mode → Protected Mode

## Goal

Transition the guest CPU from 16-bit real mode to 32-bit protected mode.
Learn how the guest switches modes itself using the **GDT** (Global Descriptor Table), **CR0.PE**, and a **far jump**.

## Background

### x86 CPU modes

```
Real mode (16-bit)  →  Protected mode (32-bit)  →  Long mode (64-bit)
    Steps 1–2                 This step                 Step 4
```

In Steps 1 and 2, the vCPU ran in real mode (CR0.PE=0). Here, we set CR0.PE to 1 and enter a 32-bit code segment. Although some 32-bit instructions are available in real mode, this step switches the default operand and address sizes to 32 bits.

### GDT (Global Descriptor Table)

In protected mode, **segment descriptor** settings govern instruction and data accesses.
The GDT is a table of these descriptors. Each entry defines:
- Base address (start of the segment)
- Limit (maximum accessible offset)
- Type (code or data, read/write permissions)
- Privilege level (ring 0–3)

```
GDT[0] = Null descriptor (not used as a valid segment)
GDT[1] = Code: base=0, limit=0xFFFFFFFF (4 GiB range), 32-bit, execute/read
GDT[2] = Data: base=0, limit=0xFFFFFFFF (4 GiB range), 32-bit, read/write
```

### Flat memory model

This guest sets both code and data segments to base=0 and limit=0xFFFFFFFF. Using the same address range this way is called a **flat memory model**; adding the segment base does not change the address.

A 4 GiB segment range does not increase guest RAM to 4 GiB. The VMM still registers 1 MiB of memory, as in Step 2, and guest paging remains disabled in this step.

### Segment selectors

A selector is a 16-bit value containing a descriptor index, a GDT/LDT selection, and a requested privilege level (RPL). This guest selects the GDT with RPL=0, so the value is the index × 8:
- Selector `0x08` → GDT[1] (code)
- Selector `0x10` → GDT[2] (data)

The CPU uses the selector to look up a descriptor and caches its fields (base, limit, type) internally. This copy is the **descriptor cache**, the hidden part of a segment register, and remains until explicitly reloaded.

### Why a far jump is necessary

Setting `CR0.PE = 1` enables protected mode, but the CS descriptor cache still holds its old real-mode values. A **far jump** (`ljmp $selector, $offset`) forces the CPU to:
1. Load the new selector into CS
2. Read the corresponding GDT entry
3. Update the descriptor cache with 32-bit attributes

Without this, the default operand and address sizes remain 16 bits.

A far jump is the most common mechanism for reloading CS during boot to complete the transition. (A far call, `iret`, or task switch can also reload CS, but `ljmp` is the standard choice for a mode transition.)

## Execution flow

```
Guest: real mode (.code16)                      Guest: protected mode (.code32)
──────────────────────────                      ───────────────────────────────
Output 'R' and newline via PIO
lgdt gdt_desc
CR0.PE = 1
ljmp $0x08, $protected_mode ──→                 Load 0x10 into DS / SS
                                                Output 'P' and newline via PIO
                                                hlt
```

Each character and newline uses a separate `out`, followed by another `KVM_RUN` call. The VMM receives four `KVM_EXIT_IO` returns and one `KVM_EXIT_HLT` return.

Guest code directs the mode transition; no dedicated VMM handler is added. Whether control-register operations and similar instructions cause hardware VM exits depends on the CPU and KVM settings. Distinguish processing inside KVM from `KVM_RUN` returning to userspace.

## Implementation

Add `guest.S` and `boot.c` / `boot.h`, and replace the inline byte array in `microkvm.c` with file loading. Add the guest binary build to the `Makefile`.

### Build pipeline

From this step onward, guest code lives in a separate assembly file:

```
guest.S → as --32 → guest.o → ld -m elf_i386 -Ttext 0x0 --oformat binary → guest.bin
```

At runtime, the VMM loads `guest.bin` at guest physical address (GPA) 0. `as --32` generates a 32-bit ELF object; `.code16` / `.code32` select instruction encoding. These directives do not change the CPU's operating mode.

The `ld` option `-Ttext 0x0` places code at address 0, and `--oformat binary` produces a flat binary without an ELF header. Matching the link address to the actual load address makes GDT and jump-target references work correctly.

### Guest assembly (guest.S)

```asm
.code16
.global _start
_start:
    /* Real mode: output 'R' through PIO */
    mov $'R', %al
    out %al, $0x10
    mov $'\n', %al
    out %al, $0x10

    /* Load the GDT */
    lgdt gdt_desc

    /* Enable protected mode: set CR0.PE = 1 */
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0

    /* Far jump to reload CS with the 32-bit code segment */
    ljmp $0x08, $protected_mode

.code32
protected_mode:
    /* Set data segments to GDT[2] */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %ss

    /* Protected mode: output 'P' through PIO */
    mov $'P', %al
    out %al, $0x10
    mov $'\n', %al
    out %al, $0x10

    hlt
```

### GDT data

```asm
.align 8
gdt:
    .quad 0                     /* GDT[0]: null */
    .quad 0x00CF9A000000FFFF    /* GDT[1] (0x08): code, 32-bit, execute/read */
    .quad 0x00CF92000000FFFF    /* GDT[2] (0x10): data, 32-bit, read/write */

gdt_desc:
    .word gdt_desc - gdt - 1    /* Limit (GDT size - 1) */
    .long gdt                   /* GDT base address */
```

### VMM: Load guest.bin from a file

The file loader goes in `boot.c`, with its declaration in `boot.h`. This excerpt omits error handling for `open`:

```c
/* Load the flat binary at GPA 0x0 */
int load_guest(const char *path, void *mem) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    fstat(fd, &st);
    read(fd, mem, st.st_size);
    close(fd);
    printf("Loaded guest: %ld bytes\n", st.st_size);
    return 0;
}
```

The VMM (`microkvm.c`) calls `load_guest("guest.bin", mem)`. The exit handler loop is unchanged from Step 2.

## Output

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
R
P
Guest halted.
```

`R` is output in real mode, and `P` after entering protected mode. `Guest halted.` is printed by the VMM after it receives the final `hlt`.

## Key insight

The guest prepares a GDT, sets CR0.PE, and uses a far jump to enter a 32-bit code segment. The VMM's exit handler is unchanged from Step 2 and needs no dedicated mode-transition handling. The guest changing its own CPU state and possible VM exits inside KVM are separate matters.

## What this step teaches

| Concept | How it appears here |
|------|--------------|
| GDT | Flat-model descriptors for code and data segments |
| CR0.PE | A single bit that switches the CPU into protected mode |
| Far jump | Reload the CS descriptor cache to complete the mode transition |
| Segment selectors | `0x08` for code, `0x10` for data—GDT selected, RPL=0 |
| Separate guest binary | Assembly → flat binary → loaded by the VMM at runtime |
| Guest autonomy | Guest code switches modes without changes to the VMM's exit handler |

## What changed

- Move guest code from a C byte array to `guest.S`; add building and loading of `guest.bin`.
- Add the GDT, CR0.PE setup, far jump, and DS/SS updates to the guest.
- The VMM's exit handler is unchanged from Step 2.

## Appendix: Decoding a GDT descriptor

Encoding of the 8-byte value `0x00CF9A000000FFFF`:

```
 63       56 55 52 51 48 47       40 39       32
┌───────────┬─────┬──────┬──────────┬──────────┐
│ Base 31:24│Flags│Lim   │  Access  │Base 23:16│
│   0x00    │ C   │19:16 │  0x9A    │  0x00    │
│           │(G=1 │ 0xF  │          │          │
│           │ D=1)│      │          │          │
└───────────┴─────┴──────┴──────────┴──────────┘
 31                16 15                 0
┌────────────────────┬────────────────────┐
│   Base 15:0        │   Limit 15:0       │
│   0x0000           │   0xFFFF           │
└────────────────────┴────────────────────┘

Decoded:
  Base  = 0x00000000
  Limit = (0xFFFFF << 12) | 0xFFF = 0xFFFFFFFF (G=1)
          Maximum offset is 4 GiB - 1; segment range is 4 GiB
  Access = 0x9A = present, code, execute/read, ring 0
  D/B   = 1 → 32-bit default operand/address size
  G     = 1 → Limit granularity is 4KB pages
```

## Next step

[Step 4: Protected Mode → Long Mode](step04_long-mode.md)—Set up page tables, enable PAE and paging, and transition to 64-bit long mode.
