# Benchmark: ioeventfd and irqfd latency measurement

## Overview

microkvm includes a built-in benchmark that measures the performance impact of ioeventfd (Step 17) and irqfd (Step 18). By toggling these features via environment variables, you can compare four configurations and observe how exit elimination affects latency and exit counts.

## Prerequisites

The benchmark instrumentation is in a separate commit on top of Step 18:

```
ec8f80d tools: add latency benchmark for ioeventfd and irqfd
```

To enable the benchmark, check out this commit (or any commit after it on the `v2` branch). The `step18` tag does **not** include the benchmark code — it contains the clean ioeventfd + irqfd implementation only.

## Usage

```bash
# Pattern 1: Baseline (Step 15-16 style — MMIO exits + ioctl IRQ)
./microkvm

# Pattern 2: ioeventfd only (Step 17 — TX kick via eventfd, IRQ via ioctl)
USE_IOEVENTFD=1 ./microkvm

# Pattern 3: irqfd only (Step 18 IRQ path — TX kick via MMIO exit)
USE_IRQFD=1 ./microkvm

# Pattern 4: Both enabled (Step 17+18 — full optimization)
USE_IOEVENTFD=1 USE_IRQFD=1 ./microkvm
```

## Workload

For consistent results, run the same workload in each pattern:

```
1. Guest shell:  cat /dev/hvc0 &
2. TX test:      echo hello > /dev/hvc0   (repeat 3 times)
3. RX test:      Ctrl-A v → type "abc" → Ctrl-A v (back to UART)
4. Exit:         Ctrl-C → stats printed to stderr
```

## Results (EC2 c7i.xlarge, Linux 7.1.0+)

### Pattern 1: Baseline (ioeventfd=OFF, irqfd=OFF)

```
MMIO exits total:           185
QueueNotify MMIO exits:
  RX queue 0:               132
  TX queue 1:               10

TX processing latency:
  Method:   MMIO exit handler
  Avg:      5467 ns (5.47 us)

IRQ injection latency:
  Method:   ioctl (KVM_IRQ_LINE x2)
  Avg:      7372 ns (7.37 us)
```

### Pattern 2: ioeventfd only (USE_IOEVENTFD=1)

```
MMIO exits total:           175
QueueNotify MMIO exits:
  RX queue 0:               132
  TX queue 1:               0
ioeventfd TX kicks:         10

TX processing latency:
  Method:   ioeventfd thread
  Avg:      5575 ns (5.58 us)

IRQ injection latency:
  Method:   ioctl (KVM_IRQ_LINE x2)
  Avg:      6608 ns (6.61 us)
```

### Pattern 3: irqfd only (USE_IRQFD=1)

```
MMIO exits total:           185
QueueNotify MMIO exits:
  RX queue 0:               132
  TX queue 1:               10

TX processing latency:
  Method:   MMIO exit handler
  Avg:      5680 ns (5.68 us)

IRQ injection latency:
  Method:   irqfd (write)
  Avg:      4670 ns (4.67 us)
```

### Pattern 4: Both (USE_IOEVENTFD=1 USE_IRQFD=1)

```
MMIO exits total:           175
QueueNotify MMIO exits:
  RX queue 0:               132
  TX queue 1:               0
ioeventfd TX kicks:         10

TX processing latency:
  Method:   ioeventfd thread
  Avg:      6676 ns (6.68 us)

IRQ injection latency:
  Method:   irqfd (write)
  Avg:      4428 ns (4.43 us)
```

## Summary

| Metric | Baseline | ioeventfd | irqfd | Both |
|--------|----------|-----------|-------|------|
| MMIO exits total | 185 | **175** | 185 | **175** |
| TX QueueNotify exits | 10 | **0** | 10 | **0** |
| TX latency (avg) | 5.47 μs | 5.58 μs | 5.68 μs | 6.68 μs |
| IRQ latency (avg) | 7.37 μs | 6.61 μs | **4.67 μs** | **4.43 μs** |

## Interpretation

> **Note:** This workload is intentionally tiny (`echo hello` × 3). Real networking or storage workloads generate thousands of notifications per second, making the exit reduction much more significant. The value here is demonstrating the *mechanism*, not the absolute numbers.

### ioeventfd effect (Pattern 1 → 2)

- TX QueueNotify MMIO exits drop from 10 to **0**
- MMIO exits total decrease by 10 (exactly the eliminated TX kicks)
- **vCPU never stops for TX** — processing moves to a separate thread
- TX processing latency slightly increases due to thread scheduling overhead, but vCPU wait time becomes zero

### irqfd effect (Pattern 1 → 3)

- IRQ injection latency drops from 7.37 μs to **4.67 μs** (~37% reduction)
- Syscalls per IRQ: 2 (ioctl × 2) → 1 (write × 1)
- MMIO exits unchanged (irqfd optimizes the RX path, not TX)

### Combined (Pattern 4)

- Both optimizations stack: zero TX exits + faster IRQ injection
- The notification path for virtio I/O is fully kernel-resident
- vCPU is never blocked by I/O notifications in either direction

### Why TX latency increases with ioeventfd

- Baseline: vCPU thread processes TX inline (no context switch)
- ioeventfd: separate thread wakes via eventfd → thread scheduling adds ~1 μs
- **Trade-off**: TX processing is slightly slower, but vCPU throughput improves because it never waits

## How it works internally

The benchmark adds:
- `USE_IOEVENTFD` / `USE_IRQFD` environment variable flags
- Conditional registration of `KVM_IOEVENTFD` and `KVM_IRQFD`
- Fallback to MMIO exit handler (TX) and ioctl (IRQ) when flags are off
- `clock_gettime(CLOCK_MONOTONIC)` around TX processing and IRQ injection
- Exit counters in the KVM_EXIT_MMIO handler
- `SIGINT` handler to cleanly stop the VM and print stats

This allows comparing all four configurations with the same binary.

## Key takeaway

ioeventfd and irqfd are not primarily about making individual operations faster. They remove userspace round-trips from the notification path, allowing the vCPU and device processing to run independently. The vCPU never waits for I/O — it fires a signal and continues. This decoupling is the foundation of high-performance virtualized I/O.
