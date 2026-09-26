# Step 29: Minimal PCI ドライバ — probe と remove

> **Phase F: Linux PCI Driver**
>
> Phase E でデバイスモデル（VMM 側）を構築した。Phase F ではドライバ（guest kernel 側）を書く。
> 両者を合わせて仮想 PCI デバイスの「ハードウェアエミュレーション」と「それを操作するソフトウェア」の両側が完成する。

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
KDIR := $(HOME)/linux-src

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
                                                       (この probe はログ表示のみ。
                                                        PCI BAR にはアクセスしない)

rmmod microkvm_pci
  → pci_unregister_driver()
    → remove()
```

## 実装

### 前提条件

guest 用 kernel を `~/linux-src`、microkvm を `~/microkvm` に配置している前提で、以下をホストで実行する。ドライバは guest に使う kernel と同じビルドツリーでビルドする。

**1. kernel config を有効化して再ビルド**

Phase F（step29〜32）で必要になる kernel config をまとめて有効化する。

```bash
$ cd ~/linux-src
$ scripts/config --enable CONFIG_MODULES
$ scripts/config --enable CONFIG_MODULE_UNLOAD
$ scripts/config --enable CONFIG_PCI_MSI
$ make olddefconfig

# 全て =y になっていることを確認
$ grep -E 'CONFIG_MODULES=|CONFIG_MODULE_UNLOAD=|CONFIG_PCI_MSI=' .config

$ make -j"$(nproc)" bzImage
$ make -j"$(nproc)" modules
$ cp arch/x86/boot/bzImage ~/microkvm/bzImage
```

`make modules` で、外部モジュールのシンボル解決に使う `Module.symvers` も生成する。

**2. driver をビルド**

```bash
$ cd ~/microkvm/driver
$ make
$ ls -l microkvm_pci.ko
```

**3. initramfs に組み込む**

既存の initramfs を新しい作業ディレクトリに展開し、`.ko` とモジュール操作用の BusyBox リンクを追加して再パックする。

```bash
$ initramfs_work=$(mktemp -d /tmp/microkvm-initramfs.XXXXXX)
$ cd "$initramfs_work"
$ sudo sh -c "zcat $HOME/microkvm/initramfs.gz | cpio -idm"
$ sudo cp ~/microkvm/driver/microkvm_pci.ko lib/modules/
$ sudo sh -c "find . | cpio -o -H newc | gzip > $HOME/microkvm/initramfs.gz"
$ sudo chown $(id -un):$(id -gn) ~/microkvm/initramfs.gz

# 組み込み確認
$ zcat ~/microkvm/initramfs.gz | cpio -t | grep microkvm_pci
```

**4. 起動して検証**

```bash
$ cd ~/microkvm
$ ./microkvm
```

起動後、guest で「出力」の `insmod` → `lsmod` → `rmmod` を実行する。

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
    dev_info(&pdev->dev, "microkvm_pci: remove called\n");
}

static const struct pci_device_id microkvm_pci_ids[] = {
    { PCI_DEVICE(MICROKVM_VENDOR, MICROKVM_DEVICE) },
    { 0, }  /* sentinel — テーブル終端 */
};
MODULE_DEVICE_TABLE(pci, microkvm_pci_ids);
```

`PCI_DEVICE()` は vendor/device ID を指定するマクロ。subvendor/subdevice は `PCI_ANY_ID`、class_mask は 0 となり、サブシステム ID とクラスを照合条件に含めない。

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
KDIR := $(HOME)/linux-src

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
microkvm_pci 0000:00:00.0: microkvm_pci: remove called
```

`probe called` は ID が一致して `probe()` が呼ばれたこと、`lsmod` はモジュールのロード、`remove called` はアンロード時の `remove()` 呼び出しを示す。

> **Note:** この時点ではドライバは*attach* しただけ。デバイスの有効化や BAR アクセスはまだ — Step 30 で行う。

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
| ID match と binding | ID が一致し、probe が成功するとデバイスにドライバが結び付く |

## 変わったこと

Step 28 からの変更:
- **新規ファイル**: `driver/Makefile`, `driver/microkvm_pci.c`

VMM コードの変更なし — このステップは純粋に guest 側のみ。

## 次のステップ

[Step 30: BAR mapping](step30_bar-mapping.md) では probe を拡張してデバイスを有効化し、BAR0 を kernel アドレス空間に map、`readl` で STATUS レジスタを読む — ドライバから VMM までの end-to-end MMIO を確認する。
