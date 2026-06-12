#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include "uart.h"
#include "virtio_mmio.h"

/* Provided by microkvm.c */
static inline uint64_t now_ns(void);

/*
 * Step 21: VM snapshot — save/restore full VM state
 *
 * File format:
 *   [header]  [regs]  [sregs]  [fpu]  [msrs]  [uart]  [virtio]  [RAM]
 */

#define SNAP_MAGIC  0x4D4B564D  /* "MKVM" */
#define SNAP_VERSION 1

/* MSR indices we need to save */
#define MSR_IA32_TSC             0x00000010
#define MSR_IA32_APICBASE        0x0000001B
#define MSR_IA32_SYSENTER_CS     0x00000174
#define MSR_IA32_SYSENTER_ESP    0x00000175
#define MSR_IA32_SYSENTER_EIP    0x00000176
#define MSR_IA32_STAR            0xC0000081
#define MSR_IA32_LSTAR           0xC0000082
#define MSR_IA32_CSTAR           0xC0000083
#define MSR_IA32_FMASK           0xC0000084
#define MSR_IA32_KERNEL_GS_BASE  0xC0000102
#define MSR_KVM_WALL_CLOCK_NEW   0x4B564D00
#define MSR_KVM_SYSTEM_TIME_NEW  0x4B564D01

#define SNAP_NUM_MSRS 12

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

struct snap_header {
    uint32_t magic;
    uint32_t version;
    uint64_t mem_size;
};

/* Virtio state without host pointers */
struct virtio_snap {
    uint32_t status;
    uint32_t host_features_sel;
    uint32_t guest_features;
    uint32_t guest_page_size;
    uint32_t queue_sel;
    uint32_t interrupt_status;
    struct virtqueue_state vqs[VIRTQ_NUM_QUEUES];
};

static int snap_save(const char *path, int vcpufd, int vmfd,
                     struct uart8250 *uart, struct virtio_mmio_dev *virtio,
                     void *mem, size_t mem_size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("snap_save open"); return -1; }

    /* Header */
    struct snap_header hdr = {
        .magic = SNAP_MAGIC,
        .version = SNAP_VERSION,
        .mem_size = mem_size,
    };
    write(fd, &hdr, sizeof(hdr));

    /* Registers */
    struct kvm_regs regs;
    ioctl(vcpufd, KVM_GET_REGS, &regs);
    write(fd, &regs, sizeof(regs));

    struct kvm_sregs sregs;
    ioctl(vcpufd, KVM_GET_SREGS, &sregs);
    write(fd, &sregs, sizeof(sregs));

    struct kvm_fpu fpu;
    ioctl(vcpufd, KVM_GET_FPU, &fpu);
    write(fd, &fpu, sizeof(fpu));

    struct kvm_lapic_state lapic;
    ioctl(vcpufd, KVM_GET_LAPIC, &lapic);
    write(fd, &lapic, sizeof(lapic));

    struct kvm_xcrs xcrs;
    ioctl(vcpufd, KVM_GET_XCRS, &xcrs);
    write(fd, &xcrs, sizeof(xcrs));

    struct kvm_vcpu_events events;
    ioctl(vcpufd, KVM_GET_VCPU_EVENTS, &events);
    write(fd, &events, sizeof(events));

    struct kvm_pit_state2 pit_state;
    ioctl(vmfd, KVM_GET_PIT2, &pit_state);
    write(fd, &pit_state, sizeof(pit_state));

    struct kvm_irqchip chips[3];
    for (int c = 0; c < 3; c++) {
        chips[c].chip_id = c;
        ioctl(vmfd, KVM_GET_IRQCHIP, &chips[c]);
        write(fd, &chips[c], sizeof(chips[c]));
    }

    struct kvm_clock_data clock;
    ioctl(vmfd, KVM_GET_CLOCK, &clock);
    write(fd, &clock, sizeof(clock));

    /* MSRs */
    struct {
        struct kvm_msrs header;
        struct kvm_msr_entry entries[SNAP_NUM_MSRS];
    } msrs;
    msrs.header.nmsrs = SNAP_NUM_MSRS;
    for (int i = 0; i < SNAP_NUM_MSRS; i++)
        msrs.entries[i].index = snap_msr_list[i];
    ioctl(vcpufd, KVM_GET_MSRS, &msrs);
    write(fd, &msrs.header.nmsrs, sizeof(uint32_t));
    write(fd, msrs.entries, sizeof(struct kvm_msr_entry) * SNAP_NUM_MSRS);

    /* UART state (no pointers, safe to write directly) */
    write(fd, uart, sizeof(*uart));

    /* Virtio state (exclude host pointers) */
    struct virtio_snap vs = {
        .status = virtio->status,
        .host_features_sel = virtio->host_features_sel,
        .guest_features = virtio->guest_features,
        .guest_page_size = virtio->guest_page_size,
        .queue_sel = virtio->queue_sel,
        .interrupt_status = virtio->interrupt_status,
    };
    memcpy(vs.vqs, virtio->vqs, sizeof(vs.vqs));
    write(fd, &vs, sizeof(vs));

    /* Guest RAM */
    write(fd, mem, mem_size);

    close(fd);
    fprintf(stderr, "[snapshot] saved to %s (%zu bytes)\n",
            path, sizeof(hdr) + sizeof(regs) + sizeof(sregs) + sizeof(fpu)
            + sizeof(uint32_t) + sizeof(struct kvm_msr_entry) * SNAP_NUM_MSRS
            + sizeof(*uart) + sizeof(vs) + mem_size);
    return 0;
}

