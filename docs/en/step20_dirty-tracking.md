# Step 20: Dirty page tracking

## Goal

Use `KVM_MEM_LOG_DIRTY_PAGES` and `KVM_GET_DIRTY_LOG` to track pages written by the guest. Display dirty page counts on demand with the `Ctrl-A d` monitor command.

## Background

### What is a dirty page?

"Dirty" means "written to." With this step's configuration:
- **Dirty page** = a 4KB page written by the guest since the last `KVM_GET_DIRTY_LOG` call
- **Clean page** = a page not written to (or not written to since reset)

### Why dirty tracking is needed

Two main use cases:
1. **Live migration (Step 22)**: send only pages changed since the previous iteration rather than copying all 128MB each time
2. **Incremental snapshots**: speed up saving by recording changed pages rather than all RAM

Both require knowing which pages changed. That is what dirty tracking provides.

### How KVM dirty tracking works

The following explains write detection using EPT write protection. The actual detection mechanism depends on CPU and KVM settings:

```
1. Set KVM_MEM_LOG_DIRTY_PAGES on a memory slot
   → KVM removes write permission from all EPT entries in the slot

2. Guest writes page X
   → EPT violation (write fault, VM exit)
   → KVM sets dirty bitmap[X] = 1
   → KVM restores write permission on page X's EPT entry
   → Resume guest (write succeeds on retry)

3. Guest writes page X again
   → EPT entry is already writable → no write fault needed for dirty detection

4. VMM calls KVM_GET_DIRTY_LOG
   → KVM copies the bitmap to userspace
   → KVM clears the bitmap (all bits → 0)
   → KVM removes write permission again (rearms tracking)
   → Next cycle begins
```

With this method, subsequent writes to a page already marked dirty and made writable need no fault for dirty detection.

This implementation does not enable manual clearing, so `KVM_GET_DIRTY_LOG` retrieves the bitmap and clears dirty bits before the ioctl returns. The next call returns only pages dirtied since the previous call—the semantics needed for iterative pre-copy migration.

### Two APIs

| API | Role |
|-----|------|
| `KVM_MEM_LOG_DIRTY_PAGES` | Flag in `kvm_userspace_memory_region` enabling tracking for a slot |
| `KVM_GET_DIRTY_LOG` | ioctl retrieving the bitmap and clearing dirty bits (with this configuration) |

## Execution flow

```
Host (microkvm)              KVM                         Guest
───────────────              ───                         ─────
SET_USER_MEMORY_REGION
  flags = KVM_MEM_LOG_DIRTY_PAGES
                             EPT: write-protect all pages
                                                         Boot (writes pages)
                             EPT violation → bitmap[X]=1
                             Restore write permission
                                                         Shell prompt
Ctrl-A d
  KVM_GET_DIRTY_LOG (slot 0)
  KVM_GET_DIRTY_LOG (slot 1)
                             Copy bitmap → userspace
                             Clear bitmap, reapply write protection
  popcount → display
                                                         echo hello
Ctrl-A d (again)
  KVM_GET_DIRTY_LOG
                             Return only pages dirtied
                             since the previous call
  popcount → display
  (191 pages in this run)
```

## Implementation

### Enable dirty logging on memory slots

```c
struct kvm_userspace_memory_region region1 = {
    .slot = MEM_SLOT0_ID,
    .flags = KVM_MEM_LOG_DIRTY_PAGES,   /* Enable dirty tracking */
    .guest_phys_addr = MEM_SLOT0_GPA,
    .memory_size = MEM_SLOT0_SIZE,
    .userspace_addr = (unsigned long)mem + MEM_SLOT0_GPA,
};
```

Set this flag on both slots (slot 0 and slot 1, separated by the MMIO hole).

### print_dirty_log()—Retrieve and display

This excerpt handles slot 0. The implementation also retrieves slot 1, handles allocation/ioctl errors, displays totals, and frees the bitmaps.

