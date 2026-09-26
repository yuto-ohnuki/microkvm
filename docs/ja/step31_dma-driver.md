# Step 31: DMA 対応ドライバ — dma_alloc_coherent と doorbell

## 目的

ドライバを拡張して DMA を実行する: coherent buffer を確保し、descriptor を構築してデバイスにアドレスを伝え、doorbell を kick。VMM が doorbell の MMIO exit 処理内で guest RAM から descriptor と payload を読み、転送結果を記録する。

## 背景

### なぜドライバから DMA が必要か

Step 30 は `readl` で1レジスタずつ読んだ（1アクセス = 1 VM exit）。大量データ転送では DMA がバイトごとの exit を回避:

```
MMIO で payload を転送する場合: 32-bit アクセスごとに MMIO exit
この DMA の投入:               3 MMIO exits (DESC_LO + DESC_HI + DOORBELL)
```

この probe では STATUS と RESULT も各1回読むため、上記の投入処理と合わせて 5 回の MMIO exit が発生する。payload 自体は guest RAM から読み出すので、バイトごとの MMIO exit は発生しない。

### dma_alloc_coherent

```c
void *dma_alloc_coherent(struct device *dev, size_t size,
                         dma_addr_t *dma_handle, gfp_t flag);
```

同じ物理メモリに対して2つのアドレスを返す:
- `void *` — kernel 仮想アドレス（CPU が使う）
- `dma_addr_t` — DMA アドレス（デバイスが使う）

"Coherent" は CPU とデバイス間で、明示的なキャッシュ同期なしに同じバッファを共有できること。ドライバが kernel VA 経由でデータを書き、VMM (デバイス) がバスアドレス (= microkvm では GPA) 経由で読む。

> **Note:** この教育実装では IOMMU がないため `dma_addr_t` は実質的に guest 物理アドレス。実ハードウェアで IOMMU がある場合、DMA アドレスは IOMMU が変換する I/O 仮想アドレスになる。

### バッファレイアウト

```
dma_buf (4096 bytes):
┌──────────────────────────┐  offset 0x00
│ struct microkvm_dma_desc │
│   .addr = dma_addr + 16  │  → 下の payload を指す
│   .len  = 18             │
│   .flags = 0 (TX)        │
├──────────────────────────┤  offset 0x10 (sizeof desc)
│ payload:                 │
│ "hello from driver\n"    │
└──────────────────────────┘
```

Descriptor と payload が1つの allocation を共有 — descriptor の `.addr` が同じバッファ内を指す。

### pci_set_master

```c
pci_set_master(pdev);  /* Command register bit 2 = Bus Master Enable */
```

PCI デバイスが DMA を開始できるよう、Bus Master Enable を設定する。出力の Command register = `0x6` は、Memory Space Enable（bit 1）と Bus Master Enable（bit 2）が立った状態。

### dma_alloc_coherent vs kmalloc

| API | 誰がアクセスできるか | 用途 |
|-----|---------------------|------|
| `kmalloc()` | CPU（DMA に使う場合は別途 DMA mapping） | 通常の kernel データ構造 |
| `dma_alloc_coherent()` | CPU + デバイス | DMA 用の共有バッファ（descriptor, data） |

`kmalloc` で確保したバッファの物理アドレスはデバイスから使えるとは限らない。`dma_alloc_coherent` は DMA アドレスも返す。キャッシュ一貫性とアクセス順序は別で、descriptor を書いてから `writel()` で doorbell を通知する。

### Phase E Step 26 との対応

| Step 26 (VMM) | Step 31 (Driver) |
|---|---|
| guest RAM から descriptor を読む | coherent buffer に descriptor を構築 |
| desc.addr で payload を特定 | desc.addr = dma_addr + sizeof(desc) |
| doorbell で DMA 実行 | writel(1, bar0 + REG_DOORBELL) |
| last_dma_len に結果を記録 | readl(bar0 + REG_RESULT) で確認 |

## 実行フロー

```
Guest (driver)               KVM                VMM
──────────────               ───                ───
dma_alloc_coherent()
  → GPA X に 4096 bytes

GPA X に descriptor 構築:
  addr = X + 16, len = 18, flags = 0
X + 16 に "hello from driver\n" 書き込み

writel(lower_32_bits(X), DESC_LO)
                             KVM_EXIT_MMIO
                                                desc_addr の下位 32-bit を設定
writel(upper_32_bits(X), DESC_HI)
                             KVM_EXIT_MMIO
                                                desc_addr の上位 32-bit を設定

readl(STATUS)
                             KVM_EXIT_MMIO
                                                → 0x1 を返す

writel(1, DOORBELL)
                             KVM_EXIT_MMIO
                                                RAM[X] から desc 読み取り
                                                RAM[X+16] から payload 読み取り
                                                write(stdout, "hello from driver\n")
                                                last_dma_len = 18

readl(RESULT)
                             KVM_EXIT_MMIO
                                                → 18 を返す
  → result = 18
```

