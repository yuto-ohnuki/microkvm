# Step 23: Live Migration (Socket)—TCP Streaming and the Source/Destination Model

## Goal

Extend Step 22's file-based migration to **sockets**, enabling live migration with **source and destination running simultaneously**. Replace a seekable file with TCP's ordered byte stream and adapt the protocol to their fundamental differences.

## Background

### What Step 22 lacks

Step 22 implemented working live migration, but as a **file-based simulator**:

```
Source:      ./microkvm → write migration.bin → process exits
Destination: ./microkvm --restore-migration migration.bin (after source exits)
```

The destination cannot start until the source finishes writing the migration file (and exits). Real live migration has **source and destination active simultaneously over a network**: the source guest runs during pre-copy, pauses briefly for stop-and-copy, and the destination takes over.

This step realizes that concurrent model by replacing file transport with a TCP socket.

### Central question: Why not send the file format unchanged over TCP?

This is the learning focus of the step. The answer lies in transport properties:

| | File (Step 22) | TCP socket (Step 23) |
|---|---|---|
| I/O completion | May complete partially | **Partial I/O**—one call may transfer only part of the data |
| Seek | `lseek()` can return to any position | **No seeking**—a one-way ordered stream |
| Updating metadata later | Seek back to the header and rewrite it at the end | **Written bytes cannot be taken back** |

Step 22 used `lseek(0)` at the end to rewrite `num_iterations` in the header; the destination then read that many dirty blocks. TCP cannot return to the beginning, so termination must be identified differently. Existing `writen()` / `readn()` helpers handle partial transfers.

### Solution 1: Phase markers (remove the seek dependency)

Instead of rewriting an iteration count in the header later, **prefix each dirty block with a one-byte phase marker**:

```
[0x01] [dirty block]   ← MIG_PHASE_DIRTY: iterations continue
[0x01] [dirty block]
[0x02] [dirty block]   ← MIG_PHASE_FINAL: final block (CPU state follows)
[cpu/device state]
```

The destination can immediately determine whether the next block is iterative or final. **It need not know the count in advance**, so no seek is required. The final marker specifies that CPU/device state follows.

This illustrates a basic stream-protocol principle: the stream must describe its own structure. Since lengths and frame boundaries cannot depend on metadata rewritten later, boundaries are embedded in the data.

### Solution 2: writen / readn (handle partial I/O)

Socket `write()`/`read()` calls may process only part of the requested bytes, depending on kernel socket-buffer capacity. Use existing helpers that loop until the full transfer completes:

```c
static ssize_t writen(int fd, const void *buf, size_t n);  /* Loop until all n bytes are written */
static ssize_t readn(int fd, void *buf, size_t n);         /* Loop until all n bytes are read */
```

Files can also have partial transfers, so they use the same helpers. These were introduced in Step 21 and are already used by Step 22's migration.

### Transport abstraction: path → fd

Move file opening out of the migration core into the caller, and **change the argument from `const char *path` to `int fd`**. Combined with phase markers and seekability handling, this allows the same transfer logic for files and sockets:

```c
/* Before (Step 22): open inside the function */
int migrate_precopy(const char *path, ...);

/* After (Step 23): receive an fd (caller chooses file/socket) */
int migrate_precopy(int fd, ...);
```

The caller prepares the fd:
- file: `open("migration.bin", ...)`
- socket source: `connect_to("tcp:host:port")`
- socket dest: `listen_on("tcp:port")` → `accept()`

The migration **algorithm does not know the transport**. This is a direct application of Unix's file-descriptor abstraction.

## Execution flow

