#include <stdio.h>          /* printf, perror */
#include <string.h>         /* memset, memcpy */
#include <stdlib.h>         /* atexit, malloc, free */
#include <stdint.h>         /* uint64_t */
#include <fcntl.h>          /* open */
#include <unistd.h>         /* close */
#include <pthread.h>        /* threads */
#include <termios.h>        /* tcsetattr, raw terminal mode */
#include <sys/ioctl.h>      /* ioctl */
#include <sys/mman.h>       /* mmap, munmap */
#include <linux/kvm.h>      /* KVM_* constants */
#include "microkvm.h"
#include "boot.h"
#include "uart.h"
#include "virtio_mmio.h"

#define CMDLINE "console=ttyS0 earlyprintk=serial rdinit=/init virtio_mmio.device=0x200@0xd0000000:5"

/* Device state */
static uint64_t msr_store = 0;
static int g_vmfd;

/* UART */
static struct uart8250 uart;

/* Virtio-mmio device */
static struct virtio_mmio_dev virtio_dev;

/* Terminal and stdin handling */
static struct termios orig_termios;

static void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

static void set_raw_terminal(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_terminal);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/* Read host stdin and deliver to guest via UART RX */
static void *stdin_thread(void *arg) {
    (void)arg;
    uint8_t c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == 0x1b) {
            while (read(STDIN_FILENO, &c, 1) == 1) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                    break;
            }
            continue;
        }
        uart_rx(&uart, c, g_vmfd);
    }
    return NULL;
}

/* Per-vCPU state */
struct vcpu {
    int fd;
    int id;
    struct kvm_run *run;
    size_t mmap_size;
};

static void *vcpu_thread(void *arg) {
    struct vcpu     *vcpu = arg;
    struct kvm_run  *run = vcpu->run;

    for (;;) {
        if (ioctl(vcpu->fd, KVM_RUN, NULL) < 0) {
            perror("KVM_RUN");
            return NULL;
        }
        switch (run->exit_reason) {
        case KVM_EXIT_HLT:
            break;
        case KVM_EXIT_IO: {
            uint16_t port = run->io.port;
            uint8_t *data = (uint8_t *)((char *)run + run->io.data_offset);
            if (port >= UART_BASE && port <= UART_BASE + 7) {
                if (run->io.direction == KVM_EXIT_IO_OUT)
                    uart_out(&uart, port, *data, g_vmfd);
                else
                    *data = uart_in(&uart, port);
            } else if (run->io.direction == KVM_EXIT_IO_IN) {
                *data = 0;
            }
            break;
        }
        case KVM_EXIT_MMIO: {
            uint64_t addr = run->mmio.phys_addr;
            if (addr >= VIRTIO_MMIO_BASE && addr < VIRTIO_MMIO_BASE + VIRTIO_MMIO_SIZE) {
                uint64_t offset = addr - VIRTIO_MMIO_BASE;
                if (run->mmio.is_write) {
                    uint32_t val = 0;
                    memcpy(&val, run->mmio.data, run->mmio.len);
                    virtio_mmio_write(&virtio_dev, offset, val, run->mmio.len);
                } else {
                    uint32_t val = virtio_mmio_read(&virtio_dev, offset, run->mmio.len);
                    memcpy(run->mmio.data, &val, run->mmio.len);
                }
            }
            break;
        }
        case KVM_EXIT_X86_WRMSR:
            if (run->msr.index == MSR_CUSTOM) {
                msr_store = run->msr.data;
                run->msr.error = 0;
            } else {
                run->msr.error = 1;
            }
            break;
        case KVM_EXIT_X86_RDMSR:
            if (run->msr.index == MSR_CUSTOM) {
                run->msr.data = msr_store;
                run->msr.error = 0;
            } else {
                run->msr.error = 1;
            }
            break;
        default:
            fprintf(stderr, "Unexpected exit reason: %d\n", run->exit_reason);
            return NULL;
        }
    }
}