static int snap_restore(const char *path, int vcpufd, int vmfd,
                        struct uart8250 *uart, struct virtio_mmio_dev *virtio,
                        void *mem, size_t mem_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("snap_restore open"); return -1; }

    struct snap_header hdr;
    read(fd, &hdr, sizeof(hdr));
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
    read(fd, &regs, sizeof(regs));

    struct kvm_sregs sregs;
    read(fd, &sregs, sizeof(sregs));

    struct kvm_fpu fpu;
    read(fd, &fpu, sizeof(fpu));

    struct kvm_lapic_state lapic;
    read(fd, &lapic, sizeof(lapic));

    struct kvm_xcrs xcrs;
    read(fd, &xcrs, sizeof(xcrs));

    struct kvm_vcpu_events events;
    read(fd, &events, sizeof(events));

    struct kvm_pit_state2 pit_state;
    read(fd, &pit_state, sizeof(pit_state));

    struct kvm_irqchip chips[3];
    for (int c = 0; c < 3; c++)
        read(fd, &chips[c], sizeof(chips[c]));

    struct kvm_clock_data clock;
    read(fd, &clock, sizeof(clock));

    uint32_t nmsrs;
    read(fd, &nmsrs, sizeof(nmsrs));
    struct {
        struct kvm_msrs header;
        struct kvm_msr_entry entries[SNAP_NUM_MSRS];
    } msrs;
    msrs.header.nmsrs = nmsrs;
    read(fd, msrs.entries, sizeof(struct kvm_msr_entry) * nmsrs);

    read(fd, uart, sizeof(*uart));

    struct virtio_snap vs;
    read(fd, &vs, sizeof(vs));

    read(fd, mem, mem_size);
    close(fd);

    /* Apply state */
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
 * Step 22: Live migration simulator — iterative pre-copy
 *
 * File format:
 *   [migrate_header]
 *   [full RAM]                              ← iteration 0
 *   [dirty_count + (page_idx, 4KB) × N]    ← iteration 1..N
 *   [dirty_count + (page_idx, 4KB) × N]    ← final (stop-and-copy)
 *   [CPU/device state — same as snapshot]   ← final state
 */

#define MIG_MAGIC   0x4D4B4D47  /* "MKMG" */
#define MIG_VERSION 1

#define MIGRATION_INTERVAL_MS       100
#define MIGRATION_MAX_ITERS         5
#define MIGRATION_THRESHOLD_PAGES   50

struct migrate_header {
    uint32_t magic;
    uint32_t version;
    uint64_t mem_size;
    uint32_t num_iterations;  /* number of dirty iterations (not counting iter 0) */
    uint32_t pad;
};

