# Step 4: Protected mode → long mode (64-bit)

## Goal

Transition the guest CPU from 32-bit protected mode to 64-bit long mode.
This introduces **page tables**, **CR3**, **CR4.PAE**, and **EFER.LME** - and reveals the two-level address translation that is central to hardware virtualization.

## Background

### Long mode requirements

To enter 64-bit long mode, four conditions must be met in order:

1. **Page tables** set up in memory and pointed to by CR3
2. **CR4.PAE = 1** - extends page table entries to 64 bits
3. **EFER.LME = 1** (MSR 0xC0000080, bit 8) - declares intent to use long mode
4. **CR0.PG = 1** - enables paging

Enabling paging with PAE and LME causes the processor to activate long mode (EFER.LMA becomes set internally). However, 64-bit instruction decoding does not begin yet - the CPU is in **compatibility mode** (a sub-mode of long mode that still executes 32-bit code). A subsequent far jump into a code segment with **L=1** begins true 64-bit instruction execution.

### x86-64 page table hierarchy

Long mode normally uses a four-level hierarchy:

```
CR3 → PML4 → PDPT → PD → PT → 4KB page
```

In this step we terminate the walk early using a 2MB page (PS=1 in the PD entry), so the PT level is skipped:

```
CR3 → PML4 (Page Map Level 4)
        └→ PDPT (Page Directory Pointer Table)
              └→ PD (Page Directory)
                    └→ 2MB page (PS=1, translation ends here)
```

Each entry is 8 bytes. Since tables are 4KB-aligned, the lower 12 bits are available for flags:

| Bit | Flag | Meaning |
|-----|------|---------|
| 0 | P | Present — entry is valid |
| 1 | RW | Writable |
| 7 | PS | Page Size — terminates walk at this level |

### Identity mapping

We map virtual address 0 → physical address 0 (identity map). This is
essential: the code is already executing at low addresses, so after paging
is enabled, the same addresses must still resolve to the same physical
locations. Without identity mapping, the CPU would immediately fetch the
next instruction through a translation that does not exist, causing a
page fault.

```
Virtual 0x000000–0x1FFFFF  →  Physical 0x000000–0x1FFFFF  (first 2MB)
```

### Two-level address translation

With paging enabled inside the guest, two independent translations happen
on every memory access:

```
Guest virtual address
        │
        │  Guest page tables (built by guest, CR3)
        ▼
Guest physical address
        │
        │  Second-stage translation (built by KVM)
        ▼
Host physical address
```

| | Guest page tables | Second-stage (EPT/NPT) |
|---|---|---|
| Built by | Guest software (OS, bootloader, or VMM on behalf of guest) | KVM kernel module |
| Translates | Guest virtual → Guest physical | Guest physical → Host physical |
| Stored in | Guest memory (CR3 points here) | KVM internal structures |
| Format | x86 page tables | Hardware-defined nested page tables (EPT/NPT) |

The hardware performs both translations transparently as part of address resolution. This is why hardware virtualization can run guests with paging enabled without trapping every memory operation.

This separation is what allows KVM to present a virtual machine with its own "physical" memory layout while backing it with arbitrary host memory. Later steps build on this: MMIO holes (Step 5), dirty page tracking, snapshots, and live migration all operate at the second-stage translation layer.

### Why the VMM sets EFER.LME

In a real boot flow, the kernel would execute `wrmsr` to set EFER.LME.
To keep the guest focused on paging and mode transitions, we initialize EFER.LME from the VMM as part of vCPU setup. This is the same approach QEMU takes when preparing initial vCPU state.

## Execution flow

```
VMM (microkvm.c)                         Guest (guest.S)
────────────────                         ───────────────
Write page tables in guest memory:
  0x70000: PML4[0] → 0x71003 (P+RW)
  0x71000: PDPT[0] → 0x72003 (P+RW)
  0x72000: PD[0]   → 0x00083 (P+RW+PS)

KVM_SET_SREGS: efer |= LME
                                         [real mode]
                                           'R' via PIO
                                           lgdt, CR0.PE=1, far jump
                                         [protected mode]
                                           'P' via PIO
                                           mov $0x70000, %cr3
                                           CR4.PAE = 1
                                           CR0.PG = 1  ← long mode (compatibility)
                                           far jump (selector 0x18, L=1)
                                         [long mode, 64-bit]
                                           'L' via PIO
                                           hlt
```

As in Step 3, the mode transition happens entirely within the guest. The VMM only sees PIO exits for character output and the final HLT.

