# Step 9: Multiple vCPUs

> **Phase B: Linux Boot**
>
> Phase A established the foundations of CPU virtualization with bare-metal guest code.
> Phase B boots a real Linux kernel: multiple vCPUs, bzImage loading, a serial console, and an interactive shell. By the end, the guest runs a complete Linux userspace provided by busybox.

## Goal

Run two vCPUs in parallel using **pthreads**, introducing shared device state and the need for synchronization. This is the first step with nondeterministic execution order.

## Background

### One vCPU = one thread

This implementation assigns a host thread to each vCPU. Each thread executes the guest inside `KVM_RUN` and handles exits when it returns. Two threads calling `KVM_RUN` independently allow concurrent vCPU execution:

```
main thread
  ├── pthread_create → vCPU 0 thread: for(;;) { KVM_RUN; handle exit; }
  ├── pthread_create → vCPU 1 thread: for(;;) { KVM_RUN; handle exit; }
  ├── pthread_join(vCPU 0)
  └── pthread_join(vCPU 1)
```

### Shared state needs synchronization

In this guest, vCPU 0 updates `device_counter`, and vCPU 1 reads it. Even with only one writer, unsynchronized reads from another thread create a data race.

`dev_lock` protects counter updates and reads. The same lock protects `msr_store` and normal per-vCPU logs, though only vCPU 0's guest code accesses MSRs here. The lock serializes handling but does not determine which vCPU runs first.

### BSP and AP initialization

On physical multiprocessor hardware:
- The **BSP** (Bootstrap Processor) boots first and performs mode transitions
- **APs** (Application Processors) start later in an environment prepared by the BSP

This step provides different initial states:
- vCPU 0 starts in real mode and transitions through all modes (Steps 3–4)
- vCPU 1 starts directly in long mode with registers configured by the VMM

INIT/SIPI, used to start APs on physical hardware, is not implemented. Before starting the threads, the VMM prepares page tables, loads the guest binary, and initializes both vCPUs. vCPU 1 can run without waiting for vCPU 0 to finish booting.

### Nondeterministic output

Because the two threads run concurrently, log ordering across vCPUs depends on scheduling. vCPU 1 may read 0, 1, or 2 depending on when its read occurs relative to vCPU 0's two writes. vCPU 0's own read follows both writes, so it returns 2.

## Execution flow

```
Guest                         KVM                         VMM (microkvm)
─────                         ───                         ──────────────
                                                          Initialize shared memory and both vCPUs
                                                          pthread_create × 2
vCPU 0: Real mode ←────────── Run vCPU 0 ←────────────── thread 0: KVM_RUN
vCPU 1: Long mode ←────────── Run vCPU 1 ←────────────── thread 1: KVM_RUN

  Each vCPU progresses independently:
vCPU 0: MMIO write × 2 ─────→ KVM_EXIT_MMIO ────────────→ counter++ under dev_lock
vCPU 1: MMIO read ──────────→ KVM_EXIT_MMIO ────────────→ Return current value under dev_lock
vCPU 1: PIO value, hlt ─────→ KVM_EXIT_HLT ─────────────→ thread 1 ends
vCPU 0: MMIO read, MSR, IRQ
        Final hlt ──────────→ KVM_EXIT_HLT ─────────────→ thread 0 ends
                                                          pthread_join × 2
                                                          Release resources
```

The read/write ordering and thread completion order in the diagram are not fixed. `KVM_RUN` itself is called outside `dev_lock`, so guest execution as a whole is not serialized.

## Implementation

Move the exit loop in `microkvm.c` into `vcpu_thread`, and add `NUM_VCPUS=2` and `VCPU1_ENTRY=0x1100` to `microkvm.h`. Add vCPU 1 code to `guest.S`, and link `-lpthread` in the `Makefile`. The main excerpts follow.

### VMM: struct vcpu

```c
struct vcpu {
    int fd;
    int id;
    struct kvm_run *run;
    size_t mmap_size;
};
```

Each vCPU has its own fd, kvm_run page, and identifier. The kvm_run page is per-vCPU: each thread reads only its own exit information.

### VMM: vCPU creation loop

```c
for (int i = 0; i < NUM_VCPUS; i++) {
    vcpus[i].fd = ioctl(vmfd, KVM_CREATE_VCPU, i);
    vcpus[i].run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, vcpus[i].fd, 0);
    /* ... Register initialization ... */
}
```

The third argument `i` in `ioctl(vmfd, KVM_CREATE_VCPU, i)` is the vCPU ID (APIC ID on x86). Store the returned fd and shared mapping separately for each vCPU.

### VMM: Initialize vCPU 1 directly in long mode

```c
/* vCPU 1: start directly in long mode */
sregs.cr0 = 0x80000011;             /* PG | PE | ET */
sregs.cr3 = 0x70000;                /* Same page tables as vCPU 0 */
sregs.cr4 = 0x20;                   /* PAE */
sregs.efer = (1 << 8) | (1 << 10);  /* LME | LMA */

sregs.cs.selector = 0x18;           /* GDT[3]: 64-bit code */
sregs.cs.l = 1;                     /* Long mode */
sregs.cs.present = 1;
sregs.cs.s = 1;
sregs.cs.type = 11;                 /* Execute/read, accessed */

regs.rip = VCPU1_ENTRY;             /* 0x1100 */
regs.rsp = 0x50000;                 /* Separate stack from vCPU 0 */
```

