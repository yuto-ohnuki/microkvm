# Step 35: Trace KVM Internals—How PIO Reaches Userspace

## Key point

**PIO to microkvm's UART is not handled by KVM's internal I/O bus and returns to userspace as `KVM_EXIT_IO`.** Verify this with tracepoints that record events and function_graph that records function-call relationships.

```text
Guest UART access → KVM PIO handler → in-kernel I/O bus
                                               ↓ Not handled
                           KVM_EXIT_IO → microkvm uart_in() / uart_out()
```

Step 34 measured time. Here we follow the functions involved and determine why control returns to microkvm.

## Preparation

Run only one microkvm process on Intel VMX. Use Terminal A for guest operations and Terminal B for host tracing. Stop Step 34's perf session first.

Start in Terminal A and wait for the guest `/ #` prompt.

```bash
cd ~/microkvm
./microkvm
```

In Terminal B, check tracefs and the required features. A regular user may not be able to enter the directory, so access each file with `sudo` instead of using `cd`. Keep using this Terminal B throughout.

```bash
trace_dir=/sys/kernel/tracing
sudo cat "$trace_dir/available_tracers"
sudo ls "$trace_dir/events/kvm/kvm_pio/enable" "$trace_dir/events/kvm/kvm_userspace_exit/enable"
sudo grep -w handle_io "$trace_dir/available_filter_functions"
```

Proceed if `function_graph`, both event `enable` files, and `handle_io` are available. Otherwise, check the host tracefs mount, loaded KVM modules, and kernel tracing configuration.

Remove PID filters below to avoid missing the vCPU thread. This also includes other VMs' events, so do not run other VMs during observation.

## Procedure and output

### 1. Observe PIO and userspace returns with tracepoints

In Terminal B, clear previous settings and the buffer, then record only these two events.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_pio/enable"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_userspace_exit/enable"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

After recording starts, run this once in Terminal A's guest.

```sh
/ # echo hello world
```

When the prompt returns, stop recording in Terminal B, save it to a file, and read it.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-pio-events.txt
head -60 ~/kvm-pio-events.txt
```

Example excerpt (`...` marks omissions):

```text
microkvm-25070 [006] 95526.463450: kvm_pio: pio_read  at 0x3fd size 1 val 0x60
microkvm-25070 [006] 95526.463470: kvm_userspace_exit: reason KVM_EXIT_IO (2)
microkvm-25070 [006] 95526.463495: kvm_pio: pio_read  at 0x3f8 ...
microkvm-25070 [006] 95526.463691: kvm_pio: pio_write at 0x3f8 size 1 val 0xd
microkvm-25070 [006] 95526.463769: kvm_pio: pio_write at 0x3f8 size 1 val 0xa
microkvm-25070 [006] 95526.463789: kvm_pio: pio_write at 0x3f9 size 1 val 0x5
```

Focus on ports and return reasons. `0x3fd` is UART status, `0x3f8` character data, and `0x3f9` interrupt enable. `KVM_EXIT_IO` indicates that PIO handling was returned to userspace.

Count events in the same saved file.

```bash
grep -c 'kvm_pio:' ~/kvm-pio-events.txt
grep -c 'kvm_userspace_exit:.*KVM_EXIT_IO' ~/kvm-pio-events.txt
```

Both counts were 398 in this measurement. They include non-character register accesses, input echo, and prompt output, so they do not equal the character count of `hello world`. Read-side `kvm_pio` is recorded on completion after receiving the value, so it need not describe the same access as the immediately following `kvm_userspace_exit`.

### 2. Look inside the PIO handler with function_graph

Next, record calls below `handle_io`. In Terminal B, disable the tracepoints and switch to function tracing.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_ftrace_pid"
echo | sudo tee "$trace_dir/set_ftrace_filter"
echo | sudo tee "$trace_dir/set_ftrace_notrace"
echo | sudo tee "$trace_dir/set_graph_notrace"
echo function_graph | sudo tee "$trace_dir/current_tracer"
echo handle_io | sudo tee "$trace_dir/set_graph_function"
echo 10 | sudo tee "$trace_dir/max_graph_depth"
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

Run this once in Terminal A's guest.

```sh
/ # echo hello
```

When the prompt returns, stop and save in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-pio-functions.txt
head -80 ~/kvm-pio-functions.txt
```

