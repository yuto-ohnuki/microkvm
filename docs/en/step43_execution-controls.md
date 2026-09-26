# Step 43: Execution Controls—When Instructions Cause VM Exits

**CPUID exits unconditionally, HLT uses a control bit, and RDMSR uses a per-MSR bitmap.** Instruction interception has different configuration granularities.

## The question

Why do CPUID, HLT, and RDMSR each cause VM exits? Confirm exits in traces and compare them with Linux v7.2's KVM implementation.

| Instruction | Mechanism determining VM exit | Observation here |
|------|----------------------|------------|
| CPUID | Unconditional exit in VMX non-root | Boot-time CPUID exits |
| HLT | `HLT exiting` control | Idle HLT exits |
| RDMSR | `Use MSR bitmaps` and target MSR read bit | IA32_APIC_BASE (0x1b) reads |

The condition causing an exit and KVM's subsequent decision to return handling to userspace are separate. Focus on the former here, then follow KVM's handling destination.

## Roles of microkvm and KVM

This step adds no monitor commands or implementation; it uses the existing Linux guest and KVM tracing.

microkvm configures CPUID responses, MSR filters, and related settings through KVM APIs. KVM's VMX implementation configures VMCS execution controls and hardware MSR bitmaps. Step 8's userspace MSR filter and the hardware MSR bitmap examined here operate at different layers.

## Observation procedure

### 1. Start recording in Terminal B (host)

Use an Intel VMX host with other VMs stopped. First confirm the required events are available.

```bash
trace_dir=/sys/kernel/tracing
sudo ls "$trace_dir/events/kvm/kvm_exit/enable" "$trace_dir/events/kvm/kvm_msr/enable"
```

Start recording before starting microkvm.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_exit/filter"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_msr/filter"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_exit/enable"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_msr/enable"
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

### 2. Start the VM in Terminal A

Run from the microkvm directory and wait for the guest prompt.

```bash
./microkvm
```

### 3. Stop, save, and read in Terminal B

Stop recording when the prompt appears so idle events do not overwrite boot records.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-controls.txt
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_exit/enable"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_msr/enable"

grep -E 'reason (CPUID|HLT|MSR_READ)' ~/kvm-controls.txt | head -30
grep -c 'reason MSR_READ' ~/kvm-controls.txt
grep -E 'msr_read[[:space:]]+1b[[:space:]]' ~/kvm-controls.txt
```

`kvm_exit` is an observation point in KVM exit handling that records hardware exit reasons. `kvm_msr` records MSR indices and handling results. Counts below are existing sample measurements; new runs need not match them.

## CPUID: Exits by VMX specification

Step 41 observed this CPUID exit. Main trace fields are excerpted below.

```text
reason CPUID rip 0x1e32c9
```

CPUID executed in VMX non-root always causes a VM exit. Unlike HLT, it has no dedicated control to enable/disable the exit. `KVM_SET_CPUID2` specifies information returned to the guest, not whether CPUID exits.

KVM dispatches `EXIT_REASON_CPUID` to `kvm_emulate_cpuid()` and responds using registered CPUID information. As confirmed in Step 41, it then advances RIP and resumes the guest.

## HLT: Exits through an execution-control bit

The idle Linux guest produced this exit.

```text
reason HLT rip 0xffffffff8138bcd5
```

HLT causes a VM exit when `HLT exiting` in primary processor-based execution controls is 1. Linux KVM names this bit `CPU_BASED_HLT_EXITING`; it is enabled in this configuration.

KVM v7.2 includes `CPU_BASED_HLT_EXITING` among required controls, but [vmx_exec_control()](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L4661) adjusts it as follows (excerpt).

```c
if (kvm_hlt_in_guest(vmx->vcpu.kvm))
    exec_control &= ~CPU_BASED_HLT_EXITING;
```

Do not infer "always enabled" from the required definition alone; read the final per-VM configuration. The observed HLT exit follows a path with this bit enabled.

Here KVM receives HLT and keeps the vCPU waiting until an interrupt or another event permits resumption. Unlike CPUID, a control chooses whether hardware executes HLT or exits so KVM can manage the wait.

## RDMSR: Select interception per MSR with a bitmap

### Two-stage decision

RDMSR interception depends on `Use MSR bitmaps` and the read bitmap.

```text
Use MSR bitmaps = 0
    → RDMSR causes a VM exit

