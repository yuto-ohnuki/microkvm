#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include "snapshot.h"
#include "microkvm.h"

/* Handle partial reads/writes and EINTR */
static ssize_t writen(int fd, const void *buf, size_t n)
{
    size_t left = n;
    const char *p = buf;
    while (left > 0) {
        ssize_t nw = write(fd, p, left);
        if (nw < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (nw == 0)
            return -1;
        p += nw;
        left -= nw;
    }
    return (ssize_t)n;
}

static ssize_t readn(int fd, void *buf, size_t n)
{
    size_t left = n;
    char *p = buf;
    while (left > 0) {
        ssize_t nr = read(fd, p, left);
        if (nr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (nr == 0)
            return -1;
        p += nr;
        left -= nr;
    }
    return (ssize_t)n;
}

/* Return -1 from the caller if the write fails */
#define WR(fd, buf, len)  do { \
    if (writen((fd), (buf), (len)) != (ssize_t)(len)) return -1; \
} while (0)

/* MSRs that define guest execution state - must be saved/restored for correct resume */
static const uint32_t snap_msr_list[SNAP_NUM_MSRS] = {
    MSR_IA32_TSC,
    MSR_IA32_APICBASE,
    MSR_IA32_SYSENTER_CS,
    MSR_IA32_SYSENTER_ESP,
    MSR_IA32_SYSENTER_EIP,
    MSR_IA32_STAR,
    MSR_IA32_LSTAR,
    MSR_IA32_CSTAR,
    MSR_IA32_FMASK,
    MSR_IA32_KERNEL_GS_BASE,
    MSR_KVM_WALL_CLOCK_NEW,
    MSR_KVM_SYSTEM_TIME_NEW,
};

/*
 * Save vCPU and device state to an open fd (without header or RAM).
 *   - vCPU state: regs, sregs, FPU, LAPIC, XCRs, vcpu_events, MSRs
 *   - VM state: PIT, IRQ chips, clock
 *   - Device state: UART, virtio-mmio
 */
static int save_cpu_state(int fd, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio)
{
    /* vCPU state */
    struct kvm_regs regs;
    ioctl(vcpufd, KVM_GET_REGS, &regs);
    WR(fd, &regs, sizeof(regs));

    struct kvm_sregs sregs;
    ioctl(vcpufd, KVM_GET_SREGS, &sregs);
    WR(fd, &sregs, sizeof(sregs));

    struct kvm_fpu fpu;
    ioctl(vcpufd, KVM_GET_FPU, &fpu);
    WR(fd, &fpu, sizeof(fpu));

    struct kvm_lapic_state lapic;
    ioctl(vcpufd, KVM_GET_LAPIC, &lapic);
    WR(fd, &lapic, sizeof(lapic));

    struct kvm_xcrs xcrs;
    ioctl(vcpufd, KVM_GET_XCRS, &xcrs);
    WR(fd, &xcrs, sizeof(xcrs));

    struct kvm_vcpu_events events;
    ioctl(vcpufd, KVM_GET_VCPU_EVENTS, &events);
    WR(fd, &events, sizeof(events));

    /* VM-wide state */
    struct kvm_pit_state2 pit_state;
    ioctl(vmfd, KVM_GET_PIT2, &pit_state);
    WR(fd, &pit_state, sizeof(pit_state));

    struct kvm_irqchip chips[3];
    for (int c = 0; c < 3; c++) {
        chips[c].chip_id = c;   /* 0=PIC master, 1=PIC slave, 2=IOAPIC */
        ioctl(vmfd, KVM_GET_IRQCHIP, &chips[c]);
        WR(fd, &chips[c], sizeof(chips[c]));
    }

    struct kvm_clock_data clock;
    ioctl(vmfd, KVM_GET_CLOCK, &clock);
    WR(fd, &clock, sizeof(clock));

    /* MSRs */
    struct {
        struct kvm_msrs header;
        struct kvm_msr_entry entries[SNAP_NUM_MSRS];
    } msrs;
    msrs.header.nmsrs = SNAP_NUM_MSRS;
    for (int i = 0; i < SNAP_NUM_MSRS; i++)
        msrs.entries[i].index = snap_msr_list[i];
    ioctl(vcpufd, KVM_GET_MSRS, &msrs);
    WR(fd, &msrs.header.nmsrs, sizeof(uint32_t));
    WR(fd, msrs.entries, sizeof(struct kvm_msr_entry) * SNAP_NUM_MSRS);

    /* Device state */
    WR(fd, uart, sizeof(*uart));

    struct virtio_snap vs = {
        .status = virtio->status,
        .host_features_sel = virtio->host_features_sel,
        .guest_features = virtio->guest_features,
        .guest_page_size = virtio->guest_page_size,
        .queue_sel = virtio->queue_sel,
        .interrupt_status = virtio->interrupt_status,
    };
    memcpy(vs.vqs, virtio->vqs, sizeof(vs.vqs));
    WR(fd, &vs, sizeof(vs));

    return 0;
}

/*
 * Print guest state via KVM_GET_REGS/KVM_GET_SREGS.
 * Values reflect KVM's logical vCPU state, not raw VMCS fields.
 */
void dump_cpu_state(int vcpufd)
{
    struct kvm_regs regs;
    ioctl(vcpufd, KVM_GET_REGS, &regs);

    struct kvm_sregs sregs;
    ioctl(vcpufd, KVM_GET_SREGS, &sregs);

    fprintf(stderr, "\n=== guest state dump (KVM-visible) ===\n");

    /* --- control flow + GPRs ---
     * RIP/RSP/RFLAGS correspond to VMCS GUEST_RIP/RSP/RFLAGS.
     * RAX/RBX/RCX/RDX are not VMCS guest-state fields; KVM maintains
     * the GPR state in software. */
    fprintf(stderr, "RIP    = 0x%016llx\n", (unsigned long long)regs.rip);
    fprintf(stderr, "RSP    = 0x%016llx\n", (unsigned long long)regs.rsp);
    fprintf(stderr, "RFLAGS = 0x%016llx\n", (unsigned long long)regs.rflags);
    fprintf(stderr, "RAX=0x%016llx RBX=0x%016llx\n",
        (unsigned long long)regs.rax, (unsigned long long)regs.rbx);
    fprintf(stderr, "RCX=0x%016llx RDX=0x%016llx\n",
        (unsigned long long)regs.rcx, (unsigned long long)regs.rdx);

    /* --- control registers ---
     * These correspond to VMCS GUEST_CR0/CR3/CR4 and GUEST_IA32_EFER, but
     * KVM's logical CR0/CR4 may differ from the raw VMCS fields because of
     * VMX guest/host masks and read shadows. */
    fprintf(stderr, "CR0    = 0x%016llx\n", (unsigned long long)sregs.cr0);
    fprintf(stderr, "CR3    = 0x%016llx\n", (unsigned long long)sregs.cr3);
    fprintf(stderr, "CR4    = 0x%016llx\n", (unsigned long long)sregs.cr4);
    fprintf(stderr, "EFER   = 0x%016llx  (LMA=%llu)\n",
        (unsigned long long)sregs.efer,
        (unsigned long long)((sregs.efer >> 10) & 1));

    /* --- segments CS/SS (correspond to VMCS GUEST_CS and GUEST_SS fields) --- */
    fprintf(stderr, "CS: sel=0x%04x base=0x%llx limit=0x%x type=0x%x db=%u l=%u\n",
        sregs.cs.selector, (unsigned long long)sregs.cs.base,
        sregs.cs.limit, sregs.cs.type, sregs.cs.db, sregs.cs.l);
    fprintf(stderr, "SS: sel=0x%04x base=0x%llx limit=0x%x type=0x%x\n",
        sregs.ss.selector, (unsigned long long)sregs.ss.base,
        sregs.ss.limit, sregs.ss.type);

    fprintf(stderr, "======================================\n");
}

/*
 * Save the VM state supported by this snapshot format to file.
 * Called from the vCPU thread when a snapshot is requested (Ctrl-A s);
 * the VM is not terminated and execution resumes afterward.
 *
 * This includes vCPU/KVM state, RAM, UART, and virtio-mmio state.
 * PCI/MSI-X/hotplug state is not included in the current snapshot format.
 */
int snap_save(const char *path, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio,
    void *mem, size_t mem_size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("snap_save open");
        return -1;
    }

    /* Header */
    struct snap_header hdr = {
        .magic = SNAP_MAGIC,
        .version = SNAP_VERSION,
        .mem_size = mem_size,
    };
    if (writen(fd, &hdr, sizeof(hdr)) != sizeof(hdr))
        goto fail;

    /* Save CPU + device state */
    if (save_cpu_state(fd, vcpufd, vmfd, uart, virtio) < 0)
        goto fail;

    /* Guest RAM (128MB) */
    if (writen(fd, mem, mem_size) != (ssize_t)mem_size)
        goto fail;

    close(fd);
    fprintf(stderr, "[snapshot] saved to %s\n", path);
    return 0;

fail:
    fprintf(stderr, "[snapshot] failed to save %s\n", path);
    close(fd);
    return -1;
}

