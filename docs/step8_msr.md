# Step 8: MSR handling

## Goal

Trap and emulate **Model-Specific Register** (MSR) accesses using `KVM_EXIT_X86_WRMSR` and `KVM_EXIT_X86_RDMSR`.
This introduces MSR exits, another important class of VM exits used for paravirtualized guest-hypervisor communication.

> **Note:** This step requires a kernel that supports
> `KVM_CAP_X86_USER_SPACE_MSR` and userspace MSR exits.

## Background

### What are MSRs?

Model-Specific Registers are a large set of CPU registers accessed via dedicated instructions (`wrmsr` / `rdmsr`). They control and expose CPU-internal features:

| MSR | Purpose |
|-----|---------|
| `IA32_TSC` (0x10) | Timestamp counter |
| `IA32_APIC_BASE` (0x1B) | Local APIC base address |
| `IA32_EFER` (0xC0000080) | Extended features (LME, NXE) |
| `MSR_KVM_*` (0x4B564Dxx) | KVM paravirt interface |

The hexadecimal prefix `0x4B564D` corresponds to the ASCII string "KVM". KVM reserves this range for synthetic hypervisor-defined MSRs.

MSRs are the primary mechanism for **paravirtualization** - the guest and hypervisor can communicate through synthetic MSRs that don't exist on real hardware. KVM uses the `0x4B564D00` range for features like kvmclock.

### Default KVM behavior

By default, KVM handles most MSR accesses internally (TSC, APIC, EFER, etc.) without exiting to userspace. For MSRs that are not handled by KVM and are not routed to userspace, the guest typically receives a #GP (General Protection Fault).

To trap specific MSRs to userspace, two steps are needed:

1. **`KVM_CAP_X86_USER_SPACE_MSR`** - enable the capability so that
   denied MSRs exit to userspace instead of injecting #GP
2. **`KVM_X86_SET_MSR_FILTER`** - specify which MSRs to deny (trap)

### wrmsr / rdmsr instructions

```
wrmsr:  ECX = MSR address,  EDX:EAX = 64-bit value to write
rdmsr:  ECX = MSR address → EDX:EAX = 64-bit value read back
```

Both require CPL=0 (ring 0). The value is split across two 32-bit registers for historical reasons (these instructions predate 64-bit mode).

### MMIO vs MSR

MMIO exposes registers through the guest physical address space - accessed with ordinary load/store instructions. MSRs expose registers through dedicated CPU instructions (`rdmsr`/`wrmsr`).

From the guest's perspective both appear as registers, but they belong to different domains:

| | MMIO (Steps 5–6) | MSR (Step 8) |
|---|---|---|
| Belongs to | Devices | The CPU itself |
| Access | `mov` (memory instructions) | `rdmsr` / `wrmsr` |
| Address space | Guest physical addresses | 32-bit MSR index |
| Trap mechanism | Memory slot hole | MSR filter bitmap |

A guest can discover MMIO regions through device enumeration (PCI, ACPI), but MSRs are part of the CPU architecture itself - every CPU core has its own MSR state. In a multi-vCPU VM, each vCPU has its own MSR state, just like a real multicore processor.

## Execution flow

```
VMM (microkvm.c)                         Guest (guest.S)
────────────────                         ───────────────
KVM_ENABLE_CAP(USER_SPACE_MSR)
KVM_X86_SET_MSR_FILTER:
  deny MSR 0x4B564D00
                                         [long mode]
                                           mov ecx, 0x4B564D00
                                           mov eax, 0x42
                                           xor edx, edx
                                           wrmsr
                                                │
KVM_EXIT_X86_WRMSR                              ▼
  run->msr.index = 0x4B564D00
  run->msr.data  = 0x42
  msr_store = 0x42
  run->msr.error = 0
ioctl(KVM_RUN)
                                           rdmsr
                                                │
KVM_EXIT_X86_RDMSR                              ▼
  run->msr.index = 0x4B564D00
  run->msr.data = msr_store (0x42)
  run->msr.error = 0
ioctl(KVM_RUN)
                                           eax = 0x42
                                           add al, '0' → 'r' (0x42+0x30=0x72)
                                           out 0x10, al
```

## Implementation

### VMM: enabling MSR trapping (two-step setup)

