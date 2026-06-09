#include <stdio.h>
#include "virtio_mmio.h"

void virtio_mmio_init(struct virtio_mmio_dev *dev)
{
    dev->status = 0;
}

uint32_t virtio_mmio_read(struct virtio_mmio_dev *dev, uint64_t offset, int len)
{
    uint32_t val = 0;

    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        val = VIRTIO_MMIO_MAGIC;
        break;
    case VIRTIO_MMIO_VERSION:
        val = 1;  /* legacy interface */
        break;
    case VIRTIO_MMIO_DEVICE_ID:
        val = VIRTIO_ID_CONSOLE;
        break;
    case VIRTIO_MMIO_VENDOR_ID:
        val = VIRTIO_VENDOR_MKVM;
        break;
    case VIRTIO_MMIO_STATUS:
        val = dev->status;
        break;
    case VIRTIO_MMIO_HOST_FEATURES:
        val = 0;  /* no features offered for now */
        break;
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        val = 0;  /* will be set in step14 */
        break;
    default:
        break;
    }

    fprintf(stderr, "[virtio-mmio] read  offset=0x%03lx → 0x%x\n",
            (unsigned long)offset, val);
    return val;
}

void virtio_mmio_write(struct virtio_mmio_dev *dev, uint64_t offset,
                       uint32_t value, int len)
{
    fprintf(stderr, "[virtio-mmio] write offset=0x%03lx ← 0x%x\n",
            (unsigned long)offset, value);

    switch (offset) {
    case VIRTIO_MMIO_STATUS:
        dev->status = value;
        if (value == 0)
            fprintf(stderr, "[virtio-mmio] device reset\n");
        break;
    case VIRTIO_MMIO_HOST_FEATURES_SEL:
        dev->host_features_sel = value;
        break;
    case VIRTIO_MMIO_GUEST_FEATURES_SEL:
        break;
    case VIRTIO_MMIO_GUEST_FEATURES:
        dev->guest_features = value;
        break;
    case VIRTIO_MMIO_GUEST_PAGE_SIZE:
        dev->guest_page_size = value;
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        dev->queue_sel = value;
        break;
    default:
        break;
    }
}