/*
 * Restore VM state from file.
 * Called before KVM_RUN — the VM has been created but not yet started.
 *
 * Apply order is critical:
 *   1.  PIT      — overwrite default timer immediately (CREATE_PIT2 arms it)
 *   2.  Clock    — set clock offset before anything reads time
 *   3.  IRQchip  — restore PIC/IOAPIC state
 *   4.  XCRs     — must precede SREGS (affects XSAVE state)
 *   5.  SREGS    — page tables, CR0/CR3/CR4
 *   6.  MSRs     — includes KVM_SYSTEM_TIME_NEW (accesses guest RAM)
 *   7.  LAPIC    — depends on APICBASE MSR being set
 *   8.  Events   — pending exceptions/interrupts
 *   9.  FPU      — XSAVE state
 *   10. REGS    — general purpose registers (last, so RIP is final)
 */
int snap_restore(const char *path, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio,
    void *mem, size_t mem_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("snap_restore open");
        return -1;
    }

    /* Validate header */
    struct snap_header hdr;
    readn(fd, &hdr, sizeof(hdr));
    if (hdr.magic != SNAP_MAGIC || hdr.version != SNAP_VERSION) {
        fprintf(stderr, "Invalid snapshot file\n");
        close(fd);
        return -1;
    }
    if (hdr.mem_size != mem_size) {
        fprintf(stderr, "Memory size mismatch: snap=%llu, vm=%zu\n",
            (unsigned long long)hdr.mem_size, mem_size);
        close(fd);
        return -1;
    }

    /* Read all state from file (in save order) */
    struct kvm_regs regs;
    readn(fd, &regs, sizeof(regs));

    struct kvm_sregs sregs;
    readn(fd, &sregs, sizeof(sregs));

    struct kvm_fpu fpu;
    readn(fd, &fpu, sizeof(fpu));

    struct kvm_lapic_state lapic;
    readn(fd, &lapic, sizeof(lapic));

    struct kvm_xcrs xcrs;
    readn(fd, &xcrs, sizeof(xcrs));

    struct kvm_vcpu_events events;
    readn(fd, &events, sizeof(events));

    struct kvm_pit_state2 pit_state;
    readn(fd, &pit_state, sizeof(pit_state));

    struct kvm_irqchip chips[3];
    for (int c = 0; c < 3; c++)
        readn(fd, &chips[c], sizeof(chips[c]));

    struct kvm_clock_data clock;
    readn(fd, &clock, sizeof(clock));

    uint32_t nmsrs;
    readn(fd, &nmsrs, sizeof(nmsrs));
    struct {
        struct kvm_msrs header;
        struct kvm_msr_entry entries[SNAP_NUM_MSRS];
    } msrs;
    msrs.header.nmsrs = nmsrs;
    readn(fd, msrs.entries, sizeof(struct kvm_msr_entry) * nmsrs);

    readn(fd, uart, sizeof(*uart));

    struct virtio_snap vs;
    readn(fd, &vs, sizeof(vs));

    /* RAM must be loaded before MSRs (KVM_SYSTEM_TIME_NEW accesses guest RAM) */
    readn(fd, mem, mem_size);
    close(fd);

    /*
     * Apply state to KVM in correct order.
     * Wrong order causes timer storms, triple faults, or silent corruption.
     */
    ioctl(vmfd, KVM_SET_PIT2, &pit_state);
    ioctl(vmfd, KVM_SET_CLOCK, &clock);
    for (int c = 0; c < 3; c++)
        ioctl(vmfd, KVM_SET_IRQCHIP, &chips[c]);
    ioctl(vcpufd, KVM_SET_XCRS, &xcrs);
    ioctl(vcpufd, KVM_SET_SREGS, &sregs);
    ioctl(vcpufd, KVM_SET_MSRS, &msrs);
    ioctl(vcpufd, KVM_SET_LAPIC, &lapic);
    ioctl(vcpufd, KVM_SET_VCPU_EVENTS, &events);
    ioctl(vcpufd, KVM_SET_FPU, &fpu);
    ioctl(vcpufd, KVM_SET_REGS, &regs);

    /* Restore device state (host pointers are NOT in the file) */
    virtio->status = vs.status;
    virtio->host_features_sel = vs.host_features_sel;
    virtio->guest_features = vs.guest_features;
    virtio->guest_page_size = vs.guest_page_size;
    virtio->queue_sel = vs.queue_sel;
    virtio->interrupt_status = vs.interrupt_status;
    memcpy(virtio->vqs, vs.vqs, sizeof(vs.vqs));

    fprintf(stderr, "[snapshot] restored from %s\n", path);
    return 0;
}

