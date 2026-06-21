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

/* Magic: "virt" in little-endian */
#define VIRTIO_MMIO_MAGIC  0x74726976

/* Device IDs (virtio spec) */
#define VIRTIO_ID_CONSOLE  3

/* Our vendor ID: "MKVM" in little-endian */
#define VIRTIO_VENDOR_MKVM  0x4D4B564D

/* Virtio-mmio device state */
struct virtio_mmio_dev {
    uint32_t status;
    uint32_t host_features_sel;
    uint32_t guest_features;
    uint32_t guest_page_size;
    uint32_t queue_sel;
};

void virtio_mmio_init(struct virtio_mmio_dev *dev);
void virtio_mmio_write(struct virtio_mmio_dev *dev, uint64_t offset,
    uint32_t value, int len);
uint32_t virtio_mmio_read(struct virtio_mmio_dev *dev, uint64_t offset, int len);

#endif