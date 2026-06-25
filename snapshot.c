#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include "snapshot.h"

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
 * Save CPU and device state to an open fd (without header or RAM).
 * Shared by snap_save() and migrate_stop_and_copy().
 */
static void save_cpu_state(int fd, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio)
{
    /* vCPU state */
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

    /* VM-wide state */
    struct kvm_pit_state2 pit_state;
    ioctl(vmfd, KVM_GET_PIT2, &pit_state);
    write(fd, &pit_state, sizeof(pit_state));

    struct kvm_irqchip chips[3];
    for (int c = 0; c < 3; c++) {
        chips[c].chip_id = c;   /* 0=PIC master, 1=PIC slave, 2=IOAPIC */
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

    /* Device state */
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
}

/*
 * Save full VM state to file.
 * Called after vCPU has stopped (Ctrl-A s → stop_requested → join).
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
    write(fd, &hdr, sizeof(hdr));

    /* Save CPU + device state */
    save_cpu_state(fd, vcpufd, vmfd, uart, virtio);

    /* Guest RAM (128MB) */
    write(fd, mem, mem_size);

    close(fd);
    fprintf(stderr, "[snapshot] saved to %s\n", path);
    return 0;
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

    /* RAM must be loaded before MSRs (KVM_SYSTEM_TIME_NEW accesses guest RAM) */
    read(fd, mem, mem_size);
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