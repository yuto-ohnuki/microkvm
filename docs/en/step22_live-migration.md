# Step 22: Live migration — iterative pre-copy

## Goal

Migrate a running VM to a new instance without stopping it first. Combine dirty page tracking (Step 20) with state save/restore (Step 21) to implement iterative pre-copy: copy RAM while the guest runs, then stop briefly to transfer the final dirty pages and CPU state.

## Background

### What is live migration?

Live migration moves a running VM from one host to another with near-zero downtime. The guest never notices — it continues executing as if nothing happened.

Why it matters:
- **Host maintenance**: kernel update or hardware replacement without stopping VMs
- **Failure avoidance**: detect impending hardware failure, evacuate VMs first
- **Resource optimization**: rebalance VMs across hosts (bin-packing)

The challenge: guest RAM is 128MB and constantly being modified. You can't just copy it once — by the time you finish copying, pages have changed. The solution is iterative convergence.

### Pre-copy algorithm

```
1. Copy all RAM (iteration 0) — guest keeps running
2. Sleep 100ms — guest dirties some pages
3. Get dirty bitmap → copy only dirty pages (iteration 1)
4. Repeat until dirty set is small enough (< 50 pages)
5. Stop vCPU → copy final dirty pages + CPU state (stop-and-copy)
6. Start destination VM from the migration file
```

The key insight: each iteration copies fewer pages because the guest can only dirty so many pages in 100ms. For an idle guest, convergence is nearly instant. For a busy guest, it may take several iterations.

### Two phases

| Phase | Runs in | VM running? | What it writes |
|-------|---------|-------------|----------------|
| Pre-copy (`migrate_precopy`) | stdin_thread | Yes | Header + full RAM + dirty iterations |
| Stop-and-copy (`migrate_stop_and_copy`) | main (after join) | No | Final dirty + CPU/device state |

The split is necessary because pre-copy must run concurrently with the vCPU (guest keeps running during RAM copy), while stop-and-copy requires the vCPU to be stopped (consistent CPU state).

### Dirty page format in the migration file

Each dirty iteration writes:
```
[uint32_t dirty_count]
[uint32_t page_idx, 4096 bytes data] × dirty_count
```

`page_idx` = GPA / 4096. The destination reads base RAM first, then overlays each dirty iteration in order — later iterations overwrite earlier values for the same page. A page may appear in multiple iterations; the newest copy always wins.

## Execution flow

```
Source VM (Ctrl-A m):
─────────────────────────────────────────────────────────────────
stdin_thread                 vCPU thread
────────────                 ───────────
Ctrl-A m detected
migrate_precopy():
  reset dirty log
  write full RAM (128MB)     [guest keeps running]
  sleep 100ms                [guest dirties pages]
  KVM_GET_DIRTY_LOG
  write dirty pages (iter 1)
  dirty_count <= 50 → done
  stop_requested = 1
                             next VM exit
                             stop_requested → break

main (after join):
  migrate_stop_and_copy():
    KVM_GET_DIRTY_LOG → write final dirty
    save_cpu_state() → write CPU/device state
    measure downtime (0.4-0.5 ms)
    update header with iteration count
    close(fd)

Destination VM (--restore-migration migration.bin):
─────────────────────────────────────────────────────────────────
main:
  migrate_restore():
    read full RAM (base)
    apply dirty iteration 1 (overlay)
    apply final dirty pages (overlay)
    read CPU/device state
    apply state (PIT → clock → ... → REGS)
  KVM_RUN → guest resumes
```

## Implementation

### snapshot.h — migration structures

```c
#define MIG_MAGIC   0x4D4B4D47  /* "MKMG" */
#define MIG_VERSION 1

#define MIGRATION_INTERVAL_MS       100
#define MIGRATION_MAX_ITERS         5
#define MIGRATION_THRESHOLD_PAGES   50

struct migrate_header {
    uint32_t magic;
    uint32_t version;
    uint64_t mem_size;
    uint32_t num_iterations;
    uint32_t pad;
};

struct migrate_context {
    int fd;                     /* open file during pre-copy */
    uint32_t num_iterations;    /* completed iterations */
};
```

The threshold (50 pages) is chosen for this educational implementation. Production hypervisors adapt the stopping condition based on network bandwidth, dirty rate, and acceptable downtime.

### snapshot.c — migrate_precopy (Phase 1)

```c
int migrate_precopy(const char *path, int vmfd, void *mem, size_t mem_size,
    struct migrate_context *ctx)
{
    /* Write placeholder header */
    /* Reset dirty log (clear bitmap before full copy) */
    /* Write full RAM — iteration 0 */
    /* Loop: sleep → KVM_GET_DIRTY_LOG → write dirty pages
       until dirty_count <= threshold or max_iters reached */
    /* Save fd and iteration count in ctx for stop-and-copy */
}
```

