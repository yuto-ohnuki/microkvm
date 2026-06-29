#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/completion.h>

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
    struct completion dma_done;
    void __iomem *bar0;
    void *dma_buf;          /* coherent DMA buffer (descriptor + payload) */
    dma_addr_t dma_addr;    /* bus address of dma_buf */
};

/* MSI-X interrupt handler — called when VMM injects KVM_SIGNAL_MSI after DMA completion */
static irqreturn_t microkvm_irq_handler(int irq, void *data)
{
    struct microkvm_dev *mdev = data;
    u32 status = readl(mdev->bar0 + REG_STATUS);

    dev_info(&mdev->pdev->dev, "IRQ: status=0x%x\n", status);
    complete(&mdev->dma_done);
    return IRQ_HANDLED;
}

/* Called when PCI core finds a device matching our ID table */
static int microkvm_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    struct microkvm_dev *mdev;
    struct microkvm_dma_desc *desc;
    char *payload;
    int ret, irq, nvec;
    u32 status;
    u32 result;

    mdev = devm_kzalloc(&pdev->dev, sizeof(*mdev), GFP_KERNEL);
    if (!mdev)
        return -ENOMEM;

    mdev->pdev = pdev;
    init_completion(&mdev->dma_done);

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

    /* Allocate MSI-X vector */
    nvec = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);
    if (nvec < 0) {
        dev_err(&pdev->dev, "Failed to allocate MSI-X vector: %d\n", nvec);
        ret = nvec;
        goto err_iomap;
    }

    /* Get Linux IRQ number for MSI-X vector 0 */
    irq = pci_irq_vector(pdev, 0);
    ret = request_irq(irq, microkvm_irq_handler, 0, "microkvm_pci", mdev);
    if (ret) {
        dev_err(&pdev->dev, "Failed to request IRQ: %d\n", ret);
        goto err_irq_vec;
    }

    /* Allocate coherent DMA buffer: descriptor at start, payload after it */
    mdev->dma_buf = dma_alloc_coherent(&pdev->dev, 4096,
        &mdev->dma_addr, GFP_KERNEL);
    if (!mdev->dma_buf) {
        ret = -ENOMEM;
        goto err_irq;
    }

    pci_set_drvdata(pdev, mdev);

    /* Build DMA descriptor at start of buffer */
    desc = mdev->dma_buf;
    payload = (char *)mdev->dma_buf + sizeof(*desc);
    memcpy(payload, "hello from driver\n", 18);

    desc->addr = mdev->dma_addr + sizeof(*desc);    /* points to payload */
    desc->len = 18;
    desc->flags = 0;    /* device reads from guest (TX) */

    /* Confirm device is ready */
    status = readl(mdev->bar0 + REG_STATUS);
    dev_info(&pdev->dev, "STATUS = 0x%x\n", status);

    /* Submit: DESC_LO/HI + doorbell */
    writel(lower_32_bits(mdev->dma_addr), mdev->bar0 + REG_DESC_LO);
    writel(upper_32_bits(mdev->dma_addr), mdev->bar0 + REG_DESC_HI);
    writel(1, mdev->bar0 + REG_DOORBELL);

    /* Wait for completion via MSI-X interrupt */
    if (!wait_for_completion_timeout(&mdev->dma_done, HZ * 5)) {
        dev_err(&pdev->dev, "DMA timeout!\n");
    } else {
        result = readl(mdev->bar0 + REG_RESULT);
        dev_info(&pdev->dev, "DMA done via MSI-X, transferred %u bytes\n", result);
    }

    return 0;

err_irq:
    free_irq(pci_irq_vector(pdev, 0), mdev);
err_irq_vec:
    pci_free_irq_vectors(pdev);
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
    free_irq(pci_irq_vector(pdev, 0), mdev);
    pci_free_irq_vectors(pdev);
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