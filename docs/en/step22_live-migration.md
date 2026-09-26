# Step 22: Live migration — iterative pre-copy

## Goal

Use a file to test live-migration pre-copy and restoration. Combine dirty page tracking (Step 20) with state save/restore (Step 21) for iterative pre-copy: copy RAM while the guest runs, then pause briefly to transfer remaining dirty pages and CPU state.

## Background

### What is live migration?

Live migration moves a VM to another host with minimal downtime. The guest runs during pre-copy, stops for the final state transfer, and resumes at the destination.

Why it matters:
- **Host maintenance**: perform kernel updates or hardware replacement without shutting down VMs
- **Failure avoidance**: detect signs of hardware failure and evacuate VMs in advance
- **Resource optimization**: redistribute VMs among hosts (bin-packing)

The challenge is that 128MB of guest RAM keeps changing. One copy is insufficient: pages may have changed by the time copying finishes. The solution is iterative convergence.

### Pre-copy algorithm

```
1. Copy all RAM (iteration 0)—guest keeps running
2. Wait 100ms—guest dirties some pages
3. Retrieve dirty bitmap → copy only dirty pages (iteration 1)
4. Repeat until the dirty set is at most 50 pages, or for up to five iterations
5. Stop vCPU → copy final dirty pages + CPU state (stop-and-copy)
6. Start the destination VM from the migration file
```

Each copy covers pages written since the previous bitmap retrieval. The set may grow or shrink with the write workload; it does not necessarily decrease. This run fell below the threshold on the first iteration.

### Two phases

| Phase | Runs in | VM running? | Contents written |
|---------|---------|------------|-------------|
| Pre-copy (`migrate_precopy`) | stdin_thread | Yes | Header + full RAM + dirty iterations |
| Stop-and-copy (`migrate_stop_and_copy`) | main (after join) | No | Final dirty pages + CPU/device state |

Why separate them: pre-copy must run concurrently with the vCPU so the guest can execute during RAM copying. Stop-and-copy must wait for the vCPU to stop to capture consistent CPU state.

### Dirty page format in the migration file

Each dirty iteration writes:
```
[uint32_t dirty_count]
[uint32_t page_idx, 4096 bytes data] × dirty_count
```

`page_idx` = GPA / 4096. The destination loads base RAM first, then overlays each dirty iteration in order. Later iterations overwrite earlier values for the same page; the latest copy always wins.

## Execution flow

```
Source VM (Ctrl-A m):
─────────────────────────────────────────────────────────────────
stdin_thread                 vCPU thread
────────────                 ───────────
Detect Ctrl-A m
migrate_precopy():
  Reset dirty log
  Write full RAM (128MB)       [guest keeps running]
  Wait 100ms                  [guest dirties pages]
  KVM_GET_DIRTY_LOG
  Write dirty pages (iter 1)
  dirty_count <= 50 → done
  stop_requested = 1
                             Next VM exit
                             stop_requested → break

main (after join):
  migrate_stop_and_copy():
    KVM_GET_DIRTY_LOG → write final dirty pages
    save_cpu_state() → write CPU/device state
    Measure stop-and-copy save time
    Update header with iteration count
    close(fd)

Destination VM (--restore-migration migration.bin):
─────────────────────────────────────────────────────────────────
main:
  migrate_restore():
    Read full RAM (base)
    Apply dirty iteration 1 (overlay)
    Apply final dirty pages (overlay)
    Read CPU/device state
    Apply state (PIT → clock → ... → REGS)
  KVM_RUN → guest resumes
```

## Implementation