## Implementation

### VMM: page table setup

```c
/* Page tables at GPA 0x70000 (well above guest code at 0x0) */
uint64_t *pml4 = (uint64_t *)((char *)mem + 0x70000);
uint64_t *pdpt = (uint64_t *)((char *)mem + 0x71000);
uint64_t *pd   = (uint64_t *)((char *)mem + 0x72000);

pml4[0] = 0x71000 | 0x3;   /* present + writable, points to PDPT */
pdpt[0] = 0x72000 | 0x3;   /* present + writable, points to PD */
pd[0]   = 0x0     | 0x83;  /* present + writable + PS (2MB huge page at phys 0) */
```

Each entry combines a physical address (upper bits) with flags (lower 12 bits). The address must be 4KB-aligned, which is why the lower bits are free for flags.

Why the VMM builds the page tables (not the guest): our guest binary is a flat binary loaded at GPA 0. It has no dynamic memory allocator. The VMM writes the tables into known locations in guest memory before execution starts. A real bootloader or OS kernel would build its own page tables.

### VMM: EFER.LME

```c
sregs.efer |= (1 << 8);   /* LME = Long Mode Enable */
ioctl(vcpufd, KVM_SET_SREGS, &sregs);
```

### Guest: transition sequence (in protected mode)

```asm
    /* Point CR3 to the page table root */
    mov $0x70000, %eax
    mov %eax, %cr3

    /* Enable PAE — required prerequisite for long mode */
    mov %cr4, %eax
    or $0x20, %eax          /* bit 5 = PAE */
    mov %eax, %cr4

    /* Enable paging → long mode activates (EFER.LME already set) */
    mov %cr0, %eax
    or $0x80000000, %eax    /* bit 31 = PG */
    mov %eax, %cr0

    /* Far jump to 64-bit code segment (GDT[3], selector 0x18) */
    ljmpl *far_ptr
```

After `CR0.PG = 1`, the CPU is in long mode compatibility sub-mode (still executing 32-bit code). The far jump loads CS with a descriptor where L=1, which switches the CPU to 64-bit instruction decoding.

### Guest: 64-bit code (manually encoded)

GNU `as` is running in 32-bit mode (`--32`), so we cannot directly emit 64-bit assembly. These instructions have identical encodings in both modes, allowing us to demonstrate long mode execution without switching toolchains:

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

The key difference from the 32-bit descriptors in Step 3:
- **L=1** — tells the CPU this is a 64-bit code segment
- **D=0** — required when L=1 (D and L cannot both be 1)
- Base and limit are ignored in long mode

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

Three characters confirm all three CPU modes executed successfully: Real → Protected → Long.

## Key insight

This is the first step where the guest manages its own virtual memory.
From now on, the guest no longer executes using physical addresses directly. Every instruction fetch and memory access goes through guest-controlled page tables.

The guest is unaware of the second translation layer. It believes it owns physical memory starting at address 0. The hardware transparently translates guest physical addresses to actual host memory through the nested page tables that KVM maintains.

This two-level model is the foundation for everything that follows:
- **Step 5**: MMIO holes in guest physical space trigger exits for device emulation
- **Steps 15–18**: Dirty page tracking and live migration operate at the second stage
- **Steps 12–14**: Virtio uses shared guest physical memory for zero-exit data transfer

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| Page tables | 3-level identity map with 2MB huge page (PT level skipped) |
| CR3 | Points to PML4 - root of guest address translation |
| CR4.PAE | Prerequisite for long mode (64-bit page table entries) |
| EFER.LME + LMA | LME declares intent; LMA activates when PG is set |
| Compatibility mode | After PG=1 but before far jump - still 32-bit execution |
| Two-level translation | Guest PT + second-stage (EPT/NPT) - hardware walks both |
| Identity mapping | Required so code survives the paging-enable moment |
| Far jump (L=1) | Reloads CS with 64-bit descriptor to begin 64-bit execution |

### Linux kernel parallel

The Linux kernel performs the same sequence during early boot:
1. Build temporary identity-mapped page tables
2. Load CR3
3. Enable CR4.PAE
4. Set EFER.LME
5. Enable CR0.PG
6. Far jump into 64-bit code

microkvm follows the same architectural requirements, but uses the smallest possible page table setup (one PML4, one PDPT, one PD with a 2MB huge page).

## Next step

[Step 5: MMIO device emulation](step5_mmio.md) - split the memory region to create a hole in guest physical address space, and handle `KVM_EXIT_MMIO` for device emulation.
