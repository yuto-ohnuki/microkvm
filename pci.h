#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/kvm.h>

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

/* Device registers within BAR0 MMIO region (offsets from BAR0 base) */
#define PCI_DEV_REG_STATUS      0x00
#define PCI_DEV_REG_DOORBELL    0x04
#define PCI_DEV_REG_RESULT      0x08
#define PCI_DEV_REG_DESC_LO     0x0C
#define PCI_DEV_REG_DESC_HI     0x10

/* MSI-X table location within BAR0 (offset 0x800, 1 vector) */
#define PCI_MSIX_TABLE_OFFSET   0x800
#define PCI_MSIX_TABLE_ENTRIES  1

/*
 * DMA descriptor — placed in guest RAM by the driver.
 * Device reads this to know where and how much data to transfer.
 */
struct dma_desc {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;     /* 0=device reads from guest */
};

/* MSI-X table entry (16 bytes per vector) */
struct msix_entry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t data;
    uint32_t ctrl;      /* bit 0: masked */
};

struct pci_device {
    uint8_t config[256];        /* PCI config space (Type 0 header) */
    uint32_t bar0_mask;         /* BAR0 size mask for probing */
    uint32_t config_address;    /* last value written to 0xCF8 */

    /* DMA state */
    uint64_t desc_addr;         /* GPA of descriptor (set via DESC_LO/HI registers) */
    uint32_t last_dma_len;      /* result of last DMA operation */
    uint8_t *ram;               /* pointer to guest RAM (set by VMM) */
    size_t ram_size;

    /* MSI-X */
    struct msix_entry msix_table[PCI_MSIX_TABLE_ENTRIES];
    int vmfd;                   /* for KVM_SIGNAL_MSI */

    /* hotplug */
    int present;                /* 0=absent (returns 0xFFFF), 1=present */
};

void pci_init(struct pci_device *dev);
void pci_config_write(struct pci_device *dev, uint8_t offset, uint32_t value, int len);
uint32_t pci_config_read(struct pci_device *dev, uint8_t offset, int len);

uint32_t pci_bar0_addr(struct pci_device *dev);
uint32_t pci_dev_mmio_read(struct pci_device *dev, uint64_t offset);
void pci_dev_mmio_write(struct pci_device *dev, uint64_t offset, uint32_t value);

uint32_t pci_msix_read(struct pci_device *dev, uint64_t offset);
void pci_msix_write(struct pci_device *dev, uint64_t offset, uint32_t value);

void pci_init_hotplug(struct pci_device *dev);

#endif /* PCI_H */