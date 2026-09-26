# Step 32: MSI-X 割り込みハンドラ — completion 駆動の DMA

## 目的

DMA の完了を MSI-X 割り込みと completion で確認してから RESULT レジスタを読む。ドライバが MSI-X handler を登録し、DMA 完了時に待機スレッドを起床 — VMM の `KVM_SIGNAL_MSI` から guest LAPIC を経由してドライバの handler まで、end-to-end の割り込み配送を実証する。

## 背景

### RESULT の直接読み取りから割り込みによる完了待ちへ

Step 31 は doorbell 直後に RESULT を読んだ — microkvm の DMA が同期的（VMM が MMIO exit から返る前に完了）なので動く。しかし実デバイスは非同期に完了する。MSI-X によりデバイスが「完了した」とドライバに通知できる。

```
Step 31: doorbell → DMA → readl(RESULT)
Step 32: doorbell → DMA → MSI-X IRQ → complete() → 完了確認 → readl(RESULT)
```

### MSI-X ドライバ API

```c
pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSIX);    /* MSI-X vector を1本確保 */
irq = pci_irq_vector(pdev, 0);                      /* Linux IRQ 番号を取得 */
request_irq(irq, handler, 0, "name", data);         /* handler を登録 */
```

`pci_irq_vector(pdev, 0)` の 0 は割り当てた vector のインデックスで、戻り値は Linux IRQ 番号。MSI メッセージ内の CPU vector 番号とは区別する。

`pci_alloc_irq_vectors` を呼ぶと Linux が MSI-X テーブル (BAR0 + 0x800) に LAPIC アドレスと vector 番号を書く。VMM はこれを記録し、DMA 完了後に `KVM_SIGNAL_MSI` で使う。

### completion

```c
struct completion dma_done;
init_completion(&dma_done);

/* プロセスコンテキスト: doorbell kick 後に待機 */
wait_for_completion_timeout(&dma_done, HZ * 5);

/* 割り込みコンテキスト: handler が通知 */
complete(&dma_done);
```

`completion` は割り込みハンドラ（atomic context）からプロセスコンテキスト（sleepable）への同期の標準メカニズム。spinlock + wait queue で実装。

```
状態遷移:
  init_completion() → 未完了
  未完了で wait      → complete() またはタイムアウトまで待機
  complete()        → 完了を記録し、待機中なら起床
  完了後に wait      → sleep せずに戻る
```

`complete()` が待機開始より先に呼ばれても、完了は記録される。`wait_for_completion_timeout(..., HZ * 5)` は最大5秒待ち、戻り値が 0 ならタイムアウト、正なら完了を表す。

### なぜ MP table が必要か

この guest 環境では、Linux が PIC のみの割り込み構成で起動すると MSI-X vector を確保できなかった。VMM が Intel MP table を guest RAM (GPA 0xF0000, e820 reserved 領域) に配置し、Linux に IOAPIC を含む割り込み構成を伝える。MSI-X の配送自体は IOAPIC を経由せず、LAPIC に届く。

MP table の内容:
- 1 processor (BSP)
- 2 buses (ISA + PCI)
- 1 IOAPIC (addr 0xFEC00000)
- 16 ISA interrupt routing entries

### Phase E Step 27 との対応

| Step 27 (VMM) | Step 32 (Driver) |
|---|---|
| MSI-X table の MMIO write を記録 | pci_alloc_irq_vectors → Linux が table に書く |
| DMA 完了後に KVM_SIGNAL_MSI | request_irq → handler が実行される |
| "No irq handler" (Step 27) | handler 登録済み → IRQ_HANDLED |
| devmem で手動設定 | Linux IRQ subsystem が自動設定 |

## 実行フロー

```
Guest (driver)               KVM                VMM
──────────────               ───                ───
pci_alloc_irq_vectors()
  → Linux が MSI-X table に書く:
    addr=0xFEE00000, data=0x22（出力例）
                             KVM_EXIT_MMIO
                                                pci_msix_write(): addr/data を記録

request_irq(handler)

writel(1, DOORBELL)
                             KVM_EXIT_MMIO
                                                DMA 実行
                                                KVM_SIGNAL_MSI(addr, data)
                             vector 0x22 を注入

microkvm_irq_handler():
  readl(STATUS)
  complete(&dma_done)

wait_for_completion_timeout() が完了を確認して返る
  readl(RESULT) → 18 bytes
```

## 実装

### 前提条件とビルド