```c
static void print_dirty_log(int vmfd, size_t mem_size) {
    /* Allocate bitmap: one bit per page, rounded up to a uint64_t boundary */
    size_t slot0_pages = MEM_SLOT0_SIZE / 4096;
    size_t slot0_bitmap_sz = (slot0_pages + 63) / 64 * 8;
    uint64_t *bitmap0 = calloc(1, slot0_bitmap_sz);

    struct kvm_dirty_log log0 = { .slot = MEM_SLOT0_ID, .dirty_bitmap = bitmap0 };

    /* Retrieve bitmap and clear dirty bits */
    ioctl(vmfd, KVM_GET_DIRTY_LOG, &log0);

    /* Count dirty pages with popcount */
    uint64_t dirty0 = 0;
    for (size_t i = 0; i < slot0_bitmap_sz / 8; i++)
        dirty0 += __builtin_popcountll(bitmap0[i]);
}
```

Bit 0 of each bitmap corresponds to the first page of its slot. The GPA for bit n is `slot start GPA + n × 4096`:
```
Guest pages:  0  1  2  3  4  5  6  7
Bitmap bits:  1  0  1  0  0  1  0  0  → 3 dirty pages (popcount = 3)
```

### Ctrl-A d monitor command

```c
if (c == 'd') {
    print_dirty_log(g_vmfd, GUEST_MEM_SIZE);
    continue;
}
```

## Output

```
/ # < Ctrl-A d >
=== Dirty page report (Ctrl-A d) ===
  Slot 0 [0x0-0xd0000]:        8 / 208 pages dirty
  Slot 1 [0xd1000-0x8000000]:  3930 / 32559 pages dirty
  Total:                       3938 pages (15752 KB)

/ # echo hello > /dev/hvc0
hello
/ # < Ctrl-A d >
=== Dirty page report (Ctrl-A d) ===
  Slot 0 [0x0-0xd0000]:        0 / 208 pages dirty
  Slot 1 [0xd1000-0x8000000]:  191 / 32559 pages dirty
  Total:                       191 pages (764 KB)

/ # mkdir /tmp
/ # dd if=/dev/zero of=/tmp/test bs=4K count=1024
1024+0 records in
1024+0 records out
4194304 bytes (4.0MB) copied, 0.013352 seconds, 299.6MB/s
/ # < Ctrl-A d >
=== Dirty page report (Ctrl-A d) ===
  Slot 0 [0x0-0xd0000]:        0 / 208 pages dirty
  Slot 1 [0xd1000-0x8000000]:  1241 / 32559 pages dirty
  Total:                       1241 pages (4964 KB)
```

How to interpret the results:

- **First report (after boot)**: 3938 pages (15752 KB)—pages dirtied since tracking was enabled.
- **Second report (after echo)**: 191 pages (764 KB)—writes since the first report, including guest activity during that interval, not only echo processing.
- **Third report (after dd of 4 MiB)**: 1241 pages (4964 KB)—4 MiB of file data equals 1024 pages. mkdir, dd, filesystem, and kernel processing also write memory, but these totals alone cannot explain the additional 217 pages.
- **Slot 0 is 0 in reports 2 and 3**: no writes to low memory were recorded in those measurement intervals.

The displayed `KB` assumes 4 KiB per page. Repeated writes to one page count as one dirty page within the interval. The second value is not cumulative from startup, consistent with clearing on retrieval.

The vCPU continues running during report collection, and the two slots are read sequentially; this is not a simultaneous snapshot of a stopped VM.

## Key insight

With this configuration, `KVM_GET_DIRTY_LOG` retrieves and clears the dirty bitmap. Live migration retrieves/clears the bitmap and copies the indicated pages, then captures writes made during copying in the next iteration. Finally, stopping the VM and transferring the remainder includes updates made during copying.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| KVM_MEM_LOG_DIRTY_PAGES | Flag enabling dirty tracking per memory slot |
| KVM_GET_DIRTY_LOG | Retrieve bitmap and clear dirty bits (with this configuration) |
| EPT write protection | Detect writes through faults; a different purpose from Step 19's unmapped-page faults |
| Dirty bitmap structure | One bit per 4KB page, uint64_t alignment |
| __builtin_popcountll | Efficiently count set bits (dirty pages) |
| Iterative delta | Second call shows only changes since the first |

## What changed

Changes from Step 19:
- **microkvm.c only**: `print_dirty_log()`, `Ctrl-A d` handler, and `KVM_MEM_LOG_DIRTY_PAGES` on both memory slots

No new files.

## Next step

[Step 21: VM Snapshot](step21_snapshot.md) saves the VM's complete state (CPU registers + device state + RAM) to a file and restores it with `--restore`, resuming from the saved point.