/*
 * Write dirty pages to migration file.
 * Format: [uint32_t count] [uint32_t page_idx, 4096 bytes data] × count
 * Returns 0 on success, sets *out_dirty_count to number of dirty pages.
 */
static int migrate_write_dirty(int fd, int vmfd, void *mem, size_t mem_size,
    uint64_t *out_dirty_count)
{
    size_t slot0_pages = MEM_SLOT0_SIZE / 4096;
    size_t slot0_bitmap_sz = (slot0_pages + 63) / 64 * 8;
    uint64_t *bitmap0 = calloc(1, slot0_bitmap_sz);

    size_t slot1_pages = (mem_size - MEM_SLOT1_GPA) / 4096;
    size_t slot1_bitmap_sz = (slot1_pages + 63) / 64 * 8;
    uint64_t *bitmap1 = calloc(1, slot1_bitmap_sz);

    struct kvm_dirty_log log0 = {
        .slot = MEM_SLOT0_ID,
        .dirty_bitmap = bitmap0
    };
    struct kvm_dirty_log log1 = {
        .slot = MEM_SLOT1_ID,
        .dirty_bitmap = bitmap1
    };

    ioctl(vmfd, KVM_GET_DIRTY_LOG, &log0);
    ioctl(vmfd, KVM_GET_DIRTY_LOG, &log1);

    /* Count total dirty pages */
    uint32_t dirty_count = 0;
    for (size_t i = 0; i < slot0_bitmap_sz / 8; i++)
        dirty_count += __builtin_popcountll(bitmap0[i]);
    for (size_t i = 0; i < slot1_bitmap_sz / 8; i++)
        dirty_count += __builtin_popcountll(bitmap1[i]);

    if (writen(fd, &dirty_count, sizeof(dirty_count)) != sizeof(dirty_count))
        goto err;

    /* Write dirty pages from slot 0 */
    for (size_t i = 0; i < slot0_pages; i++) {
        if (bitmap0[i / 64] & (1ULL << (i % 64))) {
            uint32_t page_idx = (uint32_t)i;
            if (writen(fd, &page_idx, sizeof(page_idx)) != sizeof(page_idx))
                goto err;
            if (writen(fd, (char *)mem + i * 4096, 4096) != 4096)
                goto err;
        }
    }

    /* Write dirty pages from slot 1 */
    for (size_t i = 0; i < slot1_pages; i++) {
        if (bitmap1[i / 64] & (1ULL << (i % 64))) {
            uint32_t page_idx = (uint32_t)((MEM_SLOT1_GPA / 4096) + i);
            if (writen(fd, &page_idx, sizeof(page_idx)) != sizeof(page_idx))
                goto err;
            if (writen(fd, (char *)mem + MEM_SLOT1_GPA + i * 4096, 4096) != 4096)
                goto err;
        }
    }

    *out_dirty_count = dirty_count;
    free(bitmap0);
    free(bitmap1);
    return 0;

err:
    free(bitmap0);
    free(bitmap1);
    return -1;
}

