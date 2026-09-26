# Benchmark: ioeventfd / irqfd Latency Measurements

## Overview

microkvm has a built-in benchmark for measuring the performance effects of ioeventfd (Step 17) and irqfd (Step 18). Toggle these features through environment variables and compare four configurations to observe how notification methods affect processing time and userspace MMIO exit counts.

## Prerequisites

Benchmark instrumentation is a separate commit after Step 18, available through the `benchmark` tag. The Step 18 tag does not contain benchmark code.

```bash
$ git checkout benchmark
$ make
```

## Usage

```bash
# Pattern 1: baseline (Step 15–16 style—MMIO exit + ioctl IRQ)
./microkvm

# Pattern 2: ioeventfd only (Step 17—eventfd for TX kicks, ioctl for IRQ)
USE_IOEVENTFD=1 ./microkvm

# Pattern 3: irqfd only (Step 18 IRQ path—MMIO exits for TX kicks)
USE_IRQFD=1 ./microkvm

# Pattern 4: both enabled (Step 17+18 notification methods)
USE_IOEVENTFD=1 USE_IRQFD=1 ./microkvm
```

Environment variables are checked for presence, not value. Even `USE_IRQFD=0` enables the feature, so run `unset USE_IOEVENTFD USE_IRQFD` on the host before comparing.

## Workload

TX sends 2,000 lines through `bench.sh`; RX sends `abcdefghij` plus Enter, totaling 11 characters.

### Prepare bench.sh

Extract the existing initramfs into a new working directory and add `/bin/bench.sh`. Use `sudo` for extraction because restoring `/dev/console` requires privileges.

```bash
$ initramfs_work=$(mktemp -d /tmp/microkvm-bench.XXXXXX)
$ cd "$initramfs_work"
$ zcat ~/microkvm/initramfs.gz | sudo cpio -idmv

$ sudo tee bin/bench.sh > /dev/null << 'EOF'
#!/bin/sh
N=2000
echo "[bench] TX start: $N lines -> /dev/hvc0"
i=1
while [ $i -le $N ]; do
    echo "microkvm benchmark line $i"
    i=$((i + 1))
done > /dev/hvc0
echo "[bench] TX done: $N lines"
EOF
$ sudo chmod +x bin/bench.sh

$ sudo sh -c 'find . | cpio -o -H newc' | gzip > ~/microkvm/initramfs.gz
$ cd ~/microkvm
```

Adjust `~/microkvm` to the project path on your host.

### Common procedure for all patterns

Start with each configuration under Usage and follow the same procedure:

```text
1. In guest: cat /dev/hvc0 &
2. In guest: bench.sh
3. Wait for [bench] TX done: 2000 lines
4. < Ctrl-A v > → confirm [monitor] input → hvc0 (virtio)
5. Type abcdefghij and Enter, then wait for received text to appear
6. < Ctrl-A v > → return to UART
7. Ctrl-C → execution report printed to stderr
```

## Measurements

These are measured results for the four configurations using the workload above. Every pattern had TX Count 4,011 and IRQ Count 11.

### Pattern 1: Baseline (ioeventfd=OFF, irqfd=OFF)

```
==== microkvm execution report ====
Mode: ioeventfd=OFF, irqfd=OFF

--- Userspace counters ---
MMIO exits total:           4207
QueueNotify MMIO exits:
  RX queue 0:               139
  TX queue 1:               4011
ioeventfd TX kicks:         0
IRQ inject (ioctl):         11
IRQ inject (irqfd):         0

--- TX processing latency ---
  Method:   MMIO exit handler
  Count:    4011
  Avg:      1809 ns (1.81 us)
  Min:      798 ns (0.80 us)
  Max:      11582 ns (11.58 us)

--- IRQ injection latency ---
  Method:   ioctl (KVM_IRQ_LINE x2)
  Count:    11
  Avg:      10368 ns (10.37 us)
  Min:      9352 ns (9.35 us)
  Max:      12019 ns (12.02 us)
==================================
```

### Pattern 2: ioeventfd only (USE_IOEVENTFD=1)

```
==== microkvm execution report ====
Mode: ioeventfd=ON, irqfd=OFF

--- Userspace counters ---
MMIO exits total:           196
QueueNotify MMIO exits:
  RX queue 0:               139
  TX queue 1:               0
ioeventfd TX kicks:         4011
IRQ inject (ioctl):         11
IRQ inject (irqfd):         0

--- TX processing latency ---
  Method:   ioeventfd thread
  Count:    4011
  Avg:      3452 ns (3.45 us)
  Min:      669 ns (0.67 us)
  Max:      28525 ns (28.52 us)

--- IRQ injection latency ---
  Method:   ioctl (KVM_IRQ_LINE x2)
  Count:    11
  Avg:      10533 ns (10.53 us)
  Min:      6834 ns (6.83 us)
  Max:      27552 ns (27.55 us)
==================================
```

### Pattern 3: irqfd only (USE_IRQFD=1)

