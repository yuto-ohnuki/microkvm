# Step 19: KVM MMU Stats—Observing Memory Virtualization

> **Phase D: Memory State Management**
>
> Phases A–C focused on CPU execution, device emulation, and I/O performance.
> Phase D turns to memory state: how KVM maps guest pages, tracks changes, and ultimately enables snapshots and live migration.

## Goal

Use `KVM_GET_STATS_FD` to capture per-VM / per-vCPU MMU counters before guest execution and after stopping. Observe demand paging, EPT fault resolution, and exit breakdowns through actual measurements.

## Background

### Why observe memory virtualization?

Steps 1–18 built the hypervisor. Phase D explores KVM's memory subsystem. Before dirty page tracking (Step 20) and snapshots (Step 21), we need visibility into how KVM manages guest memory.

Questions this step answers:
- How many guest pages are mapped when execution stops?
- How often do EPT faults occur, and how are they resolved?
- Which dominates the exit breakdown: I/O, memory, or HLT?

### KVM binary stats interface

`KVM_GET_STATS_FD` takes a VM or vCPU fd and returns a binary fd for reading that VM's or vCPU's statistics:

```
ioctl(vmfd, KVM_GET_STATS_FD, NULL)    → VM-level stats fd
ioctl(vcpufd, KVM_GET_STATS_FD, NULL)  → vCPU-level stats fd
```

Layout within the fd:
```
┌──────────────────────┐  offset 0
│ kvm_stats_header     │  num_desc, name_size, desc_offset, data_offset
├──────────────────────┤  offset = desc_offset
│ kvm_stats_desc[N]    │  name + type + offset within data block
├──────────────────────┤  offset = data_offset
│ uint64_t data[]      │  Actual counter values
└──────────────────────┘
```

The low four bits of each descriptor's `flags` identify the statistic type:
- **Cumulative (type=0)**: monotonically increasing counter (e.g., `pf_taken`)
- **Instant (type=1)**: snapshot of the current value (e.g., `pages_4k`)
- **Peak (type=2)**: highest value observed

### EPT demand paging

KVM does not build EPT (Extended Page Tables) for all guest memory in advance. A guest's first access to a page causes an EPT violation, and KVM maps it on demand:

```
Guest accesses an unmapped GPA
  → EPT violation (VM exit)
  → KVM page fault handler
  → Obtain the host page backing the GPA and create an EPT entry
  → pf_fixed++; pages_4k++ for a 4 KiB mapping
  → Resume guest (access succeeds on retry)
```

Building mappings on access avoids populating EPT for the entire 128 MiB beforehand. Host pages may already have been allocated when the VMM loaded the kernel or other data.

## Execution flow

```
Host (microkvm main)         KVM                    Guest
────────────────────         ───                    ─────
KVM_GET_STATS_FD (vm+vcpu)
kvm_stats_capture(before)
                                                    Boot starts
                                                    Kernel touches pages
                             EPT violation
                             → pf_fixed++
                             → pages_4k++
                                                    Boot completes
                                                    Shell prompt
Ctrl-C → stop_requested
kvm_stats_capture(after)
kvm_stats_print_delta()
  → pages_4k: 4004 (current)
  → pf_taken: +4006
  → pf_fixed: +4004
  → exits: +22173
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

Read the binary stats fd layout: header → descriptors → data block. This excerpt illustrates the read sequence, omitting error handling, storage, and cleanup.

```c
int kvm_stats_capture(int stats_fd, struct kvm_stats_reading *snap) {
    struct kvm_stats_header hdr;
    pread(stats_fd, &hdr, sizeof(hdr), 0);

    /* Each descriptor has variable length: fixed structure + name_size padding */
    size_t one_desc = sizeof(struct kvm_stats_desc) + hdr.name_size;
    char *descs = malloc(one_desc * hdr.num_desc);
    pread(stats_fd, descs, one_desc * hdr.num_desc, hdr.desc_offset);

    char *data = malloc(8 * 1024);
    pread(stats_fd, data, 8 * 1024, hdr.data_offset);

    /* Extract only scalar stats (skip histograms with size > 1) */
    /* Histogram stats contain arrays rather than one uint64_t; omitted for simplicity */
    for (unsigned int i = 0, n = 0; i < hdr.num_desc && n < KVM_STATS_MAX_ENTRIES; i++) {
        struct kvm_stats_desc *d = (void *)(descs + i * one_desc);
        if (d->size != 1) continue;
        /* Store name, value, and flags in entries[n] */
        n++;
    }
}
```

Design choices:
- Use `pread()` with explicit offsets—thread-safe, without shared seek state
- Skip histograms (`d->size > 1`); keep scalar counters only
- Store up to 64 scalars and read a fixed 8 KiB data block
- Filter displayed names by partial/exact matches (`pf_`, `pages_`, `tlb`, `exits`, etc.)
- Show deltas for cumulative stats and captured values for instant/peak stats; omit zeros

### microkvm.c — before/after capture

Capture stats twice: before `KVM_RUN` starts and after the vCPU stops. The delta also includes shell activity and idle behavior after boot.

```c
/* Before KVM_RUN */
int vm_stats_fd = ioctl(vmfd, KVM_GET_STATS_FD, NULL);
int vcpu_stats_fd = ioctl(vcpus[0].fd, KVM_GET_STATS_FD, NULL);
struct kvm_stats_reading vm_before = {0}, vcpu_before = {0};
if (vm_stats_fd >= 0)
    kvm_stats_capture(vm_stats_fd, &vm_before);
