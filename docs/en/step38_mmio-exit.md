# Step 38: MMIO Exit—From EPT Exits to KVM_EXIT_MMIO

## Key point

**The absence of `kvm_page_fault` does not necessarily mean MMIO VM exits have stopped.** Observe virtio-mmio's initial EPT violation and subsequent EPT misconfiguration after creation of an MMIO SPTE.

```text
Guest MMIO access
  → EPT violation / EPT misconfiguration (hardware VM exit)
  → KVM identifies MMIO and emulates the instruction
  → KVM_EXIT_MMIO (userspace return reason)
  → microkvm device handling
```

EPT translates guest physical addresses (GPAs) to host physical addresses. virtio-mmio at `0xd0000000` is not registered as a RAM memslot, so these accesses proceed to KVM MMIO handling. RAM accesses can also cause EPT violations when mappings are created, so EPT violation and MMIO are not synonymous.

## Preparation

Use Intel VMX / EPT. Terminal A handles microkvm/guest operations; Terminal B handles host tracing. Stop other VMs and perf, and **start with microkvm stopped too**, so recording begins before boot and captures the first access.

In Terminal B, check the required events and function (assuming host KVM modules are loaded).

```bash
trace_dir=/sys/kernel/tracing
for e in kvm_mmio kvm_page_fault kvm_emulate_insn vcpu_match_mmio; do
    sudo ls "$trace_dir/events/kvm/$e/enable"
done
sudo grep 'handle_ept_misconfig' "$trace_dir/available_filter_functions"
```

Confirm the four events and `handle_ept_misconfig` (possibly with a suffix) are available. Keep using the same Terminal B.

## Procedure and output

### 1. Record initial accesses during boot

In Terminal B, configure tracepoints and start recording.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
for e in kvm_mmio kvm_page_fault kvm_emulate_insn vcpu_match_mmio; do
    echo 0 | sudo tee "$trace_dir/events/kvm/$e/filter"
    echo 1 | sudo tee "$trace_dir/events/kvm/$e/enable"
done
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

Then start in Terminal A. This observation uses neither ioeventfd nor irqfd.

```bash
cd ~/microkvm
unset USE_IOEVENTFD USE_IRQFD
./microkvm
```

When the guest `/ #` prompt appears, stop/save in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-mmio-boot.txt
grep -iE 'd000[0-9a-f]{4}' ~/kvm-mmio-boot.txt | head -40
```

Example excerpt of initial virtio-mmio access:

```text
kvm_page_fault: rip 0xffffffff8134c508 address 0xd0000000 error_code 0x181
vcpu_match_mmio: gpa 0xd0000000 Read GPA
kvm_mmio: mmio unsatisfied-read len 4 gpa 0xd0000000 val 0x0
kvm_mmio: mmio read len 4 gpa 0xd0000000 val 0x74726976
```

`0x74726976` is the virtio MagicValue implemented in Part 1. This recording shows `kvm_page_fault` on the first access, but not on subsequent accesses to the same page, such as `0xd0000004`.

Boot also produces many RAM faults, so focus on virtio addresses. If the first access is missing, verify recording began before VM startup. If the saved header's `entries-written` exceeds `entries-in-buffer`, older records may have been overwritten.

### 2. Record MMIO reads/writes after boot

Keep the VM running and prepare virtio reception in Terminal A's guest.

```text
/ # cat /dev/hvc0 &
/ # < Ctrl-A v >
[monitor] input → hvc0 (virtio)
```

In Terminal B, start a fresh recording using procedure 1's event settings.

```bash
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

Type `abc` and Enter in Terminal A. Stop/save in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-mmio-runtime.txt
head -80 ~/kvm-mmio-runtime.txt
```

Example excerpt of one read and a subsequent write:

```text
microkvm-30942 kvm_emulate_insn: 0:ffffffff8134be37:44 8b 60 60 (prot64)
microkvm-30942 vcpu_match_mmio:  gva 0xffffc90000005060 gpa 0xd0000060 Read GPA
microkvm-30942 kvm_mmio: mmio unsatisfied-read len 4 gpa 0xd0000060 val 0x0
microkvm-30942 kvm_mmio: mmio read len 4 gpa 0xd0000060 val 0x1
microkvm-30942 kvm_emulate_insn: 0:ffffffff8134be42:44 89 60 64 (prot64)
microkvm-30942 vcpu_match_mmio:  gva 0xffffc90000005064 gpa 0xd0000064 Write GPA
microkvm-30942 kvm_mmio: mmio write len 4 gpa 0xd0000064 val 0x1
```

Focus on three points:

- `kvm_emulate_insn`: guest instruction bytes. The read here is `mov 0x60(%rax), %r12d`, passing through KVM instruction emulation.
- `unsatisfied-read → read`: microkvm handles a read KVM could not satisfy internally and returns `0x1`. `0xd0000060` is InterruptStatus; the following write to `0xd0000064` is InterruptACK.
- `vcpu_match_mmio`: matching against MMIO information cached in the vCPU. GVA/GPA are displayed, but that does not mean this function performs address translation.

No `kvm_page_fault` appeared for the target page in this measurement. Next, record a different entry point to check whether VM exits continue.

### 3. Check the EPT misconfiguration path

Switch to function_graph in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_ftrace_pid"
echo | sudo tee "$trace_dir/set_ftrace_filter"
echo | sudo tee "$trace_dir/set_ftrace_notrace"
echo | sudo tee "$trace_dir/set_graph_function"
echo | sudo tee "$trace_dir/set_graph_notrace"
echo function_graph | sudo tee "$trace_dir/current_tracer"
echo 'handle_ept_misconfig*' | sudo tee "$trace_dir/set_graph_function"
echo 10 | sudo tee "$trace_dir/max_graph_depth"
sudo cat "$trace_dir/set_graph_function"
```

