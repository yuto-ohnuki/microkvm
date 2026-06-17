<p align="center"><img src="image/microkvm_icon.png" width="250" alt="microkvm"></p>

# microkvm

A step-by-step KVM-based hypervisor for learning virtualization internals.
microkvm is an educational hypervisor built directly on top of the Linux KVM API.

Each step introduces exactly one new virtualization concept, starting from a minimal guest that executes `hlt` and gradually evolving toward a full Linux boot, virtio I/O, and live migration.
By step 11, microkvm boots a real Linux kernel and provides an interactive shell over an emulated 8250 serial console. From step 12 onward, the guest stays fixed and the VMM itself evolves - mirroring real-world hypervisor development.

Each step is intentionally minimal and self-contained. Every VM exit can be traced and inspected.

Unlike production VMMs such as QEMU, microkvm intentionally prioritizes readability, traceability, and incremental learning over performance.

## What You Will Learn

- KVM ioctl workflow (`/dev/kvm` → VM → vCPU → `KVM_RUN`)
- VM exits and exit handling
- x86 CPU privilege modes (real → protected → long mode)
- x86 paging and identity-mapped page tables
- Interrupt delivery (IDT, interrupt gates, `iretq`)
- MMIO and device emulation
- MSR handling (TSC, synthetic MSRs)
- Multi-vCPU with pthreads
- Booting a real Linux kernel from a minimal VMM
- 8250 UART emulation (TX + RX with DLAB, IER, IIR, IRQ injection)
- Virtio-mmio transport (device discovery, feature negotiation, virtqueue setup)
- Virtio-console (TX and RX via descriptor/avail/used rings)
- Exit reduction techniques (ioeventfd, irqfd)
- EPT/MMU internals and demand paging observation
- Dirty page tracking (`KVM_GET_DIRTY_LOG`)
- VM snapshot (consistent CPU/device/memory state capture and restore)
- Live migration (dirty logging, iterative pre-copy, downtime reduction)
- PCI Configuration Mechanism #1 (CF8/CFC port I/O)
- BAR probing and MMIO device register access
- DMA via descriptor ring and doorbell kick
- MSI-X interrupt delivery (`KVM_SIGNAL_MSI`)
- PCI hotplug (config-space visibility + sysfs rescan/remove)
- Linux PCI driver development (`pci_register_driver`, probe/remove lifecycle)
- BAR mapping from kernel space (`pci_iomap`, `readl`/`writel`)
- DMA from a Linux driver (`dma_alloc_coherent`, descriptor submission)
- MSI-X interrupt handling (`pci_alloc_irq_vectors`, `request_irq`)

## Why microkvm?

QEMU is production-grade and feature-rich. microkvm intentionally trades completeness for readability. Each step introduces exactly one concept, making every VM exit and device interaction easy to trace and understand.

## Current Status

- ✅ Phase A: Basics (Step 1–8)
- ✅ Phase B: Linux Boot (Step 9–11)
- ✅ Phase C: Virtio and Exit Reduction (Step 12–18)
- ✅ Phase D: Memory State Management (Step 19–22)
- ✅ Phase E: PCI Device Model (Step 23–27)
- ✅ Phase F: Linux PCI Driver (Step 28–31)
- 🔲 Phase G: KVM Internals (planned)
- 🔲 Phase H: VT-x / VMCS (research)

## Steps

Each step adds exactly one concept. Every step is tagged in git.

### Phase A: Basics (Step 1–8)

| Step | Concept | What You Learn |
|------|---------|----------------|
| 1 | `hlt` execution | KVM API skeleton |
| 2 | I/O port character output | Exit handler loop |
| 3 | Real → protected mode | GDT, CR0, far jump |
| 4 | Protected → long mode (64-bit) | Page tables, CR3, CR4.PAE, EFER.LME |
| 5 | MMIO write | MMIO emulation via guest physical address trap |
| 6 | MMIO read + device state | Bidirectional device model |
| 7 | Interrupt injection (IRQ) | KVM_INTERRUPT, IDT, iretq |
| 8 | MSR handling | TSC / synthetic MSR handling |

### Phase B: Linux Boot (Step 9–11)

| Step | Concept | What You Learn |
|------|---------|----------------|
| 9 | Multiple vCPUs | pthreads + mutex |
| **10** | **★ Boot minimal Linux** | **bzImage + serial console + initramfs** |
| **11** | **★ Interactive shell** | **UART RX, host stdin → guest, busybox sh** |

> **Linux milestone**: From Step 12 onward, microkvm uses Linux as the guest.
> The focus shifts from extending `guest.S` to evolving the VMM itself.

### Phase C: Virtio and Exit Reduction (Step 12–18)

| Step | Concept | What You Learn |
|------|---------|----------------|
| 12 | virtio-mmio device discovery | MagicValue, DeviceID, VendorID, Linux probe |
| 13 | virtio feature negotiation | DeviceFeatures, DriverFeatures, status state machine |
| 14 | virtqueue setup | QueueSel, QueueNum, QueueReady, vring GPA |
| 15 | virtio-console TX | Guest → host via descriptor/avail/used ring |
| 16 | virtio-console RX | Host → guest + IRQ injection |
| 17 | ioeventfd | Kick exit elimination (kernel-side MMIO → eventfd) |
| 18 | irqfd | Interrupt exit elimination (eventfd → kernel-side injection) |

### Benchmark (tools/)

| Tool | What You Learn |
|------|----------------|
| ioeventfd/irqfd latency bench | Quantify exit elimination impact from Step 17–18 |

> Toggle `USE_IOEVENTFD=1` / `USE_IRQFD=1` to compare. See [docs/en/benchmark.md](docs/en/benchmark.md) for details.

### Phase D: Memory State Management (Step 19–22)

