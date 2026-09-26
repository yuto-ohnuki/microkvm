# Step 41: VMCS Explorer—Read Guest CPU State through KVM

> **Part 3: Explore VT-x Through the KVM Interface**
>
> Part 3 uses microkvm's KVM APIs and existing tracing facilities to understand VT-x state management, memory translation, and execution controls. Relate observations to Linux KVM source and the Intel SDM, following guest instructions through VM exit and resumption.

## Key point

**Guest CPU state returned by KVM APIs is not a raw VMCS dump.** Relate monitor output to VMCS responsibilities, then determine whether hardware or KVM advances RIP around a CPUID exit.

```text
Guest state exposed by KVM APIs ← KVM conversion, caching, synchronization → VMCS
                                ↓
                      Update CPUID response and RIP
```

## Background

### The role of VMCS

The VMCS (Virtual Machine Control Structure) is the structure Intel VT-x uses to control guest execution and VM entry/exit. KVM's VMX implementation manages it through VMREAD, VMWRITE, and related operations.

| Category | Role |
|------|------|
| Guest-state area | Guest RIP, RSP, control registers, segments, etc. |
| Host-state area | Host RIP, RSP, CR3, etc., restored on VM exit |
| VM-execution controls | Guest execution behavior and selectable exit conditions |
| VM-entry / VM-exit controls | State handling during entry/exit |
| VM-exit information | Exit reason, instruction length, and other handling information |

VM entry loads guest state; VM exit saves guest state and loads host state. Hardware switches state according to the relevant fields and controls; KVM prepares and handles the surrounding work. General-purpose registers such as RAX are not saved in VMCS, so KVM must also save/restore them.

### Connection to Parts 1 and 2

Step 21's snapshot retrieved CPU state through `KVM_GET_REGS` / `KVM_GET_SREGS`. Here, display values from those same APIs. Combine Step 36's VM entry/exit path with Step 40's CPUID handling to locate state changes.

## Implementation added in this step

Three files change: `microkvm.c`, `snapshot.c`, and `snapshot.h`.

### Request a dump from the monitor

`stdin_thread()` sets a request flag on `< Ctrl-A p >`.

```c
if (c == 'p') {
    fprintf(stderr, "\n[monitor] dumping guest state (Ctrl-A p)\n");
    dump_requested = 1;
    continue;
}
```

`dump_requested` is declared `static volatile sig_atomic_t`. The vCPU thread checks it at the top of the loop and captures state before the next `KVM_RUN`.

```c
if (dump_requested) {
    dump_cpu_state(vcpu->fd);
    dump_requested = 0;
}
if (ioctl(vcpu->fd, KVM_RUN, NULL) < 0) {
    perror("KVM_RUN");
    return NULL;
}
```

The vCPU thread captures state after returning from `KVM_RUN`, avoiding a wait caused by issuing an ioctl for the same vCPU from the input thread. The output reflects the time the request is processed, not the instant the key is pressed. A hardware VM exit alone does not reach the loop top; `KVM_RUN` must return to userspace.

### Display state exposed by KVM

`dump_cpu_state()` in `snapshot.c` calls these two APIs.

```c
struct kvm_regs regs;
ioctl(vcpufd, KVM_GET_REGS, &regs);

struct kvm_sregs sregs;
ioctl(vcpufd, KVM_GET_SREGS, &sregs);
```

From `regs`, display RIP / RSP / RFLAGS and RAX / RBX / RCX / RDX; from `sregs`, CR0 / CR3 / CR4 / EFER and CS / SS. Extract EFER.LMA with `(sregs.efer >> 10) & 1`. This display does not use `KVM_GET_MSRS`.

## Observation A: Read guest state

Boot the Linux guest in Terminal A and enter the monitor command at the prompt.

```bash
cd ~/microkvm
./microkvm
```

```text
/ # < Ctrl-A p >
[monitor] dumping guest state (Ctrl-A p)
```

The following is an excerpt captured from a 64-bit Linux guest. Addresses and register values vary with environment and capture timing.

```text
RIP    = 0xffffffff8135de91
RSP    = 0xffffc90000003f10
RFLAGS = 0x0000000000000006
RAX=0xffff888000155e00 RBX=0xffffffff81785bc0
RCX=0x0 RDX=0x00000000000003fa
CR0    = 0x80050033
CR3    = 0x0000000004117006
CR4    = 0x00000000007706b0
EFER   = 0x0000000000000d01
CS: sel=0x0010 type=0xb db=0 l=1
SS: sel=0x0018 type=0x3
```

CR0.PG=1, CR4.PAE=1, and EFER.LMA=1 indicate active long-mode paging. CS.l=1 / db=0 identifies a 64-bit code segment. RFLAGS.IF=0 means maskable interrupts were disabled at capture time.

### Mapping KVM state to VMCS

| Displayed state | Related VMCS fields | Interpretation |
|--------------|-------------------|------------|
| RIP / RSP / RFLAGS | GUEST_RIP / GUEST_RSP / GUEST_RFLAGS | Managed through KVM caching and synchronization |
| CR0 / CR4 | GUEST_CR0 / GUEST_CR4, READ_SHADOW, guest/host masks | Distinguish guest-visible logical values from hardware values |
| CR3 | GUEST_CR3 | Guest page-table root, separate from EPT pointer |
| EFER | GUEST_IA32_EFER, etc. | Managed according to EFER switching controls on VM entry/exit |
| CS / SS | GUEST_CS_* / GUEST_SS_* | Split into selector, base, limit, and access rights |
| RAX / RBX / RCX / RDX | No corresponding guest-state fields | Managed by KVM software state and entry/exit code |

