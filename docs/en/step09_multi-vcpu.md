# Step 9: Multiple vCPUs

## Goal

Run two vCPUs in parallel using **pthreads**, introducing shared device state and the need for synchronization.
This is the first step where execution order is non-deterministic.

## Background

### One thread per vCPU

`KVM_RUN` blocks the calling thread until the guest exits. To run multiple vCPUs concurrently, each vCPU needs its own thread - exactly how QEMU structures its vCPU execution:

```
main thread
  ├── pthread_create → vCPU 0 thread: for(;;) { KVM_RUN; handle exit; }
  ├── pthread_create → vCPU 1 thread: for(;;) { KVM_RUN; handle exit; }
  ├── pthread_join(vCPU 0)
  └── pthread_join(vCPU 1)
```

### Shared state requires synchronization

Both vCPU threads access the same device state (`device_counter`, `msr_store`, `stdout`). Without synchronization, concurrent access leads to data races:

```
vCPU 0: read device_counter (=0)
vCPU 1: read device_counter (=0)    ← not yet written back
vCPU 0: write device_counter (=1)
vCPU 1: write device_counter (=1)   ← should be 2, but is 1 (lost update)
```

A mutex serializes access to shared state. This is conceptually similar to QEMU's BQL (Big QEMU Lock) - both serialize access to shared device state from concurrent vCPU threads.

### BSP and AP initialization

On real multiprocessor hardware:
- The **BSP** (Bootstrap Processor) boots first and performs mode transitions
- **APs** (Application Processors) are woken later into an environment the BSP has already prepared

We mirror this pattern:
- vCPU 0 starts in real mode and transitions through all modes (Steps 3–4)
- vCPU 1 starts directly in long mode via `KVM_SET_SREGS` - the VMM sets all the registers that vCPU 0 achieved through guest code

Real APs start from a reset state and execute a bootstrap trampoline (INIT IPI → SIPI → 16-bit real mode → trampoline → long mode), but for simplicity we initialize vCPU 1 directly in long mode.

### Non-deterministic output

With two threads running concurrently, the output order depends on scheduling. vCPU 1's output may appear between any two lines of vCPU 0's output. This is not a bug - it demonstrates real concurrent execution.

## Execution flow

```
main                    vCPU 0 thread              vCPU 1 thread
────                    ─────────────              ─────────────
pthread_create(0)       KVM_RUN
pthread_create(1)       [real mode] 'R'            KVM_RUN
                        [protected] 'P'            [long mode]
                        [long mode]                  MMIO read → 0
                          MMIO write 'M'             out '0'
                          MMIO read → 1              hlt → done
                          MSR write/read
                          'I' (interrupt)
                        hlt → done
pthread_join(0)
pthread_join(1)

Output order is non-deterministic - vCPU 1 may read the counter before or after vCPU 0.
```

## Implementation

### VMM: struct vcpu

```c
struct vcpu {
    int fd;
    struct kvm_run *run;
    int id;
    size_t mmap_size;
};
```

Each vCPU has its own fd, kvm_run page, and identifier. The kvm_run page is per-vCPU - each thread reads only its own exit information.

### VMM: vCPU creation loop

```c
for (int i = 0; i < NUM_VCPUS; i++) {
    vcpus[i].fd = ioctl(vmfd, KVM_CREATE_VCPU, i);
    vcpus[i].run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, vcpus[i].fd, 0);
    /* ... initialize registers ... */
}
```

The second argument to `KVM_CREATE_VCPU` is the vCPU index. KVM uses this internally for APIC ID assignment and other per-CPU state.

### VMM: vCPU 1 — direct long mode initialization

```c
/* vCPU 1: starts directly in long mode */
sregs.cr0 = 0x80000011;             /* PG | PE | ET */
sregs.cr3 = 0x70000;                /* same page tables as vCPU 0 */
sregs.cr4 = 0x20;                   /* PAE */
sregs.efer = (1 << 8) | (1 << 10);  /* LME | LMA */

sregs.cs.selector = 0x18;           /* GDT[3]: 64-bit code */
sregs.cs.l = 1;                     /* long mode */
sregs.cs.present = 1;
sregs.cs.s = 1;
sregs.cs.type = 11;                 /* execute/read, accessed */

regs.rip = VCPU1_ENTRY;             /* 0x1100 */
regs.rsp = 0x50000;                 /* separate stack from vCPU 0 */
```

This is the same state that vCPU 0 achieves after executing Steps 3–4, but set directly by the VMM. Each vCPU gets its own stack to avoid corruption during concurrent execution.

### VMM: mutex-protected exit handler

```c
static pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;

/* Inside vcpu_thread: */
case KVM_EXIT_MMIO:
    pthread_mutex_lock(&dev_lock);
    if (run->mmio.is_write) {
        /* ... handle write ... */
    } else {
        run->mmio.data[0] = device_counter++;
    }
    pthread_mutex_unlock(&dev_lock);
    break;
```

Every access to shared state (`device_counter`, `msr_store`, `printf`) is wrapped in the mutex. This prevents lost updates and interleaved output.

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

`.org 0x1100` places this code at a fixed offset in the binary. The VMM sets vCPU 1's RIP to this address. vCPU 1 reads from the same MMIO device as vCPU 0, demonstrating that `device_counter` is truly shared - whichever vCPU reads first gets 0, the other gets 1.

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
[vCPU 0][MSR write] 0x4b564d00 = 0x42
[vCPU 0][MSR read] 0x4b564d00 -> 0x42
[vCPU 0][PIO out port 0x10] r
[vCPU 0][PIO out port 0x10] I
[vCPU 0] halted.
```

Note: The output order is non-deterministic — vCPU 1's MMIO read interleaves with vCPU 0's PIO output. This is inherent to concurrent execution.

## Key insight

A multi-vCPU VM is fundamentally a **multi-threaded program** on the host.
Each vCPU is a host thread that alternates between running guest code (inside `KVM_RUN`) and handling exits in userspace.

All the challenges of concurrent programming apply:
- Shared mutable state needs synchronization
- Output order is non-deterministic
- Each vCPU needs its own stack and per-CPU state

The MMIO counter provides a concrete example: both vCPUs access the same virtual device. Whichever vCPU reads first receives 0; the other receives 1. Without the mutex, the read-modify-write of `device_counter` could be corrupted by a race.

This is exactly how QEMU works:
- Each vCPU is a `qemu_thread`
- Device state is protected by the BQL (Big QEMU Lock)
- Per-vCPU state (`CPUState`) is thread-local

The BQL approach is simple but limits scalability - only one thread can access device state at a time. Modern QEMU is gradually moving toward fine-grained locking for performance-critical paths.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| Thread-per-vCPU | `pthread_create` for each vCPU — same as QEMU |
| Shared device state | `device_counter`, `msr_store` accessed by both threads |
| Mutex | `pthread_mutex_t` protects shared state — analogous to BQL |
| BSP/AP pattern | vCPU 0 boots from real mode; vCPU 1 starts in long mode |
| Non-determinism | Output order depends on scheduling |
| Per-vCPU state | Separate fd, kvm_run, stack, RIP for each vCPU |

## Next step

[Step 10: Boot minimal Linux](step10_linux-boot.md) - so far, every guest has been hand-written assembly. In the next step, we replace the toy guest with a real operating system and observe how Linux uses the mechanisms introduced in Steps 1–9: mode transitions, device emulation, interrupt delivery, and MSR-based paravirtualization.
