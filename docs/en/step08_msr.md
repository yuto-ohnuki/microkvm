# Step 8: MSR Handling

## Goal

Trap and emulate **Model-Specific Register** (MSR) accesses using `KVM_EXIT_X86_WRMSR` and `KVM_EXIT_X86_RDMSR`.
Introduce MSR exits—an important class of VM exits used for paravirtualized guest–hypervisor communication.

> **Note:** This step requires a kernel supporting `KVM_CAP_X86_USER_SPACE_MSR` and `KVM_CAP_X86_MSR_FILTER`.

## Background

### What are MSRs?

Model-Specific Registers are a large set of CPU registers accessed through dedicated instructions (`wrmsr` / `rdmsr`). They control and expose internal CPU features:

| MSR | Purpose |
|-----|------|
| `IA32_TSC` (0x10) | Timestamp counter |
| `IA32_APIC_BASE` (0x1B) | Local APIC base address |
| `IA32_EFER` (0xC0000080) | Extended features (LME, NXE) |
| `MSR_KVM_*` (0x4B564Dxx) | KVM paravirtualization interfaces |

MSRs are a major mechanism for **paravirtualization**: guests and hypervisors can communicate through synthetic MSRs that do not exist in physical hardware. For example, KVM reserves the `0x4B564Dxx` range, corresponding to ASCII "KVM", for features such as kvmclock.

microkvm avoids these standard and KVM-defined ranges and uses an arbitrary experimental index, `0x20000000` (`MSR_CUSTOM`), exclusively for its educational guest. Neither the architecture nor KVM defines this value; it demonstrates using an MSR through a private agreement between guest and hypervisor.

### Default KVM behavior

By default, KVM handles most MSR accesses internally without exiting to userspace (TSC, APIC, EFER, etc.). An MSR that is neither handled by KVM nor routed to userspace normally causes a guest #GP (General Protection Fault).

This implementation configures two things to forward filter-denied MSR accesses to userspace:

1. **`KVM_CAP_X86_USER_SPACE_MSR`**—specify `KVM_MSR_EXIT_REASON_FILTER` to report filter-denied accesses to userspace instead of generating #GP
2. **`KVM_X86_SET_MSR_FILTER`**—specify which MSRs to deny (trap)

### The wrmsr / rdmsr instructions

```
wrmsr:  ECX = MSR address,  EDX:EAX = 64-bit value to write
rdmsr:  ECX = MSR address → EDX:EAX = 64-bit value read
```

Both require CPL=0 (ring 0). For historical reasons, values are split across two 32-bit registers; these instructions predate 64-bit mode.

### MMIO vs MSR

MMIO exposes registers through guest physical address space, accessed with ordinary load/store instructions. MSRs expose registers through dedicated CPU instructions (`rdmsr`/`wrmsr`).

Both appear as registers to the guest, but belong to different domains:

| | MMIO (Step 5–6) | MSR (Step 8) |
|---|---|---|
| Belongs to | Device | CPU itself |
| Access | `mov` (memory instruction) | `rdmsr` / `wrmsr` |
| Address space | Guest physical addresses | 32-bit MSR indices |
| Trapping mechanism | Memory slot hole | MSR filter bitmap |

This step stores the custom MSR in the VMM's file-scope variable `msr_store`. Per-vCPU state is not created automatically; the VMM implementation determines the storage scope.

## Execution flow

```
VMM (microkvm)                                  Guest
──────────────                                  ─────
Enable MSR exits for FILTER reasons
MSR filter: deny read/write of 0x20000000
KVM_RUN
                                                (Mode transitions, PIO, MMIO as in Step 7)
                                                ECX = 0x20000000
                                                EDX = 0, EAX = 0x42
                                                wrmsr
KVM_EXIT_X86_WRMSR
  msr_store = 0x42
  run->msr.error = 0
KVM_RUN                                         (complete wrmsr and resume)
                                                rdmsr
KVM_EXIT_X86_RDMSR
  run->msr.data = msr_store
  run->msr.error = 0
KVM_RUN                                         (complete rdmsr: EDX=0, EAX=0x42)
                                                add al, '0' → 'r'
                                                out 0x10, al
KVM_EXIT_IO: log 'r'
KVM_RUN                                         (resume)
                                                Output newline via PIO
                                                Interrupt handling and shutdown as in Step 7
```

## Implementation

Define `MSR_CUSTOM` in `microkvm.h`, add filter setup and MSR storage/response handling to `microkvm.c`, and add a write and readback to `guest.S`.

### VMM: Enable MSR trapping (two-stage setup)

Configure this after creating the VM and before running the guest. Error handling is omitted below.

```c
/* Enable userspace MSR exits */
struct kvm_enable_cap msr_cap = {
    .cap = KVM_CAP_X86_USER_SPACE_MSR,
    .args[0] = KVM_MSR_EXIT_REASON_FILTER,
};
ioctl(vmfd, KVM_ENABLE_CAP, &msr_cap);

/* Configure the filter—deny the custom MSR */
uint8_t msr_bitmap[] = {0x00};  /* bit=0 means deny (trap) */
struct kvm_msr_filter filter = {
    .flags = KVM_MSR_FILTER_DEFAULT_ALLOW,
    .ranges = {{
        .flags = KVM_MSR_FILTER_READ | KVM_MSR_FILTER_WRITE,
        .nmsrs = 1,
        .base = MSR_CUSTOM,       /* 0x20000000 */
        .bitmap = msr_bitmap,
    }},
};
ioctl(vmfd, KVM_X86_SET_MSR_FILTER, &filter);
```

