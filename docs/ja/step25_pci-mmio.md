# Step 25: PCI MMIO デバイスレジスタ via BAR0

## 目的

Step 24 で Linux が BAR0 に割り当てたアドレスの先に実際のデバイスレジスタを実装する。Guest が BAR0 + offset にアクセスすると KVM_EXIT_MMIO で VMM に到達し、デバイス状態で応答する。

## 背景

### Config space とデバイスレジスタ

Step 24 は PCI *config space* — CF8/CFC I/O ポート経由の256 byteの識別・設定領域を実装した。今回はデバイスの実際の*操作用レジスタ*を実装する。Linux が BAR0 に書いたアドレスでの MMIO アクセス。

PCI デバイスへの2つのアクセス経路:
```
Config space (CF8/CFC):  発見、識別、リソース割り当て → KVM_EXIT_IO
デバイスレジスタ (BAR):  実際のデバイス操作           → KVM_EXIT_MMIO
```

### BAR アドレス → MMIO

今回の実行では Linux が BAR0 (4 KiB) に `0x08000000` を割り当てている。この領域は guest RAM のメモリスロット外なので:
- GPA 0x08000000–0x08000FFF へのアクセス → EPT violation / EPT misconfiguration → KVM の MMIO 処理 → KVM_EXIT_MMIO → VMM が処理

これは Step 5 (MMIO hole) や Phase C (virtio-mmio) と同じメカニズム。違いは*誰がアドレスを決めるか*:
- Step 5: VMM が 0xD0000 をハードコード
- Phase C: VMM が kernel cmdline で指定
- Phase E: **Linux が BAR probing でサイズを調べ、リソース割り当てでアドレスを決定**

### デバイスレジスタのレイアウト

最小限の PCI デバイスは BAR0 からの固定オフセットに数個のレジスタを公開する:

```
BAR0 + 0x00: STATUS    (R)   デバイス ready フラグ
BAR0 + 0x04: DOORBELL  (W)   デバイスに処理開始を通知
BAR0 + 0x08: RESULT    (R)   結果用レジスタ（このステップでは固定値）
```

このデバイスでは、状態確認・処理開始の通知・結果の読み取りを3つのレジスタで表す。STATUS は ready を示す `0x01`、RESULT は `0x42` を常に返す。

### Doorbell

Doorbell レジスタは「トリガー」— 書き込むことでデバイスに作業開始を合図する。書き込む値自体に意味がないことも多い（write が発生すること自体がトリガー）。Phase C の virtio `QueueNotify` と同じ概念。

このステップでは doorbell はログメッセージを出力するだけで、STATUS や RESULT の値は変化しない。Step 26 で guest RAM から DMA descriptor を読み、一括データ転送を行うようになる。

### devmem

`devmem` は `/dev/mem` 経由で物理アドレスに直接 read/write する busybox ユーティリティ:
```
devmem 0x08000000        → GPA の 32-bit read
devmem 0x08000004 32 0x1 → 32-bit write of 0x1
```

## 実行フロー

```
Guest (userspace)            KVM                VMM (microkvm)
─────────────────            ───                ────────────────
devmem 0x08000000
  → GPA 0x08000000 を read
                             EPT による VM exit
                             KVM_EXIT_MMIO
                             addr=0x08000000
                             is_write=0
                                                bar0 = pci_bar0_addr()
                                                offset = addr - bar0 = 0x00
                                                val = pci_dev_mmio_read(0x00)
                                                → STATUS = 0x01
                             guest に返却
  → 0x01 を受信

devmem 0x08000004 32 0x1
  → GPA 0x08000004 に 0x1 write
                             KVM_EXIT_MMIO
                             is_write=1
                                                offset = 0x04
                                                pci_dev_mmio_write(0x04, 1)
                                                → DOORBELL kicked
```

## 実装

### pci.h — レジスタオフセット定義

```c
/* Device registers within BAR0 MMIO region (offsets from BAR0 base) */
#define PCI_DEV_REG_STATUS      0x00
#define PCI_DEV_REG_DOORBELL    0x04
#define PCI_DEV_REG_RESULT      0x08
```

### pci.c — BAR0 アドレスヘルパーと MMIO handler