## 実装

### driver/microkvm_pci.c の主要追加部分

```c
#include <linux/dma-mapping.h>

#define REG_DOORBELL  0x04
#define REG_RESULT    0x08
#define REG_DESC_LO   0x0C
#define REG_DESC_HI   0x10

struct microkvm_dma_desc {
    u64 addr;
    u32 len;
    u32 flags;  /* 0=device reads from guest (TX) */
};

struct microkvm_dev {
    ...
    void *dma_buf;
    dma_addr_t dma_addr;
};
```

probe 内:
```c
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
```

DMA バッファの確保に失敗した場合は `err_iomap` から BAR mapping を解除し、Step 30 の解放処理へ進む。

```c
err_iomap:
    pci_iounmap(pdev, mdev->bar0);
err_regions:
    pci_release_regions(pdev);
err_disable:
    pci_disable_device(pdev);
    return ret;
```

remove 内（BAR mapping を解除する前）:
```c
    dma_free_coherent(&pdev->dev, 4096, mdev->dma_buf, mdev->dma_addr);
```

## 出力

[Step 29 の手順](step29_pci-driver.md#前提条件)で driver を再ビルドし、initramfs 内の `.ko` を差し替えて起動する。

```
/ # insmod /lib/modules/microkvm_pci.ko
[pci] config write offset=0x04 ← 0x6 (len=2)
[pci-dev] MMIO write offset=0x0c ← 0x4137000
[pci-dev] MMIO write offset=0x10 ← 0x0
[pci-dev] MMIO read  offset=0x00 → 0x1
microkvm_pci 0000:00:00.0: STATUS = 0x1
[pci-dev] MMIO write offset=0x04 ← 0x1
[pci-dma] DMA read: 18 bytes from GPA 0x4137010
hello from driver
[pci-dev] MMIO read  offset=0x08 → 0x12
microkvm_pci 0000:00:00.0: DMA complete, transferred 18 bytes
/ # rmmod microkvm_pci
microkvm_pci 0000:00:00.0: remove called
```

この実装は doorbell 処理中に転送と RESULT の更新を終えるため、ドライバは直後に RESULT を1回読んで確認する。

全データパス: ドライバが coherent memory に descriptor 構築 → デバイスにアドレス通知 → doorbell kick → VMM が descriptor + payload を読み取り → "hello from driver" 出力 → ドライバが RESULT レジスタで確認。

## 重要な知見

ドライバから見た DMA は: バッファ確保 → descriptor 埋め → デバイスにアドレスを伝える → kick。デバイス (VMM) が残りを処理する。ドライバは MMIO レジスタ経由でデータをバイトごとにコピーしない。descriptor と doorbell を組み合わせる考え方は、NVMe や NIC のドライバにも使われる。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| dma_alloc_coherent | kernel VA とバスアドレスの両方を返すバッファ確保 |
| dma_addr_t | デバイスが使うアドレス（microkvm では GPA, IOMMU なし） |
| pci_set_master | Bus Master bit 有効化（DMA の権限） |
| Descriptor submission | desc 構築 → addr をデバイスに通知 → doorbell kick |
| lower/upper_32_bits | 64-bit DMA アドレスを2つの 32-bit register write に分割 |
| Coherent vs streaming | Coherent = キャッシュ管理不要 |
| MMIO exit 数 | この転送では payload サイズによらず投入3回、確認2回 |

## 変わったこと

Step 30 からの変更:
- **driver/microkvm_pci.c**: `#include <linux/dma-mapping.h>`, 全レジスタ defines, `struct microkvm_dma_desc`, `microkvm_dev` に DMA フィールド, `pci_set_master`, `dma_alloc_coherent`, descriptor build + submit, remove に `dma_free_coherent`

VMM の変更なし。

## 次のステップ

[Step 32: MSI-X interrupt handler](step32_msix-handler.md) では MSI-X による完了待ちを追加する。ドライバが MSI-X handler を登録し、DMA 完了時に待機スレッドを起床させる。