### snapshot.h—Migration structures

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
    int fd;                     /* Open file during pre-copy */
    uint32_t num_iterations;    /* Completed iteration count */
};
```

The threshold (50 pages) was chosen for this educational implementation. Production hypervisors adapt the stopping condition to network bandwidth, dirty rate, and allowable downtime.

### snapshot.c — migrate_precopy (Phase 1)

```c
int migrate_precopy(const char *path, int vmfd, void *mem, size_t mem_size,
    struct migrate_context *ctx)
{
    /* Write placeholder header */
    /* Reset dirty log (clear bitmap before full copy) */
    /* Write full RAM—iteration 0 */
    /* Loop: sleep → KVM_GET_DIRTY_LOG → write dirty pages
       until dirty_count <= threshold or max_iters is reached */
    /* Store fd and iteration count in ctx (for stop-and-copy) */
}
```

### snapshot.c — migrate_stop_and_copy (Phase 2)

```c
int migrate_stop_and_copy(struct migrate_context *ctx, ...) {
    uint64_t t1 = now_ns();

    /* Write final dirty pages (vCPU already stopped—consistent) */
    if (migrate_write_dirty(fd, vmfd, mem, mem_size, &final_dirty) < 0) {
        close(fd);
        return -1;
    }

    /* Reuse save_cpu_state() from Step 21 */
    if (save_cpu_state(fd, vcpufd, vmfd, uart, virtio) < 0) {
        close(fd);
        return -1;
    }

    uint64_t t2 = now_ns();
    /* downtime = t2 - t1 (stop-and-copy phase only) */

    /* Update header with final iteration count */
    lseek(fd, 0, SEEK_SET);
    if (writen(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        close(fd);
        return -1;
    }
    /* close and result display omitted */
}
```

### Changes to microkvm.c

- `Ctrl-A m`: call `migrate_precopy()` from stdin_thread; on success set `g_migrate_active = 1` and `stop_requested = 1`. Continue the VM on failure
- `--restore-migration`: parse argument and call `migrate_restore()` before KVM_RUN
- After vCPU join: run `migrate_stop_and_copy()` if `g_migrate_active`; reflect failure in the exit code
- Use the existing `now_ns()` from `microkvm.h` in `snapshot.c`

## Output

The source exits after migration completes. The excerpts below show migration and restore logs, omitting the execution report printed at exit.

```
/ # export FOO=bar
/ # echo $FOO
bar
/ # < Ctrl-A m >
[monitor] starting live migration (file)...

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
Starting guest [ioeventfd=OFF, irqfd=OFF]...

/ # echo $FOO
bar
```

How to read the values:
- **Iteration 0**: full 128MB copy (32768 pages × 4KB)
- **Iteration 1**: 31 pages written since the initial dirty-log reset, including during the full RAM copy and 100ms wait
- **31 ≤ 50 (threshold)**: stopping condition met after one iteration
- **Stop-and-copy**: the final 33 pages dirtied since the previous bitmap retrieval, plus CPU/device state. This is a different measurement interval, so exceeding 31 pages is consistent
- **Downtime 0.4ms**: time after vCPU join to retrieve/write final dirty pages and save CPU/device state. Excludes waiting for the vCPU to stop, header update, close, and destination startup/restore

After restoration, `echo $FOO` prints `bar`, confirming the shell variable is retained.

Copy volume:
```
Pages transferred: 32768 → 31 → 33 → done
               ─────    ──    ──
                   full    Δ1   final (guest stops before final)
```

## Key insight

Pre-copy first copies all RAM, tracks concurrent updates through dirty bitmaps, and overlays deltas. Finally, the vCPU stops so the remaining delta and CPU/device state can be saved. More iterations do not guarantee a smaller dirty set; the tradeoff is between copying time and work left for the final pause.

### Simulator limitations

This is a file-based simulator on a single host. The destination cannot restore until the migration file is fully written (the source process exits). Step 23's socket-based live migration connects source and destination over a network, and the destination starts after receiving a "migration complete" signal.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| Iterative pre-copy | Full copy → dirty iterations → stop-and-copy |
| Pre-copy stopping condition | This run reached the threshold at 31 pages, then copied the final 33 after stopping |
| Downtime measurement | Measure only stop-and-copy with now_ns() = 0.4ms |
| Two-phase design | Pre-copy (VM live) + stop-and-copy (VM stopped) |
| Reusing save_cpu_state | Use Step 21's helper for stop-and-copy (DRY) |
| Dirty-log reset before full copy | Ensures iteration 1 sees only *new* writes |
| File format: base + overlays | Destination applies layers in order for correctness |

## What changed

Changes from Step 21:
- **snapshot.h**: migration structures (`migrate_header`, `migrate_context`), constants, and function declarations
- **snapshot.c**: `migrate_write_dirty()`, `migrate_read_dirty()`, `migrate_precopy()`, `migrate_stop_and_copy()`, `migrate_restore()`
- **microkvm.c**: `Ctrl-A m`, `--restore-migration`, `g_migrate_ctx`/`g_migrate_active`, and post-join conditional handling
- **Makefile**: add `migration.bin` to clean

## Next step

The pre-copy live-migration **algorithm** now works. However, this remains a single-host file-based simulator: the destination cannot start until the source finishes writing the file (and exits).

[Step 23: Live Migration (Socket)](step23_live-migration-socket.md) replaces file transport with TCP sockets, enabling live migration with **source and destination running simultaneously**. Without changing the migration algorithm, learn how moving from a seekable file to a non-seekable stream changes protocol design.

Phase D (memory state management) overview:

```
Step 19: Observe (KVM MMU stats)
Step 20: Track (dirty page logging)
Step 21: Save (VM snapshot)
Step 22: Move—file transport (migration algorithm)
Step 23: Move—socket transport (streaming protocol)
```