```c
/* Step 1: Enable userspace MSR exits */
struct kvm_enable_cap msr_cap = {
    .cap = KVM_CAP_X86_USER_SPACE_MSR,
    .args[0] = KVM_MSR_EXIT_REASON_FILTER,
};
ioctl(vmfd, KVM_ENABLE_CAP, &msr_cap);

/* Step 2: Set filter — deny our custom MSR */
uint8_t msr_bitmap[] = {0x00};  /* bit=0 means deny (trap) */
struct kvm_msr_filter filter = {
    .flags = KVM_MSR_FILTER_DEFAULT_ALLOW,
    .ranges = {{
        .flags = KVM_MSR_FILTER_READ | KVM_MSR_FILTER_WRITE,
        .nmsrs = 1,
        .base = MSR_CUSTOM,       /* 0x4B564D00 */
        .bitmap = msr_bitmap,
    }},
};
ioctl(vmfd, KVM_X86_SET_MSR_FILTER, &filter);
```

`DEFAULT_ALLOW` means MSRs not in the filter are handled normally by KVM. Only our custom MSR is denied (trapped to userspace).

### VMM: handling MSR exits

```c
case KVM_EXIT_X86_WRMSR:
    if (run->msr.index == MSR_CUSTOM) {
        msr_store = run->msr.data;
        run->msr.error = 0;    /* success */
    } else {
        run->msr.error = 1;    /* inject #GP */
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

Setting `error = 0` means success - KVM resumes the guest normally.
Setting `error = 1` causes KVM to inject #GP into the guest.

### Guest: wrmsr and rdmsr

```asm
    /* wrmsr: write 0x42 to MSR 0x4B564D00 */
    .byte 0xB9, 0x00, 0x4D, 0x56, 0x4B  /* mov ecx, 0x4B564D00 */
    .byte 0x31, 0xD2                      /* xor edx, edx */
    .byte 0xB8, 0x42, 0x00, 0x00, 0x00   /* mov eax, 0x42 */
    .byte 0x0F, 0x30                      /* wrmsr */

    /* rdmsr: read back from same MSR */
    .byte 0x0F, 0x32                      /* rdmsr → eax = 0x42 */
    .byte 0x04, 0x30                      /* add al, '0' → 'r' */
    .byte 0xE6, 0x10                      /* out 0x10, al */
```

The guest writes a value to the synthetic MSR, then reads it back and outputs the result via PIO. Both instructions trap to the VMM because the MSR filter denies access to this address.

## Output

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
[PIO out port 0x10] R
[PIO out port 0x10] P
[MMIO write @ 0xd0000] M
[MMIO read  @ 0xd0000] returning 0
[PIO out port 0x10] 0
[MSR write] 0x4b564d00 = 0x42
[MSR read] 0x4b564d00 -> 0x42
[PIO out port 0x10] r
[PIO out port 0x10] I
Guest halted.
```

## Key insight

MSR trapping is the mechanism that enables **paravirtualization**. Unlike MMIO (which emulates real hardware), synthetic MSRs create a direct communication channel between guest and hypervisor that doesn't correspond to any physical device.

### Hardware emulation vs paravirtualization

MMIO emulates a device that could exist on real hardware. Synthetic MSRs expose an interface that exists only because the guest is running under a hypervisor. The guest is no longer talking to a virtual device - it is talking directly to the hypervisor.

This is how KVM implements features like:
- **kvmclock** — guest reads time via MSR instead of calibrating hardware timers
- **steal time** — hypervisor reports how much CPU time was stolen from the guest
- **PV spinlocks** — guest notifies hypervisor when spinning on a lock

The filter mechanism (`DEFAULT_ALLOW` + deny specific MSRs) means the VMM only intercepts what it needs. Standard MSRs (TSC, EFER, APIC) continue to be handled efficiently inside KVM without exiting to userspace.

### Major VM exit types introduced so far (Steps 1–8)

| Exit type | Step | Trigger |
|-----------|------|---------|
| `KVM_EXIT_HLT` | 1, 7 | `hlt` instruction |
| `KVM_EXIT_IO` | 2 | `out` / `in` instruction |
| `KVM_EXIT_MMIO` | 5, 6 | Access to GPA without memory slot |
| `KVM_EXIT_X86_WRMSR` | 8 | `wrmsr` to denied MSR |
| `KVM_EXIT_X86_RDMSR` | 8 | `rdmsr` from denied MSR |

With these exit types, a VMM can implement a surprisingly large subset of device emulation and paravirtualized interfaces. Everything from here builds on this foundation.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| MSR filter | Two-step setup: enable capability + set filter bitmap |
| wrmsr / rdmsr | ECX = address, EDX:EAX = value (64-bit split) |
| Paravirtualization | Synthetic MSRs for guest-hypervisor communication |
| Selective trapping | DEFAULT_ALLOW + deny specific MSRs |
| Error injection | `run->msr.error = 1` causes #GP in guest |

## Next step

[Step 9: Multiple vCPUs](step9_multi-vcpu.md) - so far, the VM has consisted of a single virtual CPU. Real systems are multicore. The next step introduces multiple vCPUs sharing the same guest memory and device state, requiring synchronization between host threads.