| Step | Concept | What You Learn |
|------|---------|----------------|
| 19 | KVM MMU stats explorer | EPT page-fault counters, demand paging observation |
| 20 | Dirty page tracking | `KVM_MEM_LOG_DIRTY_PAGES` + `KVM_GET_DIRTY_LOG` |
| 21 | VM snapshot | Full CPU/device/memory state restore (regs, sregs, FPU, MSRs, LAPIC, PIT, kvmclock) |
| 22 | Live migration simulator | Iterative pre-copy + stop-and-copy with downtime measurement |

**Example Results (Step 22 complete)**

```
MMU stats (Step 19):
  pages_4k: 6966, pf_fixed: 6966

Dirty tracking (Step 20):
  echo hello:  137 pages
  dd 4MB:     1224 pages

Live migration (Step 22):
  Iteration 0: 32768 pages
  Iteration 1: 27 pages
  Downtime:    0.5 ms
```

### Phase E: PCI Device Model (Step 23–27)

| Step | Concept | What You Learn |
|------|---------|----------------|
| 23 | PCI config space + BAR | Configuration Mechanism #1 (CF8/CFC), BAR probing |
| 24 | PCI MMIO register device | BAR-based device register access |
| 25 | DMA simulation | Descriptor + doorbell, bidirectional data transfer |
| 26 | MSI-X emulation | PCI capability, MMIO vector table, KVM_SIGNAL_MSI |
| 27 | PCI hotplug simulation | Config-space visibility toggle + sysfs rescan/remove |

### Phase F: Linux PCI Driver (Step 28–31)

| Step | Concept | What You Learn |
|------|---------|----------------|
| 28 | Minimal PCI driver | `pci_register_driver`, `probe/remove`, vendor/device ID match |
| 29 | BAR mapping | `pci_enable_device`, `pci_request_regions`, `pci_iomap`, `readl`/`writel` |
| 30 | DMA-capable driver | `dma_alloc_coherent`, DMA address, descriptor submission via doorbell |
| 31 | MSI-X interrupt handler | `pci_alloc_irq_vectors`, `request_irq`, completion handling |

> **Driver milestone**: Phase E built the VMM-side PCI device model.
> Phase F builds the Linux guest-side driver for the same virtual PCI device.
> Together they demonstrate both sides of a device virtualization stack:
> the device model in the VMM and the driver inside the guest kernel.

### Phase G: KVM Internals (Step 32–39) - planned

| Step | Concept | What You Learn |
|------|---------|----------------|
| 32 | VM exit profiler | Exit reason frequency and latency measurement |
| 33 | Exit timeline | Time-series exit pattern visualization |
| 34 | KVM tracepoints | ftrace / perf for KVM internal events |
| 35 | Host+guest correlation | Mapping host-side events to guest behavior |
| 36 | VM exit internals | KVM source: exit handling path |
| 37 | Interrupt virtualization internals | KVM source: LAPIC, posted interrupts |
| 38 | MSR virtualization internals | KVM source: MSR bitmap, emulation |
| 39 | CPUID virtualization internals | KVM source: CPUID filtering |

### Phase H: VT-x / VMCS (Step 40–43) - research topics

| Step | Concept | What You Learn |
|------|---------|----------------|
| 40 | VMCS explorer | VMCS field dump and analysis |
| 41 | EPT explorer | EPT table walk visualization |
| 42 | VM entry/exit controls | VMCS control field experiments |
| 43 | Tiny VMX hypervisor | Minimal VMX implementation without KVM |

> **Note:** Phase H is research-level. Step 43 in particular is a separate project in scope.

## Architecture

```
Phase A–E (Step 1–27):   Build a VMM (How to build a VMM)
Phase F   (Step 28–31):  Build a driver (How to drive the device from Linux)
Phase G   (Step 32–39):  Observe KVM (How KVM works - from outside)
Phase H   (Step 40–43):  Touch VT-x directly (How VT-x works - from inside)
```

VMCS is last because knowing MMIO, IRQ, MSR, virtio, migration, and device drivers makes every VMCS field meaningful - "that exit was controlled by *this* field."

## Navigating Steps

Each step is tagged in git. To view the code at any step:

```bash
git checkout step1    # view Step 1 code
git checkout step11   # view Linux boot milestone
git checkout step22   # view live migration
git checkout step27   # view PCI hotplug
git checkout step31   # view Linux PCI driver (MSI-X)
git checkout main     # return to latest
```

To see what changed between steps:

```bash
git diff step20..step21   # what snapshot added
```

## Building

```bash
make
./microkvm
```

For Phase B+ (Linux boot), place `bzImage` and `initramfs.gz` in the same directory:

```bash
./microkvm                                    # boot Linux
./microkvm --restore snapshot.bin             # restore from snapshot  (Step 21)
./microkvm --restore-migration migration.bin  # restore from migration (Step 22)
```

## Monitor Commands

| Key | Action |
|-----|--------|
| `Ctrl-A v` | Toggle input between ttyS0 (UART) and hvc0 (virtio-console) |
| `Ctrl-A d` | Print dirty page report |
| `Ctrl-A s` | Save VM snapshot |
| `Ctrl-A m` | Start live migration |
| `Ctrl-A h` | Toggle PCI hotplug device |

## Requirements

- Linux with KVM support (`/dev/kvm`)
- x86_64 CPU with VT-x (Intel) or SVM (AMD)
- GCC, GNU Make
- `bzImage` + `initramfs.gz` for Step 10+

## License

MIT

## References

- [Using the KVM API (LWN)](https://lwn.net/Articles/658511/)
- [Linux KVM source](https://github.com/torvalds/linux/tree/master/arch/x86/kvm)
- [Virtio specification v1.2](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html)
