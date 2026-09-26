# Step 39: MSR Exit—Separate Exit Conditions from Handling Destinations

## Key point

**An MSR access causing a VM exit does not necessarily return handling to microkvm.** Record boot-time MSR processing and distinguish internal KVM handling, guest exceptions, and userspace returns.

MSRs are CPU control/status registers accessed through `rdmsr` / `wrmsr`. Ordinary `rdmsr` specifies the MSR index in ECX and receives the value in EDX:EAX.

```text
Guest rdmsr / wrmsr
  → Hardware intercept configuration
      ├─ Not intercepted → avoid VM exit for the MSR access
      └─ VM exit → KVM MSR handling
                     ├─ Success → update value/state and resume guest
                     ├─ Rejected → guest #GP
                     └─ Matches configured conditions → return to userspace
```

VMX MSR bitmaps specify read/write interception and are managed by KVM. Step 8's MSR filter and userspace MSR handling instead configure how KVM handles accesses. **Exit conditions and subsequent handling destinations are separate decisions.** Step 43 covers the bitmap structure.

## Preparation

Use Terminal A for microkvm and Terminal B for host tracing. Stop other VMs, perf, and microkvm. This recording starts before boot.

Check the tracepoint in Terminal B, assuming host KVM modules are loaded.

```bash
trace_dir=/sys/kernel/tracing
sudo cat "$trace_dir/events/kvm/kvm_msr/format"
```

Check the fields `write` (read/write), `ecx` (MSR index), `data`, and `exception`. Log MSR indices are hexadecimal without `0x`, such as `1b`. Keep using the same Terminal B.

## Procedure and output

### 1. Save boot-time MSR handling

In Terminal B, disable previous events and record only `kvm_msr`.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_msr/filter"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_msr/enable"
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

Then start in Terminal A.

```bash
cd ~/microkvm
./microkvm
```

When the guest `/ #` prompt appears, stop/save in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-msr-boot.txt
head -60 ~/kvm-msr-boot.txt
```

### 2. Aggregate MSR indices and read/write operations

Aggregate the saved file in Terminal B.

```bash
grep -oE 'msr_(read|write)[[:space:]]+[0-9a-fA-Fx]+' ~/kvm-msr-boot.txt \
    | sort | uniq -c | sort -rn | head -30
```

The measurement had 54 `msr_read 839` events and six `msr_read 1a0` events. Main MSRs are listed below; counts and types vary with guest/host configuration.

| Observed MSR index | Meaning |
|------------------|------|
| `839`, `838`, `832`, etc. | x2APIC registers |
| `1a0` | IA32_MISC_ENABLE |
| `1b` | IA32_APIC_BASE |
| `c0000080` | EFER |
| `c0000081`, `c0000082`, `c0000084` | Syscall settings |
| `4b564d07`, etc. | KVM paravirtualization features |

This aggregation includes accesses that caused exceptions. Check `(#GP)` in the original lines next to determine whether KVM handled them successfully.

### 3. Check #GP in the same recording

```bash
grep 'kvm_msr:.*#GP' ~/kvm-msr-boot.txt
```

Example excerpt:

```text
kvm_msr: msr_read  1a6 = 0x0    (#GP)
kvm_msr: msr_read  1a7 = 0x0    (#GP)
kvm_msr: msr_read  3f6 = 0x0    (#GP)
kvm_msr: msr_read  3f7 = 0x0    (#GP)
kvm_msr: msr_write 1d9 = 0x4000 (#GP)
kvm_msr: msr_read  64e = 0x0    (#GP)
```

`(#GP)` indicates handling that returns a general protection exception to the guest. On a read, `= 0x0` does not mean a successful return of zero.

This example includes PMU-related `0x1a6` / `0x1a7` / `0x3f6` / `0x3f7` and DEBUGCTL (`0x1d9`). These are not targets of microkvm's MSR filter; KVM rejected them in this configuration.

## Reading the output

### 1. kvm_msr does not record userspace returns

`kvm_msr` is a tracepoint inside KVM MSR handling. Seeing APICBASE or EFER handling does not mean it reached microkvm's `KVM_EXIT_X86_RDMSR` handler.

microkvm configures userspace returns for `MSR_CUSTOM` (`0x20000000`). This MSR does not appear in the Linux boot recording here.

### 2. Do not infer passthrough from an absent MSR log

IA32_TSC (`0x10`) did not appear in this aggregation. However, `RDTSC` / `RDTSCP`, commonly used to read time, are different instructions from `RDMSR 0x10`. **An absent log alone cannot distinguish bitmap-based exit avoidance from the guest never accessing that MSR.**

## Checking the KVM implementation

Inspect post-read branching in Linux v7.2's [`__kvm_emulate_rdmsr()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L2107). This function excerpt marks omissions with comments.

```c
/* Variable declarations omitted */
r = kvm_emulate_msr_read(vcpu, msr, &data);

if (!r) {
    trace_kvm_msr_read(msr, data);
    /* Reflect the read value in guest registers */
} else {
    if (kvm_msr_user_space(vcpu, msr, KVM_EXIT_X86_RDMSR, 0,
                          complete_rdmsr, r))
        return 0;
    trace_kvm_msr_read_ex(msr);
}

return kvm_x86_call(complete_emulated_msr)(vcpu, r);
```

On success, the value is written to guest state. On failure, KVM first checks whether userspace handling is configured; otherwise it records the exception trace and proceeds to completion handling. Procedure 3's `(#GP)` corresponds to this exception path.

[`kvm_msr_user_space()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L2085) checks whether the failure reason is enabled for userspace handling.

```c
u64 msr_reason = kvm_msr_reason(r);

if (!(vcpu->kvm->arch.user_space_msr_mask & msr_reason))
    return 0;

vcpu->run->exit_reason = exit_reason;
/* MSR index, data, completion setup, and other details omitted */
```

Step 8 enabled `KVM_MSR_EXIT_REASON_FILTER` and denied only MSR_CUSTOM reads/writes through the filter. This combination returns `KVM_EXIT_X86_RDMSR` / `KVM_EXIT_X86_WRMSR` to microkvm. **Filter denial alone does not unconditionally return to userspace.**

## Stop tracing

Disable the event in Terminal B. The saved file can also be used for Step 40's comparison.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
```

## What we learned

- Distinguish MSR VM-exit conditions from KVM handling-destination settings.
- `kvm_msr` does not imply a userspace return; `(#GP)` is not a successful read.
- CPU features exposed to the guest affect which MSRs it accesses during boot.

## Next step

[Step 40: CPUID Exit](step40_cpuid-exit.md) changes the exposed PMU information and compares MSR accesses. PMU-related and DEBUGCTL #GP counts fell from eight to zero, while `0x64e` #GP remained. Examine this through CPUID and guest initialization.