if (vcpu_stats_fd >= 0)
    kvm_stats_capture(vcpu_stats_fd, &vcpu_before);

/* After the vCPU stops */
struct kvm_stats_reading vm_after = {0}, vcpu_after = {0};
if (vm_stats_fd >= 0) {
    kvm_stats_capture(vm_stats_fd, &vm_after);
    kvm_stats_print_delta("KVM VM stats", &vm_before, &vm_after);
    close(vm_stats_fd);
}
if (vcpu_stats_fd >= 0) {
    kvm_stats_capture(vcpu_stats_fd, &vcpu_after);
    kvm_stats_print_delta("KVM vCPU 0 stats", &vcpu_before, &vcpu_after);
    close(vcpu_stats_fd);
}
```

## Output

KVM statistics excerpt from this execution report:

```
--- KVM VM stats ---
  pages_4k                         4004 (current)

--- KVM vCPU 0 stats ---
  pf_taken                         +4006
  pf_fixed                         +4004
  pf_emulate                       +2
  pf_mmio_spte_created             +2
  tlb_flush                        +4
  exits                            +22173
  io_exits                         +15674
  mmio_exits                       +163
  halt_exits                       +1270
  irq_injections                   +98
```

How to read the values:

- **pages_4k = 4004**: current 4 KiB mappings cover about 15.64 MiB (about 12.2% of 128 MiB guest RAM). This is not a cumulative access count and can decrease when mappings are removed.
- **pf_taken = 4006, pf_fixed = 4004**: most KVM page faults were resolved by creating/updating mappings or similar handling. These are not guest OS page-fault counts.
- **pf_emulate = 2, pf_mmio_spte_created = 2**: two faults proceeded to emulation, and two MMIO SPTEs (page-table entries managed by KVM) were created. This also matches `pf_taken - pf_fixed`, but these statistics alone do not identify accessed addresses.
- **exits = 22173**: VM exits counted by KVM during the measurement period. This includes exits handled internally; not all return to userspace.
- **io_exits = 15674 (about 71%)**: many exits are for I/O ports, including UART accesses that output Linux boot logs.
- **mmio_exits = 163**: matches `MMIO exits total: 163` in Userspace counters.
- **irq_injections = 98**: KVM interrupt injections, including timers and other sources. Userspace IRQ inject counters count only virtio RX notifications, so their value of 0 is consistent.

Both modes are OFF in this run, with no TX or IRQ latency data. MMIO exits still occur during boot-time device initialization even without a virtio TX/RX workload.

## Key insight

KVM creates guest-memory mappings on access; this run had 4,004 mappings of 4 KiB when stopped. Distinguishing current mapping counts from cumulative fault counts helps explain memory management. Step 20 adds dirty page tracking to observe writes.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| KVM_GET_STATS_FD | Binary stats API for per-VM/vCPU counters |
| EPT demand paging | Create mappings on access; observe current count with pages_4k |
| pf_fixed vs. pf_emulate | Faults resolved through mappings, etc., versus faults sent to emulation |
| Reading stats fd with pread() | Thread-safe explicit offsets |
| Exit breakdown | I/O (UART) dominates boot; MMIO (virtio) is a small fraction |

## What changed

Changes from the benchmark implementation after Step 18:
- **New files**: `kvm_stats.h`, `kvm_stats.c`—stats capture and filtered delta display
- **microkvm.c**: `#include "kvm_stats.h"`, capture before/after KVM_RUN
- **Makefile**: add `kvm_stats.c` to the build

## Next step

[Step 20: Dirty Page Tracking](step20_dirty-tracking.md) uses `KVM_MEM_LOG_DIRTY_PAGES` and `KVM_GET_DIRTY_LOG` to track which pages the guest writes—a foundation for live migration.
