# Step 37: IRQ Delivery—From GSI to Guest Vector

## Key point

**The GSI number microkvm supplies differs from the vector the guest CPU receives.** Follow UART IRQ4 through KVM's interrupt-delivery path, then see how irqfd changes its entry point.

```text
microkvm: signal GSI 4
  → KVM routing table → IOAPIC pin 4
  → Deliver to LAPIC using guest-configured vector and destination
```

GSI is a logical interrupt-line number; a vector is the number the CPU uses for interrupt handling. microkvm creates interrupt controllers through `KVM_CREATE_IRQCHIP` and supplies GSI 4 plus a level to `KVM_IRQ_LINE` for UART injection. The guest's IOAPIC configuration determines the vector, not the UART injection code.

## Preparation

Use Terminal A for microkvm/guest operations and Terminal B for host tracing. Stop other VMs and perf.

Start normally in Terminal A and wait for the guest `/ #` prompt.

```bash
cd ~/microkvm
unset USE_IOEVENTFD USE_IRQFD
./microkvm
```

In Terminal B, check the target events and function. Keep using the same Terminal B.

```bash
trace_dir=/sys/kernel/tracing
for e in kvm_set_irq kvm_ioapic_set_irq kvm_apic_accept_irq; do
    sudo ls "$trace_dir/events/kvm/$e/enable"
done
sudo grep -w kvm_set_irq "$trace_dir/available_filter_functions"
```

Confirm the three `enable` files and `kvm_set_irq` appear. As in Step 35, configure tracing by accessing each file through `sudo`.

## Procedure and output

### 1. Observe UART GSI and vector

In Terminal B, configure tracepoints and start recording. Remove PID filters to capture kworker processing later. Vector numbers vary by environment, so do not exclude particular numbers.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
for e in kvm_set_irq kvm_ioapic_set_irq kvm_apic_accept_irq; do
    echo 0 | sudo tee "$trace_dir/events/kvm/$e/filter"
    echo 1 | sudo tee "$trace_dir/events/kvm/$e/enable"
done
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

Run `ls` in Terminal A's guest. Input, echo, and output cause UART interrupts.

```sh
/ # ls
```

When the prompt returns, stop/save in Terminal B and inspect events around GSI 4.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-irq-uart.txt
grep -A 4 'kvm_set_irq: gsi 4 level 1' ~/kvm-irq-uart.txt | head -30
```

Example excerpt:

```text
microkvm [012] 98222.234315: kvm_set_irq: gsi 4 level 1 source 0
microkvm [012] 98222.234319: kvm_apic_accept_irq: apicid 0 vec 33 (Fixed|edge)
microkvm [012] 98222.234321: kvm_ioapic_set_irq: pin 4 dst 0 vec 33 (Fixed|physical|edge)
microkvm [012] 98222.234322: kvm_set_irq: gsi 4 level 0 source 0
microkvm [012] 98222.234322: kvm_ioapic_set_irq: pin 4 dst 0 vec 33 (Fixed|physical|edge)
```

This example maps **GSI 4 → pin 4 → vector 33 (0x21)**. `level 1 → 0` corresponds to UART injection raising then lowering the line. The edge/level trigger mode itself comes from the guest's IOAPIC configuration.

The reason `kvm_apic_accept_irq` appears before the IOAPIC event is explained below. Other interrupts, such as timers, may interleave, so the same five lines need not always appear together. If the excerpt is insufficient, read the saved file with `less`.

### 2. Observe call relationships with function_graph

In Terminal B, stop tracepoints and record functions below `kvm_set_irq`.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_ftrace_pid"
echo | sudo tee "$trace_dir/set_ftrace_filter"
echo | sudo tee "$trace_dir/set_ftrace_notrace"
echo | sudo tee "$trace_dir/set_graph_function"
echo | sudo tee "$trace_dir/set_graph_notrace"
echo function_graph | sudo tee "$trace_dir/current_tracer"
echo kvm_set_irq | sudo tee "$trace_dir/set_graph_function"
echo 8 | sudo tee "$trace_dir/max_graph_depth"
sudo cat "$trace_dir/set_graph_function"
```

Confirm the final output is `kvm_set_irq` before starting recording.

```bash
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

Run `ls` again in Terminal A's guest.

```sh
/ # ls
```

Stop and save in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-irq-functions.txt
head -80 ~/kvm-irq-functions.txt
```

Main portion of the observed call tree (CPU/time columns and some calls omitted):

```text
kvm_set_irq {
  kvm_irq_map_gsi
  kvm_ioapic_set_irq {
    ioapic_set_irq {
      ...
      ioapic_service {
        __kvm_irq_delivery_to_apic {
          __kvm_irq_delivery_to_apic_fast {
            __apic_accept_irq {
              vt_deliver_interrupt [kvm_intel]
            }
          }
        }
      }
    }
  }
  kvm_pic_set_irq { ... }
}
```