```
==== microkvm execution report ====
Mode: ioeventfd=OFF, irqfd=ON

--- Userspace counters ---
MMIO exits total:           4207
QueueNotify MMIO exits:
  RX queue 0:               139
  TX queue 1:               4011
ioeventfd TX kicks:         0
IRQ inject (ioctl):         0
IRQ inject (irqfd):         11

--- TX processing latency ---
  Method:   MMIO exit handler
  Count:    4011
  Avg:      2190 ns (2.19 us)
  Min:      1423 ns (1.42 us)
  Max:      15926 ns (15.93 us)

--- IRQ injection latency ---
  Method:   irqfd (write)
  Count:    11
  Avg:      5813 ns (5.81 us)
  Min:      3945 ns (3.94 us)
  Max:      7078 ns (7.08 us)
==================================
```

### Pattern 4: Both enabled (USE_IOEVENTFD=1 USE_IRQFD=1)

```
==== microkvm execution report ====
Mode: ioeventfd=ON, irqfd=ON

--- Userspace counters ---
MMIO exits total:           196
QueueNotify MMIO exits:
  RX queue 0:               139
  TX queue 1:               0
ioeventfd TX kicks:         4011
IRQ inject (ioctl):         0
IRQ inject (irqfd):         11

--- TX processing latency ---
  Method:   ioeventfd thread
  Count:    4011
  Avg:      3467 ns (3.47 us)
  Min:      643 ns (0.64 us)
  Max:      28587 ns (28.59 us)

--- IRQ injection latency ---
  Method:   irqfd (write)
  Count:    11
  Avg:      6326 ns (6.33 us)
  Min:      4977 ns (4.98 us)
  Max:      7297 ns (7.30 us)
==================================
```

## Summary

| Metric | Baseline | ioeventfd | irqfd | Both |
|--------|----------|-----------|-------|------|
| MMIO exits total | 4207 | **196** | 4207 | **196** |
| TX QueueNotify exits | 4011 | **0** | 4011 | **0** |
| TX latency (average) | 1.81 μs | 3.45 μs | 2.19 μs | 3.47 μs |
| IRQ latency (average) | 10.37 μs | 10.53 μs | **5.81 μs** | **6.33 μs** |

## Interpreting the results

> **Note:** TX has 4,011 samples, but each IRQ result comes from one run with only 11 samples; these do not establish a general performance improvement rate. Values depend on environment and timing. MMIO exit counters count `KVM_EXIT_MMIO` returns to userspace, not total hardware VM exits.

### Effect of ioeventfd (pattern 1 → 2)

- TX QueueNotify MMIO exits drop from 4,011 to **0**
- Total MMIO exits drop from 4,207 to 196; the difference of 4,011 matches the removed TX QueueNotify exits
- TX moves from the vCPU thread to a separate thread, so the vCPU thread need not finish TX before resuming the guest
- TX processing latency measures `virtio_console_tx()` execution time, not vCPU pause time

### Effect of irqfd (pattern 1 → 3)

- Measured IRQ injection latency drops from 10.37 μs to **5.81 μs**. This measures host-side notification operations, not time until the guest receives the interrupt
- Syscalls per IRQ: 2 (two ioctls) → 1 (one write)
- MMIO exits are unchanged (irqfd optimizes the RX path, not TX)

### Both enabled (pattern 4)

- Userspace MMIO exits for TX QueueNotify reach zero; IRQ notification duration also decreases in this measurement
- KVM signals eventfd for TX kicks and converts RX eventfd notifications into IRQs
- The VMM still processes data, and RX notification still requires a write syscall

### Interpreting increased TX latency

- Baseline: vCPU thread processes TX inline (no context switch)
- ioeventfd: measurement starts immediately before TX processing in a separate thread, after eventfd read returns; notification-to-wakeup waiting time is excluded
- These results alone neither identify the cause of the increase nor establish a throughput improvement

## Internals

The benchmark adds:

- `USE_IOEVENTFD` / `USE_IRQFD` environment flags
- Conditional `KVM_IOEVENTFD` / `KVM_IRQFD` registration
- Fallback to the MMIO exit handler (TX) / ioctls (IRQ) when flags are OFF
- `clock_gettime(CLOCK_MONOTONIC)` around TX processing and IRQ injection
- Exit counters inside the KVM_EXIT_MMIO handler
- A `SIGINT` handler setting a stop flag; main displays stats after the vCPU thread exits

All four configurations can be compared with the same binary.

TX timing surrounds `virtio_console_tx()`; IRQ timing surrounds the branch issuing two ioctls or one write. `ioeventfd TX kicks` counts successful eventfd reads, which may combine multiple kicks. `IRQ inject (ioctl)` counts assert/deassert pairs, not individual ioctl calls.

## Key takeaway

ioeventfd separates TX processing from the vCPU thread, while irqfd replaces two IRQ-notification ioctls with one write. Processing time and notification count are different metrics; interpret effects according to what is actually measured.
