#ifndef MICROKVM_H
#define MICROKVM_H

/* VM address layout */
#define GUEST_MEM_SIZE (1 << 20)    /* 1 MB */
#define PIO_PORT 0x10               /* PIO port for character output */

/* Synthetic MSR for testing userspace MSR handling */
#define MSR_CUSTOM 0x4B564D00

#endif /* MICROKVM_H */