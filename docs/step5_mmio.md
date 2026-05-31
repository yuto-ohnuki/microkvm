# Step 5: MMIO device emulation

## Goal

Emulate a memory-mapped device by creating a **hole** in guest physical address space. When the guest writes to an address with no backing memory, KVM delivers `KVM_EXIT_MMIO` to the VMM - enabling device emulation through ordinary memory access instructions.

## Background

### From PIO to MMIO

In Step 2, the guest communicated with the host via Port I/O (`out` instruction). PIO uses a separate 16-bit address space and dedicated instructions. MMIO (Memory-Mapped I/O) is different: device registers appear as **normal memory addresses** in the guest physical address space. The guest accesses them with regular load/store instructions (`mov`).

Most modern devices use MMIO rather than PIO because:
- The address space is much larger (64-bit vs 16-bit)
- Standard memory instructions work - no special `in`/`out` needed
- Cache coherency and ordering can be controlled per-page

### How MMIO trapping works

In Step 4, we registered guest memory with `KVM_SET_USER_MEMORY_REGION`. KVM uses these memory slots to determine which guest physical addresses have backing host memory. When the guest accesses a GPA that is not backed by a registered memory slot, KVM treats the access as MMIO and exits to userspace:

```
Guest executes:  mov [0xD0000], al
                        │
                        ▼
            Guest page table lookup
            present=1 (identity mapped)
                        │
                        ▼
            Guest physical address: 0xD0000
                        │
                        ▼
            Memory slot lookup: no backing RAM
                        │
                        ▼
            KVM cannot resolve the access
            through normal guest memory
                        │
                        ▼
            KVM_EXIT_MMIO delivered to userspace
                        │
                        ▼
            microkvm device model handles it
```

This is the mechanism that real hypervisors use to emulate device registers.
QEMU's device models (virtio, e1000, AHCI, etc.) all receive MMIO accesses this way.

### PIO vs MMIO comparison

| | PIO | MMIO |
|---|---|---|
| Guest instruction | `out` / `in` (dedicated) | `mov` (regular load/store) |
| Address space | 16-bit port number | Guest physical address space |
| Trap mechanism | I/O instruction interception | Access to GPA without RAM backing |
| Data location in kvm_run | `run + run->io.data_offset` | `run->mmio.data[8]` directly |
| Real-world usage | Legacy (serial, PIC, PIT) | Modern devices (NIC, GPU, NVMe) |

### run->mmio fields

When `exit_reason == KVM_EXIT_MMIO`:

| Field | Meaning |
|-------|---------|
| `run->mmio.phys_addr` | Guest physical address accessed |
| `run->mmio.data[8]` | Data written (for writes) or buffer to fill (for reads) |
| `run->mmio.len` | Access width (1, 2, 4, or 8 bytes) |
| `run->mmio.is_write` | 1 = guest wrote to the address, 0 = guest read |

Unlike PIO where data is at an offset, MMIO data is embedded directly in the `run->mmio` struct.

## Execution flow

```
VMM (microkvm.c)                         Guest (guest.S)
────────────────                         ───────────────
Register memory slots:
  slot 0: GPA 0x00000–0xCFFFF (RAM)
  slot 1: GPA 0xD1000–0xFFFFF (RAM)
  [hole]: GPA 0xD0000–0xD0FFF (no slot)
                                         [real mode] 'R' via PIO
                                         [protected mode] 'P' via PIO
                                         [long mode]
                                           mov rbx, 0xD0000
                                           mov [rbx], al  ← store to MMIO hole
                                                │
                                                ▼
KVM_EXIT_MMIO                            (guest paused)
  phys_addr = 0xD0000
  is_write = 1
  data[0] = 'M'
printf("[MMIO write] M")
ioctl(KVM_RUN)                           ← guest resumes
                                           hlt
KVM_EXIT_HLT
```

## Implementation

### VMM: creating the MMIO hole

In Step 4, we registered one contiguous memory slot covering all of guest memory.
In Step 5, we split it into two slots with a gap:

```c
/* Slot 0: GPA 0x00000 – 0xCFFFF (832 KB) */
struct kvm_userspace_memory_region region1 = {
    .slot = 0,
    .guest_phys_addr = 0,
    .memory_size = 0xD0000,
    .userspace_addr = (unsigned long)mem,
};

/* Slot 1: GPA 0xD1000 – 0xFFFFF (188 KB) */
struct kvm_userspace_memory_region region2 = {
    .slot = 1,
    .guest_phys_addr = 0xD1000,
    .memory_size = GUEST_MEM_SIZE - 0xD1000,
    .userspace_addr = (unsigned long)mem + 0xD1000,
};

/* GPA 0xD0000 – 0xD0FFF: no slot registered → MMIO */
```