Use MSR bitmaps = 1
    → Is the target MSR within the bitmap ranges?
         ├─ Outside → VM exit
         └─ Inside → read bit 1 causes VM exit
                     read bit 0 causes no exit under this condition
```

The bitmap covers `0x00000000–0x00001fff` and `0xc0000000–0xc0001fff`, with separate read/write bits. VMCS holds the address of the 4 KiB bitmap. Disabling bitmaps means intercepting everything, not executing everything directly.

### Follow IA32_APIC_BASE (0x1b)

The boot measurement had **97** `MSR_READ` exits, and `kvm_msr` recorded the following read multiple times.

```text
msr_read 1b = 0xfee00900
```

97 is the total MSR_READ count, not the count for 0x1b alone. The `kvm_msr` line shows KVM handling an IA32_APIC_BASE read.

[alloc_loaded_vmcs()](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L3080) initializes all bitmap bits to 1 (excerpt).

```c
memset(loaded_vmcs->msr_bitmap, 0xff, PAGE_SIZE);
```

Then [vmx_set_intercept_for_msr()](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L4109) clears bits for MSRs allowed direct access. The read-side branch is shown below (excerpt).

```c
if (type & MSR_TYPE_R) {
    if (!set && kvm_msr_allowed(vcpu, msr, KVM_MSR_FILTER_READ))
        vmx_clear_msr_bitmap_read(msr_bitmap, msr);
    else
        vmx_set_msr_bitmap_read(msr_bitmap, msr);
}
```

IA32_APIC_BASE is not cleared in this configuration, so read interception remains enabled. Calculate 0x1b's position from the low-MSR-read region's start (the start of the 4 KiB bitmap) as follows.

```text
byte offset = 0x1b / 8 = 3
bit         = 0x1b % 8 = 3
mask        = 1 << 3   = 0x08
```

With this bit set to 1, guest RDMSR follows this path.

```text
Guest: RDMSR IA32_APIC_BASE (ECX = 0x1b)
    ↓ Use MSR bitmaps + target read bit = 1
hardware: EXIT_REASON_MSR_READ
    ↓
KVM: kvm_emulate_rdmsr()
    ↓ Obtain the APIC base value exposed to guest
Write result to RAX / RDX and update RIP
    ↓
Resume guest
```

This explanation connects traces to KVM initialization/configuration; it is not a direct reading of the bitmap referenced by VMCS. KVM can handle this 0x1b read internally, so the exit need not return to microkvm.

### Separate exit occurrence from handling outcome

After intercepting RDMSR, KVM may return a value or generate guest #GP for a rejected access. If the relevant reason is configured for userspace handling, it returns `KVM_EXIT_X86_RDMSR`. **VM exit transfers control to KVM; #GP is a guest-visible exception.** They are not the same event.

An MSR's absence from the log also does not prove direct execution through the bitmap; the guest may never have executed that RDMSR. This step verified the intercepted handling path for 0x1b.

## What we learned

- CPUID exits unconditionally, HLT through a control bit, and RDMSR through a decision involving bitmaps.
- MSR bitmaps specify interception separately by read/write and MSR index.
- Hardware interception and KVM handling outcomes/userspace returns are separate decisions.

## References

- [Linux KVM VMX implementation](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c): `vmx_exec_control()`, MSR interception, exit handlers
- [Linux KVM VMX control definitions](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.h): required/optional controls and MSR bitmap operations
- [Linux KVM MSR handling](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c): `kvm_emulate_rdmsr()`, register results, exception/userspace branches
- [Intel Software Developer’s Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): Volume 3, VM-Execution Controls, Instructions That Cause VM Exits, MSR Bitmaps

## Next step

[Capstone](capstone.md) combines State, Memory, and Control through RDMSR 0x1b, explaining one continuous flow from microkvm to guest execution and resumption.