This VMCS explorer follows VMCS responsibilities by relating KVM-exposed state to KVM source. KVM APIs return logical guest state, with conversion and synchronization between it and VMCS; the displayed values are not a raw VMCS dump.

Linux v7.2's [`vmx_set_cr0()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L3448) separates guest-visible and hardware values.

```c
vmcs_writel(CR0_READ_SHADOW, cr0);
vmcs_writel(GUEST_CR0, hw_cr0);
vcpu->arch.cr0 = cr0;
```

The read shadow applies to bits covered by the guest/host mask. Because KVM maps guest-visible state to hardware configuration this way, API values do not always equal raw VMCS values.

## Observation B: Follow RIP around a CPUID exit

Stop the VM in Terminal A. Configure tracing first in Terminal B to capture boot, when CPUID executes frequently. Use an Intel VMX host and stop other VMs and perf.

### 1. Configure and start recording

In Terminal B, check the required events and filter fields.

```bash
trace_dir=/sys/kernel/tracing
sudo cat "$trace_dir/events/kvm/kvm_exit/format"
sudo ls "$trace_dir/events/kvm/kvm_entry/enable"
```

Check `exit_reason`, then record CPUID exits and all entries. Intel VMX exit reason `10` is CPUID.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_entry/filter"
echo 'exit_reason == 10' | sudo tee "$trace_dir/events/kvm/kvm_exit/filter"
sudo cat "$trace_dir/events/kvm/kvm_exit/filter"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_entry/enable"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_exit/enable"
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

### 2. Boot and save the log

Start in Terminal A.

```bash
./microkvm
```

When the guest prompt appears, stop/save in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-cpuid-rip.txt
grep -A 1 'kvm_exit:.*reason CPUID' ~/kvm-cpuid-rip.txt | head -40
```

### 3. Read an exit and the next entry for the same vCPU

Match a CPUID exit with the subsequent entry from the same vCPU thread. Check the vCPU ID too if displayed. If other threads interleave, read the saved file with `less` rather than relying only on adjacent lines. If CPUID is absent, check recording order and whether buffer overwrite lost early-boot records.

RIP pairs from multiple measurements:

| RIP at CPUID exit | RIP at next entry | Difference |
|------------------|------------------|------|
| 0x1e32c9 | 0x1e32cb | +2 |
| 0x1e3312 | 0x1e3314 | +2 |
| 0x1e3347 | 0x1e3349 | +2 |

CPUID's instruction bytes are `0F A2`; RIP advances two bytes here. `kvm_entry` / `kvm_exit` are tracepoints inside KVM. Compare this observation with source to identify who performs the update.

### KVM advances RIP

On a CPUID VM exit, hardware records the guest RIP pointing to the instruction itself and its length in VMCS. KVM applies the response to registers in `kvm_emulate_cpuid()`, then advances RIP through `kvm_skip_emulated_instruction()` and VMX's `skip_emulated_instruction()`.

The following extracts the CPUID-relevant part of Linux v7.2's [`skip_emulated_instruction()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L1791), omitting intermediate checks and adjustments.

```c
instr_len = vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
/* Instruction-length checks and other details omitted */
orig_rip = kvm_rip_read(vcpu);
rip = orig_rip + instr_len;
/* Execution-mode-dependent RIP adjustment omitted */
kvm_rip_write(vcpu, rip);
```

The updated RIP is held in KVM state and synchronized to VMCS before the next entry inside [`vmx_vcpu_run()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L7546).

```c
if (kvm_register_is_dirty(vcpu, VCPU_REGS_RSP))
    vmcs_writel(GUEST_RSP, vcpu->arch.regs[VCPU_REGS_RSP]);
if (kvm_register_is_dirty(vcpu, VCPU_REG_RIP))
    vmcs_writel(GUEST_RIP, vcpu->arch.rip);
```

Hardware records instruction length; KVM uses it to update RIP. This CPUID handling completes inside KVM and does not return to microkvm because of that exit.

## State transitions on VM entry/exit

```text
microkvm: ioctl(KVM_RUN)
    ↓
KVM: prepare guest state and synchronize required values to VMCS
    ↓ VM entry (VMLAUNCH / VMRESUME)
Hardware: load guest state from VMCS
    ↓
Guest: execute CPUID
    ↓ VM exit
Hardware: save VMCS-managed guest state, record exit information, load host state
    ↓
KVM: apply CPUID response to RAX/RBX/RCX/RDX
     Update RIP from 0x1e32c9 → 0x1e32cb
    ↓ VM entry
Guest: resume at the instruction after CPUID
```

Hardware switches state according to VMCS; KVM interprets the exit and prepares guest state for subsequent execution. This round trip can repeat within one `KVM_RUN` call.

## Stop tracing

In Terminal B, disable the events and clear the CPUID filter.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_exit/filter"
```

## What we learned

- KVM API guest state relates to raw VMCS through conversion and synchronization.
- Distinguish VMCS-managed state from general-purpose registers saved/restored by KVM software.
- For CPUID, hardware records instruction length; KVM supplies the response and updates RIP.
- Hardware VM exit and `KVM_RUN` returning to userspace are separate boundaries.

## References

- [KVM API: KVM_GET_REGS / KVM_GET_SREGS](https://docs.kernel.org/virt/kvm/api.html)
- [Linux v7.2 KVM VMX implementation](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c): register synchronization, segment conversion, instruction skipping
- [Linux v7.2 KVM CPUID implementation](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/cpuid.c): `kvm_emulate_cpuid()`
- [Intel Software Developer’s Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): Volume 3, VMCS, VM Entries, VM Exits

## Next step

[Step 42](step42_ept-explorer.md) starts with memory slot display to explore guest addresses, host backing memory, and EPT.
