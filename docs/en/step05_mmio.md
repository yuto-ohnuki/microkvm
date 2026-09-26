# Step 5: MMIO Device Emulation

## Goal

Emulate a memory-mapped device by creating a **hole** in guest physical address space. When the guest writes to an address without backing memory, KVM delivers `KVM_EXIT_MMIO` to the VMM, enabling device emulation through ordinary memory-access instructions.

## Background

### From PIO to MMIO

In Step 2, the guest communicated with the host through port I/O (`out`). PIO uses a separate 16-bit address space and dedicated instructions. MMIO (Memory-Mapped I/O) instead exposes device registers as **ordinary memory addresses** in guest physical address space. The guest accesses them with normal load/store instructions (`mov`).

Most modern devices use MMIO rather than PIO because:
- It uses a large physical address space without the 16-bit port-number limit
- Standard memory instructions work—no special `in`/`out` instructions are needed

MMIO cannot necessarily be cached freely like RAM. Real drivers must handle memory attributes and access ordering appropriate to the device.

### How MMIO trapping works

Since Step 1, we have registered the mapping between guest RAM and host memory with `KVM_SET_USER_MEMORY_REGION`. Here, we exclude GPA (guest physical address) 0xD0000–0xD0FFF from the registered ranges. When the guest writes there, KVM cannot handle the access as ordinary RAM and passes it to the VMM through `KVM_EXIT_MMIO`:

```
Guest executes: mov [0xD0000], al
                    │
                    ▼
        Guest page-table lookup
        present=1 (identity mapping)
                    │
                    ▼
        Guest physical address: 0xD0000
                    │
                    ▼
        Memory slot lookup: no backing RAM
                    │
                    ▼
        KVM cannot resolve the access
        through ordinary guest memory
                    │
                    ▼
        KVM_EXIT_MMIO delivered to userspace
                    │
                    ▼
        Handled by the microkvm device model
```

This is the basic mechanism for userspace device emulation. Configurations that handle MMIO in the kernel or accelerate notifications do not necessarily return to the VMM for every access.

### PIO vs. MMIO

| | PIO | MMIO |
|---|---|---|
| Guest instruction | `out` / `in` (dedicated) | `mov` (ordinary load/store) |
| Address space | 16-bit port numbers | Guest physical address space |
| Trapping mechanism | I/O instruction interception | Access to a GPA without RAM backing |
| Data location in kvm_run | `(char *)run + run->io.data_offset` | Directly in `run->mmio.data` (8-byte array) |
| Real-world uses | Legacy devices (serial, PIC, PIT) | Modern devices (NIC, GPU, NVMe) |

### Fields in run->mmio

When `exit_reason == KVM_EXIT_MMIO`:

| Field | Meaning |
|-----------|------|
| `run->mmio.phys_addr` | Guest physical address accessed |
| `run->mmio.data` | 8-byte array: written data for a write, data supplied by the VMM for a read |
| `run->mmio.len` | Number of bytes reported in this access (1 in this guest) |
| `run->mmio.is_write` | 1 = guest wrote to the address; 0 = guest read from it |

Unlike PIO, whose data is located at an offset, MMIO data is embedded directly in the `run->mmio` structure.

## Execution flow

```
VMM (microkvm)                                  Guest
──────────────                                  ─────
Register memory slots:
  slot 0: 0x00000–0xCFFFF (RAM)
  slot 1: 0xD1000–0xFFFFF (RAM)
  0xD0000–0xD0FFF: no slot
KVM_RUN
                                                Output 'R', 'P', 'L' and newlines via PIO
                                                (KVM_RUN after handling each out)
                                                mov rbx, 0xD0000
                                                mov al, 'M'
                                                mov [rbx], al
KVM_EXIT_MMIO (write)
  phys_addr=0xD0000, len=1
  is_write=1, data[0]='M'
  Log 'M'
KVM_RUN                                         (complete write and resume)
                                                mov al, '\n'
                                                mov [rbx], al
KVM_EXIT_MMIO (write)
  Omit newline log
KVM_RUN                                         (resume)
                                                hlt
KVM_EXIT_HLT: end loop
```

The VMM receives six PIO notifications, two MMIO write notifications, and one HLT notification. Newline accesses still produce notifications even though they are omitted from the log.

## Implementation

Add memory-layout constants to `microkvm.h`, and update slot registration and the exit handler in `microkvm.c`, plus the long-mode portion of `guest.S`. Guest page tables remain as in Step 4. The main excerpts follow.

### VMM: Create an MMIO hole

Step 4 registered one contiguous memory slot covering all guest memory.
Step 5 splits it into two slots with a gap. Define the range constants in `microkvm.h` as `MEM_GAP_START=0xD0000` and `MEM_GAP_END=0xD1000`:

