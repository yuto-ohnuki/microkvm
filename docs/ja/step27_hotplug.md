# Step 27: PCI hotplug — ランタイムでのデバイス追加/削除

## 目的

PCI hotplug をシミュレートし、ランタイムにデバイスの存在を切り替える。`Ctrl-A h` で2台目の PCI デバイスを出現/消失させ、guest が標準の sysfs インターフェース（`rescan`/`remove`）で認識/除去する。

## 背景

### PCI hotplug とは

PCI hotplug はシステム動作中にデバイスを追加/削除する機能。実際のユースケース:
- サーバーの NVMe ドライブのホットスワップ
- Thunderbolt デバイスの抜き差し
- クラウド block storage のアタッチ/デタッチ（guest に PCI デバイスとして見える）
- QEMU/libvirt の `virsh attach-device` / `virsh detach-device`

### PCI での「不在」の表現

Linux がバススロットを走査して vendor ID = 0xFFFF を読むと「デバイスなし」を意味する。これは PCI の標準シグナル — 物理バスでは空スロットを読むとデータラインを駆動するデバイスがないため all-ones が返る。

```
present=1: config read → Vendor=0x1234 → デバイスが存在
present=0: config read → Vendor=0xFFFF → 空スロット
```

### Native PCIe hotplug vs sysfs rescan

| | Native PCIe hotplug | sysfs rescan (microkvm) |
|---|---|---|
| 仕組み | Root port + slot control + 割り込み通知 | Guest が明示的にバスを再走査 |
| 複雑さ | 高（state machine, attention button, power control）| 低（flag toggle + rescan） |
| Guest の動作 | 自動検出 | 手動 `echo 1 > /sys/bus/pci/rescan` |

microkvm ではシンプルな sysfs 方式を採用。教育的な価値は同等 — guest 側の動作（enumeration、BAR allocation、removal）は同一。Production システムでは追加で Hot-Plug Controller 割り込みで OS に通知し、手動 rescan を不要にしている。

## 実行フロー

```
VMM (Ctrl-A h)               Guest
──────────────               ─────
                             / # ls /sys/bus/pci/devices/
                             0000:00:00.0

pci_hotplug_dev.present = 1
"[monitor] ADDED"
                             / # echo 1 > /sys/bus/pci/rescan
                             Linux がバスを probe:
                               device=1: vendor=0x1234 → 発見!
                               BAR0 probing → 0x08002000 を割り当て
                             / # ls /sys/bus/pci/devices/
                             0000:00:00.0  0000:00:01.0

pci_hotplug_dev.present = 0
"[monitor] REMOVED"
                             / # echo 1 > .../0000:00:01.0/remove
                             Linux がデバイスを sysfs から除去
                             / # ls /sys/bus/pci/devices/
                             0000:00:00.0
```

## 実装

### pci.h — present フラグ

```c
struct pci_device {
    ...
    /* hotplug */
    int present;    /* 0=absent (returns 0xFFFF), 1=present */
};
```

### pci.c — presence ガード

```c
void pci_config_write(struct pci_device *dev, ...) {
    if (!dev->present)
        return;   /* absent デバイスへの write は無視 */
    ...
}

uint32_t pci_config_read(struct pci_device *dev, ...) {
    /* Absent device: return all-ones (0xFFFF vendor = no device) */
    if (!dev->present) {
        if (len == 4) return 0xFFFFFFFF;
        else if (len == 2) return 0xFFFF;
        else return 0xFF;
    }
    ...
}
```

### pci.c — pci_init_hotplug()

```c
/* Initialize hotplug PCI device (device=1). Starts absent (present=0) */
void pci_init_hotplug(struct pci_device *dev) {
    memset(dev, 0, sizeof(*dev));
    *(uint16_t *)&dev->config[0x00] = 0x1234;
    *(uint16_t *)&dev->config[0x02] = 0x0002;   /* 別の device ID */
    dev->config[0x0B] = 0xFF;   /* class: unassigned */
    dev->bar0_mask = ~(PCI_BAR0_SIZE - 1);
    dev->present = 0;   /* 起動時は不在 */
}
```

### microkvm.c — config routing の汎用化

```c
struct pci_device *target = NULL;
if (bus == 0 && func == 0) {
    if (device == 0) target = &pci_dev;          /* 0000:00:00.0 */
    else if (device == 1) target = &pci_hotplug_dev;  /* 0000:00:01.0 */
}
if (target) {
    pci_config_read/write(target, ...)
}
```

