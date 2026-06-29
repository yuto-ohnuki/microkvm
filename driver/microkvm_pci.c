#include <linux/module.h>
#include <linux/pci.h>

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

struct microkvm_dev {
    struct pci_dev *pdev;
    void __iomem *bar0;
};

/* Called when PCI core finds a device matching our ID table */
static int microkvm_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    struct microkvm_dev *mdev;
    int ret;
    u32 status;

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

    pci_set_drvdata(pdev, mdev);

    /* Read STATUS register - triggers KVM_EXIT_MMIO on VMM side */
    status = readl(mdev->bar0 + REG_STATUS);
    dev_info(&pdev->dev, "STATUS = 0x%x\n", status);
    return 0;

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