#ifndef MICROKVM_H
#define MICROKVM_H

/* VM configuration */
#define NUM_VCPUS 1
#define GUEST_MEM_SIZE (128 << 20)    /* 128 MB */

/*
 * Guest physical memory layout (design-time source of truth).
 *
 * The RAM is registered with KVM as two memslots with a 4 KB gap:
 *   slot 0 : [0x00000000, 0x000D0000)   832 KB RAM
 *   (gap)  : [0x000D0000, 0x000D1000)   4 KB, left unregistered
 *   slot 1 : [0x000D1000, GUEST_MEM_SIZE)  RAM
 */
#define MEM_SLOT0_ID    0
#define MEM_SLOT1_ID    1

#define MEM_SLOT0_GPA   0x00000000ULL
#define MEM_GAP_START   0x000D0000ULL   /* unregistered GPA gap */
#define MEM_GAP_END     0x000D1000ULL
#define MEM_SLOT1_GPA   MEM_GAP_END

#define MEM_SLOT0_SIZE  (MEM_GAP_START - MEM_SLOT0_GPA)
#define MEM_SLOT1_SIZE  (GUEST_MEM_SIZE - MEM_SLOT1_GPA)

/*
 * Private synthetic MSR used only by microkvm's educational guest.
 *
 * This is an arbitrary experimental value, not an architectural,
 * KVM-defined, or standardized MSR ABI.
 */
#define MSR_CUSTOM 0x20000000

#endif /* MICROKVM_H */