```c
/* BAR0 を4 KiB境界に揃え、下位12ビットを除く */
uint32_t pci_bar0_addr(struct pci_device *dev) {
    return *(uint32_t *)&dev->config[0x10] & 0xFFFFF000;
}

/* Device MMIO register write — BAR0 region */
void pci_dev_mmio_write(struct pci_device *dev, uint64_t offset, uint32_t value)
{
    fprintf(stderr, "[pci-dev] MMIO write offset=0x%02lx ← 0x%x\n",
        (unsigned long)offset, value);
    switch (offset) {
    case PCI_DEV_REG_DOORBELL:
        fprintf(stderr, "[pci-dev] doorbell kicked!\n");
        break;
    default:
        break;
    }
}

/* Device MMIO register read — BAR0 region */
uint32_t pci_dev_mmio_read(struct pci_device *dev, uint64_t offset)
{
    uint32_t val = 0;
    switch (offset) {
    case PCI_DEV_REG_STATUS:
        val = 0x01;
        break;  /* ready */
    case PCI_DEV_REG_RESULT:
        val = 0x42;
        break;  /* result */
    default:
        break;
    }
    fprintf(stderr, "[pci-dev] MMIO read  offset=0x%02lx → 0x%x\n",
        (unsigned long)offset, val);
    return val;
}
```

### microkvm.c — BAR0 MMIO routing

virtio-mmio 領域チェックの `else` ブランチとして追加:

```c
} else {
    /* PCI BAR0 MMIO — device registers accessed via BAR0 address */
    uint32_t bar0 = pci_bar0_addr(&pci_dev);
    if (bar0 && addr >= bar0 && addr < bar0 + PCI_BAR0_SIZE) {
        uint64_t offset = addr - bar0;
        if (run->mmio.is_write) {
            uint32_t val = 0;
            memcpy(&val, run->mmio.data, run->mmio.len);
            pci_dev_mmio_write(&pci_dev, offset, val);
        } else {
            uint32_t val = pci_dev_mmio_read(&pci_dev, offset);
            memcpy(run->mmio.data, &val, run->mmio.len);
        }
    }
}
```

BAR0 アドレスは config space から実行時に読み取る — ハードコードではない。Linux が割り当てるまで VMM はアドレスを知らない。これが PCI の動的な特性。

## 出力

```
/ # devmem 0x08000000
[pci-dev] MMIO read  offset=0x00 → 0x1
0x00000001

/ # devmem 0x08000004 32 0x1
[pci-dev] MMIO write offset=0x04 ← 0x1
[pci-dev] doorbell kicked!

/ # devmem 0x08000008
[pci-dev] MMIO read  offset=0x08 → 0x42
0x00000042
```

各アクセスの意味:
- `0x08000000` = BAR0 + 0x00 = STATUS read → 0x01（device ready）
- `0x08000004` = BAR0 + 0x04 = DOORBELL write → 処理トリガー
- `0x08000008` = BAR0 + 0x08 = RESULT read → 0x42（placeholder、Step 26 で DMA 結果に変更）

## 重要な知見

PCI デバイスレジスタは BAR の背後の MMIO アドレス空間に存在する。guest はそれがエミュレートされていることを知らない（気にしない）— 単に物理アドレスを read/write するだけ。VMM が KVM_EXIT_MMIO で傍受してハードウェアの錯覚を提供する。Step 5 と同じ trap-and-emulate パターンだが、今回はアドレスを guest OS 自身が BAR 割り当てで選んだ。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| BAR → MMIO mapping | Linux の BAR assignment がデバイスレジスタの base address になる |
| Device register pattern | 状態確認・通知・結果をレジスタに分ける |
| MMIO exit routing | VMM が GPA を BAR0 値と比較して正しいデバイスに routing |
| Doorbell | 書き込み専用のトリガーレジスタ（virtio QueueNotify と同じ概念）|
| devmem | ドライバなしでデバイスをテストする物理アドレス直接アクセス |

## 変わったこと

Step 24 からの変更:
- **pci.h**: デバイスレジスタ offset defines, `pci_bar0_addr()`, `pci_dev_mmio_read/write` 宣言
- **pci.c**: `pci_bar0_addr()`, `pci_dev_mmio_read()`, `pci_dev_mmio_write()` 実装
- **microkvm.c**: KVM_EXIT_MMIO handler に BAR0 MMIO routing 追加

## 次のステップ

[Step 26: DMA simulation](step26_dma.md) では doorbell handler を拡張し、guest RAM から descriptor を読んで直接メモリアクセスを行う — バイトごとの MMIO exit なしでデータ転送する。
