# Step 40: CPUID Exit—Exposed CPU Features Change Guest Behavior

## Key point

**CPUID informs the guest which CPU features to use; changing responses changes guest initialization.** Observe boot-time CPUID and compare Step 39's MSR accesses with PMU (performance monitoring) no longer exposed.

```text
microkvm registers responses through KVM_SET_CPUID2
  → Guest cpuid → VM exit → KVM responds from registered information
  → Guest initializes according to exposed features
```

microkvm obtains KVM-supported responses through `KVM_GET_SUPPORTED_CPUID`, clears the TSC-Deadline bit, and registers them for the vCPU. It does not pass raw host CPUID through unchanged. The normal configuration here retains PMU leaf `0xA`.

Runtime CPUID handling stays inside KVM. A hardware VM exit occurs, but microkvm needs no CPUID exit handler.

## Preparation

Use Terminal A to build/start microkvm and Terminal B for host recording. Begin with other VMs, perf, and microkvm stopped. Assume host KVM modules are loaded.

Check the two events in Terminal B.

```bash
trace_dir=/sys/kernel/tracing
sudo cat "$trace_dir/events/kvm/kvm_cpuid/format"
sudo ls "$trace_dir/events/kvm/kvm_msr/enable"
```

In `kvm_cpuid`, `function` is the leaf, `index` the subleaf, `rax/rbx/rcx/rdx` the response, and `found` indicates a matching entry. Record CPUID and MSR together below.

## Procedure and output

### 1. Record boot with normal settings

Configure and start recording in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
for e in kvm_cpuid kvm_msr; do
    echo 0 | sudo tee "$trace_dir/events/kvm/$e/filter"
    echo 1 | sudo tee "$trace_dir/events/kvm/$e/enable"
done
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

Start in Terminal A.

```bash
cd ~/microkvm
./microkvm
```

When the guest `/ #` prompt appears, stop/save in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-cpuid-baseline.txt
grep 'kvm_cpuid:' ~/kvm-cpuid-baseline.txt | head -30
```

Example response excerpt:

```text
func 0        rax 24 rbx 756e6547 rcx 6c65746e rdx 49656e69  found
func 1        rax a06d1 rbx 2100800 rcx f6fab223 rdx f8bfbff found
func a idx 0  rax 8300802 rbx 0 rcx 0 rdx 8602               found
func 40000000 rax 40000001 rbx 4b4d564b rcx 564b4d56 rdx 4d  found
```

Focus on two points:

- EAX (shown as `rax`) `0x08300802` for `func a`: the low eight bits indicate PMU version `2`; the next eight indicate `8` general-purpose counters.
- `func 40000000`: returns the `KVMKVMKVM` signature. Combined with feature leaf `0x40000001`, the guest detects KVM paravirtualization features.

All boot-log entries were `found`. This means requested entries were found, not that every individual feature's behavior was verified.

If needed, aggregate counts by leaf from the same saved file.

```bash
grep 'kvm_cpuid:' ~/kvm-cpuid-baseline.txt \
    | grep -oE 'func [0-9a-fA-F]+' | sort | uniq -c | sort -rn
```

### 2. Change only PMU exposure and rebuild

Stop the VM in Terminal A. In `microkvm.c`'s CPUID setup, temporarily add the following loop after the existing loop clearing leaf 1's TSC-Deadline bit and immediately before `KVM_SET_CPUID2`.

```c
/* Experiment: do not expose PMU to the guest */
for (int j = 0; j < (int)cpuid.header.nent; j++) {
    if (cpuid.entries[j].function == 0xA) {
        cpuid.entries[j].eax = 0;
        cpuid.entries[j].ebx = 0;
        cpuid.entries[j].ecx = 0;
        cpuid.entries[j].edx = 0;
    }
}
```

Add a separate loop because the existing leaf 1 loop contains `break`. Leave other CPUID settings and the guest kernel unchanged, then rebuild in Terminal A.

```bash
make
```

Do not start the VM yet; begin the next recording first.

### 3. Record the modified boot under the same conditions

In Terminal B, start recording with procedure 1's event settings.

```bash
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

Start the modified VM in Terminal A.

```bash
./microkvm
```

When the guest `/ #` appears, stop recording in Terminal B and save to a different file.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-cpuid-no-pmu.txt
grep -E 'kvm_cpuid:.*func a([[:space:]]|$)' ~/kvm-cpuid-no-pmu.txt
```

First verify that leaf `a` has zero in all of `rax/rbx/rcx/rdx`. If the line is missing or values are unchanged, check recording timing, code placement, the build, and which executable was started.

### 4. Compare MSR #GP events

In Terminal B, aggregate both files using identical criteria.

```bash
$ for log in ~/kvm-cpuid-baseline.txt ~/kvm-cpuid-no-pmu.txt; do
    echo "$log"
    grep 'kvm_msr:.*#GP' "$log" \
        | grep -Ec 'msr_(read|write)[[:space:]]+(1a6|1a7|3f6|3f7|1d9)[[:space:]]'