```c
/* Slot 0: GPA 0x00000 – 0xCFFFF (832 KB) */
struct kvm_userspace_memory_region region1 = {
    .slot = MEM_SLOT0_ID,
    .guest_phys_addr = MEM_SLOT0_GPA,
    .memory_size = MEM_SLOT0_SIZE,
    .userspace_addr = (unsigned long)mem + MEM_SLOT0_GPA,
};

/* Slot 1: GPA 0xD1000 – 0xFFFFF (188 KB) */
struct kvm_userspace_memory_region region2 = {
    .slot = MEM_SLOT1_ID,
    .guest_phys_addr = MEM_SLOT1_GPA,
    .memory_size = MEM_SLOT1_SIZE,
    .userspace_addr = (unsigned long)mem + MEM_SLOT1_GPA,
};

/* Register each slot (error handling omitted) */
ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region1);
ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region2);

/* GPA 0xD0000 – 0xD0FFF: no registered slot → MMIO */
```

The host `mmap` remains a contiguous 1 MiB region. Since 4 KiB is not registered as guest RAM, the registered RAM totals 832 KiB + 188 KiB = 1020 KiB.

| GPA range | Purpose | Size |
|---|---|---|
| 0x00000–0xCFFFF | slot 0: RAM | 832 KiB |
| 0xD0000–0xD0FFF | Unregistered: MMIO hole | 4 KiB |
| 0xD1000–0xFFFFF | slot 1: RAM | 188 KiB |

RAM accesses can also cause VM exits, for example while initially building translations. KVM can handle these internally, so they normally do not need to return to the VMM as `KVM_EXIT_MMIO`.

### VMM: Handle KVM_EXIT_MMIO

```c
case KVM_EXIT_MMIO:
    if (run->mmio.phys_addr == MEM_GAP_START && run->mmio.is_write) {
        char c = run->mmio.data[0];
        if (c != '\n')
            printf("[MMIO write @ 0x%llx] %c\n", run->mmio.phys_addr, c);
    }
    break;
```

The written byte is available in `run->mmio.data[0]`. This handler processes writes to `MEM_GAP_START`, the start of the hole, and omits newlines from the log. Calling `KVM_RUN` again after handling the access lets KVM complete the MMIO operation and resume the guest.

### Guest: Write to an MMIO address

After the long-mode PIO output, ordinary `mov` instructions write `M` and a newline. The instruction specifies a guest virtual address, but identity mapping translates it to the same GPA, 0xD0000:

```asm
    /* After PIO output in long_mode */
    .byte 0x48, 0xC7, 0xC3, 0x00, 0x00, 0x0D, 0x00     /* mov rbx, 0xD0000 */
    .byte 0xB0, 'M'                                    /* mov al, 'M' */
    .byte 0x88, 0x03                                   /* mov [rbx], al */
    .byte 0xB0, '\n'                                   /* mov al, '\n' */
    .byte 0x88, 0x03                                   /* mov [rbx], al */
```

The instruction is an ordinary store, but this GPA is not registered as RAM. The VMM implements device behavior by treating writes to this address as character output.

## Output

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
[PIO out port 0x10] R
[PIO out port 0x10] P
[PIO out port 0x10] L
[MMIO write @ 0xd0000] M
Guest halted.
```

Step 4's `putchar` is replaced with PIO logs that include the port number. `R`, `P`, and `L` are output through PIO; `M` through MMIO. Both paths omit guest-supplied newlines, so the number of log lines differs from the number of notifications.

### Why not create a hole in the guest page tables?

Marking the guest virtual-address translation not present would cause a guest page fault (#PF) before the access reaches MMIO handling. This guest has no IDT for exception handling, so it cannot handle the exception and may triple fault.

Moreover, this implementation uses 2 MiB pages without a PT-level table. Clearing the Present bit in PD[0] would invalidate the entire first 2 MiB, including the running code, rather than only 0xD0000.

```text
Guest page tables: GVA 0xD0000 → GPA 0xD0000 (present)
Memory slots:      GPA 0xD0000 → Not registered as RAM
VMM handling:      Treat writes to this address as character output
```

## Key insight

RAM and MMIO use the same memory-access instructions, but guest software distinguishes their purposes using the memory map and device information. This step keeps guest address translation valid while creating a hole in KVM's RAM registration. The VMM handles writes there to implement a character output device.

## What this step teaches

| Concept | How it appears here |
|------|--------------|
| MMIO | Device registers exposed as guest physical addresses |
| Memory slot hole | Unregistered GPA range → KVM_EXIT_MMIO |
| run->mmio | phys_addr, data[], len, is_write—all in one structure |
| I/O through ordinary instructions | Guest writes with `mov`; VMM handles the device operation |
| Second stage vs. guest PT | MMIO is determined by memory slots, not guest page tables |

## What changed

- Split RAM registration into two slots, leaving a 4 KiB hole at 0xD0000–0xD0FFF.
- Add MMIO writes to the guest and `KVM_EXIT_MMIO` handling to the VMM.
- Change PIO output to logs with port numbers; omit newline logs for both PIO and MMIO.

## Next step

[Step 6: MMIO Reads + Device State](step06_mmio-read.md)—Add MMIO read support so the guest can query device state, completing a bidirectional device model.
