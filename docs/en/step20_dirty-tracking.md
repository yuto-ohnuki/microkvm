# Step 20: Dirty page tracking

## Goal

Track which guest pages have been written to using `KVM_MEM_LOG_DIRTY_PAGES` and `KVM_GET_DIRTY_LOG`. Display dirty page counts on demand via the `Ctrl-A d` monitor command.

## Background

### What is a dirty page?

A "dirty" page is one that has been written to since the last check. In KVM's context:
- **dirty page** = a 4KB guest page that the guest wrote to since the last `KVM_GET_DIRTY_LOG` call
- **clean page** = a page not written to (or written before the last reset)

### Why track dirty pages?

Two primary use cases:
1. **Live migration (Step 22)**: instead of copying all 128MB of RAM every iteration, copy only pages that changed since the last pass
2. **Incremental snapshots**: save only modified pages instead of the entire RAM image

Both require knowing *which* pages changed. That's dirty tracking.

### How KVM dirty tracking works

KVM uses EPT write-protection to detect writes:

```
1. KVM_MEM_LOG_DIRTY_PAGES flag set on memory slot
   → KVM removes write permission from all EPT entries in that slot

2. Guest writes to page X
   → EPT violation (write fault, VM exit)
   → KVM sets dirty bitmap[X] = 1
   → KVM restores write permission on page X's EPT entry
   → Guest resumes (write succeeds on retry)

3. Guest writes to page X again
   → EPT entry already has write permission → no exit (zero cost)

4. VMM calls KVM_GET_DIRTY_LOG
   → KVM copies bitmap to userspace
   → KVM clears bitmap (all bits → 0)
   → KVM removes write permission again (re-arms tracking)
   → Next cycle begins
```

Only the first write after each `KVM_GET_DIRTY_LOG` call incurs an EPT write fault. Subsequent writes to the same page within the same cycle are free.

The key insight: `KVM_GET_DIRTY_LOG` atomically returns *and resets* the bitmap. The next call returns only pages dirtied since *this* call. This is the exact semantic needed for iterative pre-copy migration.

### Two APIs

| API | Role |
|-----|------|
| `KVM_MEM_LOG_DIRTY_PAGES` | Flag on `kvm_userspace_memory_region` — enables tracking for a slot |
| `KVM_GET_DIRTY_LOG` | ioctl — returns bitmap + clears it atomically |

## Execution flow

```
Host (microkvm)              KVM                         Guest
───────────────              ───                         ─────
SET_USER_MEMORY_REGION
  flags = KVM_MEM_LOG_DIRTY_PAGES
                             EPT: all pages write-protected
                                                         boot (writes pages)
                             EPT violation → bitmap[X]=1
                             restore write permission
                                                         shell prompt
Ctrl-A d
  KVM_GET_DIRTY_LOG (slot 0)
  KVM_GET_DIRTY_LOG (slot 1)
                             copy bitmap → userspace
                             clear bitmap, re-arm write-protect
  popcount → display
                                                         echo hello
Ctrl-A d (again)
  KVM_GET_DIRTY_LOG
                             returns only pages dirtied
                             since the previous call
  popcount → display
  (only ~188 pages this time)
```

## Implementation

### Enable dirty logging on memory slots

```c
struct kvm_userspace_memory_region region1 = {
    .slot = 0,
    .flags = KVM_MEM_LOG_DIRTY_PAGES,   /* enable dirty tracking */
    .guest_phys_addr = 0,
    .memory_size = 0xD0000,
    .userspace_addr = (unsigned long)mem,
};
```

Both slots (0 and 1, split around the MMIO hole) get this flag.

### print_dirty_log() — query and display

```c
static void print_dirty_log(int vmfd, size_t mem_size) {
    /* Allocate bitmap: 1 bit per page, rounded up to uint64_t boundary */
    size_t slot0_pages = 0xD0000 / 4096;
    size_t slot0_bitmap_sz = (slot0_pages + 63) / 64 * 8;
    uint64_t *bitmap0 = calloc(1, slot0_bitmap_sz);

    struct kvm_dirty_log log0 = { .slot = 0, .dirty_bitmap = bitmap0 };

    /* Fetch bitmap and atomically clear dirty bits */
    ioctl(vmfd, KVM_GET_DIRTY_LOG, &log0);

    /* Count dirty pages using popcount */
    uint64_t dirty0 = 0;
    for (size_t i = 0; i < slot0_bitmap_sz / 8; i++)
        dirty0 += __builtin_popcountll(bitmap0[i]);
}
```

The bitmap maps directly to guest pages:
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
=== Hello from microkvm guest! ===
/ #
=== Dirty page report (Ctrl-A d) ===
  Slot 0 [0x0-0xD0000]:        8 / 208 pages dirty
  Slot 1 [0xD1000-0x8000000]:  4259 / 32559 pages dirty
  Total:                       4267 pages (17068 KB)

/ # echo hello > /dev/hvc0
hello
/ #
=== Dirty page report (Ctrl-A d) ===
  Slot 0 [0x0-0xD0000]:        0 / 208 pages dirty
  Slot 1 [0xD1000-0x8000000]:  188 / 32559 pages dirty
  Total:                       188 pages (752 KB)

/ # mkdir /tmp
/ # dd if=/dev/zero of=/tmp/test bs=4K count=1024
1024+0 records in
1024+0 records out
/ #
=== Dirty page report (Ctrl-A d) ===
  Slot 0 [0x0-0xD0000]:        0 / 208 pages dirty
  Slot 1 [0xD1000-0x8000000]:  1233 / 32559 pages dirty
  Total:                       1233 pages (4932 KB)
```

Reading the results:
- **First call (boot)**: 4267 pages (~17MB) — all pages dirtied since tracking was enabled
- **Second call (after echo)**: 188 pages (752KB) — only pages dirtied *since the first call*
- **Third call (after dd 4MB)**: 1233 pages (~4.9MB) — 1024 data pages + ~209 pages for filesystem metadata, page cache management, and kernel bookkeeping
- **Slot 0 = 0 after first call**: low memory is static after boot — most writes now occur in normal RAM (Slot 1) rather than the low-memory boot region (BDA, boot params from Step 10)

The decreasing counts demonstrate the atomic reset behavior of `KVM_GET_DIRTY_LOG`.

## Key insight

`KVM_GET_DIRTY_LOG` does not just read — it reads *and resets*. Each call returns only the pages dirtied since the previous call. This "query + clear" semantic is exactly what live migration needs: copy dirty pages, clear the tracking, wait, then copy only the *new* dirty pages. Repeat until the dirty set converges to near zero, then stop the VM and transfer the final few pages.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| KVM_MEM_LOG_DIRTY_PAGES | Flag enables write-tracking via EPT write-protect |
| KVM_GET_DIRTY_LOG | Returns bitmap + atomically clears it |
| EPT write-protect for tracking | Same mechanism as demand paging (Step 19), different purpose |
| Dirty bitmap structure | 1 bit per 4KB page, uint64_t aligned |
| __builtin_popcountll | Counts set bits efficiently (dirty page count) |
| Iterative delta | Second call shows only changes since first call |

## What changed

From Step 19:
- **microkvm.c only**: `print_dirty_log()` function, `Ctrl-A d` handler, `KVM_MEM_LOG_DIRTY_PAGES` flag on both memory slots

No new files.

## Next step

[Step 21: VM snapshot](step21_snapshot.md) saves the complete VM state (CPU registers + device state + RAM) to a file and restores it with `--restore`, resuming execution from the exact point where it was saved.
