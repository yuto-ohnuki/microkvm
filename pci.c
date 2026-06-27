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

    dev->config_address = 0;
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