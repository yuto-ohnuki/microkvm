# Step 21: VM Snapshot—Save and Restore VM State

## Goal

Use `Ctrl-A s` to save the CPU/device state supported by this implementation and guest RAM to a file. Restore with `./microkvm --restore snapshot.bin` and resume execution from the saved point.

## Background

### What is a VM snapshot?

A VM snapshot is a checkpoint of a running virtual machine. It freezes the complete state at one instant—CPU registers, memory contents, and device state—into a file for later restoration.

It resembles physical-machine hibernation, with an advantage unique to virtualization:
- Physical machine: write to disk and power off → resume on the *same* machine
- Virtual machine: write to a file → resume on the same or a *different* host (the basis of live migration)

A snapshot saves **execution state**, not just memory contents. RAM alone does not identify where the CPU is executing (RIP), its mode (CR0/CR4/EFER), or pending interrupts; restoring only RAM is insufficient.

Use cases include debugging (reproducing crashes), fast test-environment startup (skipping boot), live migration (Step 22), and rollback (undoing changes).

This step targets saving/restoring a single vCPU on the same host with the same binary. vCPU execution stops during saving, but stdin/TX threads are not stopped, so test while I/O is quiet.

### What must be saved?

A running VM's state spans several layers:

| Layer | Contents | KVM ioctl |
|----|------|-----------|
| CPU architectural | RAX-R15, RIP, RFLAGS, segments, CR0-CR4, EFER | KVM_GET_REGS, KVM_GET_SREGS |
| FPU/SSE | x87 + XMM registers, etc. (excluding AVX extended state) | KVM_GET_FPU |
| XCRs | XCR0—which XSAVE features are enabled | KVM_GET_XCRS |
| LAPIC | Local APIC registers + timer state | KVM_GET_LAPIC |
| Pending events | Pending exceptions/interrupts, NMI state | KVM_GET_VCPU_EVENTS |
| PIT | 8254 timer channels (system tick) | KVM_GET_PIT2 |
| IRQ chips | PIC master + PIC slave + IOAPIC | KVM_GET_IRQCHIP (×3) |
| Clock | kvmclock offset (guest time reference) | KVM_GET_CLOCK |
| MSRs | TSC, APICBASE, syscall entry, kvmclock | KVM_GET_MSRS |
| Devices | UART registers, virtio state | Direct structure copy |
| Memory | Guest RAM (128MB) | Write mmap region directly |

### Why restore order matters

KVM `SET` ioctls have side effects:

| ioctl | Side effect |
|-------|--------|
| KVM_CREATE_PIT2 | Immediately arms the PIT timer (starts counting) |
| KVM_SET_MSRS (kvmclock) | KVM accesses the shared clock page in guest RAM |
| KVM_SET_LAPIC | Rearms the LAPIC timer (fires immediately if deadline is past) |
| KVM_SET_SREGS | KVM validates page tables through CR3 |
| KVM_SET_XCRS | Restores extended-state controls such as XCR0 |

Restore RAM and related CPU state first, also accounting for dependencies between timer and interrupt state. This implementation restores in this order:

```
Load RAM first (before ioctls that access guest memory)
  → PIT → Clock → IRQchip → XCRs → SREGS → MSRs → LAPIC → Events → FPU → REGS
```

PIT already exists from VM setup, so apply its saved state first. Then restore the irqchip and LAPIC.

## Execution flow

```
Save (Ctrl-A s):
─────────────────────────────────────────────────────────────────
stdin_thread                    vCPU thread
────────────                    ───────────
Detect Ctrl-A s
  snapshot_requested=1
                                Return from KVM_RUN and finish exit handling
                                Check snapshot_requested on next loop iteration
                                snap_save()
                                  → header
                                  → save_cpu_state()
                                  → writen(RAM, 128 MiB)
                                snapshot_requested=0
                                Resume execution with KVM_RUN

Restore (--restore snapshot.bin):
─────────────────────────────────────────────────────────────────
main
────
Parse --restore
Create VM, irqchip, PIT (normal setup)
Allocate memory and register slots
Skip load_bzimage and register initialization
snap_restore()
  → Validate header
  → Read state from file
  → Read RAM from file
  → apply: PIT → clock → irqchip → XCRs → SREGS → MSRs → LAPIC → events → FPU → REGS
  → Restore virtio device state
KVM_RUN → guest resumes from saved RIP
```

## Implementation

### snapshot.h—Definitions

```c
#define SNAP_MAGIC   0x4D4B564D   /* "MKVM" */
#define SNAP_VERSION 1
#define SNAP_NUM_MSRS 12

struct snap_header {
    uint32_t magic;
    uint32_t version;
    uint64_t mem_size;
};

/* virtio state without host pointers (ram and ram_size excluded) */
struct virtio_snap { ... };
```

### File layout

```
snapshot.bin
┌────────────────┐
│ Header         │  16 bytes (magic, version, mem_size)
├────────────────┤
│ CPU state      │  regs, sregs, fpu, lapic, xcrs, events
├────────────────┤
│ Platform state │  pit, irqchip×3, clock, msrs
├────────────────┤
│ Device state   │  uart, virtio_snap
├────────────────┤
│ Guest RAM      │  128 MiB
└────────────────┘
```

