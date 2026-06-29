# Step 29: BAR mapping — pci_enable_device, pci_iomap, readl

## 目的

ドライバの probe を拡張して PCI デバイスを有効化し、BAR0 を kernel 仮想アドレス空間に map、`readl` で STATUS レジスタを読む — ドライバから KVM を経由して VMM デバイスモデルまでの end-to-end MMIO を確認する。

## 背景

### PCI デバイス有効化シーケンス

ドライバがデバイスレジスタにアクセスする前に、3つのステップが必要:

```
pci_enable_device()      → Memory Space Enable を設定 (Command register bit 1)
pci_request_regions()    → BAR 領域を排他的に確保（他ドライバとの競合防止）
pci_iomap()              → BAR 物理アドレスを kernel 仮想アドレスに map (ioremap)
                           `void __iomem *` を返す — readl/writel 経由でのみアクセス可
```

3つ全て完了して初めて `readl`/`writel` でデバイスレジスタにアクセスできる。

### なぜ readl/writel を使うか（ポインタ参照ではダメな理由）

MMIO レジスタは通常のメモリではない:
- 各 read/write に副作用がある（デバイス状態が変わる）
- コンパイラがアクセスを最適化で削除・並び替えしてはいけない
- アーキテクチャ固有の memory barrier が必要

`readl`/`writel` はこれら全てを保証する。

### pci_request_regions

```c
pci_request_regions(pdev, "microkvm_pci");
```

複数のドライバが同じ BAR を同時に claim することを防ぐ。他のドライバが既にこの領域を所有していると呼び出しが失敗する — Linux の I/O アドレス空間 resource management。

### Phase E Step 24 との対応

| Step 24 (VMM) | Step 29 (Driver) |
|---|---|
| KVM_EXIT_MMIO を受信 | readl/writel を発行 |
| `pci_dev_mmio_read()` が値を返す | `readl()` が値を受け取る |
| BAR0 GPA range を管理 | `pci_iomap()` で BAR0 を map |
| `switch (offset)` で振り分け | `bar0 + REG_STATUS` で offset 指定 |

### devm_kzalloc

```c
mdev = devm_kzalloc(&pdev->dev, sizeof(*mdev), GFP_KERNEL);
```

Device-managed allocation — remove 時に自動解放。`kfree()` を手動で呼ぶ必要がない。

### pci_set_drvdata / pci_get_drvdata

ドライバ固有データ (`microkvm_dev`) を PCI デバイス構造体に紐付ける。probe で確保したリソースを remove で取り出すために使う。

## 実行フロー

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
  → GPA 0x08000000 にアクセス
                             EPT violation
                             KVM_EXIT_MMIO
                             addr=0x08000000
                                                pci_dev_mmio_read(0x00)
                                                → 0x01 (ready) を返す
  → status = 0x01
```

## 実装

### driver/microkvm_pci.c（主要な追加部分）

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

    /* Enable device — Command register の Memory Space Enable をセット */
    pci_enable_device(pdev);

    /* BAR 領域を排他的に確保 */
    pci_request_regions(pdev, "microkvm_pci");

    /* BAR0 を kernel 仮想アドレス空間に map */
    mdev->bar0 = pci_iomap(pdev, 0, 0);

    pci_set_drvdata(pdev, mdev);

    /* STATUS レジスタ読み取り — VMM 側で KVM_EXIT_MMIO が発生 */
    u32 status = readl(mdev->bar0 + REG_STATUS);
    dev_info(&pdev->dev, "STATUS = 0x%x\n", status);
}

static void microkvm_remove(struct pci_dev *pdev)
{
    struct microkvm_dev *mdev = pci_get_drvdata(pdev);

    /* probe の逆順でリソースを解放 */
    pci_iounmap(pdev, mdev->bar0);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
}
```

エラーハンドリングは標準の kernel `goto` クリーンアップパターン。

## 出力

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

VMM ログが全パスを示す: ドライバが `readl` → KVM_EXIT_MMIO → VMM の `pci_dev_mmio_read` が 0x01 を返す → ドライバが `STATUS = 0x1` を受信。

## 重要な知見

`pci_iomap` + `readl` は MMIO アクセスの標準 Linux API。ドライバから見ると単に memory-mapped アドレスを読むだけ。VM 内部では EPT violation → VM exit → VMM エミュレーション → 結果返却が起きている。ドライバコードは物理ハードウェアでも VM でも同一 — 異なるのは exit パスだけ。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| pci_enable_device | Memory Space Enable — デバイスがアクセス可能になる |
| pci_request_regions | BAR の排他的所有権（resource management） |
| pci_iomap | BAR 物理 → kernel 仮想の mapping (ioremap) |
| readl/writel | アーキテクチャ安全な MMIO アクセス (volatile + barrier) |
| devm_kzalloc | Device-managed allocation（remove 時に自動解放） |
| goto error cleanup | 標準的な kernel エラーハンドリングパターン |
| pci_set/get_drvdata | probe/remove をまたいだ private data の受け渡し |

## 変わったこと

Step 28 からの変更:
- **driver/microkvm_pci.c**: `struct microkvm_dev`, `REG_STATUS`, enable/regions/iomap/readl を含む完全な probe, 逆順 cleanup の remove

VMM の変更なし。

## 次のステップ

[Step 30: DMA-capable driver](step30_dma-driver.md) では coherent DMA buffer を確保し、descriptor を構築して doorbell を kick — VMM の DMA エンジンを通じてデータを転送する。