Confirm the parent-child relationship from **IOAPIC handling to LAPIC `__apic_accept_irq`**. This measurement also had a PIC routing entry, so `kvm_pic_set_irq` followed.

In this sample, `vt_deliver_interrupt` took 26.118 us out of 38.830 us total. This alone does not reveal the breakdown of posted-interrupt or physical IPI work. If the depth limit hides functions, set `max_graph_depth` to `12` and remeasure.

### 3. Observe which task injects through irqfd

Stop the VM in Terminal A and restart with irqfd enabled.

```bash
USE_IRQFD=1 ./microkvm
```

Prepare virtio reception in the guest and switch input destinations.

```text
/ # cat /dev/hvc0 &
/ # < Ctrl-A v >
[monitor] input → hvc0 (virtio)
```

In Terminal B, configure the same tracepoints as procedure 1 and start recording.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
for e in kvm_set_irq kvm_ioapic_set_irq kvm_apic_accept_irq; do
    echo 0 | sudo tee "$trace_dir/events/kvm/$e/filter"
    echo 1 | sudo tee "$trace_dir/events/kvm/$e/enable"
done
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

Type a few characters and Enter in Terminal A, then switch back to UART input.

```text
abc

< Ctrl-A v >
[monitor] input → ttyS0 (UART)
```

Stop/save in Terminal B, then check GSI 4 / 5 and task names on the left.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-irq-irqfd.txt
grep -E 'kvm_set_irq: gsi (4|5) ' ~/kvm-irq-irqfd.txt | head -30
```

Example excerpt:

```text
kworker/6:1-137  99143.183804: kvm_set_irq: gsi 5 level 1 source 0
kworker/6:1-137  99143.183808: kvm_set_irq: gsi 5 level 0 source 0
microkvm-30492   99143.183994: kvm_set_irq: gsi 4 level 1 source 0
microkvm-30492   99143.183996: kvm_set_irq: gsi 4 level 0 source 0
```

UART GSI 4 was recorded in the microkvm task issuing the ioctl; virtio GSI 5 appeared in kworker context. Since irqfd is registered for GSI 5, staying in UART input mode cannot make this comparison. The source below shows whether irqfd always uses kworker.

## Checking the KVM implementation

### IOAPIC uses the guest-configured vector

Linux v7.2's [`ioapic_service()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/ioapic.c#L457) obtains delivery information from the redirection table. Excerpt:

```c
union kvm_ioapic_redirect_entry *entry = &ioapic->redirtbl[irq];
/* Declarations, mask checks, and other details omitted */
irqe.dest_id = entry->fields.dest_id;
irqe.vector = entry->fields.vector;
/* Other delivery settings and branches omitted */
```

After delivery to LAPIC using this information, the caller [`ioapic_set_irq()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/ioapic.c#L240) records its trace.

```c
ret = ioapic_service(ioapic, irq, line_status);

out:
trace_kvm_ioapic_set_irq(entry.bits, irq, ret == 0);
```

This explains why the LAPIC event preceded the IOAPIC event in procedure 1. **Trace-recording order need not match function-call order.**

### irqfd defers to a workqueue when direct injection is unavailable

Linux v7.2's [`irqfd_wakeup()`](https://github.com/torvalds/linux/blob/v7.2/virt/kvm/eventfd.c#L203) takes this branch after eventfd notification (function excerpt).

```c
if (unlikely(!irqfd_is_active(irqfd)) ||
    kvm_arch_set_irq_inatomic(&irq, kvm,
                              KVM_USERSPACE_IRQ_SOURCE_ID, 1,
                              false) == -EWOULDBLOCK)
    schedule_work(&irqfd->inject);
```

When injection cannot be handled immediately, work is queued and later calls `kvm_set_irq`. The observed kworker records are consistent with this path. kworker execution is therefore not an inherent requirement of irqfd.

irqfd removes per-virtio-IRQ `KVM_IRQ_LINE` calls, but microkvm's `write(eventfd)` remains a syscall. Different task names alone do not establish a speedup. Execution-report IRQ counters cover virtio RX IRQ5, not UART IRQ4 ioctls.

Step 36's `vt_sync_pir_to_irr()` is vCPU-side processing that synchronizes pending posted interrupts into LAPIC IRR. It is on the receiving side, not the entrance to the injection path observed here.

## Stop tracing

Clear the settings in Terminal B.

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_graph_function"
echo 0 | sudo tee "$trace_dir/max_graph_depth"
```

## What we learned

- The GSI microkvm signals and the vector the guest receives are different.
- KVM delivers interrupts according to routing and guest IOAPIC configuration.
- irqfd connects eventfd to injection; the observed workqueue path joins common GSI routing.

## Next step

[Step 38: MMIO Exit](step38_mmio-exit.md) follows guest memory accesses through KVM MMIO handling to a `KVM_EXIT_MMIO` return.
