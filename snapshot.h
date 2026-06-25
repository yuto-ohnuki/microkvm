#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stdint.h>
#include <linux/kvm.h>
#include "uart.h"
#include "virtio_mmio.h"

/*
 * VM Snapshot — save and restore full VM state to/from a file.
 *
 * A snapshot captures everything needed to resume execution:
 *   - CPU state: regs, sregs, FPU, LAPIC, XCRs, vcpu_events, MSRs
 *   - System state: PIT, IRQ chips (PIC master/slave + IOAPIC), clock
 *   - Device state: UART, virtio-mmio
 *   - Guest RAM (128MB)
 *
 * File format (sequential writes, read back in same order):
 *   [header] [regs] [sregs] [fpu] [lapic] [xcrs] [events]
 *   [pit] [irqchip×3] [clock] [msrs] [uart] [virtio] [RAM]
 *
 * Restore order of KVM ioctls is critical:
 *   PIT → clock → irqchip → XCRs → SREGS → MSRs → LAPIC → events → FPU → REGS
 *   (PIT must be first to overwrite default timer before it fires)
 */

#define SNAP_MAGIC   0x4D4B564D     /* "MKVM" */
#define SNAP_VERSION 1

/*
 * MSR indices to save/restore.
 * These MSRs define the guest's execution environment:
 *   - TSC: timestamp counter (guest time reference)
 *   - APICBASE: LAPIC base address and enable bit
 *   - SYSENTER_*: fast syscall entry points (32-bit)
 *   - STAR/LSTAR/CSTAR/FMASK: fast syscall entry points (64-bit)
 *   - KERNEL_GS_BASE: per-CPU data pointer for swapgs
 *   - KVM_WALL_CLOCK_NEW/SYSTEM_TIME_NEW: paravirt clock (kvmclock)
 */
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

/* File header: validates format and memory size on restore */
struct snap_header {
    uint32_t magic;
    uint32_t version;
    uint64_t mem_size;
};

/*
 * Virtio device state for snapshot - excludes host pointers (ram, ram_size).
 * On restore, ram/ram_size are set from the new process's mmap, not from file.
 */
struct virtio_snap {
    uint32_t status;
    uint32_t host_features_sel;
    uint32_t guest_features;
    uint32_t guest_page_size;
    uint32_t queue_sel;
    uint32_t interrupt_status;
    struct virtqueue_state vqs[VIRTQ_NUM_QUEUES];
};

int snap_save(const char *path, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio,
    void *mem, size_t mem_size);
int snap_restore(const char *path, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio,
    void *mem, size_t mem_size);

#endif /* SNAPSHOT_H */