### Comparing save and restore order

```
Save (sequential writes):       Restore (read everything, then apply):
  Header                          Read: Header → CPU → Platform
  CPU state                              → Device → RAM
  Platform state                  Apply: PIT → Clock → IRQchip
  Device state                           → XCRs → SREGS → MSRs
  RAM                                    → LAPIC → Events → FPU → REGS
                                  (Load RAM before applying state—MSRs access guest memory)
```

### snapshot.c—save_cpu_state (shared helper)

```c
static int save_cpu_state(int fd, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio)
{
    /* vCPU: regs, sregs, fpu, lapic, xcrs, events */
    /* VM-wide: pit, irqchip×3, clock */
    /* MSRs: 12 entries (TSC, APICBASE, SYSENTER, STAR/LSTAR/CSTAR/FMASK,
             KERNEL_GS_BASE, KVM_WALL_CLOCK_NEW, KVM_SYSTEM_TIME_NEW) */
    /* Devices: uart structure, virtio_snap structure */
}
```

`writen()` and `readn()` handle short transfers and `EINTR`. `save_cpu_state()` returns -1 on a write failure and propagates it to `snap_save()`. However, KVM ioctl results and restore-side readn results are unchecked, so the implementation does not detect every failure.

This helper is also shared with Step 22's `migrate_stop_and_copy()`.

### snapshot.c — snap_save

```c
int snap_save(const char *path, ...) {
    /* File creation and other details omitted */
    if (writen(fd, &hdr, sizeof(hdr)) != sizeof(hdr))
        goto fail;
    if (save_cpu_state(fd, vcpufd, vmfd, uart, virtio) < 0)
        goto fail;
    if (writen(fd, mem, mem_size) != (ssize_t)mem_size)
        goto fail;
    /* close and success/failure handling omitted */
}
```

### snapshot.c — snap_restore

```c
int snap_restore(const char *path, ...) {
    /* 1. Read all state from file (in save order) */
    /* 2. Read RAM */
    /* 3. Apply to KVM in the correct order:
         PIT → Clock → IRQchip → XCRs → SREGS → MSRs → LAPIC → Events → FPU → REGS */
    /* 4. Restore virtio device state (file contains no host pointers) */
}
```

### Changes to microkvm.c

- Parse the `--restore` argument
- Guard bzImage/initramfs loading and register initialization with `if (!restore_path)`
- `Ctrl-A s` handler: set `snapshot_requested = 1`
- vCPU loop: `snap_save("snapshot.bin", ...)` → clear flag → resume `KVM_RUN`
- Before KVM_RUN: call `snap_restore(restore_path, ...)` when restoring
- Add `KVM_EXIT_FAIL_ENTRY` case (for restore debugging)
- Set `KVM_PIT_SPEAKER_DUMMY` when creating PIT

## Output

After saving, stop the VM with Ctrl-C, then restore from `snapshot.bin`.

```
/ # export FOO=bar
/ # echo $FOO
bar
/ # < Ctrl-A s >
[monitor] saving snapshot...
[snapshot] saved to snapshot.bin
```
```
$ ./microkvm --restore snapshot.bin
[snapshot] restored from snapshot.bin
Starting guest...
Starting guest [ioeventfd=OFF, irqfd=OFF]...

/ # echo $FOO
bar
```

After restoration, `echo $FOO` still prints `bar`, confirming that the shell variable from the saved state is retained.

## Key insight

A VM snapshot saves CPU, interrupt-controller, and device state as well as RAM. Restoration loads RAM first, then sets MSRs that access guest memory and related timer/interrupt state in the required order. The distinction between file save order and KVM application order is important.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| VM state coverage | Combine CPU + platform + device + RAM state to restore execution |
| Restore dependencies | PIT before LAPIC, XCRs before SREGS, RAM before MSRs |
| Excluding host pointers | virtio_snap omits `uint8_t *ram`; process-specific pointers cannot be serialized |
| save_cpu_state helper | Shared by snapshots and migration (DRY save, explicit restore) |
| KVM_PIT_SPEAKER_DUMMY | Prevent PIT channel 2 speaker-emulation issues during boot calibration |
| KVM_EXIT_FAIL_ENTRY | Help debug invalid VMCS state during restore development |

## What changed

Changes from Step 20:
- **New files**: `snapshot.h` (structures, MSR definitions, API), `snapshot.c` (save_cpu_state, snap_save, snap_restore)
- **microkvm.c**: `--restore`, `Ctrl-A s`, `KVM_EXIT_FAIL_ENTRY`, `KVM_PIT_SPEAKER_DUMMY`, and `if (!restore_path)` guards for boot/register setup
- **Makefile**: add `snapshot.c`; add `snapshot.bin` to clean

## Next step

[Step 22: Live Migration](step22_live-migration.md) combines dirty page tracking (Step 20) and snapshots (Step 21) to implement iterative pre-copy migration, transferring VM state while the guest keeps running.

A snapshot **copies everything once**. Live migration **copies everything once, then sends only what changed**.
