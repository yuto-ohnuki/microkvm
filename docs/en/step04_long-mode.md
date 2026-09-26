# Step 4: Protected Mode → Long Mode (64-bit)

## Goal

Transition the guest CPU from 32-bit protected mode to 64-bit long mode.
Introduce **page tables**, **CR3**, **CR4.PAE**, and **EFER.LME**, and explain the two-stage address translation at the heart of hardware virtualization.

## Background

### Requirements for long mode

In this implementation, the VMM prepares the state, and the guest transitions from protected mode (CR0.PE=1, CR0.PG=0) in this order:

1. The **VMM** builds page tables in guest memory
2. The **VMM** initializes EFER.LME=1 (bit 8 of MSR 0xC0000080)
3. The **guest** sets CR3 to the page-table root address
4. The **guest** sets CR4.PAE=1 (required for long-mode paging)
5. The **guest** sets CR0.PG=1 to enable paging
6. The **guest** makes a far jump to a code segment with L=1 and D=0

Enabling paging with PAE and LME set activates long mode (EFER.LMA is set internally). However, 64-bit instruction decoding has not yet begun: the CPU is in **compatibility mode**, a long-mode submode that runs 32-bit code. The subsequent far jump to a code segment with **L=1** starts 64-bit instruction execution.

### x86-64 page-table hierarchy

Long mode normally uses a four-level hierarchy:

```
CR3 → PML4 → PDPT → PD → PT → 4KB page
```

This step uses 2MB pages (PS=1 in the PD entry) to terminate the walk early, skipping the PT level:

```
CR3 → PML4 (Page Map Level 4)
        └→ PDPT (Page Directory Pointer Table)
              └→ PD (Page Directory)
                    └→ 2MB page (PS=1, translation ends here)
```

Each entry is 8 bytes. Page tables are aligned to 4 KiB boundaries, and the base of a 2 MiB page must be aligned to a 2 MiB boundary. This step uses three flags:

| Bit | Flag | Meaning |
|-----|--------|------|
| 0 | P | Present—entry is valid |
| 1 | RW | Writable |
| 7 | PS | Page Size—set to 1 in the PD entry here to point to a 2 MiB page |

### Identity mapping

Mapping a guest virtual address to the same guest physical address is called identity mapping. This implementation maps the low-address region directly so that code, the GDT, and the far-jump pointer remain accessible at the same addresses immediately after paging is enabled.

```
Virtual 0x000000–0x1FFFFF → Physical 0x000000–0x1FFFFF (first 2MB)
```

The guest page tables map the first 2 MiB, but the VMM still registers only 1 MiB of RAM. Creating a page-table mapping does not add the corresponding guest RAM.

### Two-stage address translation

With EPT (Intel) / NPT (AMD), addresses are translated in two stages after guest paging is enabled:

```
Guest virtual address
        │
        │  Guest page tables (built by the guest, CR3)
        ▼
Guest physical address
        │
        │  Second-stage translation (built by KVM)
        ▼
Host physical address
```

