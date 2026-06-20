# Step 3: Real mode → protected mode

## Goal

Transition the guest CPU from 16-bit real mode to 32-bit protected mode.
This introduces the **GDT** (Global Descriptor Table), **CR0.PE**, and the **far jump** - the same sequence the Linux kernel performs during early boot.

## Background

### x86 CPU modes

```
Real mode (16-bit)  →  Protected mode (32-bit)  →  Long mode (64-bit)
     ↑ power-on            ↑ CR0.PE=1                  ↑ CR0.PG=1 + EFER.LME
```

In Steps 1 and 2, the vCPU ran in real mode (CS.base=0, RIP=0, no GDT).
To use 32-bit instructions and flat memory addressing, we must switch to protected mode.

### GDT (Global Descriptor Table)

In protected mode, every memory access goes through a **segment descriptor**.
The GDT is a table of these descriptors. Each one defines:
- Base address (where the segment starts)
- Limit (how large it is)
- Type (code or data, readable/writable)
- Privilege level (ring 0–3)

```
GDT[0] = null descriptor     (required by CPU - index 0 is always invalid)
GDT[1] = code: base=0, limit=4GB, 32-bit, execute/read
GDT[2] = data: base=0, limit=4GB, 32-bit, read/write
```

### Why does Linux still need a GDT?

Modern operating systems use paging rather than segmentation for memory isolation. However, the x86 architecture requires valid segment descriptors before protected mode can be entered.

Linux therefore uses a **flat memory model**:

```
code: base=0, limit=4GB
data: base=0, limit=4GB
```

Segmentation remains enabled architecturally, but becomes effectively invisible to software. The GDT is a mandatory gate you must pass through to reach protected mode - even if you immediately rely on paging for everything else.

### Segment selectors

A selector is a byte offset into the GDT. Each entry is 8 bytes, so:
- Selector `0x08` → GDT[1] (code)
- Selector `0x10` → GDT[2] (data)

The CPU uses the selector to look up the descriptor and cache its fields internally (base, limit, type). This cached copy is called the **descriptor cache** - the hidden part of the segment register that persists until explicitly reloaded.

### Why a far jump is needed

Setting `CR0.PE = 1` enables protected mode, but the CPU's descriptor cache for CS still holds the old real-mode values. A **far jump** (`ljmp $selector, $offset`) forces the CPU to:
1. Load the new selector into CS
2. Read the corresponding GDT entry
3. Update the descriptor cache with 32-bit attributes

Without this, the CPU would continue fetching instructions as 16-bit despite being in protected mode.

A far jump is the most common mechanism used during boot to reload CS and complete the transition. (Far calls, `iret`, and task switches can also reload CS, but `ljmp` is the standard choice for mode transitions.)

## Execution flow

```
Guest (real mode, .code16)                Guest (protected mode, .code32)
──────────────────────────                ────────────────────────────────
mov $'R', %al
out %al, $0x10             → 'R'
lgdt gdt_desc              [GDT registered]
mov %cr0, %eax
or $1, %eax
mov %eax, %cr0             [PE=1, protected mode enabled]
ljmp $0x08, $protected_mode ────────────→ mov $0x10, %ax
                                          mov %ax, %ds
                                          mov %ax, %ss
                                          mov $'P', %al
                                          out %al, $0x10  → 'P'
                                          hlt
```

Note: `lgdt`, `mov %cr0`, and `ljmp` do **not** cause VM exits. These execute directly inside the guest because the guest kernel runs with CPL=0 inside VMX non-root mode, allowing it to modify its own control registers and descriptor tables. Only the `out` and `hlt` instructions cause exits (as in Step 2).

## Implementation

### Build pipeline

From this step onward, guest code lives in a separate assembly file:

```
guest.S  →  as --32  →  guest.o  →  ld --oformat binary  →  guest.bin
                                         (flat binary, no ELF headers)
```

The VMM loads `guest.bin` at GPA 0 at runtime. The `--32` flag tells the assembler to produce 32-bit code (with `.code16` sections handled correctly). `ld --oformat binary` strips all ELF metadata - the output is raw machine code starting at address 0.

### Guest assembly (guest.S)

```asm
.code16
.global _start
_start:
    /* Real mode: output 'R' via PIO */
    mov $'R', %al
    out %al, $0x10
    mov $'\n', %al
    out %al, $0x10

    /* Load GDT */
    lgdt gdt_desc

    /* Enable protected mode: set CR0.PE = 1 */
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0

    /* Far jump to reload CS with 32-bit code segment */
    ljmp $0x08, $protected_mode

.code32
protected_mode:
    /* Set data segments to GDT[2] */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %ss

    /* Protected mode: output 'P' via PIO */
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
    .quad 0x00CF9A000000FFFF    /* GDT[1] (0x08): code, 32-bit, exec/read */
    .quad 0x00CF92000000FFFF    /* GDT[2] (0x10): data, 32-bit, read/write */

gdt_desc:
    .word gdt_desc - gdt - 1   /* limit (size of GDT - 1) */
    .long gdt                   /* base address of GDT */
```

### VMM: loading guest.bin from file

The file loader lives in `boot.c`:

```c
/* Load a flat binary into guest memory at GPA 0x0 */
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

The VMM (`microkvm.c`) calls `load_guest("guest.bin", mem)`. The exit handler loop remains the same as Step 2.

## Output

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
R
P
Guest halted.
```

## Key insight

Protected mode is not entered by the hypervisor. The guest modifies its own CPU state and performs the transition entirely inside guest execution.

From the VMM's perspective, nothing special happened - the only exits remain the PIO operations and the final HLT. This continues the pattern from Step 2: the hypervisor is not constantly involved in guest execution.

microkvm and Linux are doing the same thing here: load a GDT, enable CR0.PE, and reload CS with a far jump. The difference is only what happens next - Linux continues to set up paging and drivers, while our guest simply prints a character and halts.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| GDT | Flat-model descriptors for code and data segments |
| CR0.PE | Single bit that switches the CPU to protected mode |
| Far jump | Reloads CS descriptor cache to complete the mode transition |
| Segment selectors | `0x08` for code, `0x10` for data — byte offsets into GDT |
| Separate guest binary | Assembly → flat binary → loaded by VMM at runtime |
| Guest autonomy | Mode transitions happen without VM exits |

## Appendix: Decoding a GDT descriptor

The 8-byte value `0x00CF9A000000FFFF` encodes:

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
  Limit = 0xFFFFF × 4KB (G=1) = 4GB
  Type  = 0x9A = code, execute/read, ring 0
  D/B   = 1 → 32-bit default operand/address size
  G     = 1 → limit granularity is 4KB pages
```

## Next step

[Step 4: Protected → long mode](step04_long-mode.md) — set up page tables, enable PAE and paging, and transition to 64-bit long mode.
