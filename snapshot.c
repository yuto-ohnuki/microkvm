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
 * Save the VM state supported by this snapshot format to file.
 * Called from the vCPU thread when a snapshot is requested (Ctrl-A s);
 * the VM is not terminated and execution resumes afterward.
 *
 * This includes vCPU/KVM state, RAM, UART, and virtio-mmio state.
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
