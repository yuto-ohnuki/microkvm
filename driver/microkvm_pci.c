#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>

/*
 * microkvm PCI device driver
 *
 * Matches the custom PCI device emulated by microkvm VMM (vendor=0x1234, device=0x0001).
 * Phase F builds this driver step-by-step: probe → BAR mapping → DMA → MSI-X.
 */

/* Must match VMM's pci.h definitions */
#define MICROKVM_VENDOR     0x1234
#define MICROKVM_DEVICE     0x0001

/* BAR0 register offsets (must match VMM pci.h) */
#define REG_STATUS          0x00
#define REG_DOORBELL        0x04
#define REG_RESULT          0x08
#define REG_DESC_LO         0x0C
#define REG_DESC_HI         0x10

/* DMA descriptor layout (must match VMM's struct dma_desc) */
struct microkvm_dma_desc {
    u64 addr;
    u32 len;
    u32 flags;  /* 0=device reads from guest (TX) */
};

struct microkvm_dev {
    struct pci_dev *pdev;
    void __iomem *bar0;
    void *dma_buf;          /* coherent DMA buffer (descriptor + payload) */
    dma_addr_t dma_addr;    /* bus address of dma_buf */
};

/* Called when PCI core finds a device matching our ID table */
static int microkvm_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    struct microkvm_dev *mdev;
    struct microkvm_dma_desc *desc;
    char *payload;
    int ret;
    u32 status;
    u32 result;

    mdev = devm_kzalloc(&pdev->dev, sizeof(*mdev), GFP_KERNEL);
    if (!mdev)
        return -ENOMEM;

    /* Enable device — sets Memory Space Enable in Command register */
    ret = pci_enable_device(pdev);
    if (ret)
        return ret;

    /* Claim BAR regions exclusively (prevents other drivers from using them) */
    ret = pci_request_regions(pdev, "microkvm_pci");
    if (ret)
        goto err_disable;

    /* Map BAR0 physical address into kernel virtual address space */
    mdev->bar0 = pci_iomap(pdev, 0, 0);
    if (!mdev->bar0) {
        ret = -EIO;
        goto err_regions;
    }

    /* Enable bus mastering — sets bit 2 in Command register (required for DMA) */
    pci_set_master(pdev);

    /* Allocate coherent DMA buffer: descriptor at start, payload after it */
    mdev->dma_buf = dma_alloc_coherent(&pdev->dev, 4096,
        &mdev->dma_addr, GFP_KERNEL);
    if (!mdev->dma_buf) {
        ret = -ENOMEM;
        goto err_iomap;
    }

    pci_set_drvdata(pdev, mdev);

    /* Build DMA descriptor at start of buffer */
    desc = mdev->dma_buf;
    payload = (char *)mdev->dma_buf + sizeof(*desc);
    memcpy(payload, "hello from driver\n", 18);

    desc->addr = mdev->dma_addr + sizeof(*desc);    /* points to payload */
    desc->len = 18;
    desc->flags = 0;    /* device reads from guest (TX) */

    /* Tell device where descriptor is */
    writel(lower_32_bits(mdev->dma_addr), mdev->bar0 + REG_DESC_LO);
    writel(upper_32_bits(mdev->dma_addr), mdev->bar0 + REG_DESC_HI);

    /* Confirm device is ready */
    status = readl(mdev->bar0 + REG_STATUS);
    dev_info(&pdev->dev, "STATUS = 0x%x\n", status);

    /* Kick doorbell — triggers DMA on VMM side */
    writel(1, mdev->bar0 + REG_DOORBELL);

    /* Read result — number of bytes transferred */
    result = readl(mdev->bar0 + REG_RESULT);
    dev_info(&pdev->dev, "DMA complete, transferred %u bytes\n", result);

    return 0;

err_iomap:
    pci_iounmap(pdev, mdev->bar0);
err_regions:
    pci_release_regions(pdev);
err_disable:
    pci_disable_device(pdev);
    return ret;
}

/* Called on rmmod or device removal */
static void microkvm_remove(struct pci_dev *pdev)
{
    struct microkvm_dev *mdev = pci_get_drvdata(pdev);

    /* Release resources in reverse order of probe acquisition */
    dma_free_coherent(&pdev->dev, 4096, mdev->dma_buf, mdev->dma_addr);
    pci_iounmap(pdev, mdev->bar0);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
    dev_info(&pdev->dev, "remove called\n");
}

static const struct pci_device_id microkvm_pci_ids[] = {
    { PCI_DEVICE(MICROKVM_VENDOR, MICROKVM_DEVICE) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, microkvm_pci_ids);

static struct pci_driver microkvm_pci_driver = {
    .name     = "microkvm_pci",
    .id_table = microkvm_pci_ids,
    .probe    = microkvm_probe,
    .remove   = microkvm_remove,
};
module_pci_driver(microkvm_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("microkvm PCI device driver");