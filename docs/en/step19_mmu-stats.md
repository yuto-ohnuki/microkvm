# Step 19: KVM MMU stats — observing memory virtualization

> **Phase D: Memory State Management**
>
> Phases A–C focused on CPU execution, device emulation, and I/O performance.
> Phase D shifts to memory state: how KVM maps guest pages, tracks modifications,
> and ultimately enables snapshots and live migration.

## Goal

Use `KVM_GET_STATS_FD` to capture per-VM and per-vCPU MMU counters before and after boot. Observe demand paging, EPT fault resolution, and exit breakdown through real numbers.

## Background

### Why observe memory virtualization?

Steps 1-18 built the hypervisor. Phase D shifts to understanding what happens *inside* KVM's memory subsystem. Before tracking dirty pages (Step 20) or implementing snapshots (Step 21), we need visibility into how KVM manages guest memory.

Questions this step answers:
- How many guest pages are actually mapped during boot?
- How many EPT faults occur and how are they resolved?
- What fraction of exits are I/O vs memory vs halts?

### KVM's binary stats interface

Before Linux 5.15, KVM stats were only available through debugfs (`/sys/kernel/debug/kvm/`), which showed aggregate counts across all VMs. `KVM_GET_STATS_FD` provides per-VM and per-vCPU stats through a binary fd:

```
ioctl(vmfd, KVM_GET_STATS_FD, NULL)    → fd for VM-level stats
ioctl(vcpufd, KVM_GET_STATS_FD, NULL)  → fd for vCPU-level stats
```

The fd contains three sections:
```
┌──────────────────────┐  offset 0
│ kvm_stats_header     │  num_desc, name_size, desc_offset, data_offset
├──────────────────────┤  offset = desc_offset
│ kvm_stats_desc[N]    │  name + type + offset into data block
├──────────────────────┤  offset = data_offset
│ uint64_t data[]      │  actual counter values
└──────────────────────┘
```

Each descriptor has a `flags` field encoding the stat type:
- **Cumulative (type=0)**: monotonically increasing counter (e.g., `pf_taken`)
- **Instant (type=1)**: current snapshot value (e.g., `pages_4k`)
- **Peak (type=2)**: high watermark

### EPT demand paging

KVM does not pre-populate the EPT (Extended Page Table) for all guest memory. When the guest first accesses a page, an EPT violation occurs and KVM maps it on demand:

```
Guest accesses unmapped GPA
  → EPT violation (VM exit)
  → KVM page fault handler
  → allocate host page, create EPT entry
  → pf_fixed++, pages_4k++
  → resume guest (access succeeds on retry)
```

This means only pages the guest actually touches during boot get EPT entries — not the full 128MB.

## Execution flow

```
Host (microkvm main)         KVM                    Guest
────────────────────         ───                    ─────
KVM_GET_STATS_FD (vm+vcpu)
kvm_stats_capture(before)
                                                    boot starts
                                                    kernel touches pages
                             EPT violation
                             → pf_fixed++
                             → pages_4k++
                                                    boot completes
                                                    shell prompt
Ctrl-C → stop_requested
kvm_stats_capture(after)
kvm_stats_print_delta()
  → pages_4k: 4325 (current)
  → pf_taken: +4327
  → pf_fixed: +4325
  → exits: +27665
```

## Implementation

### kvm_stats.h

New header defining the stats capture interface:

```c
#define KVM_STATS_MAX_ENTRIES 64

struct kvm_stat_entry {
    char name[48];
    uint64_t value;
    uint32_t flags;
};

struct kvm_stats_reading {
    unsigned int count;
    struct kvm_stat_entry entries[KVM_STATS_MAX_ENTRIES];
};

int kvm_stats_capture(int stats_fd, struct kvm_stats_reading *snap);
void kvm_stats_print_delta(const char *label,
    const struct kvm_stats_reading *before,
    const struct kvm_stats_reading *after);
```

### kvm_stats.c — capture

Reads the binary stats fd layout: header → descriptors → data block.

```c
int kvm_stats_capture(int stats_fd, struct kvm_stats_reading *snap) {
    struct kvm_stats_header hdr;
    pread(stats_fd, &hdr, sizeof(hdr), 0);

    /* Each descriptor is variable-length: fixed struct + name_size padding */
    size_t one_desc = sizeof(struct kvm_stats_desc) + hdr.name_size;
    char *descs = malloc(one_desc * hdr.num_desc);
    pread(stats_fd, descs, one_desc * hdr.num_desc, hdr.desc_offset);

    char *data = malloc(8 * 1024);
    pread(stats_fd, data, 8 * 1024, hdr.data_offset);

    /* Extract scalar stats only (skip histograms where size > 1) */
    /* Histogram stats contain arrays rather than single uint64_t values,
       so they are omitted for simplicity. */
    for (unsigned int i = 0; i < hdr.num_desc; i++) {
        struct kvm_stats_desc *d = (void *)(descs + i * one_desc);
        if (d->size != 1) continue;
        /* save name, value, flags */
    }
}
```

