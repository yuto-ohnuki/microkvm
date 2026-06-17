#include <stdio.h>          /* printf, perror */
#include <string.h>         /* memset, memcpy */
#include <fcntl.h>          /* open */
#include <unistd.h>         /* close */
#include <sys/ioctl.h>      /* ioctl */
#include <sys/mman.h>       /* mmap, munmap */
#include <linux/kvm.h>      /* KVM_* constants */
#include "microkvm.h"

static const unsigned char guest_code[] = {
    0xf4    /* hlt */
};

int main(void) {
    int     kvmfd, vmfd, vcpufd;
    struct  kvm_sregs    sregs;
    struct  kvm_regs     regs;
    struct  kvm_run      *run;
    int     mmap_size;
    void    *mem;

    /* Open /dev/kvm */
    kvmfd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvmfd < 0) {
        perror("/dev/kvm");
        return 1;
    }

    /* Create VM */
    vmfd = ioctl(kvmfd, KVM_CREATE_VM, 0);
    if (vmfd < 0) {
        perror("KVM_CREATE_VM");
        return 1;
    }

    /* Allocate guest memory */
    mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap guest memory");
        return 1;
    }

    /* Register memory with KVM */
    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = GUEST_MEM_SIZE,
        .userspace_addr = (unsigned long)mem,
    };
    if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return 1;
    }

    /* Load guest binary */
    memcpy(mem, guest_code, sizeof(guest_code));

    /* Create vcpu */
    vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, 0);
    if (vcpufd < 0) {
        perror("KVM_CREATE_VCPU");
        return 1;
    }

    /* mmap the kvm_run structure */
    mmap_size = ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mmap_size < 0) {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        return 1;
    }
    run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
        MAP_SHARED, vcpufd, 0);
    if (run == MAP_FAILED) {
        perror("mmap vcpu");
        return 1;
    }

    /* Initialize registers */
    /* Special registers: set CS to point to address 0 */
    if (ioctl(vcpufd, KVM_GET_SREGS, &sregs) < 0) {
        perror("KVM_GET_SREGS");
        return 1;
    }
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    if (ioctl(vcpufd, KVM_SET_SREGS, &sregs) < 0) {
        perror("KVM_SET_SREGS");
        return 1;
    }

    /* General registers: set IP to 0 (start of guest mode) */
    memset(&regs, 0, sizeof(regs));
    regs.rip = 0;
    regs.rflags = 0x2;  /* bit 1 is always on x86 */
    if (ioctl(vcpufd, KVM_SET_REGS, &regs)) {
        perror("KVM_SET_REGS");
        return 1;
    }

    /* Run the guest code */
    printf("Starting guest...\n");
    if (ioctl(vcpufd, KVM_RUN, NULL) < 0) {
        perror("KVM_RUN");
        return 1;
    }

    printf("Exit reason: %d\n", run->exit_reason);
    switch (run->exit_reason) {
    case KVM_EXIT_HLT:
        printf("Guest executed HLT. Success!\n");
        break;
    default:
        fprintf(stderr, "Unexpected exit reason: %d\n", run->exit_reason);
        break;
    }

    /* Cleanup */
    close(vcpufd);
    close(vmfd);
    close(kvmfd);
    munmap(run, mmap_size);
    munmap(mem, GUEST_MEM_SIZE);
    return 0;
}