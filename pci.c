#include <unistd.h>
#include <sys/ioctl.h>
#include "pci.h"

/*
 * Initialize PCI device with Type 0 config header.
 * Linux will discover this via bus enumeration (lspci / sysfs).
 */
void pci_init(struct pci_device *dev)
{
    memset(dev->config, 0, sizeof(dev->config));

    /* Vendor ID / Device ID — identifies the device to the OS */
    *(uint16_t *)&dev->config[0x00] = PCI_VENDOR_ID;
    *(uint16_t *)&dev->config[0x02] = PCI_DEVICE_ID;

    /* Status: capabilities list present (bit 4) */
    *(uint16_t *)&dev->config[0x06] = 0x0010;

    /* Revision ID */
    dev->config[0x08] = 0x01;

    /* Class code: 0xFF0000 (unassigned — no standard driver needed) */
    dev->config[0x09] = 0x00;   /* prog-if */
    dev->config[0x0A] = 0x00;   /* subclass */
    dev->config[0x0B] = 0xFF;   /* class */

    /* Header type: 0 (endpoint, not bridge) */
    dev->config[0x0E] = 0x00;

    /* BAR0: MMIO, 32-bit, non-prefetchable (bit 0 = 0 means MMIO) */
    *(uint32_t *)&dev->config[0x10] = 0x00000000;

    /* BAR0 size mask: writing 0xFFFFFFFF and reading back reveals size */
    dev->bar0_mask = ~(PCI_BAR0_SIZE - 1);

    /* Capabilities pointer → offset 0x40 */
    dev->config[0x34] = 0x40;

    /* MSI-X capability at 0x40 (11 bytes) */
    dev->config[0x40] = 0x11;       /* cap ID = MSI-X */
    dev->config[0x41] = 0x00;       /* next cap = NULL */

    /* Message Control: table size = 0 (means 1 entry), function not masked */
    *(uint16_t *)&dev->config[0x42] = 0x0000;

    /* Table BIR=0, offset=0x800 */
    *(uint32_t *)&dev->config[0x44] = PCI_MSIX_TABLE_OFFSET;

    /* PBA BIR=0, offset=0xC00 */
    *(uint32_t *)&dev->config[0x48] = 0x00000C00;

    dev->config_address = 0;
}

/* Extract BAR0 base address (mask out type bits in lower 4 bits) */
uint32_t pci_bar0_addr(struct pci_device *dev) {
    return *(uint32_t *)&dev->config[0x10] & 0xFFFFF000;
}

/*
 * Handle PCI config space write.
 * BAR0 has special handling for size probing:
 *   - Write 0xFFFFFFFF → store mask (next read returns size info)
 *   - Write address → store aligned address
 */
void pci_config_write(struct pci_device *dev, uint8_t offset, uint32_t value, int len)
{
    fprintf(stderr, "[pci] config write offset=0x%02x ← 0x%x (len=%d)\n",
        offset, value, len);

    if (offset == 0x10) {
        /* BAR0 write — handle size probing */
        if (value == 0xFFFFFFFF) {
            *(uint32_t *)&dev->config[0x10] = dev->bar0_mask;
        } else {
            *(uint32_t *)&dev->config[0x10] = value & dev->bar0_mask;
        }
        return;
    }

    /* Generic config write */
    if (len == 4)
        *(uint32_t *)&dev->config[offset] = value;
    else if (len == 2)
        *(uint16_t *)&dev->config[offset] = (uint16_t)value;
    else
        dev->config[offset] = (uint8_t)value;

}

/* Read PCI config space register at given offset and length (1/2/4 bytes) */
uint32_t pci_config_read(struct pci_device *dev, uint8_t offset, int len)
{
    uint32_t value = 0;
    if (len == 4)
        value = *(uint32_t *)&dev->config[offset];
    else if (len == 2)
        value = *(uint16_t *)&dev->config[offset];
    else
        value = dev->config[offset];

    fprintf(stderr, "[pci] config read  offset=0x%02x → 0x%x (len=%d)\n",
        offset, value, len);
    return value;
}

