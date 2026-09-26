# Step 30: BAR mapping — pci_enable_device, pci_iomap, readl

## Goal

Extend the driver's probe to enable the PCI device, map BAR0 into kernel virtual address space, and read STATUS with `readl`. Verify end-to-end MMIO from the driver through KVM to the VMM device model.

## Background

### PCI device enablement sequence

This driver prepares access to device registers in this order:

```
pci_enable_device()      → Set Memory Space Enable (Command register bit 1)
pci_request_regions()    → Claim BAR regions exclusively (prevent driver conflicts)
pci_iomap()              → Map BAR physical address into kernel virtual space (ioremap)
                           Return `void __iomem *`; access through MMIO accessors
```

BAR0 is an MMIO region; `pci_iomap(pdev, 0, 0)` maps all of it. The last argument, 0, selects the full BAR length.

### Why readl/writel instead of pointer dereferences?

MMIO registers are not ordinary memory:
- Reads/writes may have side effects (change device state)
- The compiler must not optimize away or reorder accesses
- Architecture-specific memory barriers are needed

`readl`/`writel` are 32-bit MMIO accessors providing architecture-specific handling of access width and ordering. Use these APIs instead of ordinary memory pointer dereferences.

### pci_request_regions

```c
pci_request_regions(pdev, "microkvm_pci");
```

This prevents multiple drivers from claiming the same BAR simultaneously. The call fails if another driver already owns the region—Linux resource management for I/O address space.

### Correspondence with Phase E, Step 25

| Step 25 (VMM) | Step 30 (Driver) |
|---|---|
| Receive KVM_EXIT_MMIO | Issue readl/writel |
| `pci_dev_mmio_read()` returns a value | `readl()` receives the value |
| Manage BAR0 GPA range | Map BAR0 with `pci_iomap()` |
| Dispatch through `switch (offset)` | Specify offset with `bar0 + REG_STATUS` |

### devm_kzalloc

```c
mdev = devm_kzalloc(&pdev->dev, sizeof(*mdev), GFP_KERNEL);
```

Device-managed allocation is freed automatically if probe fails or after the remove callback finishes. `mdev` needs no `kfree()`, but resources obtained through `pci_iomap()` and `pci_request_regions()` must be released explicitly.

### pci_set_drvdata / pci_get_drvdata

Associate driver-private data (`microkvm_dev`) with the PCI device structure, allowing remove to retrieve resources allocated by probe.

## Execution flow

```
Guest (driver probe)         KVM                VMM
────────────────────         ───                ───
pci_enable_device()
                             KVM_EXIT_IO
                             config write 0x04←0x2
                                                Command: Memory Enable

pci_request_regions()
  → Claim BAR region inside Linux

pci_iomap(pdev, 0, 0)
  → ioremap(0x08000000)

readl(bar0 + 0x00)
  → Access GPA 0x08000000
                             VM exit due to EPT
                             KVM_EXIT_MMIO
                             addr=0x08000000
                                                pci_dev_mmio_read(0x00)
                                                → Return 0x01 (ready)
  → status = 0x01
```

## Implementation

### driver/microkvm_pci.c (main additions)

```c
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
```

On failure, `goto` releases only resources already acquired, in reverse order. After successful probe, remove unmaps the BAR, releases its region, and disables the device.

## Output

Follow [Step 29's procedure](step29_pci-driver.md#prerequisites) to rebuild the driver, replace the `.ko` in initramfs, and boot.

```
/ # insmod /lib/modules/microkvm_pci.ko
microkvm_pci: loading out-of-tree module taints kernel.
[pci] config read  offset=0x04 → 0x0 (len=2)
microkvm_pci 0000:00:00.0: enabling device (0000 -> 0002)
[pci] config write offset=0x04 ← 0x2 (len=2)
[pci-dev] MMIO read  offset=0x00 → 0x1
microkvm_pci 0000:00:00.0: STATUS = 0x1
/ # rmmod microkvm_pci
microkvm_pci 0000:00:00.0: remove called
```

The VMM log shows the full path: driver `readl` → KVM_EXIT_MMIO → VMM `pci_dev_mmio_read` returns 0x01 → driver receives `STATUS = 0x1`.

## Key insight

`pci_iomap` + `readl` are standard Linux MMIO APIs. The driver simply reads a memory-mapped address. In this VM, the path is EPT-related VM exit → KVM MMIO handling → KVM_EXIT_MMIO → VMM device handling → result return. Driver code is the same on physical hardware and in a VM; the exit path differs.

## What this step teaches

| Concept | How it appears here |
|------|---------------|
| pci_enable_device | Memory Space Enable makes the device accessible |
| pci_request_regions | Exclusive BAR ownership (resource management) |
| pci_iomap | Map BAR physical address to kernel virtual address (ioremap) |
| readl/writel | 32-bit MMIO access and ordering control |
| devm_kzalloc | Automatically free mdev on probe failure or after remove |
| goto error cleanup | Standard kernel error-handling pattern |
| pci_set/get_drvdata | Carry private data between probe and remove |

## What changed

Changes from Step 29:
- **driver/microkvm_pci.c**: `struct microkvm_dev`, `REG_STATUS`, full probe with enable/regions/iomap/readl, and reverse-order cleanup in remove

No VMM changes.

## Next step

[Step 31: DMA-Capable Driver](step31_dma-driver.md) allocates a coherent DMA buffer, builds a descriptor, and kicks the doorbell to transfer data through the VMM's DMA engine.
