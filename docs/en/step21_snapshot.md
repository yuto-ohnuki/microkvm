# Step 21: VM snapshot — save and restore full VM state

## Goal

Save the entire VM state (CPU registers, device state, guest RAM) to a file with `Ctrl-A s`, and restore it with `./microkvm --restore snapshot.bin` to resume execution from the exact saved point.

## Background

### What is a VM snapshot?

A VM snapshot is a checkpoint of a running virtual machine. It freezes the complete state — CPU registers, memory contents, device state — into a file at a specific moment. Later, that file can be loaded to resume execution from exactly where it stopped.

Similar to hibernate on a physical machine, but virtualization makes it more powerful:
- Physical machine: write to disk → power off → resume on the *same* machine
- Virtual machine: write to file → resume on same *or different* host (= foundation for live migration)

A snapshot captures **execution state**, not just memory contents. RAM alone is useless without knowing where the CPU was executing (RIP), what mode it was in (CR0/CR4/EFER), or what interrupts were pending.

Use cases: debugging (reproduce a crash), fast test environments (skip boot), live migration (Step 22), rollback (undo changes).

### What state must be saved?

A running VM's state spans multiple layers:

| Layer | What | KVM ioctl |
|-------|------|-----------|
| CPU architectural | RAX-R15, RIP, RFLAGS, segments, CR0-CR4, EFER | KVM_GET_REGS, KVM_GET_SREGS |
| FPU/SSE/AVX | x87 + SSE + AVX registers (used by kernel internally) | KVM_GET_FPU |
| XCRs | XCR0 — which XSAVE features are enabled | KVM_GET_XCRS |
| LAPIC | Local APIC registers + timer state | KVM_GET_LAPIC |
| Pending events | Pending exceptions/interrupts, NMI state | KVM_GET_VCPU_EVENTS |
| PIT | 8254 timer channels (system tick) | KVM_GET_PIT2 |
| IRQ chips | PIC master + PIC slave + IOAPIC | KVM_GET_IRQCHIP (×3) |
| Clock | kvmclock offset (guest time reference) | KVM_GET_CLOCK |
| MSRs | TSC, APICBASE, syscall entry, kvmclock | KVM_GET_MSRS |
| Devices | UART registers, virtio state | Direct struct copy |
| Memory | Guest RAM (128MB) | Direct write from mmap region |

### Why restore order matters

KVM's `SET` ioctls have side effects:

| ioctl | Side effect |
|-------|-------------|
| KVM_CREATE_PIT2 | Arms PIT timer immediately (starts counting) |
| KVM_SET_MSRS (kvmclock) | KVM accesses guest RAM's shared clock page |
| KVM_SET_LAPIC | Re-arms LAPIC timer (may fire immediately if deadline passed) |
| KVM_SET_SREGS | KVM validates page tables via CR3 |
| KVM_SET_XCRS | Must match CR4.OSXSAVE or VMCS becomes invalid |

Wrong order causes timer storms (nested interrupt overflow), triple faults, or VMCS invalid state. The correct restore order is:

```
RAM first (loaded before any ioctl that accesses guest memory)
  → PIT → Clock → IRQchip → XCRs → SREGS → MSRs → LAPIC → Events → FPU → REGS
```

PIT must be first because `KVM_CREATE_PIT2` (called during VM setup) arms a default timer — we must immediately overwrite it with the saved state before it fires.

## Execution flow

```
Save (Ctrl-A s):
─────────────────────────────────────────────────────────────────
stdin_thread          vCPU thread              main
────────────          ───────────              ────
Ctrl-A s detected
  stop_requested=1
                      next VM exit
                      checks stop_requested
                      → break → return
                                               pthread_join()
                                               snap_save()
                                                 → header
                                                 → save_cpu_state()
                                                 → write(RAM, 128MB)
                                               exit

Restore (--restore snapshot.bin):
─────────────────────────────────────────────────────────────────
main
────
parse --restore
create VM, irqchip, PIT (normal setup)
allocate memory, register slots
skip load_bzimage, skip register init
snap_restore()
  → validate header
  → read state from file
  → read RAM from file
  → apply: PIT → clock → irqchip → XCRs → SREGS → MSRs → LAPIC → events → FPU → REGS
  → restore virtio device state
KVM_RUN → guest resumes from saved RIP
```

## Implementation

### snapshot.h — definitions

```c
#define SNAP_MAGIC   0x4D4B564D   /* "MKVM" */
#define SNAP_VERSION 1
#define SNAP_NUM_MSRS 12

struct snap_header {
    uint32_t magic;
    uint32_t version;
    uint64_t mem_size;
};

/* Virtio state without host pointers (ram, ram_size excluded) */
struct virtio_snap { ... };
```

### File layout