/* Read and apply dirty pages from migration file */
static int migrate_read_dirty(int fd, void *mem, size_t mem_size)
{
    uint32_t dirty_count;
    if (readn(fd, &dirty_count, sizeof(dirty_count)) != sizeof(dirty_count))
        return -1;
    if (dirty_count > mem_size / 4096) {
        fprintf(stderr, "[migration] invalid dirty page count: %u\n", dirty_count);
        return -1;
    }
    for (uint32_t i = 0; i < dirty_count; i++) {
        uint32_t page_idx;
        if (readn(fd, &page_idx, sizeof(page_idx)) != sizeof(page_idx))
            return -1;
        if (page_idx >= mem_size / 4096) {
            fprintf(stderr, "[migration] invalid page index: %u\n", page_idx);
            return -1;
        }
        if (readn(fd, (char *)mem + (uint64_t)page_idx * 4096, 4096) < 0)
            return -1;
    }
    return (int)dirty_count;
}

/*
 * Phase 1: pre-copy — runs while VM is still live (called from stdin_thread).
 * Writes header + full RAM + iterative dirty pages.
 * Closes fd on failure; on success, stores it in ctx for
 * migrate_stop_and_copy() to close.
 */
int migrate_precopy(int fd, int vmfd, void *mem, size_t mem_size,
    struct migrate_context *ctx)
{
    fprintf(stderr, "\n=== Live migration simulator ===\n");