```
Destination (start first: ./microkvm --incoming tcp:4444)   Source (./microkvm → Ctrl-A t)
──────────────────────────────────────────────────         ──────────────────────────────
main:                                                        stdin_thread          vCPU thread
  listen_on("tcp:4444")                                      ────────────          ───────────
    socket / bind / listen
    Block in accept() ←─────────────────────────────────┐
                                                        │    Detect Ctrl-A t
                                                        └──  connect_to("tcp:127.0.0.1:4444")
  migrate_restore(fd, ...):                                  migrate_precopy(fd, ...):
    readn header                ←──────[migrate_header]───    writen header
    readn 128MB base RAM        ←──────[full RAM]─────────    writen full RAM (128MB) [guest running]
    for (;;):                                                 Wait 100ms             [guest dirties pages]
      readn phase marker        ←──────[0x01][dirty]──────    writen 0x01 + dirty pages
      readn dirty block                                       (repeat until convergence)
                                                             stop_requested = 1
                                                                                    Next VM exit → break
                                                             main (after join):
                                                             migrate_stop_and_copy():
      phase==FINAL → break      ←──────[0x02][dirty]──────      writen 0x02 + final dirty
      readn CPU/device state    ←──────[cpu state]────────      writen CPU/device state
    Apply state
  KVM_RUN → guest resumes
                                                                  close(fd) → process exits
```

The destination's `accept()` waits for the source's `connect()`. Header → RAM → dirty blocks → CPU state then flow **as a stream** over the same TCP connection. The source guest stays active throughout pre-copy and pauses only for stop-and-copy.

## Implementation

### snapshot.h—Protocol version and phase markers

```c
#define MIG_MAGIC   0x4D4B4D47  /* "MKMG" */
#define MIG_VERSION 2           /* Step 22 used 1; bump for the stream-format change */

/* One-byte phase marker placed before each dirty block */
#define MIG_PHASE_DIRTY     0x01    /* Iterations continue */
#define MIG_PHASE_FINAL     0x02    /* Final dirty block (CPU state follows) */
```

Increase `MIG_VERSION` from 1 to 2. Since the stream format changed, the header version check rejects old migration.bin files (version 1).

Change function signatures from path to fd:

```c
int migrate_precopy(int fd, int vmfd, void *mem, size_t mem_size,
    struct migrate_context *ctx);
int migrate_restore(int fd, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio,
    void *mem, size_t mem_size);
```

### snapshot.c — writen / readn helper

```c
/* Repeat partial writes and retry EINTR.
 * Treat write returning 0 without progress as an error too. */
static ssize_t writen(int fd, const void *buf, size_t n)
{
    size_t left = n;
    const char *p = buf;
    while (left > 0) {
        ssize_t nw = write(fd, p, left);
        if (nw < 0) {
            if (errno == EINTR) continue;   /* Interrupted by signal → retry */
            return -1;
        }
        if (nw == 0) return -1;             /* No write progress */
        p += nw; left -= nw;
    }
    return (ssize_t)n;
}
/* readn is implemented symmetrically */
```

`readn()` treats EOF before the required amount arrives as an error. Ignore `SIGPIPE` when sending through a socket so disconnection is reported as a `write()` error:

```c
signal(SIGPIPE, SIG_IGN);
```

Existing return-value checks propagate failures to the caller; the VM continues if pre-copy fails.

### snapshot.c—Send and receive phase markers

The sender (source) writes a marker before each dirty iteration:

```c
/* migrate_precopy: inside the iteration loop */
uint8_t phase = MIG_PHASE_DIRTY;
writen(fd, &phase, sizeof(phase));
migrate_write_dirty(fd, vmfd, mem, mem_size, &dirty_count);

/* migrate_stop_and_copy: before the final block */
uint8_t phase = MIG_PHASE_FINAL;
writen(fd, &phase, sizeof(phase));
migrate_write_dirty(fd, vmfd, mem, mem_size, &final_dirty);
```

The receiver (destination) replaces Step 22's `num_iterations` loop with a **marker-driven loop**:

```c
/* migrate_restore: Step 22 used for(i < hdr.num_iterations) */
for (;;) {
    uint8_t phase;
    if (readn(fd, &phase, sizeof(phase)) != sizeof(phase))
        goto fail;
    if (phase != MIG_PHASE_DIRTY && phase != MIG_PHASE_FINAL) {
        fprintf(stderr, "[migration] invalid phase: %#x\n", phase);
        goto fail;   /* Detect malformed stream */
    }
    int dirty = migrate_read_dirty(fd, mem, mem_size);
    if (dirty < 0) goto fail;
    if (phase == MIG_PHASE_FINAL) break;   /* CPU state follows final */
}
```

### snapshot.c—lseek fallback (share file/socket handling)