MSI-X に必要な `CONFIG_PCI_MSI` は [Phase F 冒頭（step29）](step29_pci-driver.md#前提条件) で有効済み。ここでは MP table を追加した VMM と、割り込み対応の driver をビルドする。

```bash
# MP table を追加した VMM と driver をビルド
$ cd ~/microkvm
$ make
$ cd driver
$ make
```

[Step 29 の initramfs 組み込み手順](step29_pci-driver.md#前提条件)で `.ko` を差し替え、`~/microkvm` で `./microkvm` を起動する。

### VMM 変更 (platform.c + boot.c)

新規ファイル `platform.c` / `platform.h` — MP table セットアップ:
```c
void setup_mp_table(void *mem) {
    /* MP Floating Pointer at 0xF0000 */
    /* MP Config Table at 0xF0010:
       - 1 processor, 2 buses, 1 IOAPIC, 16 IRQ entries */
}
```

`boot.c` の `load_initramfs()` から呼び出し。e820 で 0x9F000–0x100000 を reserved にする設定も必要（boot.c に実装済み）。

### ドライバ変更 (microkvm_pci.c)

```c
#include <linux/interrupt.h>
#include <linux/completion.h>

struct microkvm_dev {
    ...
    struct completion dma_done;
};

static irqreturn_t microkvm_irq_handler(int irq, void *data)
{
    struct microkvm_dev *mdev = data;
    u32 status = readl(mdev->bar0 + REG_STATUS);

    dev_info(&mdev->pdev->dev, "IRQ: status=0x%x\n", status);
    complete(&mdev->dma_done);
    return IRQ_HANDLED;
}

/* probe 内（mdev 確保後）: */
    mdev->pdev = pdev;
    init_completion(&mdev->dma_done);

/* BAR mapping と pci_set_master() の後: */
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

/* doorbell 後: */
    /* Wait for completion via MSI-X interrupt */
    if (!wait_for_completion_timeout(&mdev->dma_done, HZ * 5)) {
        dev_err(&pdev->dev, "DMA timeout!\n");
    } else {
        result = readl(mdev->bar0 + REG_RESULT);
        dev_info(&pdev->dev, "DMA done via MSI-X, transferred %u bytes\n", result);
    }

/* remove 内: */
    free_irq(pci_irq_vector(pdev, 0), mdev);
    pci_free_irq_vectors(pdev);
```

IRQ 確保失敗時は `err_iomap`、handler 登録失敗時は `err_irq_vec`、DMA バッファ確保失敗時は `err_irq` へ進み、取得済みのリソースを解放する。remove では handler と vector を解放してから DMA バッファと BAR を解放する。

## 出力

```
/ # insmod /lib/modules/microkvm_pci.ko
[pci-msix] table write offset=0x800 val=0xfee00000
[pci-msix] table write offset=0x808 val=0x22
[pci-dev] MMIO read  offset=0x00 → 0x1
microkvm_pci 0000:00:00.0: STATUS = 0x1
[pci-dev] MMIO write offset=0x0c ← 0x4136000
[pci-dev] MMIO write offset=0x10 ← 0x0
[pci-dev] MMIO write offset=0x04 ← 0x1
[pci-dma] DMA read: 18 bytes from GPA 0x4136010
hello from driver
[pci-msix] IRQ injected: addr=0xfee00000 data=0x22
[pci-dev] MMIO read  offset=0x00 → 0x1
microkvm_pci 0000:00:00.0: IRQ: status=0x1
[pci-dev] MMIO read  offset=0x08 → 0x12
microkvm_pci 0000:00:00.0: DMA done via MSI-X, transferred 18 bytes
/ # rmmod microkvm_pci
microkvm_pci 0000:00:00.0: remove called
```

全フロー: Linux が MSI-X table をプログラム → ドライバが doorbell kick → VMM が DMA 実行 → VMM が MSI-X 注入 → handler 起動 → completion で完了を確認 → ドライバが result 確認。

## 重要な知見

ドライバは doorbell で処理を投入し、handler の `complete()` で完了を確認してから RESULT を読む。完了待ちと割り込み通知を completion で結び付けることで、通知が待機開始の前後どちらに届いても処理を進められる。この環境では、MSI-X を Linux のドライバから利用するために MP table による割り込み構成の提示も追加した。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| pci_alloc_irq_vectors | Linux が MSI-X table に LAPIC addr + vector を書く |
| request_irq | MSI-X vector の handler 登録 |
| completion | 割り込みハンドラ → プロセスコンテキストの同期 |
| wait_for_completion_timeout | 完了済みなら即座に戻り、未完了なら最大5秒待つ |
| MP table | IOAPIC 発見のための platform topology（BIOS/ACPI 非提供時） |
| e820 reserved | MP table 領域を Linux に上書きされないよう保護 |
| IRQ_HANDLED | 割り込み処理完了の報告 |
| End-to-end path | ドライバ → VMM DMA → KVM_SIGNAL_MSI → LAPIC → handler |

## 変わったこと

Step 31 からの変更:
- **新規ファイル**: `platform.c`, `platform.h`（MP table セットアップ）
- **boot.c**: `#include "platform.h"`, `setup_mp_table(mem)` 呼び出し, boot.h に `MP_TABLE_ADDR`
- **driver/microkvm_pci.c**: `<linux/interrupt.h>`, `<linux/completion.h>`, IRQ handler, `pci_alloc_irq_vectors`, `request_irq`, `wait_for_completion_timeout`, remove にクリーンアップ
- **Makefile**: `platform.c` 追加

## 次のステップ

Phase F 完了。ドライバの全ライフサイクル:

```
Step 29: Probe (ID match)
Step 30: BAR mapping (readl/writel)
Step 31: DMA (descriptor + doorbell)
Step 32: MSI-X (interrupt-driven completion)
```

microkvm は仮想 PCI デバイスの**両側** — VMM デバイスモデルと guest kernel ドライバ — を実装し、ドライバ投入から DMA 経由の割り込み駆動完了までの完全なデータパスを実証する。