int main(void) {

    /* Disable stdout buffering so kernel output appears immediately */
    setbuf(stdout, NULL);
    set_raw_terminal();

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
    g_vmfd = vmfd;

    /* Create UART */
    uart_init(&uart);

    /* Initialize virtio-mmio device */
    virtio_mmio_init(&virtio_dev);

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

    /* Create in-kernel interrupt controller (PIC/IOAPIC/LAPIC) and timer (PIT).
     * This allows KVM to deliver IRQs without exiting to userspace. */
    if (ioctl(vmfd, KVM_CREATE_IRQCHIP, 0) < 0) {
        perror("KVM_CREATE_IRQCHIP");
        return 1;
    }

    struct kvm_pit_config pit = {
        .flags = 0,
    };
    if (ioctl(vmfd, KVM_CREATE_PIT2, &pit) < 0) {
        perror("KVM_CREATE_PIT2");
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

    /* Load bzImage*/
    if (load_bzimage("bzImage", mem, CMDLINE) < 0) {
        return 1;
    }

    /* Load initramfs (required for userspace) */
    uint32_t initrd_size;
    if (load_initramfs("initramfs.gz", mem, &initrd_size) < 0)
        fprintf(stderr, "Warning: no initramfs.gz found\n");

    /* Give virtio device access to guest memory */
    virtio_dev.ram = (uint8_t *)mem;
    virtio_dev.ram_size = GUEST_MEM_SIZE;

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

        /* Set CPUID — kernel's verify_cpu checks for long mode support.
         * Get host-supported CPUID and pass it to the guest vCPU. */
        struct {
            struct kvm_cpuid2 header;
            struct kvm_cpuid_entry2 entries[100];
        } cpuid;
        cpuid.header.nent = 100;

        if (ioctl(kvmfd, KVM_GET_SUPPORTED_CPUID, &cpuid) < 0) {
            perror("KVM_GET_SUPPORTED_CPUID");
            return 1;
        }

        /* Hide TSC-Deadline timer — our minimal PIT/LAPIC doesn't support it */
        for (int j = 0; j < (int)cpuid.header.nent; j++) {
            if (cpuid.entries[j].function == 1) {
                cpuid.entries[j].ecx &= ~(1 << 24);
                break;
            }
        }

        if (ioctl(vcpus[i].fd, KVM_SET_CPUID2, &cpuid) < 0) {
            perror("KVM_SET_CPUID2");
            return 1;
        }

        /* Set TSC frequency (1GHz) — needed for LAPIC timer calibration.
         * Non-fatal: kernel can calibrate TSC itself if this fails. */
        if (ioctl(vcpus[i].fd, KVM_SET_TSC_KHZ, 1000000UL) < 0) {
            perror("KVM_SET_TSC_KHZ");
        }

        /* mmap kvm_run: shared page for exit information */
        vcpus[i].run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
            MAP_SHARED, vcpus[i].fd, 0);
        if (vcpus[i].run == MAP_FAILED) {
            perror("mmap vcpu");
            return 1;
        }

        /* Initialize registers: 32-bit protected mode (Linux Boot Protocol)
         * Kernel's startup_32 expects flat segments with full 4GB access */
        struct kvm_sregs    sregs;
        struct kvm_regs     regs;

        if (ioctl(vcpus[i].fd, KVM_GET_SREGS, &sregs) < 0) {
            perror("KVM_GET_SREGS");
            return 1;
        }

        /* Control registers: protected mode, no paging (kernel enables it) */
        sregs.cr0 = 0x11;               /* PE | ET */

        /* Code segment: 32-bit, flat, execute/read */
        sregs.cs.base = 0;
        sregs.cs.selector = 0x10;
        sregs.cs.type = 11;             /* execute/read, accessed */
        sregs.cs.present = 1;
        sregs.cs.s = 1;
        sregs.cs.db = 1;                /* 32-bit mode */
        sregs.cs.g = 1;                 /* 4KB granularity */
        sregs.cs.limit = 0xFFFFFFFF;    /* 4GB flat */

        /* Data segments: 32-bit, flat, read/write (all identical) */
        sregs.ds.base = 0;
        sregs.ds.selector = 0x18;
        sregs.ds.type = 3;              /* read/write, accessed */
        sregs.ds.present = 1;
        sregs.ds.s = 1;
        sregs.ds.db = 1;
        sregs.ds.g = 1;
        sregs.ds.limit = 0xFFFFFFFF;
        sregs.es = sregs.ds;
        sregs.fs = sregs.ds;
        sregs.gs = sregs.ds;
        sregs.ss = sregs.ds;

        memset(&regs, 0, sizeof(regs));
        regs.rip = KERNEL_ADDR;         /* kernel entry point (startup_32) */
        regs.rsi = BOOT_PARAMS_ADDR;    /* boot_params pointer passed in %esi */
        regs.rflags = 0x2;

        if (ioctl(vcpus[i].fd, KVM_SET_SREGS, &sregs) < 0) {
            perror("KVM_SET_SREGS");
            return 1;
        }
        if (ioctl(vcpus[i].fd, KVM_SET_REGS, &regs) < 0) {
            perror("KVM_SET_REGS");
            return 1;
        }
    }

    /* Run */
    printf("Starting guest...\n");

    pthread_t stdin_tid;
    pthread_create(&stdin_tid, NULL, stdin_thread, NULL);

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