| | Guest page tables | Second stage (EPT/NPT) |
|---|---|---|
| Built by | Guest software (OS, bootloader, or VMM on the guest's behalf) | KVM kernel module |
| Translation | Guest virtual → guest physical | Guest physical → host physical |
| Storage | Guest memory (CR3 points to the root) | Host memory managed by KVM |
| Format | x86 page tables | Hardware-defined nested page tables (EPT/NPT) |

Hardware performs both translations, so ordinary memory accesses do not need to return to the VMM. Translation results are cached in the TLB and elsewhere; the entire page-table hierarchy is not walked on every access.

This separation lets us manage guest-visible physical addresses independently of the host memory actually used. Step 5 leaves a region of guest physical address space without RAM and treats it as an MMIO device.

### Why the VMM sets EFER.LME

EFER is an MSR (model-specific register) and can also be set by the guest's `wrmsr` instruction. To keep the guest transition code short, this implementation initializes LME through `KVM_SET_SREGS` in the VMM before execution starts.

## Execution flow

```
VMM (microkvm)                                  Guest
──────────────                                  ─────
Build page tables:
  0x70000: PML4[0] = 0x71003
  0x71000: PDPT[0] = 0x72003
  0x72000: PD[0]   = 0x00083
KVM_SET_SREGS: EFER.LME = 1
KVM_RUN
                                                [Real mode]
                                                  Output 'R' and newline via PIO
                                                  lgdt, CR0.PE=1, far jump
                                                [Protected mode]
                                                  Output 'P' and newline via PIO
                                                  CR3 = 0x70000
                                                  CR4.PAE = 1
                                                  CR0.PG = 1 → compatibility mode
                                                  far jump (CS=0x18, L=1)
                                                [Long mode: 64-bit]
                                                  Output 'L' and newline via PIO
                                                  hlt
```

Each character and newline uses a separate `out`, followed by another `KVM_RUN` call. The VMM receives six `KVM_EXIT_IO` returns and one `KVM_EXIT_HLT` return. Distinguish VM exits handled inside KVM, such as those caused by control-register operations, from returns to userspace.

## Implementation

The changed files are `microkvm.c` and `guest.S`. `boot.c` / `boot.h`, the `Makefile`, and the VMM's exit handler are unchanged from Step 3.

### VMM: Set up page tables

```c
/* Place page tables at GPA 0x70000 (well above the guest code at GPA 0) */
uint64_t *pml4 = (uint64_t *)((char *)mem + 0x70000);
uint64_t *pdpt = (uint64_t *)((char *)mem + 0x71000);
uint64_t *pd   = (uint64_t *)((char *)mem + 0x72000);

pml4[0] = 0x71000 | 0x3;   /* present + writable, points to PDPT */
pdpt[0] = 0x72000 | 0x3;   /* present + writable, points to PD */
pd[0]   = 0x0     | 0x83;  /* present + writable + PS (2MB huge page at physical 0) */
```

`0x3` is P + RW; `0x83` is P + RW + PS. PML4[0] and PDPT[0] point to the next table, while PD[0] points to the 2 MiB page starting at GPA 0. The remaining entries stay invalid (P=0) because anonymous `mmap` initializes them to zero.

The VMM prepares page tables at fixed addresses to simplify the guest implementation. The guest could build them by writing to the same locations itself; a dynamic memory allocator is not required.

### VMM: EFER.LME

```c
sregs.efer |= (1 << 8);   /* LME = Long Mode Enable */
ioctl(vcpufd, KVM_SET_SREGS, &sregs);
```

### Guest: Transition sequence (in protected mode)

```asm
    /* Set CR3 to the page-table root */
    mov $0x70000, %eax
    mov %eax, %cr3

    /* Enable PAE—a prerequisite for long mode */
    mov %cr4, %eax
    or $0x20, %eax          /* bit 5 = PAE */
    mov %eax, %cr4

    /* Enable paging → activate long mode (EFER.LME is already set) */
    mov %cr0, %eax
    or $0x80000000, %eax    /* bit 31 = PG */
    mov %eax, %cr0

    /* Far jump to the 64-bit code segment (GDT[3], selector 0x18) */
    ljmpl *far_ptr
```

After `CR0.PG = 1`, the CPU is in long-mode compatibility submode (still executing 32-bit code). Loading a descriptor with L=1 into CS through the far jump switches the CPU to 64-bit instruction decoding.

`far_ptr` is a 6-byte object containing a 32-bit offset followed by a 16-bit selector. `ljmpl *far_ptr` reads it and enters the code segment defined by GDT[3].

```asm
.align 4
far_ptr:
    .long long_mode      /* Target offset */
    .word 0x18           /* Selector for GDT[3] */
```

### Guest: 64-bit code (manual encoding)

This implementation uses `.byte` to encode `mov al, imm8`, `out`, and `hlt` directly; their encodings are unchanged in 64-bit mode. This is an implementation choice, not a restriction preventing `as --32` from expressing 64-bit instructions. GNU `as` also supports switching instruction encoding with `.code64`.

```asm
long_mode:
    .byte 0xB0, 'L'     /* mov al, 'L' */
    .byte 0xE6, 0x10    /* out 0x10, al */
    .byte 0xB0, '\n'    /* mov al, '\n' */
    .byte 0xE6, 0x10    /* out 0x10, al */
    .byte 0xF4          /* hlt */
```

### GDT: 64-bit code segment

```asm
    .quad 0x00209A0000000000    /* GDT[3] (selector 0x18): 64-bit code */
    .quad 0x0000920000000000    /* GDT[4] (selector 0x20): 64-bit data */
```

The main differences from Step 3 in the GDT[3] code descriptor are:
- **L=1**—tells the CPU this is a 64-bit code segment
- **D=0**—required when L=1 (D and L cannot both be 1)
- This code segment's base and limit are ignored in 64-bit mode

GDT[4] is also added, but the guest in this step does not load selector `0x20`. DS/SS remain `0x10`, as in Step 3.

## Output

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
R
P
L
Guest halted.
```

The three characters confirm successful execution in all three CPU modes: real → protected → long.

## Key insight

In this step, the VMM prepares guest page tables and EFER.LME, and the guest enables paging and enters 64-bit code. Guest virtual → guest physical translation and KVM-managed guest physical → host physical translation serve different roles. Mapping 2 MiB in the guest does not increase the 1 MiB of RAM provided by the VMM.

## What this step teaches

| Concept | How it appears here |
|------|--------------|
| Page tables | Three-level identity map using a 2MB huge page (PT level skipped) |
| CR3 | Points to PML4—the root of guest address translation |
| CR4.PAE | Prerequisite for long mode (64-bit page-table entries) |
| EFER.LME + LMA | LME requests long mode; LMA becomes active when PG is set |
| Compatibility mode | After PG=1 but before the far jump—still 32-bit execution |
| Two-stage translation | Separate guest virtual → guest physical from guest physical → host physical |
| Identity mapping | Access the same code and data before and after enabling paging |
| Far jump (L=1) | Reload CS with a 64-bit descriptor to begin 64-bit execution |

## What changed

- `microkvm.c`: Add guest page-table construction and EFER.LME initialization.
- `guest.S`: Add CR3, CR4.PAE, and CR0.PG setup, a far-jump pointer, GDT entries, and output of `L`.
- The VMM's exit handler is unchanged from Step 3.

## Next step

[Step 5: MMIO Device Emulation](step05_mmio.md)—Split memory regions to create a hole in guest physical address space, and handle `KVM_EXIT_MMIO` for device emulation.