Observed read path (timestamp/duration columns and some calls omitted):

```text
handle_io [kvm_intel]() {
  kvm_fast_pio [kvm]() {
    emulator_pio_in_out [kvm]() {
      kvm_io_bus_read [kvm]() {
        kvm_io_bus_get_first_dev [kvm]() {
          kvm_io_bus_sort_cmp [kvm]();
          kvm_io_bus_sort_cmp [kvm]();
          kvm_io_bus_sort_cmp [kvm]();
        }
      }
    }
    kvm_get_linear_rip [kvm]() { ... }
  }
}
```

Follow `handle_io → kvm_fast_pio → emulator_pio_in_out → kvm_io_bus_read`. Writes show `kvm_io_bus_write` instead. If the beginning has no read path, search the saved file for `kvm_io_bus_read`.

`kvm_get_linear_rip()` obtains the current instruction's linear address. This path saves it for PIO completion after returning from userspace.

The tree shows an I/O-bus lookup, but not its return value. Check the source below for the condition that returns to userspace. function_graph covers kernel functions under `handle_io`; it does not include microkvm's terminal output time.

## Checking the KVM implementation

Linux v7.2's [`emulator_pio_in_out()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L8390) checks the I/O-bus result and prepares userspace information. The excerpt focuses on branching; comments mark omissions.

```c
/* Initial declarations and checks omitted */
for (i = 0; i < count; i++) {
    if (in)
        r = kvm_io_bus_read(vcpu, KVM_PIO_BUS, port, size, data);
    else
        r = kvm_io_bus_write(vcpu, KVM_PIO_BUS, port, size, data);

    if (r) {
        if (i == 0)
            goto userspace_io;
        /* Handling when processing fails partway through omitted */
    }
    /* Data update omitted */
}
return 1;
userspace_io:
/* PIO data preparation omitted */
vcpu->run->exit_reason = KVM_EXIT_IO;
/* Setting direction / size / count / port, etc., omitted */
return 0;
```

microkvm emulates this UART; the in-kernel I/O bus does not handle it. Execution therefore reaches `userspace_io` and sets `KVM_EXIT_IO`, matching the return reason observed at the tracepoint.

microkvm's `case KVM_EXIT_IO:` checks the UART port range and dispatches reads to `uart_in()` and writes to `uart_out()`. A write round trip looks like this (a processing diagram, not a raw trace of one event):

```text
Guest out instruction
  → VM exit (IO_INSTRUCTION)
  → handle_io → kvm_fast_pio → kvm_io_bus_write
  → KVM_RUN returns with KVM_EXIT_IO
  → microkvm uart_out() handles it
  → Next KVM_RUN → guest resumes
```

## Stop tracing

Clear tracing settings in Terminal B. Both saved files remain available for later inspection.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_graph_function"
echo | sudo tee "$trace_dir/set_event"
echo 0 | sudo tee "$trace_dir/max_graph_depth"
```

If the recording is empty, check that `tracing_on` was `1` during guest operations and that the events/functions checked during preparation are available. See the [ftrace documentation](https://www.kernel.org/doc/html/latest/trace/ftrace.html) for setting details.

## What we learned

- Tracepoints show PIO and userspace returns; function_graph shows the call path.
- UART accesses not handled by the I/O bus reach microkvm through `KVM_EXIT_IO`.
- UART also accesses non-character registers, so character count and PIO count differ.

## Next step

[Step 36: KVM Exit Pipeline](step36_exit-pipeline.md) follows the full sequence of VM entry, exit, and handler calls, including the stages before `handle_io`.