Since seeking fails on sockets, `migrate_stop_and_copy` rewrites the header **only when the seek succeeds**:

```c
if (lseek(fd, 0, SEEK_SET) >= 0) {   /* File: success → update header */
    struct migrate_header hdr = { ... };
    writen(fd, &hdr, sizeof(hdr));
}
/* Socket: lseek returns -1 → skip (phase markers make header num_iterations unnecessary) */
```

For both files and sockets, the destination uses phase markers to detect the end, not `num_iterations`. The file version still rewrites the iteration count in the header, but now uses version 2 format.

### snapshot.c—Bounds checks (defensive stream validation)

Reuse the bounds checks in Step 22's `migrate_read_dirty` for socket reception:

```c
if (dirty_count > mem_size / 4096) { /* Invalid count → abort */ return -1; }
...
if (page_idx >= mem_size / 4096) {   /* Out-of-range page index → abort */ return -1; }
```

Besides existing dirty_count, page_idx, and MSR-count checks, this step detects invalid phase markers.

### microkvm.c — connect_to / listen_on

Source (TCP client):

```c
/* spec: "tcp:<ipv4>:<port>" or "tcp:<port>" (defaults to 127.0.0.1) */
static int connect_to(const char *spec)
{
    /* Skip "tcp:", split host/port with strrchr(':'), validate port range with strtol */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  /* Discussed below */
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { /* Detect invalid address */ }
    if (connect(sock, ...) < 0) { perror("connect"); ... }
    return sock;
}
```

Destination (TCP server):

```c
/* spec: "tcp:<port>" */
static int listen_on(const char *spec)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, ...);   /* Avoid "Address already in use" on restart */
    bind(srv, ...); listen(srv, 1);
    fprintf(stderr, "[migration] waiting for connection on port %ld...\n", port);
    int sock = accept(srv, NULL, NULL);     /* Block waiting for source to connect */
    close(srv);                             /* Listening socket no longer needed */
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, ...);
    return sock;
}
```

### microkvm.c—Key bindings and --incoming

Keep file and socket modes available through **separate keys**:

- `Ctrl-A m` = **file-based** (write `migration.bin`, version 2 format)
- `Ctrl-A t` = **socket-based** (connect and send to destination; t stands for TCP/transport)

```c
/* stdin_thread: socket transmission */
if (c == 't') {
    fprintf(stderr, "\n[monitor] starting live migration (socket)...\n");
    int mig_fd = connect_to("tcp:127.0.0.1:4444");
    if (mig_fd < 0) {
        fprintf(stderr, "[monitor] migration aborted, VM continues\n");
    } else if (migrate_precopy(mig_fd, g_vmfd, virtio_dev.ram,
        GUEST_MEM_SIZE, &g_migrate_ctx) == 0) {
        /* Stop the vCPU for the final transfer */
        g_migrate_active = 1;
        stop_requested = 1;
    } else {
        fprintf(stderr, "[monitor] migration aborted, VM continues\n");
    }
    continue;
}
```

Destination startup flag:

```c
/* main: accept --incoming tcp:PORT during argument validation */
char *incoming_spec = NULL;
/* Check argc == 3 as for --restore / --restore-migration,
 * and set incoming_spec = argv[2] for --incoming */

/* Do not load bzImage for incoming mode (RAM arrives through migration) */
if (!restore_path && !migrate_restore_path && !incoming_spec)
    load_bzimage("bzImage", mem, CMDLINE);

/* Accept incoming migration via socket */
if (incoming_spec) {
    int mig_fd = listen_on(incoming_spec);
    if (mig_fd < 0) return 1;
    if (migrate_restore(mig_fd, vcpus[0].fd, vmfd, &uart, &virtio_dev,
        mem, GUEST_MEM_SIZE) < 0)
        return 1;
}
```

## Output

