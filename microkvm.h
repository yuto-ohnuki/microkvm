#ifndef MICROKVM_H
#define MICROKVM_H

/* VM configuration */
#define NUM_VCPUS 1
#define GUEST_MEM_SIZE (128 << 20)    /* 128 MB */

/* Synthetic MSR for testing userspace MSR handling */
#define MSR_CUSTOM 0x4B564D00

#endif /* MICROKVM_H */