/* Write dirty pages: [uint32_t count] [uint32_t page_idx, 4096 bytes] × count */
static int migrate_write_dirty(int fd, int vmfd, void *mem, size_t mem_size,
                               uint64_t *out_dirty_count) {
    size_t slot0_pages = 0xD0000 / 4096;
    size_t slot0_bitmap_sz = (slot0_pages + 63) / 64 * 8;
    uint64_t *bitmap0 = calloc(1, slot0_bitmap_sz);

    size_t slot1_pages = (mem_size - 0xD1000) / 4096;
    size_t slot1_bitmap_sz = (slot1_pages + 63) / 64 * 8;
    uint64_t *bitmap1 = calloc(1, slot1_bitmap_sz);

    struct kvm_dirty_log log0 = { .slot = 0, .dirty_bitmap = bitmap0 };
    struct kvm_dirty_log log1 = { .slot = 1, .dirty_bitmap = bitmap1 };

    ioctl(vmfd, KVM_GET_DIRTY_LOG, &log0);
    ioctl(vmfd, KVM_GET_DIRTY_LOG, &log1);

    /* Count total dirty pages */
    uint32_t dirty_count = 0;
    for (size_t i = 0; i < slot0_bitmap_sz / 8; i++)
        dirty_count += __builtin_popcountll(bitmap0[i]);
    for (size_t i = 0; i < slot1_bitmap_sz / 8; i++)
        dirty_count += __builtin_popcountll(bitmap1[i]);

    /* Write count */
    write(fd, &dirty_count, sizeof(dirty_count));

    /* Write slot 0 dirty pages (GPA 0x0 - 0xD0000) */
    for (size_t i = 0; i < slot0_pages; i++) {
        if (bitmap0[i / 64] & (1ULL << (i % 64))) {
            uint32_t page_idx = (uint32_t)i;  /* GPA = page_idx * 4096 */
            write(fd, &page_idx, sizeof(page_idx));
            write(fd, (char *)mem + i * 4096, 4096);
        }
    }

    /* Write slot 1 dirty pages (GPA 0xD1000 - end) */
    for (size_t i = 0; i < slot1_pages; i++) {
        if (bitmap1[i / 64] & (1ULL << (i % 64))) {
            /* page_idx in terms of full address space */
            uint32_t page_idx = (uint32_t)((0xD1000 / 4096) + i);
            write(fd, &page_idx, sizeof(page_idx));
            write(fd, (char *)mem + 0xD1000 + i * 4096, 4096);
        }
    }

    *out_dirty_count = dirty_count;
    free(bitmap0);
    free(bitmap1);
    return 0;
}

/* Read and apply dirty pages from migration file */
static int migrate_read_dirty(int fd, void *mem) {
    uint32_t dirty_count;
    if (read(fd, &dirty_count, sizeof(dirty_count)) != sizeof(dirty_count))
        return -1;
    for (uint32_t i = 0; i < dirty_count; i++) {
        uint32_t page_idx;
        read(fd, &page_idx, sizeof(page_idx));
        read(fd, (char *)mem + (uint64_t)page_idx * 4096, 4096);
    }
    return (int)dirty_count;
}

/*
 * migrate_save: called from main thread after vCPU has stopped.
 * The pre-copy iterations happen while vCPU is still running (called from stdin_thread),
 * so we split into two phases:
 *   Phase 1 (migrate_precopy): runs while VM is live, writes header + base + iterations
 *   Phase 2 (migrate_stop_and_copy): runs after vCPU stops, writes final dirty + state
 */

struct migrate_context {
    int fd;
    uint32_t num_iterations;
};