Key design choices:
- `pread()` with explicit offsets — thread-safe, no seek state
- Skip histograms (`d->size > 1`) — only scalar counters
- Filter by name prefix (`pf_*`, `pages_*`, `tlb*`, `exits`, etc.) at display time

### microkvm.c — before/after capture

Stats are captured at two points: just before `KVM_RUN` starts and after the vCPU stops. The delta shows what happened during the guest's execution.

```c
/* Before KVM_RUN */
int vm_stats_fd = ioctl(vmfd, KVM_GET_STATS_FD, NULL);
int vcpu_stats_fd = ioctl(vcpus[0].fd, KVM_GET_STATS_FD, NULL);
struct kvm_stats_reading vm_before = {0}, vcpu_before = {0};
kvm_stats_capture(vm_stats_fd, &vm_before);
kvm_stats_capture(vcpu_stats_fd, &vcpu_before);

/* After vCPU stops */
struct kvm_stats_reading vm_after = {0}, vcpu_after = {0};
kvm_stats_capture(vm_stats_fd, &vm_after);
kvm_stats_print_delta("KVM VM stats (boot delta)", &vm_before, &vm_after);
kvm_stats_capture(vcpu_stats_fd, &vcpu_after);
kvm_stats_print_delta("KVM vCPU 0 stats (boot delta)", &vcpu_before, &vcpu_after);
```

## Output

```
--- KVM VM stats (boot delta) ---
  pages_4k                         4325 (current)

--- KVM vCPU 0 stats (boot delta) ---
  pf_taken                         +4327
  pf_fixed                         +4325
  pf_emulate                       +2
  pf_mmio_spte_created             +2
  tlb_flush                        +4
  exits                            +27665
  io_exits                         +15047
  mmio_exits                       +163
  halt_exits                       +4316
  irq_injections                   +93
```

Reading the numbers:
- **pages_4k = 4325**: EPT maps 4325 pages = ~17MB out of 128MB guest RAM (demand paging). This is a current mapping count, not a cumulative counter — it can decrease if pages are unmapped.
- **pf_taken ≈ pf_fixed**: nearly every page fault was resolved by creating a new EPT mapping. `pf_taken` counts all handled page faults, while `pf_fixed` counts those resolved by installing a normal EPT mapping. The difference (4327 - 4325 = 2) matches `pf_emulate` — the MMIO special mappings.
- **pf_emulate = 2**: MMIO region (virtio-mmio at 0xD0000000) — special PTE created
- **exits = 27665**: total VM exits during boot
- **io_exits = 15047 (54%)**: UART serial output dominates. This matches Phase B/C — the Linux console still uses the 8250 UART (Step 11) for all boot messages.
- **mmio_exits = 163 (<1%)**: virtio device access is minimal

## Key insight

KVM does not pre-map all guest memory. Only ~13% of the 128MB guest RAM is actually touched during a minimal Linux boot. Each first access triggers an EPT violation → KVM allocates a host page and maps it in the EPT. This demand paging is the same mechanism that enables dirty tracking (Step 20): KVM controls EPT permissions to observe guest memory behavior.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| KVM_GET_STATS_FD | Binary stats API for per-VM/vCPU counters |
| EPT demand paging | pages_4k grows as guest touches new pages |
| pf_fixed vs pf_emulate | Normal mapping vs MMIO SPTE creation |
| pread() for stats fd | Thread-safe reads at explicit offsets |
| Exit breakdown | I/O (UART) dominates boot, MMIO (virtio) is minimal |

## What changed

From Step 18:
- **New files**: `kvm_stats.h`, `kvm_stats.c` — stats capture and filtered delta display
- **microkvm.c**: `#include "kvm_stats.h"`, before/after capture around KVM_RUN
- **Makefile**: add `kvm_stats.c` to build

## Next step

[Step 20: Dirty page tracking](step20_dirty-tracking.md) uses `KVM_MEM_LOG_DIRTY_PAGES` and `KVM_GET_DIRTY_LOG` to track which pages the guest has written to — the foundation for live migration.