```
=== Destination (Terminal 1)—start first ===
$ ./microkvm --incoming tcp:4444
[migration] waiting for connection on port 4444...
        (Blocks here until Ctrl-A t is pressed on the source)
[migration] receiving state...
[migration] base RAM loaded (128 MB)
[migration] phase=iter: applied 31 dirty pages
[migration] phase=final: applied 46 dirty pages
[migration] restore complete
Starting guest...
Starting guest [ioeventfd=OFF, irqfd=OFF]...

/ # echo $A
123

=== Source (Terminal 2) ===
$ ./microkvm
/ # export A=123
/ # echo $A
123
/ # < Ctrl-A t >
[monitor] starting live migration (socket)...

=== Live migration simulator ===
Iteration 0: full RAM copy 32768 pages
Iteration 1: 31 dirty pages
Stop-and-copy: 46 dirty pages
Downtime: 0.4 ms
Migration complete: migration.bin
================================
        (Prints execution report and exits; report omitted)
```

How to read the output:

- `Migration complete: migration.bin` is a fixed message from shared code. With `Ctrl-A t`, data is sent to a TCP socket, not a file
- The destination's `accept()` blocks waiting for the source's `connect()` (Ctrl-A t)
- `phase=iter` / `phase=final` show that the destination identifies block types by **reading phase markers**, unlike Step 22's iteration-number logs
- `echo $A` on the destination prints `123`, confirming the shell variable is retained
- Downtime 0.4ms is source-side time to send the final marker, dirty pages, and CPU/device state; it excludes completion of destination restoration
- Source `Iteration 1: 31` matches destination `phase=iter: 31`, showing the same dirty block arrived through the stream

## Key insight

TCP cannot seek, so phase markers before dirty blocks indicate whether iterations continue or end. Existing `writen()` / `readn()` handle partial transfers, and migration operates on an fd supplied by the caller. This division allows pre-copy and stop-and-copy to be shared by file and socket transports.

### Out-of-scope topics

microkvm is an educational VMM. The following belong to production migration and are intentionally excluded:
- Bandwidth limits (`--bandwidth`), timeouts/reconnection/disconnection recovery
- TLS/encryption and compatibility negotiation
- `Ctrl-A t` currently hardcodes `tcp:127.0.0.1:4444` as its destination (for same-host localhost experiments)

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| Transport abstraction | `path` → `fd`; migration algorithm does not distinguish file/socket |
| Partial I/O | Socket `write`/`read` may complete partially → loop in `writen`/`readn` |
| Self-describing stream | Embed boundaries and termination with phase markers (0x01/0x02) |
| Seekable vs. stream | Files allow a final header rewrite via `lseek`; sockets do not, changing the design |
| Source/destination model | Both remain active while streaming state over TCP |
| `--incoming` / `accept` | Destination listens first and waits for source connection |
| TCP_NODELAY | Avoid Nagle delays for small metadata (markers, counts) |
| SO_REUSEADDR | Avoid "Address already in use" on restart |
| Protocol versioning | Bump `MIG_VERSION` (1 → 2) when stream format changes |
| Defensive stream validation | Bounds-check dirty_count/page_idx and detect invalid phases |

## What changed

Changes from Step 22:
- **snapshot.h**: `MIG_VERSION` 1 → 2, add `MIG_PHASE_DIRTY`/`MIG_PHASE_FINAL`, change `migrate_precopy`/`migrate_restore` signatures from `path` to `fd`
- **snapshot.c**: path → fd, phase marker transmission/reception, marker-driven restore loop, header update only when seekable
- **microkvm.c**: add `connect_to()`/`listen_on()`, parse `--incoming tcp:PORT`, add `Ctrl-A t` (socket) separately from `Ctrl-A m` (file), add `incoming_spec` branches (skip bzImage/initramfs + `migrate_restore`), ignore SIGPIPE, and validate destination/arguments
- Retain file-based migration; both file and socket modes use version 2 format

## Next step

Phase D (memory state management) is complete. Overview:

```
Step 19: Observe (KVM MMU stats)
Step 20: Track (dirty page logging)
Step 21: Save (VM snapshot)
Step 22: Move—file transport (migration algorithm)
Step 23: Move—socket transport (streaming protocol)
```

Step 22 covered the migration **algorithm**, and Step 23 covered **transport/protocol**. Next is [Step 24: PCI Config Space](step24_pci-config.md): implement x86's standard device-discovery mechanism so Linux can scan the bus and find a custom device.
