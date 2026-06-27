#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * PCI Configuration Mechanism
 *
 * x86 PCI config space access uses two I/O ports:
 *   0xCF8 (CONFIG_ADDRESS): selects bus/device/function/register
 *   0xCFC (CONFIG_DATA): reads/writes the selected register
 *
 * Address format written to 0xCF8:
 *   [31]    = enable bit (must be 1)
 *   [23:16] = bus number
 *   [15:11] = device number
 *   [10:8]  = function number
 *   [7:2]   = register offset (dword-aligned)
 *   [1:0]   = 0
 */
#define PCI_CONFIG_ADDR_PORT    0x0CF8
#define PCI_CONFIG_DATA_PORT    0x0CFC

/* Our custom PCI device identity */
#define PCI_VENDOR_ID           0x1234
#define PCI_DEVICE_ID           0x0001
#define PCI_BAR0_SIZE           4096    /* 4KB MMIO region */

struct pci_device {
    uint8_t config[256];       /* PCI config space (Type 0 header) */
    uint32_t bar0_mask;        /* BAR0 size mask for probing */
    uint32_t config_address;   /* last value written to 0xCF8 */
};

void pci_init(struct pci_device *dev);
void pci_config_write(struct pci_device *dev, uint8_t offset, uint32_t value, int len);
uint32_t pci_config_read(struct pci_device *dev, uint8_t offset, int len);

#endif /* PCI_H */