#include <stdio.h>          /* printf, perror */
#include <string.h>         /* memset, memcpy */
#include <stdint.h>         /* uint64_t */
#include <fcntl.h>          /* open */
#include <unistd.h>         /* close */
#include <pthread.h>        /* threads */
#include <sys/ioctl.h>      /* ioctl */
#include <sys/mman.h>       /* mmap, munmap */
#include <linux/kvm.h>      /* KVM_* constants */
#include "microkvm.h"
#include "boot.h"

/* Simple MMIO device state: increments on write, returns value on read */
static uint8_t device_counter = 0;

/* Storage for synthetic MSR (wrmsr writes, rdmsr reads back) */
static uint64_t msr_store = 0;

/* Protects shared device state and stdout from concurrent vCPU access */
static pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;

struct vcpu {
    int fd;
    int id;
    struct kvm_run *run;
    size_t mmap_size;
};

static void *vcpu_thread(void *arg) {
    struct vcpu     *vcpu = arg;
    struct kvm_run  *run = vcpu->run;
    int irq_injected = 0;

    for (;;) {
        if (ioctl(vcpu->fd, KVM_RUN, NULL) < 0) {
            perror("KVM_RUN");
            return NULL;
        }
        switch (run->exit_reason) {
        case KVM_EXIT_HLT:
            if (vcpu->id == 0 && !irq_injected) {
                struct kvm_interrupt irq = {
                    .irq = 32
                };
                if (ioctl(vcpu->fd, KVM_INTERRUPT, &irq) < 0) {
                    perror("KVM_INTERRUPT");
                    return NULL;
                }
                irq_injected = 1;
            } else {
                pthread_mutex_lock(&dev_lock);
                printf("[vCPU %d] halted.\n", vcpu->id);
                pthread_mutex_unlock(&dev_lock);
                return NULL;
            }
            break;
        case KVM_EXIT_IO:
            if (run->io.port == PIO_PORT && run->io.direction == KVM_EXIT_IO_OUT) {
                char c = *(char *)((char *)run + run->io.data_offset);
                if (c != '\n') {
                    pthread_mutex_lock(&dev_lock);
                    printf("[vCPU %d][PIO out port 0x%x] %c\n", vcpu->id, run->io.port, c);
                    pthread_mutex_unlock(&dev_lock);
                }
            }
            break;
        case KVM_EXIT_MMIO:
            if (run->mmio.phys_addr == 0xD0000) {
                pthread_mutex_lock(&dev_lock);
                if (run->mmio.is_write) {
                    char c = run->mmio.data[0];
                    device_counter++;
                    if (c != '\n')
                        printf("[vCPU %d][MMIO write @ 0x%llx] %c\n", vcpu->id, run->mmio.phys_addr, c);
                } else {
                    run->mmio.data[0] = device_counter;
                    printf("[vCPU %d][MMIO read  @ 0x%llx] returning %d\n",
                        vcpu->id, run->mmio.phys_addr, device_counter);
                }
                pthread_mutex_unlock(&dev_lock);
            }
            break;
        case KVM_EXIT_X86_WRMSR:
            if (run->msr.index == MSR_CUSTOM) {
                pthread_mutex_lock(&dev_lock);
                msr_store = run->msr.data;
                printf("[vCPU %d][MSR write] 0x%x = 0x%llx\n",
                    vcpu->id, run->msr.index, (unsigned long long)run->msr.data);
                pthread_mutex_unlock(&dev_lock);
                run->msr.error = 0;
            } else {
                run->msr.error = 1;     /* inject #GP for unknown MSR */
            }
            break;
        case KVM_EXIT_X86_RDMSR:
            if (run->msr.index == MSR_CUSTOM) {
                pthread_mutex_lock(&dev_lock);
                run->msr.data = msr_store;
                printf("[vCPU %d][MSR read] 0x%x -> 0x%llx\n",
                    vcpu->id, run->msr.index, (unsigned long long)run->msr.data);
                pthread_mutex_unlock(&dev_lock);
                run->msr.error = 0;
            } else {
                run->msr.error = 1;     /* inject #GP for unknown MSR */
            }
            break;
        default:
            fprintf(stderr, "[vCPU %d] Unexpected exit reason: %d\n",
                vcpu->id, run->exit_reason);
            return NULL;
        }
    }
}

