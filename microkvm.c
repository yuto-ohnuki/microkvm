#include <stdio.h>          /* printf, perror */
#include <string.h>         /* memset, memcpy */
#include <stdlib.h>         /* atexit, malloc, free */
#include <stdint.h>         /* uint64_t */
#include <fcntl.h>          /* open */
#include <unistd.h>         /* close */
#include <pthread.h>        /* threads */
#include <termios.h>        /* tcsetattr, raw terminal mode */
#include <signal.h>         /* signal handling */
#include <time.h>           /* clock_gettime for latency measurement */
#include <sys/ioctl.h>      /* ioctl */
#include <sys/mman.h>       /* mmap, munmap */
#include <sys/eventfd.h>    /* eventfd for ioeventfd/irqfd */
#include <linux/kvm.h>      /* KVM_* constants */
#include "microkvm.h"
#include "boot.h"
#include "uart.h"
#include "snapshot.h"
#include "kvm_stats.h"
#include "virtio_mmio.h"

#define CMDLINE "console=ttyS0 earlyprintk=serial rdinit=/init virtio_mmio.device=0x200@0xd0000000:5"

/* ===== Benchmark: exit counting and latency measurement =====
 *
 * Controlled by environment variables:
 *   USE_IOEVENTFD=1  → transmitq kick via ioeventfd (Step 17 style)
 *   USE_IRQFD=1      → IRQ5 injection via irqfd (Step 18 style)
 *
 * Without these, falls back to MMIO exit / ioctl style (Step 15-16).
 * Ctrl-C prints the exit & latency report and exits.
 */

/* Runtime flags */
static int use_ioeventfd = 0;   /* 0=Step15 style, 1=Step17 style */
static int use_irqfd = 0;       /* 0=Step16 style, 1=Step18 style */

static volatile sig_atomic_t stop_requested;
static void sigint_handler(int sig)
{
    (void)sig;
    stop_requested = 1;
}

/* Latency measurement */
struct latency_stats {
    uint64_t count;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
};

static struct latency_stats tx_lat;
static struct latency_stats irq_lat;

static void lat_init(struct latency_stats *s) {
    s->count = 0;
    s->total_ns = 0;
    s->min_ns = UINT64_MAX;
    s->max_ns = 0;
}

static inline void lat_record(struct latency_stats *s, uint64_t ns) {
    s->count++;
    s->total_ns += ns;
    if (ns < s->min_ns) s->min_ns = ns;
    if (ns > s->max_ns) s->max_ns = ns;
}

/* Exit stats counters */
static uint64_t mmio_exit_count = 0;
static uint64_t ioeventfd_kick_count = 0;
static uint64_t irqfd_write_count = 0;
static uint64_t ioctl_irq_count = 0;
static uint64_t queue_notify_rx_mmio_count = 0;
static uint64_t queue_notify_tx_mmio_count = 0;

/* Print exit counts and latency stats collected during the run.
 * Called on Ctrl-C (after vCPU thread exits). */
