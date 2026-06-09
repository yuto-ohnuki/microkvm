#include <stdio.h>      /* printf, perror */
#include <string.h>     /* memset, memcpy */
#include <stdlib.h>     /* malloc, free */
#include <stdint.h>     /* uint64_t */
#include <fcntl.h>      /* open, O_RDWR */
#include <unistd.h>     /* close */
#include <pthread.h>    /* threads */
#include <termios.h>    /* tcsetattr, raw terminal mode */
#include <sys/ioctl.h>  /* ioctl */
#include <sys/mman.h>   /* mmap, munmap */
#include <sys/stat.h>   /* fstat */
#include <linux/kvm.h>  /* KVM_* constants */
#include "uart.h"
#include "virtio_mmio.h"

#define MSR_CUSTOM 0x4B564D00
#define GUEST_MEM_SIZE (128 << 20)    /* 128 MB */
#define NUM_VCPUS 1

#define BOOT_PARAMS_ADDR 0x7000
#define CMDLINE_ADDR     0x20000
#define KERNEL_ADDR      0x100000
#define CMDLINE          "console=ttyS0 earlyprintk=serial virtio_mmio.device=0x200@0xd0000000:5"

static int load_bzimage(const char *path, void *mem) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return -1;
    }

    struct stat st;
    fstat(fd, &st);

    void *bzimage = malloc(st.st_size);
    if (!bzimage) {
        perror("malloc");
        close(fd);
        return -1;
    }
    read(fd, bzimage, st.st_size);
    close(fd);

    /* Parse setup header */
    uint8_t *hdr = bzimage;
    uint8_t setup_sects = hdr[0x1F1];
    if (setup_sects == 0)
        setup_sects = 4;
    uint32_t setup_size = (setup_sects + 1) * 512;
    uint32_t kernel_size = st.st_size - setup_size;

    if (memcmp(hdr + 0x202, "HdrS", 4) != 0) {
        fprintf(stderr, "Not a valid bzImage (no HdrS magic)\n");
        free(bzimage);
        return -1;
    }

    uint16_t protocol = *(uint16_t *)(hdr + 0x206);
    printf("bzImage: protocol %d.%d, setup %d bytes, kernel %d bytes\n",
           protocol >> 8, protocol & 0xff, setup_size, kernel_size);

    /* Setup boot_params (zero page) */
    memset((char *)mem + BOOT_PARAMS_ADDR, 0, 4096);
    memcpy((char *)mem + BOOT_PARAMS_ADDR + 0x1F1, hdr + 0x1F1, setup_size - 0x1F1);

    /* Command line */
    strcpy((char *)mem + CMDLINE_ADDR, CMDLINE);
    *(uint32_t *)((char *)mem + BOOT_PARAMS_ADDR + 0x228) = CMDLINE_ADDR;

    /* type_of_loader (required, non-zero) */
    *((char *)mem + BOOT_PARAMS_ADDR + 0x210) = 0xFF;

    /* loadflags: LOADED_HIGH | CAN_USE_HEAP */
    *((char *)mem + BOOT_PARAMS_ADDR + 0x211) |= 0x01 | 0x80;

    /* Copy protected-mode kernel to 1MB */
    memcpy((char *)mem + KERNEL_ADDR, (char *)bzimage + setup_size, kernel_size);
    printf("Kernel loaded at 0x%x (%d bytes)\n", KERNEL_ADDR, kernel_size);

    /* e820 memory map (offset 0x2D0, each entry 20 bytes) */
    char *e820 = (char *)mem + BOOT_PARAMS_ADDR + 0x2D0;
    *(uint64_t *)(e820 + 0)  = 0;
    *(uint64_t *)(e820 + 8)  = 0x9FC00;
    *(uint32_t *)(e820 + 16) = 1;  /* E820_RAM */
    *(uint64_t *)(e820 + 20) = 0x100000;
    *(uint64_t *)(e820 + 28) = GUEST_MEM_SIZE - 0x100000;
    *(uint32_t *)(e820 + 36) = 1;  /* E820_RAM */
    *((char *)mem + BOOT_PARAMS_ADDR + 0x1E8) = 2;

    free(bzimage);
    return 0;
}

/* Device state */
static uint64_t msr_store = 0;
static int g_vmfd;

/* Devices */
static struct uart8250 uart;
static struct virtio_mmio_dev virtio_dev;

/* vCPU thread */
struct vcpu {
    int fd;
    struct kvm_run *run;
    int id;
    size_t mmap_size;
};