static int migrate_precopy(const char *path, int vmfd, void *mem, size_t mem_size,
                           struct migrate_context *ctx) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("migrate_precopy open"); return -1; }

    fprintf(stderr, "\n=== Live migration simulator ===\n");

    /* Write placeholder header (will update num_iterations later) */
    struct migrate_header hdr = {
        .magic = MIG_MAGIC,
        .version = MIG_VERSION,
        .mem_size = mem_size,
        .num_iterations = 0,
    };
    write(fd, &hdr, sizeof(hdr));

    /* Iteration 0: full RAM copy */
    /* First, get dirty log to reset the bitmap (so subsequent iterations are delta-only) */
    {
        size_t slot0_pages = 0xD0000 / 4096;
        size_t slot0_bitmap_sz = (slot0_pages + 63) / 64 * 8;
        uint64_t *bm0 = calloc(1, slot0_bitmap_sz);
        size_t slot1_pages = (mem_size - 0xD1000) / 4096;
        size_t slot1_bitmap_sz = (slot1_pages + 63) / 64 * 8;
        uint64_t *bm1 = calloc(1, slot1_bitmap_sz);
        struct kvm_dirty_log dl0 = { .slot = 0, .dirty_bitmap = bm0 };
        struct kvm_dirty_log dl1 = { .slot = 1, .dirty_bitmap = bm1 };
        ioctl(vmfd, KVM_GET_DIRTY_LOG, &dl0);
        ioctl(vmfd, KVM_GET_DIRTY_LOG, &dl1);
        free(bm0);
        free(bm1);
    }

    uint32_t total_pages = (uint32_t)(mem_size / 4096);
    write(fd, mem, mem_size);
    fprintf(stderr, "Iteration 0: full RAM copy %u pages\n", total_pages);

    /* Iterative pre-copy (VM still running) */
    uint32_t iter = 0;
    for (iter = 0; iter < MIGRATION_MAX_ITERS; iter++) {
        usleep(MIGRATION_INTERVAL_MS * 1000);

        uint64_t dirty_count = 0;
        migrate_write_dirty(fd, vmfd, mem, mem_size, &dirty_count);
        fprintf(stderr, "Iteration %u: %llu dirty pages\n",
                iter + 1, (unsigned long long)dirty_count);

        if (dirty_count <= MIGRATION_THRESHOLD_PAGES)
            break;
    }

    ctx->fd = fd;
    ctx->num_iterations = iter + 1;  /* number of dirty iterations written */
    return 0;
}

static int migrate_stop_and_copy(struct migrate_context *ctx, int vcpufd, int vmfd,
                                 struct uart8250 *uart, struct virtio_mmio_dev *virtio,
                                 void *mem, size_t mem_size) {
    int fd = ctx->fd;
    uint64_t t1 = now_ns();

    /* Final dirty pages (after vCPU stopped) */
    uint64_t final_dirty = 0;
    migrate_write_dirty(fd, vmfd, mem, mem_size, &final_dirty);

    /* Write CPU/device state (same format as snapshot, minus RAM) */
    struct kvm_regs regs;
    ioctl(vcpufd, KVM_GET_REGS, &regs);
    write(fd, &regs, sizeof(regs));

    struct kvm_sregs sregs;
    ioctl(vcpufd, KVM_GET_SREGS, &sregs);
    write(fd, &sregs, sizeof(sregs));

    struct kvm_fpu fpu;
    ioctl(vcpufd, KVM_GET_FPU, &fpu);
    write(fd, &fpu, sizeof(fpu));

    struct kvm_lapic_state lapic;
    ioctl(vcpufd, KVM_GET_LAPIC, &lapic);
    write(fd, &lapic, sizeof(lapic));

    struct kvm_xcrs xcrs;
    ioctl(vcpufd, KVM_GET_XCRS, &xcrs);
    write(fd, &xcrs, sizeof(xcrs));

    struct kvm_vcpu_events events;
    ioctl(vcpufd, KVM_GET_VCPU_EVENTS, &events);
    write(fd, &events, sizeof(events));

    struct kvm_pit_state2 pit_state;
    ioctl(vmfd, KVM_GET_PIT2, &pit_state);
    write(fd, &pit_state, sizeof(pit_state));

    struct kvm_irqchip chips[3];
    for (int c = 0; c < 3; c++) {
        chips[c].chip_id = c;
        ioctl(vmfd, KVM_GET_IRQCHIP, &chips[c]);
        write(fd, &chips[c], sizeof(chips[c]));
    }

    struct kvm_clock_data clock;
    ioctl(vmfd, KVM_GET_CLOCK, &clock);
    write(fd, &clock, sizeof(clock));

    struct {
        struct kvm_msrs header;
        struct kvm_msr_entry entries[SNAP_NUM_MSRS];
    } msrs;
    msrs.header.nmsrs = SNAP_NUM_MSRS;
    for (int i = 0; i < SNAP_NUM_MSRS; i++)
        msrs.entries[i].index = snap_msr_list[i];
    ioctl(vcpufd, KVM_GET_MSRS, &msrs);
    write(fd, &msrs.header.nmsrs, sizeof(uint32_t));
    write(fd, msrs.entries, sizeof(struct kvm_msr_entry) * SNAP_NUM_MSRS);

    write(fd, uart, sizeof(*uart));

    struct virtio_snap vs = {
        .status = virtio->status,
        .host_features_sel = virtio->host_features_sel,
        .guest_features = virtio->guest_features,
        .guest_page_size = virtio->guest_page_size,
        .queue_sel = virtio->queue_sel,
        .interrupt_status = virtio->interrupt_status,
    };
    memcpy(vs.vqs, virtio->vqs, sizeof(vs.vqs));
    write(fd, &vs, sizeof(vs));

    uint64_t t2 = now_ns();
    double downtime_ms = (double)(t2 - t1) / 1e6;

    /* Update header with final iteration count */
    struct migrate_header hdr = {
        .magic = MIG_MAGIC,
        .version = MIG_VERSION,
        .mem_size = mem_size,
        .num_iterations = ctx->num_iterations,
    };
    lseek(fd, 0, SEEK_SET);
    write(fd, &hdr, sizeof(hdr));

    close(fd);

    fprintf(stderr, "Stop-and-copy: %llu dirty pages\n", (unsigned long long)final_dirty);
    fprintf(stderr, "Downtime: %.1f ms\n", downtime_ms);
    fprintf(stderr, "Migration complete: migration.bin\n");
    fprintf(stderr, "================================\n");
    return 0;
}

