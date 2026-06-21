#include <stdio.h>
#include "virtio_mmio.h"

/* Reset device state */
void virtio_mmio_init(struct virtio_mmio_dev *dev)
{
    dev->status = 0;
}

/* Handle guest read from virtio-mmio register space */
uint32_t virtio_mmio_read(struct virtio_mmio_dev *dev, uint64_t offset, int len)
{
    uint32_t val = 0;

    switch(offset) {
    case VIRTIO_MMIO_STATUS:
        val = dev->status;
        break;
    case VIRTIO_MMIO_MAGIC_VALUE:
        val = VIRTIO_MMIO_MAGIC;
        break;
    case VIRTIO_MMIO_VERSION:
        val = 1;
        break;
    case VIRTIO_MMIO_DEVICE_ID:
        val = VIRTIO_ID_CONSOLE;
        break;
    case VIRTIO_MMIO_VENDOR_ID:
        val = VIRTIO_VENDOR_MKVM;
        break;
    case VIRTIO_MMIO_HOST_FEATURES:
        val = 0;
        break;
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        val = (dev->queue_sel < VIRTQ_NUM_QUEUES) ? 128 : 0;
        break;
    case VIRTIO_MMIO_QUEUE_PFN:
        val = (dev->queue_sel < VIRTQ_NUM_QUEUES) ? dev->vqs[dev->queue_sel].pfn : 0;
        break;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        val = 0;
        break;
    default:
        break;
    }

    fprintf(stderr, "[virtio-mmio] read  offset=0x%03lx → 0x%x\n",
        (unsigned long)offset, val);
    return val;
}

/* Handle guest write to virtio-mmio register space */
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
    case VIRTIO_MMIO_QUEUE_NUM:
        if (dev->queue_sel < VIRTQ_NUM_QUEUES)
            dev->vqs[dev->queue_sel].num = value;
        break;
    case VIRTIO_MMIO_QUEUE_ALIGN:
        if (dev->queue_sel < VIRTQ_NUM_QUEUES)
            dev->vqs[dev->queue_sel].align = value;
        break;
    case VIRTIO_MMIO_QUEUE_PFN:
        if (dev->queue_sel < VIRTQ_NUM_QUEUES) {
            dev->vqs[dev->queue_sel].pfn = value;
            if (value) {
                uint64_t gpa = (uint64_t)value * dev->guest_page_size;
                uint32_t num = dev->vqs[dev->queue_sel].num;
                uint32_t align = dev->vqs[dev->queue_sel].align;
                uint64_t avail = gpa + num * 16;
                uint64_t used = (avail + 6 + 2 * num + align - 1) & ~((uint64_t)align - 1);
                fprintf(stderr, "[virtio-mmio] queue %d: desc=0x%lx avail=0x%lx used=0x%lx (num=%d)\n",
                    dev->queue_sel, gpa, avail, used, num);
            }
        }
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        break;
    default:
        break;
    }
}