int main(void) {
    int     kvmfd, vmfd;
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

    /* Enable userspace MSR handling */
    struct kvm_enable_cap msr_cap = {
        .cap = KVM_CAP_X86_USER_SPACE_MSR,
        .args[0] = KVM_MSR_EXIT_REASON_FILTER,
    };
    if (ioctl(vmfd, KVM_ENABLE_CAP, &msr_cap) < 0) {
        perror("KVM_CAP_X86_USER_SPACE_MSR");
        return 1;
    }

    /* Setup MSR filter - trap custom MSR to userspace */
    uint8_t msr_bitmap[] = {0x00};  /* bit=0: deny -> trap to userspace */
    struct kvm_msr_filter filter = {
        .flags = KVM_MSR_FILTER_DEFAULT_ALLOW,
        .ranges = {{
            .flags = KVM_MSR_FILTER_READ | KVM_MSR_FILTER_WRITE,
            .nmsrs = 1,
            .base = MSR_CUSTOM,
            .bitmap = msr_bitmap,
        }},
    };
    if (ioctl(vmfd, KVM_X86_SET_MSR_FILTER, &filter) < 0) {
        perror("KVM_X86_SET_MSR_FILTER");
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
    if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region1) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION slot 1");
        return 1;
    }

    struct kvm_userspace_memory_region region2 = {
        .slot = 1,
        .guest_phys_addr = 0xD1000,
        .memory_size = GUEST_MEM_SIZE - 0xD1000,
        .userspace_addr = (unsigned long)mem + 0xD1000,
    };
    if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region2) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION slot 2");
        return 1;
    }

    /* Setup page tables for long mode (identity map, first 2 MB) */
    uint64_t *pml4 = (uint64_t *)((char *)mem + 0x70000);
    uint64_t *pdpt = (uint64_t *)((char *)mem + 0x71000);
    uint64_t *pd   = (uint64_t *)((char *)mem + 0x72000);

    pml4[0] = 0x71000 | 0x3;    /* present + writable, points to PDPT */
    pdpt[0] = 0x72000 | 0x3;    /* present + writable, points to PD */
    pd[0]   = 0x0     | 0x83;   /* present + writable + huge (2MB page at 0x0) */

    /* Load guest binary */
    if (load_guest("guest.bin", mem) < 0) {
        return 1;
    }

    /* Get vCPU mmap size */
    mmap_size = ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, NULL);

    /* Create vCPU and initialize */
    struct vcpu     vcpus[NUM_VCPUS];
    pthread_t       threads[NUM_VCPUS];

    for (int i = 0; i < NUM_VCPUS; i++) {
        vcpus[i].id = i;
        vcpus[i].mmap_size = mmap_size;

        vcpus[i].fd = ioctl(vmfd, KVM_CREATE_VCPU, i);
        if (vcpus[i].fd < 0) {
            perror("KVM_CREATE_VCPU");
            return 1;
        }
        vcpus[i].run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
            MAP_SHARED, vcpus[i].fd, 0);
        if (vcpus[i].run == MAP_FAILED) {
            perror("mmap vcpu");
            return 1;
        }

        /* Initialize registers */
        struct kvm_sregs    sregs;
        struct kvm_regs     regs;

        if (ioctl(vcpus[i].fd, KVM_GET_SREGS, &sregs) < 0) {
            perror("KVM_GET_SREGS");
            return 1;
        }

        if (i == 0) {
            /* vCPU 0: starts in real mode at RIP=0 (goes through mode transitions) */
            sregs.cs.base = 0;
            sregs.cs.selector = 0;
            sregs.efer |= (1 << 8);             /* LME - activates when guest enables paging */

            memset(&regs, 0, sizeof(regs));
            regs.rip = 0;
            regs.rflags = 0x2;                  /* bit 1 always set on x86 */
        } else {
            /* vCPU 1: starts directly in long mode at vcpu1_entry.
             * These register values replicate the state that vCPU 0 reaches
             * after executing mode transitions in guest.S (step 3-4). */

            /* Control registers: enable paging + protected mode + PAE */
            sregs.cr0 = 0x80000011;             /* PG | PE | ET */
            sregs.cr3 = 0x70000;                /* page table (shared with vCPU 0) */
            sregs.cr4 = 0x20;                   /* PAE */
            sregs.efer = (1 << 8) | (1 << 10);  /* LME | LMA */

            /* Code segment: 64-bit mode (equivalent to far jump to GDT[3]) */
            sregs.cs.base = 0;
            sregs.cs.selector = 0x18;           /* GDT[3]: 64-bit code */
            sregs.cs.type = 11;                 /* execute/read, accessed */
            sregs.cs.present = 1;
            sregs.cs.s = 1;                     /* code/data segment */
            sregs.cs.l = 1;                     /* long mode */
            sregs.cs.g = 0;

            /* Data/stack segments (equivalent to mov $0x20, %ax; mov %ax, %ds) */
            sregs.ds.base = 0;
            sregs.ds.selector = 0x20;           /* GDT[4]: 64-bit data */
            sregs.ds.type = 3;                  /* read/write, accessed */
            sregs.ds.present = 1;
            sregs.ds.s = 1;
            sregs.ss = sregs.ds;

            memset(&regs, 0, sizeof(regs));
            regs.rip = VCPU1_ENTRY;
            regs.rsp = 0x50000;                 /* separate stack from vCPU 0 */
            regs.rflags = 0x2;
        }

        if (ioctl(vcpus[i].fd, KVM_SET_SREGS, &sregs) < 0) {
            perror("KVM_SET_SREGS");
            return 1;
        }
        if (ioctl(vcpus[i].fd, KVM_SET_REGS, &regs) < 0) {
            perror("KVM_SET_REGS");
            return 1;
        }
    }

    /* Run the guest code */
    printf("Starting guest...\n");
    for (int i = 0; i < NUM_VCPUS; i++) {
        pthread_create(&threads[i], NULL, vcpu_thread, &vcpus[i]);
    }
    for (int i = 0; i < NUM_VCPUS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Cleanup */
    for (int i = 0; i < NUM_VCPUS; i++) {
        munmap(vcpus[i].run, mmap_size);
        close(vcpus[i].fd);
    }
    close(vmfd);
    close(kvmfd);
    munmap(mem, GUEST_MEM_SIZE);
    return 0;
}