static void print_exit_stats(void) {
    fprintf(stderr, "\n==== microkvm exit & latency report ====\n");
    fprintf(stderr, "Mode: ioeventfd=%s, irqfd=%s\n",
        use_ioeventfd ? "ON" : "OFF",
        use_irqfd ? "ON" : "OFF");

    fprintf(stderr, "\n--- Exit counts ---\n");
    fprintf(stderr, "MMIO exits total:           %llu\n", (unsigned long long)mmio_exit_count);
    fprintf(stderr, "QueueNotify MMIO exits:\n");
    fprintf(stderr, "  RX queue 0:               %llu\n", (unsigned long long)queue_notify_rx_mmio_count);
    fprintf(stderr, "  TX queue 1:               %llu\n", (unsigned long long)queue_notify_tx_mmio_count);
    fprintf(stderr, "ioeventfd TX kicks:         %llu\n", (unsigned long long)ioeventfd_kick_count);
    fprintf(stderr, "IRQ inject (ioctl):         %llu\n", (unsigned long long)ioctl_irq_count);
    fprintf(stderr, "IRQ inject (irqfd):         %llu\n", (unsigned long long)irqfd_write_count);

    fprintf(stderr, "\n--- TX processing latency ---\n");
    if (tx_lat.count > 0) {
        fprintf(stderr, "  Method:   %s\n", use_ioeventfd ? "ioeventfd thread" : "MMIO exit handler");
        fprintf(stderr, "  Count:    %llu\n", (unsigned long long)tx_lat.count);
        fprintf(stderr, "  Avg:      %llu ns (%.2f us)\n",
            (unsigned long long)(tx_lat.total_ns / tx_lat.count),
            (double)(tx_lat.total_ns / tx_lat.count) / 1000.0);
        fprintf(stderr, "  Min:      %llu ns (%.2f us)\n",
            (unsigned long long)tx_lat.min_ns, (double)tx_lat.min_ns / 1000.0);
        fprintf(stderr, "  Max:      %llu ns (%.2f us)\n",
            (unsigned long long)tx_lat.max_ns, (double)tx_lat.max_ns / 1000.0);
    } else {
        fprintf(stderr, "  (no TX data)\n");
    }

    fprintf(stderr, "\n--- IRQ injection latency ---\n");
    if (irq_lat.count > 0) {
        fprintf(stderr, "  Method:   %s\n", use_irqfd ? "irqfd (write)" : "ioctl (KVM_IRQ_LINE x2)");
        fprintf(stderr, "  Count:    %llu\n", (unsigned long long)irq_lat.count);
        fprintf(stderr, "  Avg:      %llu ns (%.2f us)\n",
            (unsigned long long)(irq_lat.total_ns / irq_lat.count),
            (double)(irq_lat.total_ns / irq_lat.count) / 1000.0);
        fprintf(stderr, "  Min:      %llu ns (%.2f us)\n",
            (unsigned long long)irq_lat.min_ns, (double)irq_lat.min_ns / 1000.0);
        fprintf(stderr, "  Max:      %llu ns (%.2f us)\n",
            (unsigned long long)irq_lat.max_ns, (double)irq_lat.max_ns / 1000.0);
    } else {
        fprintf(stderr, "  (no IRQ data)\n");
    }
    fprintf(stderr, "========================================\n");
}

/* Device state */
static uint64_t msr_store = 0;
static int g_vmfd;

/* Live migration status */
static struct migrate_context g_migrate_ctx;
static int g_migrate_active = 0;

/* UART */
static struct uart8250 uart;

/* Virtio-mmio device */
static struct virtio_mmio_dev virtio_dev;

/* Eventfd for transmitq kick */
static int txkick_fd;

/* eventfd for IRQ5 injection */
static int irq5_fd;