Confirm the target function name appears, then start recording.

```bash
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

With Terminal A still in virtio input mode, type a few more characters and Enter. Stop/save in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-mmio-functions.txt
head -100 ~/kvm-mmio-functions.txt
grep -c 'handle_ept_misconfig.*() {' ~/kvm-mmio-functions.txt
```

Observed function path (CPU/time columns and some calls omitted):

```text
handle_ept_misconfig.part.0 [kvm_intel]() {
  kvm_io_bus_write [kvm]() { ... }
  kvm_mmu_page_fault [kvm]() {
    handle_mmio_page_fault [kvm]() {
      get_sptes_lockless → kvm_tdp_mmu_get_walk
    }
    x86_emulate_instruction [kvm]() {
      x86_decode_insn { ... }
    }
  }
}
```

`handle_ept_misconfig` appeared 36 times. Counts vary with input and other activity, and tracing this function is not limited to virtio. The point is that **the EPT misconfiguration handling path continues to run after boot**.

## Checking the KVM implementation

### Why the page-fault event is absent

Linux v7.2's [`handle_ept_violation()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L5967) contains `trace_kvm_page_fault()`. [`handle_ept_misconfig()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L5983) calls MMU handling through a different path.

```c
/* Inside handle_ept_misconfig(); initial checks omitted */
gpa = vmcs_read64(GUEST_PHYSICAL_ADDRESS);
if (!is_guest_mode(vcpu) &&
    !kvm_io_bus_write(vcpu, KVM_FAST_MMIO_BUS, gpa, 0, NULL)) {
    trace_kvm_fast_mmio(gpa);
    return kvm_skip_emulated_instruction(vcpu);
}

return kvm_mmu_page_fault(vcpu, gpa, PFERR_RSVD_MASK, NULL, 0);
```

The initial `kvm_io_bus_write` tries `KVM_FAST_MMIO_BUS`. If it cannot handle the access, execution reaches `kvm_mmu_page_fault()`. **A function named page_fault does not necessarily record a `kvm_page_fault` event.**

### MMIO SPTEs help identify accesses after exit

An MMIO SPTE is a special page-table entry identifying a known MMIO page. In this EPT configuration, accessing it causes misconfiguration. After exit, KVM checks the vCPU's MMIO cache or the SPTE.

At the start of [`handle_mmio_page_fault()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/mmu/mmu.c#L4505), a cache match returns a result requesting emulation.

```c
if (mmio_info_in_cache(vcpu, addr, direct))
    return RET_PF_EMULATE;
```

Otherwise it checks the SPTE and returns the same `RET_PF_EMULATE` if it is a valid MMIO SPTE. **MMIO SPTEs do not eliminate VM exits; they shorten MMIO identification after exit.**

microkvm then receives the address, length, direction, and data in `run->mmio`. It does not decode instructions; it dispatches by address range to `virtio_mmio_read()` / `virtio_mmio_write()`. Unlike Step 35's PIO path, this MMIO path shows instruction decoding, but that alone does not establish overall performance differences.

## Stop tracing

Switch Terminal A's input back.

```text
< Ctrl-A v >
[monitor] input → ttyS0 (UART)
```

Clear the settings in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_graph_function"
echo 0 | sudo tee "$trace_dir/max_graph_depth"
```

## What we learned

- Hardware EPT exits and userspace `KVM_EXIT_MMIO` returns are separate stages.
- Exits may continue through EPT misconfiguration even without `kvm_page_fault` events.
- KVM identifies MMIO and emulates instructions; microkvm handles device reads/writes.

## Next step

[Step 39: MSR Exit](step39_msr-exit.md) separates the conditions causing `rdmsr` / `wrmsr` exits from the choice between KVM handling and userspace returns.