The host-side `mmap` is still one contiguous 1MB allocation. We simply tell KVM about two non-contiguous ranges, leaving a 4KB gap that becomes our MMIO device address.

```
Guest Physical Address space:

0x00000              0xCFFFF  0xD0000  0xD0FFF  0xD1000              0xFFFFF
┌────────────────────┬────────────────────┬────────────────────────────────┐
│   slot 0 (RAM)     │   MMIO hole (4KB)  │   slot 1 (RAM)                 │
│   normal access    │   no backing store │   normal access                │
│   → no exit        │   → KVM_EXIT_MMIO  │   → no exit                    │
└────────────────────┴────────────────────┴────────────────────────────────┘
```

### VMM: handling KVM_EXIT_MMIO

```c
case KVM_EXIT_MMIO:
    if (run->mmio.phys_addr == 0xD0000 && run->mmio.is_write) {
        char c = run->mmio.data[0];
        if (c != '\n')
            printf("[MMIO write @ 0x%llx] %c\n", run->mmio.phys_addr, c);
    }
    break;
```

The data is directly available in `run->mmio.data[]` - no offset calculation needed (unlike PIO's `data_offset`).

### Guest: writing to the MMIO address

In long mode, the guest stores a byte to GPA 0xD0000 using a regular `mov`:

```asm
long_mode:
    .byte 0x48, 0xC7, 0xC3, 0x00, 0x00, 0x0D, 0x00     /* mov rbx, 0xD0000 */
    .byte 0xB0, 'M'                                    /* mov al, 'M' */
    .byte 0x88, 0x03                                   /* mov [rbx], al */
```

Notice that the instruction itself contains no indication that this is a device access. The CPU executes an ordinary store instruction. Whether the address refers to RAM or a device is determined entirely by the address translation layers.

## Output

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
[PIO out port 0x10] R
[PIO out port 0x10] P
[MMIO write @ 0xd0000] M
Guest halted.
```

The output now shows both PIO and MMIO exits, making the two mechanisms visible side by side.

## Why not use the guest page table for MMIO?

A natural question: could we mark GPA 0xD0000 as "not present" in the guest page table instead?

No - that would cause a **guest-internal page fault** (#PF), not an MMIO exit. KVM would inject #PF into the guest, and since we have no IDT, this leads to a triple fault.

MMIO trapping operates at the **memory slot layer** (second-stage translation), not at the guest page table layer. The guest page table says "this address is valid" (present=1 in our identity map), but there is no memory slot backing it - that's what triggers the exit to userspace.

```
Guest page table:  GPA 0xD0000   → present (identity mapped)
Memory slots:      GPA 0xD0000   → no slot registered
Result:            KVM_EXIT_MMIO → userspace handles it
```

## Key insight

The guest cannot tell whether a particular address refers to RAM or a device register. Both are accessed with ordinary memory instructions. This abstraction is what allows operating systems to use the same load/store instructions for RAM, PCIe devices, framebuffers, and MMIO registers.

MMIO emulation is a direct consequence of the two-level translation introduced in Step 4. The VMM controls which guest physical addresses have backing memory and which don't. Addresses without backing memory become device registers - the VMM intercepts every access and emulates the device behavior.

### Real-world example

PCIe devices expose registers through MMIO regions called BARs (Base Address Registers). When a Linux driver writes to an MMIO register:

```c
writel(value, bar + offset);
```

the CPU executes an ordinary store instruction to a physical address. In this step, this store hits a memory slot hole and exits to the VMM for device emulation. Production VMMs use the same basic idea, although some devices may be handled in-kernel or accelerated through other mechanisms.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| MMIO | Device registers as guest physical addresses |
| Memory slot hole | Unregistered GPA range → KVM_EXIT_MMIO |
| run->mmio | phys_addr, data[], len, is_write — all in one struct |
| Transparent trapping | Guest uses normal `mov` — unaware of the trap |
| Second-stage vs guest PT | MMIO is a property of memory slots, not guest page tables |

## Next step

[Step 6: MMIO read + device state](step6_mmio-read.md) - add MMIO read support so the guest can query device state, completing the bidirectional device model.