static void *vcpu_thread(void *arg) {
    struct vcpu *vcpu = arg;
    struct kvm_run *run = vcpu->run;

    for (;;) {
        if (ioctl(vcpu->fd, KVM_RUN, NULL) < 0) {
            perror("KVM_RUN");
            return NULL;
        }
        switch (run->exit_reason) {
            case KVM_EXIT_IO: {
                uint16_t port = run->io.port;
                uint8_t *data = (uint8_t *)((char *)run + run->io.data_offset);
                if (port >= 0x3F8 && port <= 0x3FF) {
                    if (run->io.direction == KVM_EXIT_IO_OUT)
                        uart_out(&uart, port, *data, g_vmfd);
                    else
                        *data = uart_in(&uart, port);
                } else if (run->io.direction == KVM_EXIT_IO_IN) {
                    *data = 0;
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
            case KVM_EXIT_HLT:
                break;
            case KVM_EXIT_MMIO: {
                uint64_t addr = run->mmio.phys_addr;
                if (addr >= VIRTIO_MMIO_BASE &&
                    addr < VIRTIO_MMIO_BASE + VIRTIO_MMIO_SIZE) {
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
            default:
                fprintf(stderr, "Unexpected exit reason: %d\n", run->exit_reason);
                return NULL;
        }
    }
}

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

int main(void) {
    setbuf(stdout, NULL);

    int kvmfd, vmfd;
    int    mmap_size;
    void   *mem;

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

    /* Initialize devices */
    uart_init(&uart);
    virtio_mmio_init(&virtio_dev);

    /* Enable userspace MSR handling */
    struct kvm_enable_cap msr_cap = {
        .cap = KVM_CAP_X86_USER_SPACE_MSR,
        .args[0] = KVM_MSR_EXIT_REASON_FILTER,
    };
    if (ioctl(vmfd, KVM_ENABLE_CAP, &msr_cap) < 0){
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

    /* Create in-kernel PIC + PIT */
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

    /* Load bzImage */
    if (load_bzimage("bzImage", mem) < 0) {
        return 1;
    }

    /* Load initramfs (required for userspace) */
    int fd = open("initramfs.gz", O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        fstat(fd, &st);
        uint32_t initrd_addr = 0x4000000;   /* 64MB */
        read(fd, (char *)mem + initrd_addr, st.st_size);
        close(fd);
        *(uint32_t *)((char *)mem + BOOT_PARAMS_ADDR + 0x218) = initrd_addr;
        *(uint32_t *)((char *)mem + BOOT_PARAMS_ADDR + 0x21C) = st.st_size;
        printf("initramfs loaded at 0x%x (%ld bytes)\n", initrd_addr, st.st_size);
    }

    /* Get vcpu mmap size */
    mmap_size = ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, NULL);

    /* Create vCPUs and initialize */
    struct vcpu vcpus[NUM_VCPUS];
    pthread_t threads[NUM_VCPUS];

    for (int i = 0; i < NUM_VCPUS; i++) {
        vcpus[i].id = i;
        vcpus[i].mmap_size = mmap_size;

        vcpus[i].fd = ioctl(vmfd, KVM_CREATE_VCPU, i);
        if (vcpus[i].fd < 0) {
            perror("KVM_CREATE_VCPU");
            return 1;
        }

        /* Set CPUID — kernel's verify_cpu checks for long mode support */
        struct {
            struct kvm_cpuid2 header;
            struct kvm_cpuid_entry2 entries[100];
        } cpuid;
        cpuid.header.nent = 100;
        if (ioctl(kvmfd, KVM_GET_SUPPORTED_CPUID, &cpuid) < 0) {
            perror("KVM_GET_SUPPORTED_CPUID");
            return 1;
        }
        for (int j = 0; j < (int)cpuid.header.nent; j++) {
            if (cpuid.entries[j].function == 1) {
                cpuid.entries[j].ecx &= ~(1 << 24);  /* hide TSC-Deadline */
                break;
            }
        }
        if (ioctl(vcpus[i].fd, KVM_SET_CPUID2, &cpuid) < 0) {
            perror("KVM_SET_CPUID2");
            return 1;
        }

        /* Set TSC frequency (1GHz) — needed for LAPIC timer calibration */
        ioctl(vcpus[i].fd, KVM_SET_TSC_KHZ, 1000000UL);

        /* mmap kvm_run: shared page for exit information */
        vcpus[i].run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
            MAP_SHARED, vcpus[i].fd, 0);
        if (vcpus[i].run == MAP_FAILED) {
            perror("mmap vcpu");
            return 1;
        }

        /* Initialize registers: 32-bit protected mode (Linux Boot Protocol)
         * Kernel's startup_32 expects flat segments with full 4GB access */
        struct kvm_sregs sregs;
        struct kvm_regs regs;

        if (ioctl(vcpus[i].fd, KVM_GET_SREGS, &sregs) < 0) {
            perror("KVM_GET_SREGS");
            return 1;
        }

        sregs.cr0 = 0x11;              /* PE | ET (no paging — kernel enables it) */

        /* Code segment: 32-bit, flat, execute/read */
        sregs.cs.base = 0;
        sregs.cs.selector = 0x10;
        sregs.cs.type = 11;            /* execute/read, accessed */
        sregs.cs.present = 1;
        sregs.cs.s = 1;
        sregs.cs.db = 1;               /* 32-bit mode */
        sregs.cs.g = 1;                /* 4KB granularity */
        sregs.cs.limit = 0xFFFFFFFF;   /* 4GB flat */

        /* Data segments: 32-bit, flat, read/write (all identical) */
        sregs.ds.base = 0;
        sregs.ds.selector = 0x18;
        sregs.ds.type = 3;             /* read/write, accessed */
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
        regs.rip = KERNEL_ADDR;        /* kernel entry point (startup_32) */
        regs.rsi = BOOT_PARAMS_ADDR;   /* boot_params pointer passed in %esi */
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
    set_raw_terminal();
    printf("Starting guest with %d vCPUs...\n", NUM_VCPUS);

    pthread_t stdin_tid;
    pthread_create(&stdin_tid, NULL, stdin_thread, NULL);

    for (int i = 0; i < NUM_VCPUS; i++) {
        pthread_create(&threads[i], NULL, vcpu_thread, &vcpus[i]);
    }
    for (int i = 0; i < NUM_VCPUS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Cleanup*/
    for (int i = 0; i < NUM_VCPUS; i++) {
        munmap(vcpus[i].run, mmap_size);
        close(vcpus[i].fd);
    }
    close(vmfd);
    close(kvmfd);
    munmap(mem, GUEST_MEM_SIZE);
    return 0;
}