```
snapshot.bin
┌────────────────┐
│ Header         │  16 bytes (magic, version, mem_size)
├────────────────┤
│ CPU state      │  ~2.3 KB (regs, sregs, fpu, lapic, xcrs, events)
├────────────────┤
│ Platform state │  ~1.9 KB (pit, irqchip×3, clock, msrs)
├────────────────┤
│ Device state   │  ~72 bytes (uart, virtio_snap)
├────────────────┤
│ Guest RAM      │  128 MB (99.99% of file size)
└────────────────┘
```

### Save vs Restore order

```
Save (sequential write):        Restore (read all, then apply):
  Header                          Read: Header → CPU → Platform
  CPU state                              → Device → RAM
  Platform state                  Apply: PIT → Clock → IRQchip
  Device state                           → XCRs → SREGS → MSRs
  RAM                                    → LAPIC → Events → FPU → REGS
                                  (RAM loaded before apply — MSRs access guest memory)
```

### snapshot.c — save_cpu_state (shared helper)

```c
static void save_cpu_state(int fd, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio)
{
    /* vCPU: regs, sregs, fpu, lapic, xcrs, events */
    /* VM-wide: pit, irqchip×3, clock */
    /* MSRs: 12 entries (TSC, APICBASE, SYSENTER, STAR/LSTAR/CSTAR/FMASK,
             KERNEL_GS_BASE, KVM_WALL_CLOCK_NEW, KVM_SYSTEM_TIME_NEW) */
    /* Devices: uart struct, virtio_snap struct */
}
```

This helper is shared with `migrate_stop_and_copy()` in Step 22.

### snapshot.c — snap_save

```c
int snap_save(const char *path, ...) {
    write(fd, &hdr, sizeof(hdr));       /* header */
    save_cpu_state(fd, vcpufd, vmfd, uart, virtio);  /* CPU + devices */
    write(fd, mem, mem_size);           /* 128MB RAM */
}
```

### snapshot.c — snap_restore

```c
int snap_restore(const char *path, ...) {
    /* 1. Read all state from file (in save order) */
    /* 2. Read RAM */
    /* 3. Apply to KVM in correct order:
         PIT → Clock → IRQchip → XCRs → SREGS → MSRs → LAPIC → Events → FPU → REGS */
    /* 4. Restore virtio device state (host pointers NOT from file) */
}
```

### microkvm.c changes

- `--restore` argument parsing
- `if (!restore_path)` guards around bzImage loading and register initialization
- `Ctrl-A s` handler: sets `stop_requested = 1`
- After vCPU join: `snap_save("snapshot.bin", ...)`
- Before KVM_RUN: `snap_restore(restore_path, ...)` if restoring
- `KVM_EXIT_FAIL_ENTRY` case added for debugging restore failures
- `KVM_PIT_SPEAKER_DUMMY` flag on PIT creation

## Output

```
/ # export FOO=bar
/ # echo $FOO
bar
/ #
[monitor] saving snapshot...
[snapshot] saved to snapshot.bin

$ ./microkvm --restore snapshot.bin
[snapshot] restored from snapshot.bin
Starting guest...

/ # echo $FOO
bar
```

The shell variable `FOO` survives the save/restore cycle — proof that the entire guest state (CPU + memory + devices) was correctly preserved and restored.

## Key insight

VM snapshot is not just "dump registers to a file." The restore *order* is critical because KVM ioctls have side effects — they trigger timers, access guest memory, and validate CPU state against each other. The key discovery: `KVM_CREATE_PIT2` arms a default timer that must be immediately overwritten by `KVM_SET_PIT2` before it fires into the restored LAPIC state, or a timer interrupt storm causes a stack overflow. This teaches that hypervisor state is deeply interconnected, not a flat register dump.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| VM state completeness | Must save CPU + platform + devices + RAM — missing any piece causes failure |
| Restore order dependencies | PIT before LAPIC, XCRs before SREGS, RAM before MSRs |
| Host pointer exclusion | virtio_snap omits `uint8_t *ram` — process-specific pointers cannot be serialized |
| save_cpu_state helper | Shared between snapshot and migration (DRY for save, explicit for restore) |
| KVM_PIT_SPEAKER_DUMMY | Prevent PIT channel 2 speaker emulation issues during boot calibration |
| KVM_EXIT_FAIL_ENTRY | Debug aid for invalid VMCS state during restore development |

## What changed

From Step 20:
- **New files**: `snapshot.h` (structs, MSR defines, API), `snapshot.c` (save_cpu_state, snap_save, snap_restore)
- **microkvm.c**: `--restore` arg, `Ctrl-A s`, `KVM_EXIT_FAIL_ENTRY`, `KVM_PIT_SPEAKER_DUMMY`, boot/regs guarded by `if (!restore_path)`
- **Makefile**: add `snapshot.c`, add `snapshot.bin` to clean target

## Next step

[Step 22: Live migration](step22_live-migration.md) combines dirty page tracking (Step 20) with snapshot (Step 21) to implement iterative pre-copy migration — transferring VM state while the guest continues running.

Snapshot copies **everything once**. Live migration copies **everything once, then only what changed**.
