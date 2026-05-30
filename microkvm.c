#include <stdio.h>      /* printf, perror */
#include <string.h>     /* memset, memcpy */
#include <stdint.h>     /* uint64_t */
#include <fcntl.h>      /* open, O_RDWR */
#include <unistd.h>     /* close */
#include <sys/ioctl.h>  /* ioctl */
#include <sys/mman.h>   /* mmap, munmap */
#include <sys/stat.h>   /* fstat */
#include <linux/kvm.h>  /* KVM_* constants */

#define GUEST_MEM_SIZE (1 << 20)    /* 1 MB */
#define IO_PORT 0x10

static int load_guest(const char *path, void *mem) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return -1;
    }

    struct stat st;
    fstat(fd, &st);
    read(fd, mem, st.st_size);
    close(fd);
    printf("Loaded guest: %ld bytes\n", st.st_size);
    return 0;
}

static uint8_t device_counter = 0;

int main(void) {
    int kvmfd, vmfd, vcpufd;
    struct kvm_sregs    sregs;
    struct kvm_regs     regs;
    struct kvm_run      *run;
    int    mmap_size;
    void   *mem;
    int    irq_injected = 0;

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

    /* Register memory with KVM - split into two regions, leaving MMIO hole */
    struct kvm_userspace_memory_region region1 = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = 0xD0000,
        .userspace_addr = (unsigned long)mem,
    };
    if (ioctl(vmfd,KVM_SET_USER_MEMORY_REGION, &region1) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION slot 1");
        return 1;
    }

    struct kvm_userspace_memory_region region2 = {
        .slot = 1,
        .guest_phys_addr = 0xD1000,
        .memory_size = GUEST_MEM_SIZE - 0xD1000,
        .userspace_addr = (unsigned long)mem + 0xD1000,
    };
    if (ioctl(vmfd,KVM_SET_USER_MEMORY_REGION, &region2) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION slot 2");
        return 1;
    }

    /* Setup page tables for long mode (identity map, first 2 MB) */
    uint64_t *pml4 = (uint64_t *)((char *)mem + 0x70000);
    uint64_t *pdpt = (uint64_t *)((char *)mem + 0x71000);
    uint64_t *pd   = (uint64_t *)((char *)mem + 0x72000);

    pml4[0] = 0x71000 | 0x3;
    pdpt[0] = 0x72000 | 0x3;
    pd[0]   = 0x0     | 0x83;

    /* Load guest binary */
    if (load_guest("guest.bin", mem) < 0) {
        return 1;
    }

    /* Create vCPU */
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

    /* Initialization registers */
    /* Special registers: set CS to point to address 0 */
    if (ioctl(vcpufd, KVM_GET_SREGS, &sregs) < 0) {
        perror("KVM_GET_SREGS");
        return 1;
    }
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    sregs.efer |= (1 << 8);
    if (ioctl(vcpufd, KVM_SET_SREGS, &sregs) < 0) {
        perror("KVM_SET_SREGS");
        return 1;
    }

    /* General registers: set IP to 0 (start of guest mode) */
    memset(&regs, 0, sizeof(regs));
    regs.rip = 0;
    regs.rflags = 0x2;  /* bit 1 is always on x86 */
    if (ioctl(vcpufd, KVM_SET_REGS, &regs) < 0) {
        perror("KVM_SET_REGS");
        return 1;
    }

    /* Run the guest code */
    printf("Starting guest...\n");
    for (;;) {
        if (ioctl(vcpufd, KVM_RUN, NULL) < 0) {
            perror("KVM_RUN");
            return 1;
        }
        switch (run->exit_reason) {
            case KVM_EXIT_IO:
                if (run->io.port == IO_PORT && run->io.direction == KVM_EXIT_IO_OUT) {
                    char c = *(char *)((char *)run + run->io.data_offset);
                    if (c != '\n')
                        printf("[PIO out port 0x%x] %c\n", run->io.port, c);
                }
                break;
            case KVM_EXIT_MMIO:
                if (run->mmio.phys_addr == 0xD0000) {
                    if (run->mmio.is_write) {
                        char c = run->mmio.data[0];
                        if (c != '\n')
                            printf("[MMIO write @ 0x%llx] %c\n", run->mmio.phys_addr, c);
                    } else {
                        run->mmio.data[0] = device_counter++;
                        printf("[MMIO read  @ 0x%llx] returning %d\n",
                            run->mmio.phys_addr, device_counter - 1);
                    }
                }
                break;
            case KVM_EXIT_HLT:
                if (!irq_injected) {
                    struct kvm_interrupt irq = {
                        .irq = 32
                    };
                    if (ioctl(vcpufd, KVM_INTERRUPT, &irq) < 0) {
                        perror("KVM_INTERRUPT");
                        return 1;
                    }
                    irq_injected = 1;
                } else {
                    printf("Guest halted.\n");
                    goto done;
                }
                break;
            default:
                fprintf(stderr, "Unexpected exit reason: %d\n", run->exit_reason);
                break;
        }
    }

done:
    /* Cleanup */
    close(vcpufd);
    close(vmfd);
    close(kvmfd);
    munmap(run, mmap_size);
    munmap(mem, GUEST_MEM_SIZE);
    return 0;
}