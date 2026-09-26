# Step 34: Analyze VM Exits—Interpret Measured Time

## Key point

**Time from exit to next entry includes userspace round trips and waiting, as well as KVM processing.** Add Min / Avg / Max to Step 33's counts and Time% to compare workloads.

```text
kvm_exit (T1) → KVM processing, userspace round trips, waiting → next kvm_entry (T2)
              └─────── exit-to-reentry latency = T2 - T1 ──────┘
```

This is not the cost of the hardware VM-exit transition alone. The question is whether a long measured duration necessarily means expensive processing.

## Observation tools

- `perf kvm stat live`: view counts and times as workloads change.
- `perf record` + `perf script`: save exit/entry event sequences and aggregate timings from the same recording.

Use one microkvm process with one vCPU. The examples below are recorded measurements; timings and counts vary by environment and operations.

## Procedure and output

### 1. Compare workloads with live

Start the VM in Terminal A. Measure patterns (1)–(3) separately, restarting the VM for each.

```bash
cd ~/microkvm
./microkvm
```

In Terminal B, observe each pattern with this command.

```bash
sudo perf kvm stat live -p $(pgrep -x microkvm)
```

Operations in Terminal A (guest):

**(1) Idle**: when `/ #` appears, wait without typing.

**(2) Workload without output**: run the following, discarding output to `/dev/null`.

```sh
/ # while true; do ls /proc >/dev/null; cat /proc/cpuinfo >/dev/null; done
```

**(3) Workload with output**: run the following to continuously output through UART.

```sh
/ # while true; do cat /proc/cpuinfo; done
```

Example output for (1), idle:

```text
                VM-EXIT  Samples  Samples%  Time%   Min Time   Max Time    Avg time
                    HLT      113   100.00%  100.00% 3851.97us  4092.26us  3977.68us ( +- 0.07% )
```

Example output for (2), workload without output:

```text
                VM-EXIT  Samples  Samples%  Time%   Min Time   Max Time    Avg time
     EXTERNAL_INTERRUPT      106    80.30%   96.95%    9.90us    14.21us    10.88us ( +- 0.87% )
       PREEMPTION_TIMER       25    18.94%    1.88%    0.70us     1.14us     0.89us ( +- 2.15% )
     EXTERNAL_INTERRUPT        1     0.76%    1.17%   13.87us    13.87us    13.87us ( +- 0.00% )
```

Example output for (3), workload with output:

```text
                VM-EXIT  Samples  Samples%  Time%   Min Time   Max Time    Avg time
         IO_INSTRUCTION     1459    98.98%   99.97%   10.02us  6265.74us   258.98us ( +- 11.94% )
     EXTERNAL_INTERRUPT        9     0.61%    0.02%    9.19us    10.34us     9.61us ( +- 1.31% )
       PREEMPTION_TIMER        5     0.34%    0.00%    0.53us     0.62us     0.55us ( +- 3.31% )
         IO_INSTRUCTION        1     0.07%    0.00%   10.33us    10.33us    10.33us ( +- 0.00% )
```

HLT dominates while waiting for input; IO_INSTRUCTION dominates continuous UART output. With output, IO_INSTRUCTION ranges from Min 10.02 us to Max 6265.74 us, so Avg 258.98 us alone cannot represent individual durations.

### 2. Save and aggregate an event sequence with record

Next, use `dd`. Transferred data is not sent to UART, but command input and the completion message still use UART.

```bash
# Terminal B: record for 10 seconds
sudo perf record -e 'kvm:kvm_exit,kvm:kvm_entry' -p $(pgrep -x microkvm) -o ~/kvm-dd.data -- sleep 10

# Immediately after recording starts, run in Terminal A (guest)
/ # dd if=/dev/zero of=/dev/null bs=1M count=500
```

Pair each exit with the next entry for the single vCPU. Exclude exits without a matching entry, such as at the end of the recording.