With `nmsrs=1`, only bit 0 of the bitmap controls reads/writes to `MSR_CUSTOM`. Zero means deny; enabling `KVM_MSR_EXIT_REASON_FILTER` makes these accesses notify the VMM. Without that exit setting, a denied access would cause a guest #GP.

`DEFAULT_ALLOW` means the filter does not deny MSRs outside the specified range. It does not make unsupported MSRs usable; accesses outside the range follow KVM's normal handling.

### VMM: Handle MSR exits

```c
/* File scope in microkvm.c */
static uint64_t msr_store = 0;

/* Inside the exit handler (logging omitted) */
case KVM_EXIT_X86_WRMSR:
    if (run->msr.index == MSR_CUSTOM) {
        msr_store = run->msr.data;
        run->msr.error = 0;    /* Success */
    } else {
        run->msr.error = 1;    /* Inject #GP */
    }
    break;

case KVM_EXIT_X86_RDMSR:
    if (run->msr.index == MSR_CUSTOM) {
        run->msr.data = msr_store;
        run->msr.error = 0;
    } else {
        run->msr.error = 1;
    }
    break;
```

On the next `KVM_RUN`, KVM completes the MSR operation if `error=0`. For a read, `data` is reflected in EDX:EAX. If `error=1`, KVM injects a guest #GP.

### Guest: wrmsr and rdmsr

```asm
    /* wrmsr: write 0x42 to MSR 0x20000000 */
    .byte 0xB9, 0x00, 0x00, 0x00, 0x20      /* mov ecx, 0x20000000 */
    .byte 0x31, 0xD2                        /* xor edx, edx */
    .byte 0xB8, 0x42, 0x00, 0x00, 0x00      /* mov eax, 0x42 */
    .byte 0x0F, 0x30                        /* wrmsr */

    /* rdmsr: read back from the same MSR */
    .byte 0x0F, 0x32                        /* rdmsr → eax = 0x42 */
    .byte 0x04, 0x30                        /* add al, '0' → 'r' */
    .byte 0xE6, 0x10                        /* out 0x10, al */
    .byte 0xB0, '\n'                        /* mov al, '\n' */
    .byte 0xE6, 0x10                        /* out 0x10, al */
```

The guest writes a value to the synthetic MSR, reads it back, and outputs the result through PIO. Because the MSR filter denies access to this address, both instructions are trapped to the VMM.

## Output

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
[PIO out port 0x10] R
[PIO out port 0x10] P
[PIO out port 0x10] L
[MMIO write @ 0xd0000] M
[MMIO read  @ 0xd0000] returning 2
[PIO out port 0x10] 2
[MSR write] 0x20000000 = 0x42
[MSR read] 0x20000000 -> 0x42
[PIO out port 0x10] r
[PIO out port 0x10] I
Guest halted.
```

The MSR write/read logs confirm that 0x42 was stored and returned unchanged. The PIO `r` comes from `0x42 + 0x30 = 0x72`, not a decimal formatting of the value. The final `I` and `Guest halted.` come from the interrupt handling retained from Step 7.

## Key insight

Selecting an MSR through the filter and handling storage and readback in the VMM implements a custom MSR interface. Whether an interface is paravirtualized depends on the guest–VMM agreement; the distinction between MMIO and MSRs alone does not determine this.

### Relationship to KVM paravirtualization MSRs

kvmclock uses MSRs to register shared memory and related information, then the guest reads time information from that memory. Unlike this step's direct return of a value through `rdmsr`, standard KVM features are handled inside KVM.

### Main VM exit types introduced so far (Steps 1–8)

| Exit type | Step | Trigger |
|-------------|------|---------|
| `KVM_EXIT_HLT` | 1, 7 | `hlt` instruction |
| `KVM_EXIT_IO` | 2 | `out` / `in` instruction |
| `KVM_EXIT_MMIO` | 5, 6 | Access to a GPA without a memory slot |
| `KVM_EXIT_X86_WRMSR` | 8 | `wrmsr` to a denied MSR |
| `KVM_EXIT_X86_RDMSR` | 8 | `rdmsr` from a denied MSR |

This lists the main exits covered so far, not every KVM exit type.

## What this step teaches

| Concept | How it appears here |
|------|--------------|
| MSR filter | Two-stage setup: enable capability + configure filter bitmap |
| wrmsr / rdmsr | ECX = address, EDX:EAX = value (split 64-bit value) |
| Paravirtualization | Synthetic MSRs for guest–hypervisor communication |
| Selective trapping | DEFAULT_ALLOW + deny a specific MSR |
| Error injection | `run->msr.error = 1` causes a guest #GP |

## What changed

- `microkvm.h`: Add the custom educational MSR index `MSR_CUSTOM` (0x20000000).
- `microkvm.c`: Add MSR exit enablement, a filter, `msr_store`, and read/write handling.
- `guest.S`: Add a write/readback of 0x42 and PIO output of the result.

## Next step

[Step 9: Multiple vCPUs](step09_multi-vcpu.md)—So far, the VM has had one virtual CPU. Real systems are multicore. Next, introduce multiple vCPUs sharing guest memory and device state, requiring synchronization between host threads.
