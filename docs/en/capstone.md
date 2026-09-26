# Capstone: Follow RDMSR from Guest to KVM and Back

**RDMSR 0x1b causes a VM exit, but KVM can supply the value, so this handling does not return `KVM_RUN` to microkvm.**

## The question

Connect Part 3's State, Memory, and Control through one `RDMSR IA32_APIC_BASE (0x1b)`: why it exits, who returns the result, and where guest execution resumes.

## Observations

[Step 43](step43_execution-controls.md)'s boot measurement recorded 97 MSR_READ events in `kvm_exit`, and the following read in `kvm_msr`.

```text
msr_read 1b = 0xfee00900
```

97 counts all MSR_READ exits, not only 0x1b. Use this record and KVM source to follow the successful 0x1b path. The diagram below explains processing; it is not a raw trace of one operation. Use Step 43's recording procedure to verify it again.

## The RDMSR round trip

```text
microkvm
    │ ioctl(KVM_RUN)                     userspace → KVM
    ▼
KVM: prepare guest state
    │ VM entry
    ▼
Guest: ECX = 0x1b, execute RDMSR
    │ Use MSR bitmaps enabled, target read bit = 1
    ▼
hardware VM exit                         guest → KVM
    │ Save VMCS-managed guest state and exit information
    │ Load host state
    ▼
KVM: kvm_emulate_rdmsr()
    │ Obtain guest IA32_APIC_BASE
    │ Write result to RAX / RDX and update RIP
    │ VM entry
    ▼
Guest: resume at the instruction after RDMSR
```

### 1. microkvm runs the vCPU

microkvm's vCPU thread calls `ioctl(vcpu->fd, KVM_RUN, NULL)`, handing execution to KVM. KVM synchronizes required guest state to VMCS and performs VM entry.

As seen in [Step 41](step41_vmcs-explorer.md), guest execution state combines VMCS state such as RIP/RSP with registers such as RAX managed in KVM software.

### 2. Hardware intercepts RDMSR

RDMSR reads the MSR indexed by ECX and returns a 64-bit result in EDX:EAX. The index here is `0x1b`.

In this configuration, `Use MSR bitmaps` is enabled and 0x1b's read bit is 1. The CPU intercepts the instruction, records `EXIT_REASON_MSR_READ`, and transfers control to KVM. This is the role of **Control**.

Guest RIP at VM exit points to RDMSR. Hardware saves VMCS-managed guest state and exit information, then loads host state. This is the **State** transition.

### 3. KVM prepares the value to return

The v7.2 VMX exit handler dispatches `EXIT_REASON_MSR_READ` to `kvm_emulate_rdmsr()`. Following retrieval into [x86.c](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L4440) shows the value comes from guest APIC state (excerpt).

```c
case MSR_IA32_APICBASE:
    msr_info->data = vcpu->arch.apic_base;
    break;
```

On a successful read, [`__kvm_emulate_rdmsr()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L2107) calls `trace_kvm_msr_read()` and sets the result for ordinary RDMSR as follows (successful-branch excerpt).

```c
kvm_eax_write(vcpu, data);
kvm_edx_write(vcpu, data >> 32);
```

Returning the observed `0xfee00900` as the RDMSR result sets registers as follows.

```text
EAX = 0xfee00900   (low 32 bits)
EDX = 0x00000000   (high 32 bits)
```

RAX/RDX are not VMCS guest-state fields. KVM updates its register state and applies it to the CPU on the entry path.

### 4. Advance RIP and resume

On success, KVM completes the instruction and advances RIP. VMX instruction skipping uses the length hardware recorded in `VM_EXIT_INSTRUCTION_LEN`. This is the same division seen with CPUID in [Step 41](step41_vmcs-explorer.md): hardware supplies instruction length, and KVM updates RIP.

KVM performs VM entry with the updated state; the guest continues after RDMSR. Since this handling completes within KVM, it never reaches microkvm's `switch (run->exit_reason)`. The same `KVM_RUN` may later return for another reason, such as MMIO or a signal.

## Where Memory fits

[Step 42](step42_ept-explorer.md)'s EPT translates GPA→HPA. RDMSR's operand, however, is an MSR index, not a memory address. **This MSR_READ exit is caused by MSR interception, not an EPT fault.**

EPT supports guest instruction fetches and other memory accesses, but that is separate from the condition intercepting RDMSR.

## Outcomes other than success

### Return an exception to the guest

Existing measurements also recorded this read.

```text
msr_read 64e = 0x0 (#GP)
```

This does not record a successful return of zero. It is the path where KVM rejects the access and injects guest #GP. Instead of advancing past RDMSR as on success, it enters guest exception handling.

VM exit transfers control from guest to KVM; #GP is an exception subsequently delivered to the guest.

### Return handling to userspace

microkvm enables `KVM_MSR_EXIT_REASON_FILTER` through `KVM_CAP_X86_USER_SPACE_MSR` and denies MSR_CUSTOM (`0x20000000`) reads/writes through a filter. These accesses return to microkvm as `KVM_EXIT_X86_RDMSR` / `KVM_EXIT_X86_WRMSR`.

For a read, microkvm sets `run->msr.data` to the stored value, sets `run->msr.error = 0`, and calls `KVM_RUN` again. KVM uses that result to complete the instruction. This is [Step 8](step08_msr.md)'s userspace round trip, unlike 0x1b handling entirely inside KVM.

```text
Read 0x1b
    guest → VM exit → KVM returns value → guest

Read MSR_CUSTOM
    guest → VM exit → KVM_RUN returns → microkvm sets value
                                      ↓ Next KVM_RUN
                       guest ← KVM completes instruction
```

## Completing Part 3

Step 41 related KVM-exposed CPU state to VMCS, Step 42 related memory slots to EPT, and Step 43 examined instruction interception. For RDMSR 0x1b, these relationships are:

- **Control**: the MSR bitmap intercepts RDMSR, causing an MSR_READ exit.
- **State**: hardware switches VMCS state; KVM updates the read result and RIP.
- **Memory**: EPT supports instruction fetches and other accesses but does not cause this exit.
- **Userspace boundary**: KVM handles 0x1b, so this exit does not return to microkvm.

## References

- [Linux KVM VMX implementation](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c): MSR interception, exit handlers, RIP updates
- [Linux KVM x86 implementation](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c): `kvm_emulate_rdmsr()` and MSR handling
- [KVM API](https://docs.kernel.org/virt/kvm/api.html): `KVM_RUN`, `KVM_CAP_X86_USER_SPACE_MSR`, `KVM_X86_SET_MSR_FILTER`
- [Intel Software Developer’s Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): RDMSR, MSR Bitmaps, VM Exits