```bash
$ sudo perf script -i ~/kvm-dd.data | awk '
  /kvm_exit/ { for(i=1;i<=NF;i++) if($i=="reason"){reason=$(i+1);break}
    for(i=1;i<=NF;i++) if($i ~ /^[0-9]+\.[0-9]+:$/){ts=$i;sub(/:$/,"",ts);exit_ts=ts+0}
    have_exit=1; next }
  /kvm_entry/ && have_exit {
    for(i=1;i<=NF;i++) if($i ~ /^[0-9]+\.[0-9]+:$/){ts=$i;sub(/:$/,"",ts);entry_ts=ts+0}
    dt=(entry_ts-exit_ts)*1e6; n[reason]++; sum[reason]+=dt
    if(mn[reason]==""||dt<mn[reason])mn[reason]=dt; if(dt>mx[reason])mx[reason]=dt; have_exit=0 }
  END {
    printf "%-20s %8s %10s %10s %10s\n","REASON","count","min_us","avg_us","max_us"
    for(r in n) printf "%-20s %8d %10.2f %10.2f %10.2f\n",r,n[r],mn[r],sum[r]/n[r],mx[r] | "sort -k2 -rn"
  }'
```

Aggregated results (us):

```text
REASON                  count     min_us     avg_us     max_us
HLT                      2496    1050.00    3974.76    4208.00
IO_INSTRUCTION            505       9.00      10.81      27.00
EPT_VIOLATION              56       5.00       6.36      45.00
EXTERNAL_INTERRUPT         17       8.00      10.12      17.00
MSR_READ                   10       6.00       7.30       8.00
PREEMPTION_TIMER            5       1.00       1.20       2.00
```

## Reading the output

### 1. HLT's roughly 4 ms includes waiting

HLT Avg 3974.76 us includes waiting until the vCPU resumes. It does not mean KVM's HLT processing alone took about 4 ms; interpret it separately from other reasons.

### 2. I/O timing differences alone do not establish a cause

The recorded IO_INSTRUCTION values were Min 9 / Avg 10.81 / Max 27 us. These are shorter than the live workload with output, but **both measurement method and workload changed, so the difference cannot be attributed conclusively to terminal I/O or measurement method.** Identifying the cause requires comparable conditions.

Min / Avg / Max show the range, but not the distribution's shape—for example, whether most samples were near the minimum.

## Checking the KVM implementation

As in Step 33, refer to Linux v7.2's [`vmx_vcpu_run()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L7505). Focus on where the two tracepoints sit relative to guest execution.

```c
fastpath_t vmx_vcpu_run(struct kvm_vcpu *vcpu, u64 run_flags)
{
    bool force_immediate_exit = run_flags & KVM_RUN_FORCE_IMMEDIATE_EXIT;
    struct vcpu_vmx *vmx = to_vmx(vcpu);

    /* Pre-entry checks and other details omitted */
    trace_kvm_entry(vcpu, force_immediate_exit);

    /* Guest-state and timer preparation omitted */
    vmx_vcpu_enter_exit(vcpu, __vmx_vcpu_enter_flags(vmx));

    /* Host-state restoration and failure branches omitted */
    trace_kvm_exit(vcpu, KVM_ISA_VMX);

    /* Post-exit processing omitted */
    return vmx_exit_handlers_fastpath(vcpu, force_immediate_exit);
}
```

`trace_kvm_entry()` precedes guest execution; `trace_kvm_exit()` follows control returning to KVM. We measure **the exit in one call to the entry in the next call**, not entry → exit within the same call.

This placement shows that the interval includes post-exit processing and waiting; it does not measure hardware transition timestamps themselves.

## What we learned

- Exit-to-reentry latency is elapsed time including processing and waiting.
- Read the range together with the workload, rather than judging by averages alone.
- To isolate latency causes, keep measurement method and workload conditions consistent.

## Next step

[Step 35: Trace KVM Internals](step35_trace-kvm-internals.md) follows PIO exit handling with ftrace to see which functions execute within that time.
