# Step 29: BAR mapping — pci_enable_device, pci_iomap, readl

## Goal

Extend the driver's probe function to enable the PCI device, map BAR0 into kernel virtual address space, and read the STATUS register with `readl` — confirming end-to-end MMIO access from driver through KVM to the VMM device model.

## Background

### PCI device enablement sequence

Before a driver can access device registers, three steps are required:

```
pci_enable_device()      → sets Memory Space Enable (Command register bit 1)
pci_request_regions()    → exclusively claims BAR regions (prevents conflicts)
pci_iomap()              → maps BAR physical address to kernel virtual address (ioremap)
                           Returns `void __iomem *` — must only be accessed via readl/writel
```

Only after all three can the driver use `readl`/`writel` to access device registers.

### Why readl/writel instead of pointer dereference?

MMIO registers are not normal memory:
- Each read/write may have side effects (device state changes)
- Compiler must not optimize away or reorder accesses
- Architecture-specific memory barriers are needed

`readl`/`writel` provide all of these guarantees. A raw pointer dereference (`*(volatile uint32_t *)addr`) would lack proper barriers on some architectures.

### pci_request_regions

```c
pci_request_regions(pdev, "microkvm_pci");
```

Prevents multiple drivers from claiming the same BAR simultaneously. If another driver already owns this region, the call fails — this is Linux's resource management for I/O address space.

### Phase E Step 24 correspondence

| Step 24 (VMM) | Step 29 (Driver) |
|---|---|
| Receives KVM_EXIT_MMIO | Issues readl/writel |
| `pci_dev_mmio_read()` returns value | `readl()` receives value |
| Manages BAR0 GPA range | `pci_iomap()` maps BAR0 |
| `switch (offset)` dispatches | `bar0 + REG_STATUS` selects register |

### devm_kzalloc

```c
mdev = devm_kzalloc(&pdev->dev, sizeof(*mdev), GFP_KERNEL);
```

Device-managed allocation — automatically freed when the device is removed. No manual `kfree()` needed in the remove path.

### pci_set_drvdata / pci_get_drvdata

Attaches driver-private data (`microkvm_dev`) to the PCI device structure. Allows remove() to retrieve resources allocated during probe().

## Execution flow

```
Guest (driver probe)         KVM                VMM
────────────────────         ───                ───
pci_enable_device()
                             KVM_EXIT_IO
                             config write 0x04←0x2
                                                Command: Memory Enable

pci_iomap(bar0)
  → ioremap(0x08000000)

readl(bar0 + 0x00)
  → access GPA 0x08000000
                             EPT violation
                             KVM_EXIT_MMIO
                             addr=0x08000000
                                                pci_dev_mmio_read(0x00)
                                                → return 0x01 (ready)
  → status = 0x01
```

## Implementation

### driver/microkvm_pci.c (key additions)

```c
#define REG_STATUS  0x00

struct microkvm_dev {
    struct pci_dev *pdev;
    void __iomem *bar0;
};

static int microkvm_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    struct microkvm_dev *mdev;

    mdev = devm_kzalloc(&pdev->dev, sizeof(*mdev), GFP_KERNEL);

    /* Enable device — sets Memory Space Enable in Command register */
    pci_enable_device(pdev);

    /* Claim BAR regions exclusively */
    pci_request_regions(pdev, "microkvm_pci");

    /* Map BAR0 into kernel virtual address space */
    mdev->bar0 = pci_iomap(pdev, 0, 0);

    pci_set_drvdata(pdev, mdev);

    /* Read STATUS register — triggers KVM_EXIT_MMIO on VMM side */
    u32 status = readl(mdev->bar0 + REG_STATUS);
    dev_info(&pdev->dev, "STATUS = 0x%x\n", status);
}

static void microkvm_remove(struct pci_dev *pdev)
{
    struct microkvm_dev *mdev = pci_get_drvdata(pdev);

    /* Release in reverse order of acquisition */
    pci_iounmap(pdev, mdev->bar0);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
}
```

Error handling uses the standard kernel `goto` cleanup pattern.

## Output

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

The VMM log shows the full path: driver calls `readl` → KVM_EXIT_MMIO → VMM's `pci_dev_mmio_read` returns 0x01 → driver receives `STATUS = 0x1`.

## Key insight

`pci_iomap` + `readl` is the standard Linux API for MMIO access. From the driver's perspective, it's just reading a memory-mapped address. Under the hood in a VM, this triggers an EPT violation → VM exit → VMM emulation → result returned. The driver code is identical whether running on physical hardware or in a VM — only the exit path differs.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| pci_enable_device | Sets Memory Space Enable — device becomes accessible |
| pci_request_regions | Exclusive BAR ownership (resource management) |
| pci_iomap | BAR physical → kernel virtual mapping (ioremap) |
| readl/writel | Architecture-safe MMIO access (volatile + barriers) |
| devm_kzalloc | Device-managed allocation (auto-freed on remove) |
| goto error cleanup | Standard kernel error handling pattern |
| pci_set/get_drvdata | Attach private data to pci_dev for use across probe/remove |

## What changed

From Step 28:
- **driver/microkvm_pci.c**: `struct microkvm_dev`, `REG_STATUS`, full probe with enable/regions/iomap/readl, proper remove with reverse cleanup

No VMM changes.

## Next step

[Step 30: DMA-capable driver](step30_dma-driver.md) allocates a coherent DMA buffer, builds a descriptor, and kicks the doorbell — transferring data from the driver through the VMM's DMA engine.