    /* Write placeholder header (num_iterations updated in stop-and-copy) */
    struct migrate_header hdr = {
        .magic = MIG_MAGIC,
        .version = MIG_VERSION,
        .mem_size = mem_size,
        .num_iterations = 0,
    };
    if (writen(fd, &hdr, sizeof(hdr)) != sizeof(hdr))
        goto err;

    /* Reset dirty log before full copy (so iterations get delta only) */
    {
        size_t slot0_pages = MEM_SLOT0_SIZE / 4096;
        size_t slot0_bitmap_sz = (slot0_pages + 63) / 64 * 8;
        uint64_t *bm0 = calloc(1, slot0_bitmap_sz);
        size_t slot1_pages = (mem_size - MEM_SLOT1_GPA) / 4096;
        size_t slot1_bitmap_sz = (slot1_pages + 63) / 64 * 8;
        uint64_t *bm1 = calloc(1, slot1_bitmap_sz);
        struct kvm_dirty_log dl0 = { .slot = MEM_SLOT0_ID, .dirty_bitmap = bm0 };
        struct kvm_dirty_log dl1 = { .slot = MEM_SLOT1_ID, .dirty_bitmap = bm1 };
        ioctl(vmfd, KVM_GET_DIRTY_LOG, &dl0);
        ioctl(vmfd, KVM_GET_DIRTY_LOG, &dl1);
        free(bm0);
        free(bm1);
    }

    /* Iteration 0: full RAM copy */
    uint32_t total_pages = (uint32_t)(mem_size / 4096);
    if (writen(fd, mem, mem_size) != (ssize_t)mem_size)
        goto err;
    fprintf(stderr, "Iteration 0: full RAM copy %u pages\n", total_pages);

