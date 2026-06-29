# Step 28: Minimal PCI ドライバ — probe と remove

> **Phase F: Linux PCI Driver**
>
> Phase E でデバイスモデル（VMM 側）を構築した。Phase F ではドライバ（guest kernel 側）を書く。
> 両者を合わせて仮想 PCI デバイスの「ハードウェアエミュレーション」と「それを操作するソフトウェア」の
> 両側が完成する。

## 目的

microkvm PCI デバイス (vendor 0x1234, device 0x0001) に ID match する最小限の Linux kernel module を作成し、`insmod` で `probe` 関数が自動的に呼ばれることを確認する。

## 背景

### Phase E ↔ Phase F の接続

Phase E は*デバイス*を実装した — VMM が config space read に vendor=0x1234, device=0x0001 を返す。Phase F は*ドライバ*を実装する — 「この ID のデバイスを扱える」と宣言する guest kernel module。

両者は Vendor/Device ID で接続される:
```
VMM (pci.c):     config[0x00] = 0x1234, config[0x02] = 0x0001
Driver (.c):     #define MICROKVM_VENDOR 0x1234, #define MICROKVM_DEVICE 0x0001
```

これが一致しないと PCI core は probe を呼ばない。物理デバイスのドライバも同じ。

### Linux PCI ドライバのライフサイクル

```
1. Boot: PCI bus scan が各スロットの config space から vendor/device ID を読む
2. insmod: ドライバが pci_register_driver() で ID テーブルを登録
3. Match: PCI core が一致するデバイスを発見
4. Probe: ドライバの probe() が呼ばれる — デバイス初期化
5. rmmod: remove() が呼ばれる — デバイス解放
```

### module_pci_driver マクロ

```c
module_pci_driver(microkvm_pci_driver);
```

`module_init` + `module_exit` + `pci_register_driver` / `pci_unregister_driver` に展開される。シンプルな PCI ドライバの boilerplate を省略する。

### Out-of-tree module build

```makefile
obj-m := microkvm_pci.o
KDIR := $(HOME)/linux-src_v2

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
```

`-C $(KDIR)` で kernel の Makefile に制御を委譲。`M=$(PWD)` で「このディレクトリのモジュールをビルドせよ」と指示。`Module.symvers`（kernel tree で `make modules` して生成）が必要。

## 実行フロー

```
Guest (insmod)               PCI Core                  VMM
──────────────               ────────                  ───
insmod microkvm_pci.ko
  → pci_register_driver()
                             デバイスリストを走査
                             device 0000:00:00.0
                               vendor=0x1234 ✓
                               device=0x0001 ✓
                             → probe() を呼び出し

probe():
  dev_info("probe called")
                                                       (VMM との通信なし
                                                        — probe は純粋に kernel 内)

rmmod microkvm_pci
  → remove()
  → pci_unregister_driver()
```

## 実装

### 前提条件

- kernel config: `CONFIG_MODULES=y` + `CONFIG_MODULE_UNLOAD=y`
- kernel tree で `make modules`（`Module.symvers` 生成）
- `.ko` を initramfs の `/lib/modules/` に配置

### driver/microkvm_pci.c

```c
#include <linux/module.h>
#include <linux/pci.h>

/* Must match VMM's pci.h definitions */
#define MICROKVM_VENDOR  0x1234
#define MICROKVM_DEVICE  0x0001

/* Called when PCI core finds a device matching our ID table */
static int microkvm_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    dev_info(&pdev->dev, "microkvm_pci: probe called\n");
    return 0;
}

/* Called on rmmod or device removal */
static void microkvm_remove(struct pci_dev *pdev)
{
    dev_info(&pdev->dev, "remove called\n");
}

static const struct pci_device_id microkvm_pci_ids[] = {
    { PCI_DEVICE(MICROKVM_VENDOR, MICROKVM_DEVICE) },
    { 0, }  /* sentinel — テーブル終端 */
};
MODULE_DEVICE_TABLE(pci, microkvm_pci_ids);
```

`PCI_DEVICE()` は vendor/device ID を含む `struct pci_device_id` エントリに展開されるマクロ（他フィールドはワイルドカード match 用にゼロ）。

```c
static struct pci_driver microkvm_pci_driver = {
    .name     = "microkvm_pci",
    .id_table = microkvm_pci_ids,
    .probe    = microkvm_probe,
    .remove   = microkvm_remove,
};
module_pci_driver(microkvm_pci_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("microkvm PCI device driver");
```

### driver/Makefile

```makefile
obj-m := microkvm_pci.o
KDIR := $(HOME)/linux-src_v2

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
```

## 出力

```
/ # insmod /lib/modules/microkvm_pci.ko
microkvm_pci: loading out-of-tree module taints kernel.
microkvm_pci 0000:00:00.0: microkvm_pci: probe called
/ # lsmod
Module                  Size  Used by    Tainted: G
microkvm_pci           12288  0
/ # rmmod microkvm_pci
microkvm_pci 0000:00:00.0: remove called
```

`probe called` で確認: PCI core が vendor/device ID で VMM デバイスとドライバを match し、初期化関数を呼んだ。

> **Note:** この時点ではドライバは*attach* しただけ。デバイスの有効化や BAR アクセスはまだ — Step 29 で行う。

> **Note:** "taints kernel" は out-of-tree module で正常。デバッグ目的で kernel にマークが付くだけでエラーではない。

## 重要な知見

PCI ドライバの本質は「ID テーブル + 2つのコールバック」。kernel の PCI サブシステムがバス走査、ID マッチング、ライフサイクル管理の全てを担当する。ドライバは「何を扱えるか」を宣言し、probe/remove を提供するだけ。シンプルな教育モジュールから NVMe / NIC の production ドライバまで、全て同じ構造。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| pci_device_id テーブル | Vendor/Device ID によるマッチング（物理ドライバと同じ） |
| probe/remove | デバイスライフサイクルのコールバック |
| module_pci_driver | init/exit の boilerplate を省略するマクロ |
| Out-of-tree build | `make -C $(KDIR) M=$(PWD) modules` |
| Module.symvers | モジュールリンクに必要な kernel シンボルテーブル |
| ID match = binding | ID が一致して初めてドライバがデバイスを所有する |

## 変わったこと

Step 27 からの変更:
- **新規ファイル**: `driver/Makefile`, `driver/microkvm_pci.c`
- **`.gitignore`**: driver ビルド成果物のパターン

VMM コードの変更なし — このステップは純粋に guest 側のみ。

## 次のステップ

[Step 29: BAR mapping](step29_bar-mapping.md) では probe を拡張してデバイスを有効化し、BAR0 を kernel アドレス空間に map、`readl` で STATUS レジスタを読む — ドライバから VMM までの end-to-end MMIO を確認する。
