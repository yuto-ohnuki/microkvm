#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "virtio_mmio.h"

void virtio_mmio_init(struct virtio_mmio_dev *dev)
{
    dev->status = 0;
}

/*
 * Legacy vring layout:
 *   desc:  base
 *   avail: base + num * 16
 *   used:  align_up(avail_end, align)
 *     where avail_end = avail_offset + 6 + 2*num
 */
static uint64_t vring_base(struct virtio_mmio_dev *dev, int qidx)
{
    return (uint64_t)dev->vqs[qidx].pfn * dev->guest_page_size;
}

static uint64_t vring_desc_addr(struct virtio_mmio_dev *dev, int qidx)
{
    return vring_base(dev, qidx);
}

static uint64_t vring_avail_addr(struct virtio_mmio_dev *dev, int qidx)
{
    return vring_base(dev, qidx) + dev->vqs[qidx].num * 16;
}

static uint64_t vring_used_addr(struct virtio_mmio_dev *dev, int qidx)
{
    uint64_t avail = vring_avail_addr(dev, qidx);
    uint64_t avail_size = 6 + 2 * dev->vqs[qidx].num;
    uint64_t align = dev->vqs[qidx].align;
    return (avail + avail_size + align - 1) & ~(align - 1);
}

/*
 * Process transmitq (queue 1): read descriptors the guest made available,
 * write their payload to stdout, then post them to the used ring.
 *
 * ram: pointer to the start of guest physical memory (mmap'd region)
 * ram_size: total guest RAM size
 */
static void virtio_console_tx(struct virtio_mmio_dev *dev,
                              uint8_t *ram, size_t ram_size)
{
    int qidx = 1;  /* transmitq */
    struct virtqueue_state *vq = &dev->vqs[qidx];

    uint64_t desc_base  = vring_desc_addr(dev, qidx);
    uint64_t avail_base = vring_avail_addr(dev, qidx);
    uint64_t used_base  = vring_used_addr(dev, qidx);

    /* Bounds check: ensure vring addresse fall within guest RAM */
    if (desc_base >= ram_size || avail_base >= ram_size || used_base >= ram_size)
        return;

    /* Read avail->idx (guest increments this after adding buffers) */
    uint16_t avail_idx;
    memcpy(&avail_idx, ram + avail_base + 2, sizeof(uint16_t));

    while (vq->last_avail_idx != avail_idx) {
        /* Get descriptor head index from avail ring */
        uint16_t ring_slot = vq->last_avail_idx % vq->num;
        uint16_t desc_idx;
        memcpy(&desc_idx, ram + avail_base + 4 + ring_slot * 2, sizeof(uint16_t));

        /* Walk the descriptor chain */
        uint16_t cur = desc_idx;
        for (;;) {
            /* Prevent out-of-bounds descriptor walk */
            if (cur >= vq->num)
                break;

            struct vring_desc desc;
            memcpy(&desc, ram + desc_base + cur * 16, sizeof(desc));

            /* TX: device reads from buffer (flags should NOT have WRITE) */
            if (!(desc.flags & VRING_DESC_F_WRITE)) {
                if (desc.addr + desc.len <= ram_size) {
                    write(STDOUT_FILENO, ram + desc.addr, desc.len);
                }
            }

            if (desc.flags & VRING_DESC_F_NEXT)
                cur = desc.next;
            else
                break;
        }

        /* Post to used ring */
        uint16_t used_idx;
        memcpy(&used_idx, ram + used_base + 2, sizeof(uint16_t));
        uint16_t used_slot = used_idx % vq->num;

        struct vring_used_elem elem = { .id = desc_idx, .len = 0 };
        memcpy(ram + used_base + 4 + used_slot * 8, &elem, sizeof(elem));

        used_idx++;
        memcpy(ram + used_base + 2, &used_idx, sizeof(uint16_t));

        vq->last_avail_idx++;
    }
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

void virtio_mmio_write(struct virtio_mmio_dev *dev, uint64_t offset,
                       uint32_t value, int len)
{
    if (offset != VIRTIO_MMIO_QUEUE_NOTIFY)
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
        if (value == 1) {   /* transmitq */
            virtio_console_tx(dev, dev->ram, dev->ram_size);
        }
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        break;
    default:
        break;
    }
}