/* Device MMIO register read — BAR0 region */
uint32_t pci_dev_mmio_read(struct pci_device *dev, uint64_t offset)
{
    uint32_t val = 0;
    switch (offset) {
    case PCI_DEV_REG_STATUS:
        val = 0x01;
        break;  /* ready */
    case PCI_DEV_REG_RESULT:
        val = dev->last_dma_len;
        break;  /* result */
    default:
        break;
    }
    fprintf(stderr, "[pci-dev] MMIO read  offset=0x%02lx → 0x%x\n",
        (unsigned long)offset, val);
    return val;
}

/* Device MMIO register write — BAR0 region */
void pci_dev_mmio_write(struct pci_device *dev, uint64_t offset, uint32_t value)
{
    fprintf(stderr, "[pci-dev] MMIO write offset=0x%02lx ← 0x%x\n",
        (unsigned long)offset, value);
    switch (offset) {
    case PCI_DEV_REG_DOORBELL: {
        /* Read descriptor from guest RAM */
        if (dev->desc_addr + sizeof(struct dma_desc) > dev->ram_size)
            break;
        struct dma_desc desc;
        memcpy(&desc, dev->ram + dev->desc_addr, sizeof(desc));
        if (desc.addr + desc.len > dev->ram_size)
            break;

        if (desc.flags == 0) {
            /* DMA: Device reads from guest (TX path) */
            fprintf(stderr, "[pci-dma] DMA read: %u bytes from GPA 0x%lx\n",
                desc.len, (unsigned long)desc.addr);
            write(STDOUT_FILENO, dev->ram + desc.addr, desc.len);
        } else {
            /* DMA: Device writes to guest (RX path) */
            const char *msg = "DMA-WRITE-OK\n";
            size_t msg_len = strlen(msg);
            if (msg_len > desc.len)
                msg_len = desc.len;
            memcpy(dev->ram + desc.addr, msg, msg_len);
            fprintf(stderr, "[pci-dma] DMA write: %zu bytes to GPA 0x%lx\n",
                msg_len, (unsigned long)desc.addr);
        }
        dev->last_dma_len = desc.len;

        /* Inject MSI-X after DMA completion */
        if (!(dev->msix_table[0].ctrl & 1) && dev->msix_table[0].addr_lo) {
            struct kvm_msi msi = {
                .address_lo = dev->msix_table[0].addr_lo,
                .address_hi = dev->msix_table[0].addr_hi,
                .data = dev->msix_table[0].data,
            };
            ioctl(dev->vmfd, KVM_SIGNAL_MSI, &msi);
            fprintf(stderr, "[pci-msix] IRQ injected: addr=0x%x data=0x%x\n",
                msi.address_lo, msi.data);
        }
        break;
    }
    case PCI_DEV_REG_DESC_LO:
        dev->desc_addr = (dev->desc_addr & 0xFFFFFFFF00000000ULL) | value;
        break;
    case PCI_DEV_REG_DESC_HI:
        dev->desc_addr = (dev->desc_addr & 0xFFFFFFFF) | ((uint64_t)value << 32);
        break;
    default:
        break;
    }
}

/* Read MSI-X table entry — guest driver reads to check vector configuration */
uint32_t pci_msix_read(struct pci_device *dev, uint64_t offset) {
    uint32_t *table = (uint32_t *)dev->msix_table;
    return table[(offset - PCI_MSIX_TABLE_OFFSET) / 4];
}

/* Write MSI-X table entry — guest driver programs addr/data/ctrl for each vector */
void pci_msix_write(struct pci_device *dev, uint64_t offset, uint32_t value) {
    uint32_t *table = (uint32_t *)dev->msix_table;
    table[(offset - PCI_MSIX_TABLE_OFFSET) / 4] = value;
    fprintf(stderr, "[pci-msix] table write offset=0x%lx val=0x%x\n",
            (unsigned long)offset, value);
}