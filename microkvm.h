#ifndef MICROKVM_H
#define MICROKVM_H

#include <time.h>
#include <stdint.h>

/* VM configuration */
#define NUM_VCPUS 1
#define GUEST_MEM_SIZE (128 << 20)    /* 128 MB */

/*
 * Private synthetic MSR used only by microkvm's educational guest.
 *
 * This is an arbitrary experimental value, not an architectural,
 * KVM-defined, or standardized MSR ABI.
 */
#define MSR_CUSTOM 0x20000000

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#endif /* MICROKVM_H */