static int migrate_restore(const char *path, int vcpufd, int vmfd,
                           struct uart8250 *uart, struct virtio_mmio_dev *virtio,
                           void *mem, size_t mem_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("migrate_restore open"); return -1; }

    struct migrate_header hdr;
    read(fd, &hdr, sizeof(hdr));
    if (hdr.magic != MIG_MAGIC || hdr.version != MIG_VERSION) {
        fprintf(stderr, "Invalid migration file\n");
        close(fd);
        return -1;
    }
    if (hdr.mem_size != mem_size) {
        fprintf(stderr, "Memory size mismatch: file=%llu, vm=%zu\n",
                (unsigned long long)hdr.mem_size, mem_size);
        close(fd);
        return -1;
    }

    fprintf(stderr, "[migration] restoring from %s\n", path);

    /* 1. Base RAM (iteration 0) */
    read(fd, mem, mem_size);
    fprintf(stderr, "[migration] base RAM loaded (%zu MB)\n", mem_size >> 20);

    /* 2. Apply dirty iterations */
    for (uint32_t i = 0; i < hdr.num_iterations; i++) {
        int dirty = migrate_read_dirty(fd, mem);
        fprintf(stderr, "[migration] iteration %u: applied %d dirty pages\n", i + 1, dirty);
    }

    /* 3. Final dirty pages (stop-and-copy) */
    int final_dirty = migrate_read_dirty(fd, mem);
    fprintf(stderr, "[migration] final: applied %d dirty pages\n", final_dirty);

    /* 4. Restore CPU/device state (same apply order as snap_restore) */
    struct kvm_regs regs;
    read(fd, &regs, sizeof(regs));

    struct kvm_sregs sregs;
    read(fd, &sregs, sizeof(sregs));

    struct kvm_fpu fpu;
    read(fd, &fpu, sizeof(fpu));

    struct kvm_lapic_state lapic;
    read(fd, &lapic, sizeof(lapic));

    struct kvm_xcrs xcrs;
    read(fd, &xcrs, sizeof(xcrs));

    struct kvm_vcpu_events events;
    read(fd, &events, sizeof(events));

    struct kvm_pit_state2 pit_state;
    read(fd, &pit_state, sizeof(pit_state));

    struct kvm_irqchip chips[3];
    for (int c = 0; c < 3; c++)
        read(fd, &chips[c], sizeof(chips[c]));

    struct kvm_clock_data clock;
    read(fd, &clock, sizeof(clock));

    uint32_t nmsrs;
    read(fd, &nmsrs, sizeof(nmsrs));
    struct {
        struct kvm_msrs header;
        struct kvm_msr_entry entries[SNAP_NUM_MSRS];
    } msrs;
    msrs.header.nmsrs = nmsrs;
    read(fd, msrs.entries, sizeof(struct kvm_msr_entry) * nmsrs);

    read(fd, uart, sizeof(*uart));

    struct virtio_snap vs;
    read(fd, &vs, sizeof(vs));
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
}

#endif /* SNAPSHOT_H */