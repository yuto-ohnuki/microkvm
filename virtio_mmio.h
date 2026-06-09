#ifndef VIRTIO_MMIO_H
#define VIRTIO_MMIO_H

#include <stdint.h>

/* Base address: above 128MB guest RAM, ensures automatic EPT violation */
#define VIRTIO_MMIO_BASE  0xD0000000
#define VIRTIO_MMIO_SIZE  0x200

/* Register offsets (legacy v1, all 32-bit wide) */
#define VIRTIO_MMIO_MAGIC_VALUE         0x000
#define VIRTIO_MMIO_VERSION             0x004
#define VIRTIO_MMIO_DEVICE_ID           0x008
#define VIRTIO_MMIO_VENDOR_ID           0x00C
#define VIRTIO_MMIO_STATUS              0x070

/* Feature negotiation */
#define VIRTIO_MMIO_HOST_FEATURES       0x010
#define VIRTIO_MMIO_HOST_FEATURES_SEL   0x014
#define VIRTIO_MMIO_GUEST_FEATURES      0x020
#define VIRTIO_MMIO_GUEST_FEATURES_SEL  0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE     0x028
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034

/* Virtqueue setup */
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_ALIGN         0x03C
#define VIRTIO_MMIO_QUEUE_PFN           0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064

#define VIRTQ_NUM_QUEUES 2      /* receiveq (0) + transmitq (1) */
#define VIRTQ_MAX_SIZE   128    /* max descriptors per queue */

/* Magic: "virt" in little-endian */
#define VIRTIO_MMIO_MAGIC  0x74726976

/* Device IDs (virtio spec) */
#define VIRTIO_ID_CONSOLE  3

/* Our vendor ID: "MKVM" in little-endian */
#define VIRTIO_VENDOR_MKVM  0x4D4B564D

struct virtqueue_state {
    uint32_t num;
    uint32_t align;
    uint32_t pfn;
};

struct virtio_mmio_dev {
    uint32_t status;
    uint32_t host_features_sel;
    uint32_t guest_features;
    uint32_t guest_page_size;
    uint32_t queue_sel;
    struct virtqueue_state vqs[VIRTQ_NUM_QUEUES];
};

void virtio_mmio_init(struct virtio_mmio_dev *dev);
void virtio_mmio_write(struct virtio_mmio_dev *dev, uint64_t offset,
                       uint32_t value, int len);
uint32_t virtio_mmio_read(struct virtio_mmio_dev *dev, uint64_t offset, int len);

#endif