The VMM directly sets control registers and segment attributes. Assigning `cs.selector=0x18` does not itself load the GDT, so CS attributes must also be set. DS/SS are initialized outside the excerpt above; not all registers are identical between the vCPUs. The guest stack tops are 0x60000 for vCPU 0 and 0x50000 for vCPU 1, using non-overlapping regions.

### VMM: Mutex-protected exit handler

```c
static pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;

/* Inside vcpu_thread: */
case KVM_EXIT_MMIO:
    if (run->mmio.phys_addr == MEM_GAP_START) {
        pthread_mutex_lock(&dev_lock);
        if (run->mmio.is_write) {
            device_counter++;
        } else {
            run->mmio.data[0] = device_counter;
        }
        /* Logging in the actual code is also under this lock (omitted here) */
        pthread_mutex_unlock(&dev_lock);
    }
    break;
```

Shared-state accesses and their corresponding logs are handled under the same lock. This prevents another vCPU's handling from interleaving within an operation, but does not fix the ordering of log lines.

### VMM: Manage startup and shutdown

```c
for (int i = 0; i < NUM_VCPUS; i++) {
    pthread_create(&threads[i], NULL, vcpu_thread, &vcpus[i]);
}
for (int i = 0; i < NUM_VCPUS; i++) {
    pthread_join(threads[i], NULL);
}
```

Start both threads, wait for them to finish, and finally release shared memory and other resources. Interrupt injection occurs only on vCPU 0's first HLT; vCPU 1 stops on its first HLT. `irq_injected` is local to each `vcpu_thread`.

### Guest: vCPU 1 entry point

```asm
.org 0x1100
vcpu1_entry:
    .byte 0x48, 0xC7, 0xC3, 0x00, 0x00, 0x0D, 0x00     /* mov rbx, 0xD0000 */
    .byte 0x8A, 0x03                                   /* mov al, [rbx] */
    .byte 0x04, 0x30                                   /* add al, '0' */
    .byte 0xE6, 0x10                                   /* out 0x10, al */
    .byte 0xF4                                         /* hlt */
```

`.org 0x1100` places this code at a fixed offset in the binary. The VMM sets vCPU 1's RIP to that address. vCPU 1 reads the same MMIO device as vCPU 0, demonstrating that `device_counter` is truly shared. Scheduling determines when each vCPU reads, and the returned value depends on the counter state at that instant (nondeterministic).

## Output

```
$ ./microkvm
Loaded guest: 8232 bytes
Starting guest...
[vCPU 0][PIO out port 0x10] R
[vCPU 1][MMIO read  @ 0xd0000] returning 0
[vCPU 0][PIO out port 0x10] P
[vCPU 1][PIO out port 0x10] 0
[vCPU 1] halted.
[vCPU 0][PIO out port 0x10] L
[vCPU 0][MMIO write @ 0xd0000] M
[vCPU 0][MMIO read  @ 0xd0000] returning 2
[vCPU 0][PIO out port 0x10] 2
[vCPU 0][MSR write] 0x20000000 = 0x42
[vCPU 0][MSR read] 0x20000000 -> 0x42
[vCPU 0][PIO out port 0x10] r
[vCPU 0][PIO out port 0x10] I
[vCPU 0] halted.
```

In this run, vCPU 1 reads before vCPU 0 writes, returning 0. The log confirms `Loaded guest: 8232 bytes` and that both vCPUs halt. Different log ordering or a different vCPU 1 value on another run is normal if it follows from the timing described above.

## Key insight

Each vCPU has its own registers and `kvm_run`, while sharing guest memory and VMM device state. A mutex prevents data races on shared state but does not fix execution order between vCPUs. Therefore, even with correct synchronization, vCPU 1's read value varies with timing.

## What this step teaches

| Concept | How it appears here |
|------|--------------|
| Thread-per-vCPU | Assign a host thread to each vCPU with `pthread_create` |
| Shared device state | vCPU 0 updates the counter; vCPU 1 reads the same counter |
| Mutex | Mutual exclusion for shared-state updates and reads |
| BSP/AP pattern | vCPU 0 boots from real mode; vCPU 1 starts in long mode |
| Nondeterminism | Output order depends on scheduling |
| Per-vCPU state | Separate fd, kvm_run, stack, and RIP for each vCPU |

## What changed

- Extract the exit loop into `vcpu_thread`; group each vCPU's fd, shared mapping, and identifier in `struct vcpu`.
- Run two vCPUs in separate threads and protect shared device state with a mutex.
- Add long-mode initialization for vCPU 1 and guest code at 0x1100.
- Add vCPU IDs to logs and release resources after all threads finish.

## Next step

[Step 10: Minimal Linux Boot](step10_linux-boot.md)—All guests so far have been handwritten assembly. Next, replace the toy guest with a real OS and observe how Linux uses the mechanisms introduced in Steps 1–9: mode transitions, device emulation, interrupt delivery, and MSR-based paravirtualization.
