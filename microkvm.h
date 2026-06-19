#ifndef MICROKVM_H
#define MICROKVM_H

/* VM configuration */
#define NUM_VCPUS 2
#define GUEST_MEM_SIZE (1 << 20)    /* 1 MB */

/* VM address layout */
#define VCPU1_ENTRY 0x1100          /* vCPU 1 entry point in guest binary */
#define PIO_PORT 0x10               /* PIO port for character output */

/* Synthetic MSR for testing userspace MSR handling */
#define MSR_CUSTOM 0x4B564D00

#endif /* MICROKVM_H */