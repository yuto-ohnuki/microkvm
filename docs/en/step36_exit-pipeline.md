# Step 36: KVM Exit Pipeline—Guest Execution and Exit Handling

## Key point

**After a VM exit, KVM checks whether a fastpath can handle it, then either reenters the guest or proceeds through normal exit handling.** Locate Step 35's `handle_io` within this overall flow.

```text
vcpu_enter_guest()
  → Prepare entry (registers, interrupt state, etc.)
  → Run guest → VM exit → fastpath decision
       ↑                    ├─ Can reenter → repeat same loop
       └────────────────────┘
                            └─ Otherwise → restore host state
                                           → handle_exit()
                                             Check result / normal dispatch if needed
```

A single `KVM_RUN` can call `vcpu_enter_guest()` repeatedly. A single `vcpu_enter_guest()` can also reenter the guest. **Fastpath versus normal handling is separate from whether execution returns to userspace.**

## Preparation

As in Step 35, boot a microkvm Linux guest in Terminal A and operate host tracefs in Terminal B. Stop other VMs and perf.

In Terminal B, check whether the target function can be traced.

```bash
trace_dir=/sys/kernel/tracing
sudo grep 'vcpu_enter_guest' "$trace_dir/available_filter_functions"
```

Confirm that `vcpu_enter_guest`, `vcpu_enter_guest.constprop.0`, or a similar name appears. The `vcpu_enter_guest*` pattern below also covers such suffixes.

## Procedure and output

### 1. Configure the recording target

In Terminal B, clear Step 35's settings and select functions below `vcpu_enter_guest`.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_ftrace_pid"
echo | sudo tee "$trace_dir/set_ftrace_filter"
echo | sudo tee "$trace_dir/set_ftrace_notrace"
echo | sudo tee "$trace_dir/set_graph_function"
echo | sudo tee "$trace_dir/set_graph_notrace"
echo function_graph | sudo tee "$trace_dir/current_tracer"
echo 'vcpu_enter_guest*' | sudo tee "$trace_dir/set_graph_function"
echo 6 | sudo tee "$trace_dir/max_graph_depth"
sudo cat "$trace_dir/set_graph_function"
```

Confirm the target function appears in the final output. If it says `all functions enabled`, filtering failed; do not start recording. Check the function name and configuration errors first.

### 2. Operate the guest and save the recording

In Terminal B, clear the buffer and start recording.

```bash
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

Run this once in Terminal A's guest.

```sh
/ # echo hi
```

As soon as the prompt returns, stop and save in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-exit-pipeline.txt
head -120 ~/kvm-exit-pipeline.txt
```

### 3. Inspect function order

The following extracts the main calls from the observed sequence. Time columns and intermediate calls are omitted, with `...` indicating omissions.

```text
vcpu_enter_guest() {
  ...
  vt_prepare_switch_to_guest() {
    vmx_prepare_switch_to_guest() {
      ...
    }
  }
  vt_sync_pir_to_irr() {
    ...
  }
  ...
  kvm_load_guest_pkru();
  vt_vcpu_run() {
    ...
    vmx_exit_handlers_fastpath() {
      handle_fastpath_hlt() {
        kvm_emulate_halt() {
          ...
        }
      }
    }
  }
  kvm_load_host_pkru();
  ...
  vt_handle_exit() {
    vmx_handle_exit() {
      __vmx_handle_exit() {
        ...
      }
    }
  }
}
```

Actual output also includes CPU numbers and `us` durations. Function names and visible depth vary with kernel version and build.

First inspect calls around `vt_vcpu_run` to identify entry preparation and host-state restoration. Then find `vmx_exit_handlers_fastpath` and `vt_handle_exit` to follow exit handling. If the first 120 lines do not show the full call, continue reading the saved file.

```bash
less ~/kvm-exit-pipeline.txt
```

Even while measuring `echo hi`, HLT from input waiting appears. The example above captured HLT; not every invocation has the same handler. If required internal functions are hidden by the depth limit, set `max_graph_depth` to `10` and repeat from procedure 2.

## Reading the output

### 1. Work also occurs before and after the handler

`vt_prepare_switch_to_guest` and `kvm_load_guest_pkru` prepare guest execution; `kvm_load_host_pkru` restores host state. `vt_sync_pir_to_irr` synchronizes interrupt state, examined further in Step 37.

One measured `vcpu_enter_guest()` call took about 41.8 us. This includes guest execution, so do not compare it with Step 34's exit → next-entry duration.

### 2. handle_exit can appear after fastpath handling

This example handles HLT through `handle_fastpath_hlt → kvm_emulate_halt`. A later `vt_handle_exit` does not mean HLT was handled again by a normal handler. `handle_exit` receives the fastpath result and decides whether normal dispatch is needed.

Step 35's `handle_io` lies after normal dispatch. Handling HLT through fastpath alone does not establish immediate guest reentry. Check the conditions in the source.

## Checking the KVM implementation

Linux v7.2's reentry loop in [`vcpu_enter_guest()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L11112) checks the return value as follows (comments mark omissions).

```c
for (;;) {
    /* Pre-guest execution handling omitted */
    exit_fastpath = kvm_x86_call(vcpu_run)(vcpu, run_flags);
    if (likely(exit_fastpath != EXIT_FASTPATH_REENTER_GUEST))
        break;

    if (kvm_lapic_enabled(vcpu))
        kvm_x86_call(sync_pir_to_irr)(vcpu);

    if (unlikely(kvm_vcpu_exit_request(vcpu))) {
        exit_fastpath = EXIT_FASTPATH_EXIT_HANDLED;
        break;
    }
    /* run_flags and stats updates omitted */
}
/* Host-state restoration and other details omitted */
r = kvm_x86_call(handle_exit)(vcpu, exit_fastpath);
return r;
```

`EXIT_FASTPATH_REENTER_GUEST` attempts reentry; other results leave the loop. On VMX, vcpu_run reaches `vmx_vcpu_run()`, where [vmx_exit_handlers_fastpath()](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L7428) makes the decision after Step 33's `kvm_exit` trace.

The observed [`handle_fastpath_hlt()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L11780) returns as follows.

```c
fastpath_t handle_fastpath_hlt(struct kvm_vcpu *vcpu)
{
    if (!kvm_pmu_is_fastpath_emulation_allowed(vcpu))
        return EXIT_FASTPATH_NONE;

    if (!kvm_emulate_halt(vcpu))
        return EXIT_FASTPATH_EXIT_USERSPACE;

    if (kvm_vcpu_running(vcpu))
        return EXIT_FASTPATH_REENTER_GUEST;

    return EXIT_FASTPATH_EXIT_HANDLED;
}
```

It returns `REENTER_GUEST` when the vCPU can run, or `EXIT_HANDLED` when execution cannot continue, for example in halted state. **Fastpath means neither "no VM exit" nor "always immediate reentry."**

## Stop tracing

Clear the settings in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_graph_function"
echo | sudo tee "$trace_dir/set_event"
echo 0 | sudo tee "$trace_dir/max_graph_depth"
```

## What we learned

- Inside `KVM_RUN`, entry preparation, guest execution, and exit handling repeat.
- Fastpath return values determine reentry or transition to the normal path.
- Distinguish function-processing paths from the decision to return to userspace.

## Next step

[Step 37: IRQ Delivery](step37_irq-exit.md) follows interrupts from microkvm's `KVM_IRQ_LINE` / irqfd to the guest, clarifying the role of `vt_sync_pir_to_irr()` observed here.