static void *txkick_thread(void *arg) {
    (void)arg;
    uint64_t val;
    while (read(txkick_fd, &val, sizeof(val)) == sizeof(val)) {
        ioeventfd_kick_count++;
        uint64_t t1 = now_ns();
        virtio_console_tx(&virtio_dev, virtio_dev.ram, virtio_dev.ram_size);
        uint64_t t2 = now_ns();
        lat_record(&tx_lat, t2 - t1);   /* measure TX processing time */
    }
    return NULL;
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

/*
 * Query and display dirty page counts for both memory slots.
 * KVM_GET_DIRTY_LOG returns a bitmap (1 bit per 4KB page) of pages
 * written by the guest since the last call, then clears the bits.
 * Called via Ctrl-A d monitor command.
 */
static void print_dirty_log(int vmfd, size_t mem_size) {
    /* Slot 0: 0 - 0xD0000 (832KB = 208 pages) */
    size_t slot0_pages = 0xD0000 / 4096;
    size_t slot0_bitmap_sz = (slot0_pages + 63) / 64 * 8;
    uint64_t *bitmap0 = calloc(1, slot0_bitmap_sz);

    /* Slot 1: 0xD1000 - end (128MB - 0xD1000) */
    size_t slot1_pages = (mem_size - 0xD1000) / 4096;
    size_t slot1_bitmap_sz = (slot1_pages + 63) / 64 * 8;
    uint64_t *bitmap1 = calloc(1, slot1_bitmap_sz);

    if (!bitmap0 || !bitmap1) {
        fprintf(stderr, "print_dirty_log: alloc failed\n");
        free(bitmap0);
        free(bitmap1);
        return;
    }

    struct kvm_dirty_log log0 = {
        .slot = 0,
        .dirty_bitmap = bitmap0
    };
    struct kvm_dirty_log log1 = {
        .slot = 1,
        .dirty_bitmap = bitmap1
    };

    /* Fetch bitmap and atomically clear dirty bits */
    if (ioctl(vmfd, KVM_GET_DIRTY_LOG, &log0) < 0)
        perror("KVM_GET_DIRTY_LOG slot 0");
    if (ioctl(vmfd, KVM_GET_DIRTY_LOG, &log1) < 0)
        perror("KVM_GET_DIRTY_LOG slot 1");

    /* Count dirty pages using popcount (number of set bits) */
    char label0[64], label1[64];
    snprintf(label0, sizeof(label0), "Slot 0 [0x0-0xD0000]:");
    snprintf(label1, sizeof(label1), "Slot 1 [0xD1000-0x%lx]:", (unsigned long)mem_size);

    uint64_t dirty0 = 0, dirty1 = 0;
    for (size_t i = 0; i < slot0_bitmap_sz / 8; i++)
        dirty0 += __builtin_popcountll(bitmap0[i]);
    for (size_t i = 0; i < slot1_bitmap_sz / 8; i++)
        dirty1 += __builtin_popcountll(bitmap1[i]);

    uint64_t total = dirty0 + dirty1;

    /* Output the result */
    fprintf(stderr, "\n=== Dirty page report (Ctrl-A d) ===\n");
    fprintf(stderr, "  %-28s %llu / %zu pages dirty\n",
        label0, (unsigned long long)dirty0, slot0_pages);
    fprintf(stderr, "  %-28s %llu / %zu pages dirty\n",
        label1, (unsigned long long)dirty1, slot1_pages);
    fprintf(stderr, "  %-28s %llu pages (%llu KB)\n",
        "Total:", (unsigned long long)total, (unsigned long long)(total * 4));

    free(bitmap0);
    free(bitmap1);
}

/*
 * Read host stdin and deliver to guest.
 * Supports two modes (toggled via Ctrl-A v):
 *   - UART mode (default): characters go to ttyS0 via uart_rx()
 *   - Virtio mode: characters go to /dev/hvc0 via virtio_console_rx() + IRQ 5
 */
static void *stdin_thread(void *arg) {
    (void)arg;
    uint8_t c;
    int virtio_mode = 0;

    while (read(STDIN_FILENO, &c, 1) == 1) {
        /* Ctrl-A = monitor escape */
        if (c == 0x01) {
            if (read(STDIN_FILENO, &c, 1) != 1)
                break;
            if (c == 'v') {
                virtio_mode = !virtio_mode;
                fprintf(stderr, "\n[monitor] input → %s\n",
                    virtio_mode ? "hvc0 (virtio)" : "ttyS0 (UART)");
                if (!virtio_mode)
                    uart_rx(&uart, '\n', g_vmfd);
                continue;
            }
            if (c == 'd') {
                print_dirty_log(g_vmfd, GUEST_MEM_SIZE);
                continue;
            }
            if (c == 's') {
                fprintf(stderr, "\n[monitor] saving snapshot...\n");
                stop_requested = 1;
                continue;
            }
            if (c == 'm') {
                fprintf(stderr, "\n[monitor] starting live migration...\n");
                if (migrate_precopy("migration.bin", g_vmfd, virtio_dev.ram,
                    GUEST_MEM_SIZE, &g_migrate_ctx) == 0) {
                    g_migrate_active = 1;
                }
                stop_requested = 1;
                continue;
            }
            continue;
        }

        /* Skip terminal escape sequences */
        if (c == 0x1b) {
            while (read(STDIN_FILENO, &c, 1) == 1) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                    break;
            }
            continue;
        }

        /* Deliver character to guest via selected channel */
        if (virtio_mode) {
            if (virtio_console_rx(&virtio_dev, &c, 1) == 0) {
                /* Measure IRQ injection latency */
                uint64_t t1 = now_ns();
                if (use_irqfd) {
                    uint64_t val = 1;
                    write(irq5_fd, &val, sizeof(val));
                    irqfd_write_count++;
                } else {
                    /* Fallback: ioctl-based IRQ injection (Step 16 style) */
                    struct kvm_irq_level irq = {
                        .irq = 5,
                        .level = 1
                    };
                    ioctl(g_vmfd, KVM_IRQ_LINE, &irq);
                    irq.level = 0;
                    ioctl(g_vmfd, KVM_IRQ_LINE, &irq);
                    ioctl_irq_count++;
                }
                uint64_t t2 = now_ns();
                lat_record(&irq_lat, t2 - t1);
            }
        } else {
            uart_rx(&uart, c, g_vmfd);
        }
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
        if (stop_requested)
            break;
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
            mmio_exit_count++;
            uint64_t addr = run->mmio.phys_addr;
            if (addr >= VIRTIO_MMIO_BASE && addr < VIRTIO_MMIO_BASE + VIRTIO_MMIO_SIZE) {
                uint64_t offset = addr - VIRTIO_MMIO_BASE;

                /* Track QueueNotify and handle TX in exit handler if no ioeventfd */
                if (addr == VIRTIO_MMIO_BASE + VIRTIO_MMIO_QUEUE_NOTIFY && run->mmio.is_write) {
                    uint32_t nval = 0;
                    memcpy(&nval, run->mmio.data, run->mmio.len);
                    if (nval == 0)
                        queue_notify_rx_mmio_count++;
                    else if (nval == 1) {
                        queue_notify_tx_mmio_count++;

                        /* Fallback: TX processing in exit handler (Step 15 style) */
                        if (!use_ioeventfd) {
                            uint64_t t1 = now_ns();
                            virtio_console_tx(&virtio_dev, virtio_dev.ram, virtio_dev.ram_size);
                            uint64_t t2 = now_ns();
                            lat_record(&tx_lat, t2 - t1);
                        }
                    }
                }

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
        case KVM_EXIT_FAIL_ENTRY:
            fprintf(stderr, "KVM_EXIT_FAIL_ENTRY: hardware_entry_failure_reason=0x%llx\n",
                (unsigned long long)run->fail_entry.hardware_entry_failure_reason);
            return NULL;
        default:
            fprintf(stderr, "Unexpected exit reason: %d\n", run->exit_reason);
            return NULL;
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {

    /* Disable stdout buffering so kernel output appears immediately */
    setbuf(stdout, NULL);

    /* Ctrl-C stops the VM and prints exit/latency stats */
    signal(SIGINT, sigint_handler);

    /* Check for --restore mode */
    char *restore_path = NULL;
    if (argc > 2 && strcmp(argv[1], "--restore") == 0)
        restore_path = argv[2];

    /* Check for --restore-migration mode */
    char *migrate_restore_path = NULL;
    if (argc > 2 && strcmp(argv[1], "--restore-migration") == 0)
        migrate_restore_path = argv[2];

    /* Parse runtime flags from environment:
     *   USE_IOEVENTFD=1 ./microkvm  → Step 17 style TX kick
     *   USE_IRQFD=1 ./microkvm      → Step 18 style IRQ injection */
    use_ioeventfd = (getenv("USE_IOEVENTFD") != NULL);
    use_irqfd = (getenv("USE_IRQFD") != NULL);

    /* Initialize latency stats */
    lat_init(&tx_lat);
    lat_init(&irq_lat);

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

    /* Create eventfd for transmitq kick (ioeventfd) */
    txkick_fd = eventfd(0, EFD_CLOEXEC);
    if (txkick_fd < 0) {
        perror("eventfd");
        return 1;
    }

    /* Register ioeventfd: when guest writes 1 to 0xD0000050, KVM signals txkick_fd */
    if (use_ioeventfd) {
        struct kvm_ioeventfd ioeventfd = {
            .addr = VIRTIO_MMIO_BASE + VIRTIO_MMIO_QUEUE_NOTIFY,
            .len = 4,
            .datamatch = 1,     /* only trigger for transmitq (value==1) */
            .fd = txkick_fd,
            .flags = KVM_IOEVENTFD_FLAG_DATAMATCH,
        };
        if (ioctl(vmfd, KVM_IOEVENTFD, &ioeventfd) < 0) {
            perror("KVM_IOEVENTFD");
            return 1;
        }
    }

    /* Create irqfd: writing to this fd injects IRQ5 into guest */
    irq5_fd = eventfd(0, EFD_CLOEXEC);
    if (irq5_fd < 0) {
        perror("eventfd irq5");
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

    /* Create in-kernel interrupt controller (PIC/IOAPIC/LAPIC) and timer (PIT).
     * This allows KVM to deliver IRQs without exiting to userspace. */
    if (ioctl(vmfd, KVM_CREATE_IRQCHIP, 0) < 0) {
        perror("KVM_CREATE_IRQCHIP");
        return 1;
    }

    struct kvm_pit_config pit = {
        .flags = KVM_PIT_SPEAKER_DUMMY,
    };
    if (ioctl(vmfd, KVM_CREATE_PIT2, &pit) < 0) {
        perror("KVM_CREATE_PIT2");
        return 1;
    }

    /* Register irqfd: writing to irq5_fd injects IRQ5 into guest
     * without any ioctl — KVM handles it entirely in kernel space */
    if (use_irqfd) {
        struct kvm_irqfd irqfd = {
            .fd = irq5_fd,
            .gsi = 5,   /* IRQ5 = virtio-mmio interrupt line */
            .flags = 0,
        };
        if (ioctl(vmfd, KVM_IRQFD, &irqfd) < 0) {
            perror("KVM_IRQFD");
            return 1;
        }
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
        .flags = KVM_MEM_LOG_DIRTY_PAGES,
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
        .flags = KVM_MEM_LOG_DIRTY_PAGES,
        .guest_phys_addr = 0xD1000,
        .memory_size = GUEST_MEM_SIZE - 0xD1000,
        .userspace_addr = (unsigned long)mem + 0xD1000,
    };
    if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region2) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION slot 2");
        return 1;
    }

    /* Load bzImage*/
    if (!restore_path && !migrate_restore_path) {
        if (load_bzimage("bzImage", mem, CMDLINE) < 0) {
            return 1;
        }

        /* Load initramfs (required for userspace) */
        uint32_t initrd_size;
        if (load_initramfs("initramfs.gz", mem, &initrd_size) < 0)
            fprintf(stderr, "Warning: no initramfs.gz found\n");
    }

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
        if (!restore_path && !migrate_restore_path) {
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
    }

    /* Restore VM state if --restore was specified */
    if (restore_path) {
        if (snap_restore(restore_path, vcpus[0].fd, vmfd, &uart, &virtio_dev,
            mem, GUEST_MEM_SIZE) < 0)
            return 1;
    }

    /* Restore from migration file if --restore-migration was specified */
    if (migrate_restore_path) {
        if (migrate_restore(migrate_restore_path, vcpus[0].fd, vmfd, &uart, &virtio_dev,
            mem, GUEST_MEM_SIZE) < 0)
            return 1;
    }

    /* KVM MMU stats - take before snapshot */
    int vm_stats_fd = ioctl(vmfd, KVM_GET_STATS_FD, NULL);
    int vcpu_stats_fd = ioctl(vcpus[0].fd, KVM_GET_STATS_FD, NULL);
    struct kvm_stats_reading vm_before = {0}, vcpu_before = {0};
    if (vm_stats_fd >= 0)
        kvm_stats_capture(vm_stats_fd, &vm_before);
    if (vcpu_stats_fd >= 0)
        kvm_stats_capture(vcpu_stats_fd, &vcpu_before);

    /* Run */
    set_raw_terminal();
    printf("Starting guest...\n");
    fprintf(stderr, "Starting guest [ioeventfd=%s, irqfd=%s]...\n",
        use_ioeventfd ? "ON" : "OFF", use_irqfd ? "ON" : "OFF");

    pthread_t stdin_tid;
    pthread_create(&stdin_tid, NULL, stdin_thread, NULL);

    pthread_t txkick_tid;
    if (use_ioeventfd)
        pthread_create(&txkick_tid, NULL, txkick_thread, NULL);

    for (int i = 0; i < NUM_VCPUS; i++) {
        pthread_create(&threads[i], NULL, vcpu_thread, &vcpus[i]);
    }
    for (int i = 0; i < NUM_VCPUS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Save snapshot or complete migration depending on trigger */
    if (g_migrate_active) {
        migrate_stop_and_copy(&g_migrate_ctx, vcpus[0].fd, vmfd,
            &uart, &virtio_dev, mem, GUEST_MEM_SIZE);
    } else {
        snap_save("snapshot.bin", vcpus[0].fd, vmfd, &uart, &virtio_dev,
            mem, GUEST_MEM_SIZE);
    }

    /* Print exit counts and latency stats (benchmark report) */
    print_exit_stats();

    /* KVM MMU stats - take after snapshot and print delta */
    struct kvm_stats_reading vm_after = {0}, vcpu_after = {0};
    if (vm_stats_fd >= 0) {
        kvm_stats_capture(vm_stats_fd, &vm_after);
        kvm_stats_print_delta("KVM VM stats (boot delta)", &vm_before, &vm_after);
        close(vm_stats_fd);
    }
    if (vcpu_stats_fd >= 0) {
        kvm_stats_capture(vcpu_stats_fd, &vcpu_after);
        kvm_stats_print_delta("KVM vCPU 0 stats (boot delta)", &vcpu_before, &vcpu_after);
        close(vcpu_stats_fd);
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