### snapshot.c — migrate_stop_and_copy (Phase 2)

```c
int migrate_stop_and_copy(struct migrate_context *ctx, ...) {
    uint64_t t1 = now_ns();

    /* Write final dirty pages (vCPU is stopped — consistent) */
    migrate_write_dirty(fd, vmfd, mem, mem_size, &final_dirty);

    /* Reuse save_cpu_state() from Step 21 */
    save_cpu_state(fd, vcpufd, vmfd, uart, virtio);

    uint64_t t2 = now_ns();
    /* downtime = t2 - t1 (only the stop-and-copy phase) */

    /* Update header with final iteration count */
    lseek(fd, 0, SEEK_SET);
    write(fd, &hdr, sizeof(hdr));
}
```

### microkvm.c changes

- `Ctrl-A m`: calls `migrate_precopy()` from stdin_thread, sets `g_migrate_active = 1`
- `--restore-migration`: parses argument, calls `migrate_restore()` before KVM_RUN
- After vCPU join: `if (g_migrate_active)` → `migrate_stop_and_copy()`, else → `snap_save()`
- `now_ns()` moved to `microkvm.h` (shared between `microkvm.c` and `snapshot.c`)

## Output

```
/ # export FOO=bar
/ #
[monitor] starting live migration...

=== Live migration simulator ===
Iteration 0: full RAM copy 32768 pages
Iteration 1: 31 dirty pages
Stop-and-copy: 33 dirty pages
Downtime: 0.4 ms
Migration complete: migration.bin
================================

$ ./microkvm --restore-migration migration.bin
[migration] restoring from migration.bin
[migration] base RAM loaded (128 MB)
[migration] iteration 1: applied 31 dirty pages
[migration] final: applied 33 dirty pages
[migration] restore complete
Starting guest...

/ # echo $FOO
bar
```

Reading the numbers:
- **Iteration 0**: full 128MB copied (32768 pages × 4KB)
- **Iteration 1**: only 31 pages changed during the 100ms sleep (idle guest)
- **31 < 50 (threshold)**: convergence achieved in 1 iteration
- **Stop-and-copy**: 33 final dirty pages + CPU state
- **Downtime 0.4ms**: time the guest was actually stopped (state dump only)

Convergence visualized:
```
Pages to transfer:  32768 → 31 → 33 → done
                    ─────    ──    ──
                    full     Δ1    final   (guest stops here)
```

## Key insight

Live migration is not a single operation — it's a convergence loop. The dirty set shrinks with each iteration because the guest can only dirty a bounded number of pages per time interval. For an idle guest, convergence is nearly instant (31 pages in 100ms). For a write-heavy workload, more iterations are needed and downtime may increase. The fundamental trade-off: pre-copy time (total migration duration) vs downtime (guest-visible pause).

### Simulator limitation

This is a file-based simulator on a single host. The migration file must be fully written (source process exits) before the destination can restore. In production, source and destination communicate over a network, and the destination starts only after receiving the "migration complete" signal.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| Iterative pre-copy | Full copy → dirty iterations → stop-and-copy |
| Dirty convergence | 32768 → 31 → 33 pages (idle guest converges in 1 iteration) |
| Downtime measurement | now_ns() around stop-and-copy only = 0.4ms |
| Two-phase design | precopy (VM live) + stop-and-copy (VM stopped) |
| save_cpu_state reuse | Step 21's helper used in stop-and-copy (DRY) |
| Dirty log reset before full copy | Ensures iteration 1 sees only *new* writes |
| File format: base + overlays | Destination applies layers in order for correctness |

## What changed

From Step 21:
- **snapshot.h**: migration structs (`migrate_header`, `migrate_context`), constants, function declarations
- **snapshot.c**: `migrate_write_dirty()`, `migrate_read_dirty()`, `migrate_precopy()`, `migrate_stop_and_copy()`, `migrate_restore()`
- **microkvm.c**: `Ctrl-A m` handler, `--restore-migration`, `g_migrate_ctx`/`g_migrate_active`, conditional save/migrate after join
- **microkvm.h**: `now_ns()` moved here (shared utility)
- **Makefile**: `migration.bin` added to clean target

## Next step

Phase D is complete. The full memory state management progression:

```
Step 19: Observe (KVM MMU stats)
Step 20: Track (dirty page logging)
Step 21: Save (VM snapshot)
Step 22: Move (live migration)
```

Each step builds on the previous, culminating in a working pre-copy live migration simulator.