done
/home/fedora/kvm-cpuid-baseline.txt
8
/home/fedora/kvm-cpuid-no-pmu.txt
0
```

The measured target-MSR #GP count was **eight with normal settings and zero with PMU hidden**. Counts vary, so compare against your own baseline. If the normal configuration already has zero, this environment cannot demonstrate the same reduction.

To distinguish disappearance of #GP from reduced access itself, also compare target-MSR lines without filtering for exceptions.

```bash
$ for log in ~/kvm-cpuid-baseline.txt ~/kvm-cpuid-no-pmu.txt; do
    echo "$log"
    grep -E 'kvm_msr:.*msr_(read|write)[[:space:]]+(1a6|1a7|3f6|3f7|1d9)[[:space:]]' "$log"
done
/home/fedora/kvm-cpuid-baseline.txt
        microkvm-14196   [001] ..... 15326.957001: kvm_msr: msr_read 1a6 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.957013: kvm_msr: msr_read 1a7 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.957023: kvm_msr: msr_read 3f6 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.957033: kvm_msr: msr_read 3f7 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.957043: kvm_msr: msr_read 3f7 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.957053: kvm_msr: msr_read 3f7 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.957063: kvm_msr: msr_read 3f7 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.973043: kvm_msr: msr_read 1d9 = 0x0
        microkvm-14196   [001] ..... 15326.973053: kvm_msr: msr_write 1d9 = 0x4000 (#GP)
/home/fedora/kvm-cpuid-no-pmu.txt
```

#GP disappeared for the compared PMU-related MSRs and DEBUGCTL, while `0x64e` #GP remained. Hiding PMU does not eliminate every #GP.

```bash
$ grep -E 'kvm_msr:.*msr_read[[:space:]]+64e[[:space:]].*#GP' \
    ~/kvm-cpuid-baseline.txt ~/kvm-cpuid-no-pmu.txt
/home/fedora/kvm-cpuid-baseline.txt:        microkvm-14196   [001] ..... 15327.220312: kvm_msr: msr_read 64e = 0x0 (#GP)
/home/fedora/kvm-cpuid-no-pmu.txt:        microkvm-14589   [002] ..... 15467.774987: kvm_msr: msr_read 64e = 0x0 (#GP)
```

## Checking the KVM implementation

### KVM responds using registered CPUID

Linux v7.2's [`kvm_emulate_cpuid()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/cpuid.c#L2160) reads the guest request and writes the response to registers. Excerpt:

```c
/* Declarations and CPUID permission checks omitted */
eax = kvm_eax_read(vcpu);
ecx = kvm_ecx_read(vcpu);
kvm_cpuid(vcpu, &eax, &ebx, &ecx, &edx, false);
kvm_eax_write(vcpu, eax);
kvm_ebx_write(vcpu, ebx);
kvm_ecx_write(vcpu, ecx);
kvm_edx_write(vcpu, edx);
return kvm_skip_emulated_instruction(vcpu);
```

EAX specifies the leaf and ECX the subleaf. `kvm_cpuid()` responds from the vCPU's CPUID entries, reflects the result in guest registers, and advances the instruction. microkvm need not respond at runtime.

### Leaf 0xA also configures KVM's PMU

Updating CPUID configuration also reaches `kvm_pmu_refresh()`. [`intel_pmu_refresh()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/pmu_intel.c#L524) reads leaf `0xA` as follows.

```c
/* Earlier function processing omitted */
entry = kvm_find_cpuid_entry(vcpu, 0xa);
if (!entry)
    return;

eax.full = entry->eax;
edx.full = entry->edx;

pmu->version = eax.split.version_id;
if (!pmu->version)
    return;
```

Thus this change affects both guest feature detection and KVM's virtual PMU configuration. The comparison shows that PMU exposure changes boot-time MSR accesses. Exposing PMU does not imply support for every model-specific PMU-related MSR.

## Finish the experiment and restore the original settings

Stop the VM in Terminal A, delete **only the experimental loop added in procedure 2**, and run `make`.

Disable recording in Terminal B. Keep the comparison logs.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
```

## What we learned

- CPUID responses are registered at startup; guest instructions are handled inside KVM.
- Changing exposed CPU features changes guest initialization and MSR accesses.
- CPUID also configures KVM's internal features, not just guest-visible information.

## Completing Part 2

Steps 33–40 distinguished hardware VM exits from userspace returns and compared perf/ftrace observations with KVM source. Looking beyond counts and timings to where values were recorded and what changed in a comparison helps clarify KVM's and microkvm's roles.