### microkvm.c — Ctrl-A h モニタコマンド

```c
if (c == 'h') {
    pci_hotplug_dev.present = !pci_hotplug_dev.present;
    fprintf(stderr, "\n[monitor] PCI device 0000:00:01.0 %s\n",
        pci_hotplug_dev.present ? "ADDED ..." : "REMOVED ...");
}
```

### microkvm.c — hotplug デバイスの BAR0 MMIO routing

```c
} else {
    uint32_t bar0_hp = pci_bar0_addr(&pci_hotplug_dev);
    if (bar0_hp && addr >= bar0_hp && addr < bar0_hp + PCI_BAR0_SIZE) {
        /* pci_hotplug_dev に routing */
    }
}
```

## 出力

```
/ # ls /sys/bus/pci/devices/
0000:00:00.0

(Ctrl-A h を押す)
[monitor] PCI device 0000:00:01.0 ADDED (run: echo 1 > /sys/bus/pci/rescan)

/ # echo 1 > /sys/bus/pci/rescan
pci 0000:00:01.0: [1234:0002] type 00 class 0xff0000 conventional PCI endpoint
pci 0000:00:01.0: BAR 0 [mem 0x08002000-0x08002fff]: assigned

/ # ls /sys/bus/pci/devices/
0000:00:00.0  0000:00:01.0

/ # cat /sys/bus/pci/devices/0000:00:01.0/device
0x0002

(Ctrl-A h を押す)
[monitor] PCI device 0000:00:01.0 REMOVED (run: echo 1 > /sys/bus/pci/devices/0000:00:01.0/remove)

/ # echo 1 > /sys/bus/pci/devices/0000:00:01.0/remove
/ # ls /sys/bus/pci/devices/
0000:00:00.0
```

hotplug デバイスは device ID 0x0002（main device は 0x0001）で区別可能。BAR アドレス (0x08002000) は各 enumeration パスで Linux の resource allocator が割り当てるため、rescan のたびに同じ値が保証されるわけではない。

> **Note:** `rescan` は PCI core に新デバイスの発見を依頼する。`remove` は既に enumeration 済みのデバイスを kernel から切り離す。逆操作ではない — `remove` しても将来の rescan でデバイスが見えなくなるわけではない（それには `Ctrl-A h` で `present` を toggle する必要がある）。

## 重要な知見

PCI hotplug の本質はシンプル: config read が有効な vendor ID を返すか 0xFFFF を返すかを制御する。0xFFFF ならスロットは空に見え、有効な値なら Linux が完全な enumeration シーケンス（config read → BAR probe → resource assignment）を実行する — boot 時の発見と全く同じ。Production の複雑さ（native PCIe hotplug）は guest への*自動通知*から来る。microkvm は手動 sysfs rescan でこれを回避するが、同じ kernel コードパスが走る。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| PCI hotplug | config space の可視性を切り替えてランタイムにデバイス追加/削除 |
| Vendor 0xFFFF | PCI 標準の「デバイス不在」シグナル |
| sysfs rescan | `echo 1 > /sys/bus/pci/rescan` で Linux がバスを再走査 |
| sysfs remove | `echo 1 > .../remove` でデバイスを kernel から切り離す |
| Config routing の汎用化 | `target` ポインタで BDF 番号に応じたデバイスを選択 |
| Hotplug 時の BAR re-probing | boot 時と同じ probe シーケンスが走る |

## 変わったこと

Step 26 からの変更:
- **pci.h**: `int present` フィールド、`pci_init_hotplug()` 宣言
- **pci.c**: `pci_init()` に `dev->present = 1`、`pci_init_hotplug()`、config_read/write に presence ガード
- **microkvm.c**: `pci_hotplug_dev` グローバル、`Ctrl-A h` handler、`target` ポインタで config routing 汎用化、hotplug BAR0 MMIO routing、init + RAM 設定

## 次のステップ

Phase E 完了。PCI デバイスの全ライフサイクル:

```
Step 23: 発見 (config space enumeration)
Step 24: アクセス (BAR 経由のデバイスレジスタ)
Step 25: 転送 (DMA)
Step 26: 通知 (MSI-X 割り込み)
Step 27: ライフサイクル (hotplug 追加/削除)
```
