# Step 33: Observe VM Exits—Compare Three Observation Points

> **Phase G: Understand KVM Through VM Exits**
>
> Part 2 observes the microkvm built in Part 1 with perf / ftrace and compares the results with Linux KVM source.

## Key point

**A hardware VM exit does not necessarily return `KVM_RUN` to microkvm.** Compare custom counters, KVM binary stats, and perf to see this distinction.

An Intel VT-x VM exit transfers control from guest execution to KVM. KVM then either handles it internally or returns handling to the userspace VMM.

```text
guest → hardware VM exit → KVM
                            ├─ Handle inside KVM → resume guest
                            │   Examples observed here: HLT, PREEMPTION_TIMER
                            │
                            └─ KVM_RUN returns → microkvm handles it
                                Examples: userspace MMIO / PIO
```

Only the lower path reaches Part 1's `switch (run->exit_reason)`. Hardware exit reasons and userspace `KVM_EXIT_*` values are different classifications.

## Observation tools

| Tool | What it counts | Use here |
|----------|----------------|--------------|
| Custom MMIO counter | Calls to microkvm's `KVM_EXIT_MMIO` handling | Check MMIO reaching userspace |
| KVM binary stats (Step 19) | Cumulative values at KVM processing points | Check PIO, HLT, and total exits in addition to MMIO |
| perf kvm stat | KVM exit/entry tracepoints | Check counts by reason and time until the next entry |

Custom counters increment after `KVM_RUN` returns. perf uses `kvm:kvm_exit` / `kvm:kvm_entry` inside KVM, so it can also observe exits that never return to userspace.

## Procedure and output

The following observes a Linux guest on Intel VMX. Run only one microkvm process.

```bash
# Terminal A: boot Linux with microkvm, then run ls / echo / cat in the guest
cd ~/microkvm && ./microkvm

# Terminal B: observe for 15 seconds (operate the guest during this interval)
sudo perf kvm stat record -p $(pgrep -x microkvm) -- sleep 15
sudo perf kvm stat report
```

### Observed results

These are sample measurements. Counts and times vary with environment and operations.

`perf kvm stat report` (15 seconds, guest ls/echo/cat):

```
             Event name     Samples   Sample%       Time%   Mean Time (ns)
         IO_INSTRUCTION        6747    63.00%       0.00%            11081
                    HLT        3750    35.00%      99.00%          3945206
     EXTERNAL_INTERRUPT          45     0.00%       0.00%            10419
       PREEMPTION_TIMER          32     0.00%       0.00%              678
               MSR_READ          15     0.00%       0.00%             6409
```

Report when microkvm exits:

```text
==== microkvm execution report ====
Mode: ioeventfd=OFF, irqfd=OFF

--- Userspace counters ---
MMIO exits total:           163
QueueNotify MMIO exits:
  RX queue 0:               128
  TX queue 1:               0
ioeventfd TX kicks:         0
IRQ inject (ioctl):         0
IRQ inject (irqfd):         0

--- TX processing latency ---
  (no TX data)

--- IRQ injection latency ---
  (no IRQ data)

--- KVM VM stats ---
  pages_4k                         4318 (current)

--- KVM vCPU 0 stats ---
  pf_taken                         +4321
  pf_fixed                         +4318
  pf_emulate                       +3
  pf_mmio_spte_created             +3
  tlb_flush                        +27
  exits                            +55962
  io_exits                         +23072
  mmio_exits                       +163
  halt_exits                       +26578
  irq_injections                   +1
==================================
```

perf covers 15 seconds, while custom counters and binary stats cover startup through shutdown. **Different measurement periods and collection points mean perf Samples and stats `exits` should not be compared directly.**

## Reading the output

### 1. perf also sees exits microkvm does not handle

There are 32 `PREEMPTION_TIMER` events. KVM handles these internally; they do not cause `KVM_RUN` to return. Hardware VM exits occur even when no microkvm handler receives them.

### 2. Counters for the same MMIO events agree

The custom counter `163` and binary stats `mmio_exits +163` agree in this run. Both count the corresponding MMIO returns to userspace.

By contrast, binary stats `exits +55962` includes more than MMIO. Its different magnitude reflects a different counting scope.

### 3. HLT Time% is not processing cost

`IO_INSTRUCTION` accounts for 63% of events, while `HLT` accounts for 99% of time. In microkvm, PIO such as UART accesses appears as `IO_INSTRUCTION`.

perf measures elapsed time from exit to the next entry, including waiting and scheduling. HLT here includes vCPU idle waiting, so **a large Time% does not directly mean expensive processing.**

## Checking the KVM implementation

Read Linux v7.2's [`vmx_vcpu_run()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L7505). This excerpt preserves the ordering of VM entry/exit, tracing, and subsequent decisions; comments mark omissions. It is a reference version for checking processing order, not necessarily the kernel used for measurement.

```c
fastpath_t vmx_vcpu_run(struct kvm_vcpu *vcpu, u64 run_flags)
{
    bool force_immediate_exit = run_flags & KVM_RUN_FORCE_IMMEDIATE_EXIT;
    struct vcpu_vmx *vmx = to_vmx(vcpu);

    /* Initialization and guest-state preparation omitted */
    vmx_vcpu_enter_exit(vcpu, __vmx_vcpu_enter_flags(vmx));

    /* Host-state restoration and other details omitted */
    if (unlikely(vmx->fail))
        return EXIT_FASTPATH_NONE;

    trace_kvm_exit(vcpu, KVM_ISA_VMX);
    if (unlikely(vmx_get_exit_reason(vcpu).failed_vmentry))
        return EXIT_FASTPATH_NONE;

    /* Interrupt-state handling and other details omitted */
    return vmx_exit_handlers_fastpath(vcpu, force_immediate_exit);
}
```

Read these three points in order:

1. `vmx_vcpu_enter_exit()` runs the guest, then control returns to KVM.
2. `trace_kvm_exit()` records the exit.
3. `vmx_exit_handlers_fastpath()` then selects the handling path (details in Step 36).

**The trace occurs before the decision to return to userspace, so it can record exits handled entirely within KVM.** It remains an observation point on KVM's processing path, not a direct record of all hardware activity. For example, the early return on `vmx->fail` in the excerpt bypasses this tracepoint.

## What we learned

- Hardware VM exit and `KVM_RUN` returning to userspace are separate boundaries.
- perf observes inside KVM; custom counters observe the userspace side.
- Exit count and time from exit to next entry are different metrics.

## Next step

[Step 34: Analyze VM Exits](step34_analyze-vm-exits.md) examines exit-to-entry timing, separating HLT waiting time and comparing workloads.