    /* Iterative pre-copy: wait → get dirty → write dirty → repeat */
    uint32_t iter = 0;
    for (iter = 0; iter < MIGRATION_MAX_ITERS; iter++) {
        usleep(MIGRATION_INTERVAL_MS * 1000);

        uint8_t phase = MIG_PHASE_DIRTY;
        if (writen(fd, &phase, sizeof(phase)) != sizeof(phase))
            goto err;

        uint64_t dirty_count = 0;
        if (migrate_write_dirty(fd, vmfd, mem, mem_size, &dirty_count) < 0)
            goto err;
        fprintf(stderr, "Iteration %u: %llu dirty pages\n",
            iter + 1, (unsigned long long)dirty_count);

        if (dirty_count <= MIGRATION_THRESHOLD_PAGES)
            break;
    }

    ctx->fd = fd;
    ctx->num_iterations = iter + 1;
    return 0;

err:
    close(fd);
    return -1;
}

/*
 * Phase 2: stop-and-copy — runs after vCPU has stopped.
 * Writes final dirty pages + CPU/device state, measures downtime.
 */
int migrate_stop_and_copy(struct migrate_context *ctx, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio,
    void *mem, size_t mem_size)
{
    int fd = ctx->fd;
    uint64_t t1 = now_ns();

    uint8_t phase = MIG_PHASE_FINAL;
    if (writen(fd, &phase, sizeof(phase)) != sizeof(phase)) {
        close(fd);
        return -1;
    }

    /* Final dirty pages (after vCPU stopped — guaranteed consistent) */
    uint64_t final_dirty = 0;
    if (migrate_write_dirty(fd, vmfd, mem, mem_size, &final_dirty) < 0) {
        close(fd);
        return -1;
    }

    /* Save CPU + device state */
    if (save_cpu_state(fd, vcpufd, vmfd, uart, virtio) < 0) {
        close(fd);
        return -1;
    }

    uint64_t t2 = now_ns();
    double downtime_ms = (double)(t2 - t1) / 1e6;

    /* Rewrite the iteration count for seekable output only */
    if (lseek(fd, 0, SEEK_SET) >= 0) {
        struct migrate_header hdr = {
            .magic = MIG_MAGIC,
            .version = MIG_VERSION,
            .mem_size = mem_size,
            .num_iterations = ctx->num_iterations,
        };
        if (writen(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
            close(fd);
            return -1;
        }
    }
    close(fd);

    fprintf(stderr, "Stop-and-copy: %llu dirty pages\n", (unsigned long long)final_dirty);
    fprintf(stderr, "Downtime: %.1f ms\n", downtime_ms);
    fprintf(stderr, "Migration complete: migration.bin\n");
    fprintf(stderr, "================================\n");
    return 0;
}

/*
 * Restore from migration file.
 * Applies base RAM, then each dirty iteration in order, then CPU/device state.
 */
int migrate_restore(int fd, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio,
    void *mem, size_t mem_size)
{
    struct migrate_header hdr;
    if (readn(fd, &hdr, sizeof(hdr)) != sizeof(hdr))
        goto fail;
    if (hdr.magic != MIG_MAGIC || hdr.version != MIG_VERSION) {
        fprintf(stderr, "Invalid migration file\n");
        goto fail;
    }
    if (hdr.mem_size != mem_size) {
        fprintf(stderr, "Memory size mismatch: file=%llu, vm=%zu\n",
            (unsigned long long)hdr.mem_size, mem_size);
        goto fail;
    }

    fprintf(stderr, "[migration] receiving state...\n");

    /* Base RAM (iteration 0) */
    if (readn(fd, mem, mem_size) != (ssize_t)mem_size)
        goto fail;
    fprintf(stderr, "[migration] base RAM loaded (%zu MB)\n", mem_size >> 20);

    /* Apply dirty iterations (overlay on top of base) */
    for (;;) {
        uint8_t phase;
        if (readn(fd, &phase, sizeof(phase)) != sizeof(phase))
            goto fail;
        if (phase != MIG_PHASE_DIRTY && phase != MIG_PHASE_FINAL) {
            fprintf(stderr, "[migration] invalid phase: %#x\n", phase);
            goto fail;
        }

        int dirty = migrate_read_dirty(fd, mem, mem_size);
        if (dirty < 0)
            goto fail;
        fprintf(stderr, "[migration] phase=%s: applied %d dirty pages\n",
            phase == MIG_PHASE_FINAL ? "final" : "iter", dirty);

        if (phase == MIG_PHASE_FINAL)
            break;
    }

    /* Restore CPU/device state (same apply order as snap_restore) */
    struct kvm_regs regs;
    if (readn(fd, &regs, sizeof(regs)) != sizeof(regs))
        goto fail;

    struct kvm_sregs sregs;
    if (readn(fd, &sregs, sizeof(sregs)) != sizeof(sregs))
        goto fail;

    struct kvm_fpu fpu;
    if (readn(fd, &fpu, sizeof(fpu)) != sizeof(fpu))
        goto fail;

    struct kvm_lapic_state lapic;
    if (readn(fd, &lapic, sizeof(lapic)) != sizeof(lapic))
        goto fail;

    struct kvm_xcrs xcrs;
    if (readn(fd, &xcrs, sizeof(xcrs)) != sizeof(xcrs))
        goto fail;

    struct kvm_vcpu_events events;
    if (readn(fd, &events, sizeof(events)) != sizeof(events))
        goto fail;

    struct kvm_pit_state2 pit_state;
    if (readn(fd, &pit_state, sizeof(pit_state)) != sizeof(pit_state))
        goto fail;

    struct kvm_irqchip chips[3];
    for (int c = 0; c < 3; c++)
        if (readn(fd, &chips[c], sizeof(chips[c])) != sizeof(chips[c]))
            goto fail;

    struct kvm_clock_data clock;
    if (readn(fd, &clock, sizeof(clock)) != sizeof(clock))
        goto fail;

    uint32_t nmsrs;
    if (readn(fd, &nmsrs, sizeof(nmsrs)) != sizeof(nmsrs))
        goto fail;
    if (nmsrs > SNAP_NUM_MSRS) {
        fprintf(stderr, "[migration] invalid nmsrs: %u\n", nmsrs);
        goto fail;
    }
    struct {
        struct kvm_msrs header;
        struct kvm_msr_entry entries[SNAP_NUM_MSRS];
    } msrs;
    msrs.header.nmsrs = nmsrs;
    if (readn(fd, msrs.entries, sizeof(struct kvm_msr_entry) * nmsrs)
        != (ssize_t)(sizeof(struct kvm_msr_entry) * nmsrs))
        goto fail;

    if (readn(fd, uart, sizeof(*uart)) != sizeof(*uart))
        goto fail;

    struct virtio_snap vs;
    if (readn(fd, &vs, sizeof(vs)) != sizeof(vs))
        goto fail;
    close(fd);

    /* Apply in correct order (same as snap_restore) */
    ioctl(vmfd, KVM_SET_PIT2, &pit_state);
    ioctl(vmfd, KVM_SET_CLOCK, &clock);
    for (int c = 0; c < 3; c++)
        ioctl(vmfd, KVM_SET_IRQCHIP, &chips[c]);
    ioctl(vcpufd, KVM_SET_XCRS, &xcrs);
    ioctl(vcpufd, KVM_SET_SREGS, &sregs);
    ioctl(vcpufd, KVM_SET_MSRS, &msrs);
    ioctl(vcpufd, KVM_SET_LAPIC, &lapic);
    ioctl(vcpufd, KVM_SET_VCPU_EVENTS, &events);
    ioctl(vcpufd, KVM_SET_FPU, &fpu);
    ioctl(vcpufd, KVM_SET_REGS, &regs);

    /* Restore virtio device state */
    virtio->status = vs.status;
    virtio->host_features_sel = vs.host_features_sel;
    virtio->guest_features = vs.guest_features;
    virtio->guest_page_size = vs.guest_page_size;
    virtio->queue_sel = vs.queue_sel;
    virtio->interrupt_status = vs.interrupt_status;
    memcpy(virtio->vqs, vs.vqs, sizeof(vs.vqs));

    fprintf(stderr, "[migration] restore complete\n");
    return 0;

fail:
    close(fd);
    return -1;
}
