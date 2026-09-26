# Step 12: virtio-mmio デバイス検出

> **Phase C: 高性能 I/O (virtio)**
>
> Phase B は UART でシリアル I/O を実現した。文字の送受信には、VMM による PIO 処理が必要だった。
> Phase C では標準的な準仮想化 I/O フレームワークである virtio を実装する: 共有メモリリング、バッチ通知、カーネル内イベント配送（ioeventfd/irqfd）。

## 目的

virtio-mmio の識別レジスタと Status を実装し、Linux に virtio-console デバイスを認識させる。この段階ではキューを用意しないため、virtio-console の初期化は途中で失敗する。

## 背景

### なぜ virtio か?

Step 10–11 では、ゲストが 8250 UART (ポート 0x3F8) のデータや状態レジスタにアクセスするたびに、`KVM_EXIT_IO` で VMM に処理が戻る。文字単位の処理は、大量のデータを扱う際の負担になる。

virtio は **virtqueue** という共有メモリ上のキューで、データバッファの位置と長さを受け渡す。データをまとめて処理し、通知 (kick) の回数を減らせる。このステップでは、そのキューを使う前提となるデバイス検出を扱う。

### virtio-mmio トランスポート

virtio 仕様は複数のトランスポート (PCI, MMIO, Channel I/O) を定義している。microkvm では最もシンプルな **virtio-mmio** を使用 — PCI バスのエミュレーションが不要。デバイスは固定 guest 物理アドレスのフラットな MMIO レジスタ領域として現れる。

この VMM は、デバイスの位置を Linux のカーネルコマンドラインで伝える:

```
virtio_mmio.device=0x200@0xd0000000:5
```
フォーマット: `<サイズ>@<ベースアドレス>:<IRQ番号>`。Linux がこれを解析して platform device を登録し、`virtio_mmio_probe()` で識別レジスタを読み取る。IRQ 5 はここで宣言するが、このタグには virtio の割り込み通知処理はまだない。

### なぜ GPA 0xD0000000 か?

ゲストメモリの上限は 128MB (`0x08000000`)。`0xD0000000` は登録済みのメモリスロット外なので、KVM はこの領域へのアクセスを MMIO として VMM に渡す。VMM は `KVM_EXIT_MMIO` のアドレスを判定し、virtio のレジスタとして応答する。Step 5 と同じ仕組みだが、従来の `0xD0000` の MMIO ホールとは別の領域。

### レジスタレイアウト (識別)

| Offset | 名前 | 値 | 意味 |
|--------|------|-------|---------|
| 0x000 | MagicValue | 0x74726976 | ASCII "virt" (リトルエンディアン) — virtio デバイスであることを確認 |
| 0x004 | Version | 1 | Legacy MMIO インターフェース (Version=1)。virtio 仕様全体のバージョン番号ではない |
| 0x008 | DeviceID | 3 | デバイス種別: virtio-console (1=net, 2=block, 3=console)。Step 11 の UART の延長として選択 |
| 0x00C | VendorID | 0x4D4B564D | microkvm 独自のベンダー識別値 |
| 0x070 | Status | (読み書き) | ドライバが設定する状態ビット。このタグでは値を保存し、読み返すだけ |

## 実行フロー

```text
Guest (Linux)               KVM                         VMM
     |                       |                           | Status=0 に初期化
     |                       |                           | CMDLINE に位置・IRQ を指定
     | 起動中にデバイス登録     |                           |
     |-- MagicValue 読み取り->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | virtio_mmio_read()
     |                       |                           | data に 0x74726976 を格納
     |                       |<-- KVM_RUN ---------------|
     |<-- 読み取り完了 --------|                           |
     | Version / DeviceID / VendorID も同じ経路で読む       |
     |-- Status 書き込み ---->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | dev->status に保存
     |                       |<-- KVM_RUN ---------------|
     |<-- 続行 ---------------|                           |
     | virtio-console 初期化  |                           |
     |- QueueNumMax 読み取り->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | 未実装なので 0 を返す
     | キュー初期化に失敗       |                           |
     | Status に FAILED を追加して書き込む                  |
```

Status は `0` (リセット) → `1` (ACKNOWLEDGE) → `3` (ACKNOWLEDGE | DRIVER) と進むが、このステップでは DRIVER_OK に到達しない。

## 実装

### 新規ファイル

**`virtio_mmio.h`** — レジスタオフセット定義とデバイス状態:

```c
#define VIRTIO_MMIO_BASE  0xD0000000
#define VIRTIO_MMIO_SIZE  0x200

struct virtio_mmio_dev {
    uint32_t status;
};
```

**`virtio_mmio.c`** — 読み取りハンドラ:

```c
uint32_t virtio_mmio_read(struct virtio_mmio_dev *dev, uint64_t offset, int len)
{
    uint32_t val = 0;

    switch (offset) {
    case VIRTIO_MMIO_STATUS:
        val = dev->status;
        break;
    case VIRTIO_MMIO_MAGIC_VALUE:
        val = VIRTIO_MMIO_MAGIC;
        break;
    case VIRTIO_MMIO_VERSION:
        val = 1;
        break;
    case VIRTIO_MMIO_DEVICE_ID:
        val = VIRTIO_ID_CONSOLE;
        break;
    case VIRTIO_MMIO_VENDOR_ID:
        val = VIRTIO_VENDOR_MKVM;
        break;
    default:
        break;
    }

    fprintf(stderr, "[virtio-mmio] read  offset=0x%03lx → 0x%x\n",
        (unsigned long)offset, val);
    return val;
}
```

未実装のレジスタは `0` を返す。書き込み側は Status だけを保存し、それ以外はログに記録するだけで状態を更新しない。例えば `GuestPageSize` (`0x028`) への書き込みも、このタグでは保存しない。

### microkvm.c の変更

1. kernel command line を更新:

```c
#define CMDLINE "console=ttyS0 earlyprintk=serial rdinit=/init virtio_mmio.device=0x200@0xd0000000:5"
```

2. `main()` 内でデバイス初期化:

```c
virtio_mmio_init(&virtio_dev);
```

3. MMIO exit ハンドラが virtio レジスタに振り分け:

```c
case KVM_EXIT_MMIO: {
    uint64_t addr = run->mmio.phys_addr;
    if (addr >= VIRTIO_MMIO_BASE && addr < VIRTIO_MMIO_BASE + VIRTIO_MMIO_SIZE) {
        uint64_t offset = addr - VIRTIO_MMIO_BASE;
        if (run->mmio.is_write) {
            uint32_t val = 0;
            memcpy(&val, run->mmio.data, run->mmio.len);
            virtio_mmio_write(&virtio_dev, offset, val, run->mmio.len);
        } else {
            uint32_t val = virtio_mmio_read(&virtio_dev, offset, run->mmio.len);
            memcpy(run->mmio.data, &val, run->mmio.len);
        }
    }
    break;
}
```

## 前提条件

### カーネル設定の追加と再ビルド

x86_64 Linux ホストで、Step 10 に使ったカーネルソースと `.config` を引き継ぐ。`make tinyconfig` は再実行せず、virtio 関連の設定を追加する。以下はカーネルソースが `~/linux-src`、VMM が `~/microkvm` にある場合の手順。

```bash
$ cd ~/linux-src

$ scripts/config --enable CONFIG_VIRTIO_MENU
$ scripts/config --enable CONFIG_VIRTIO_MMIO
$ scripts/config --enable CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES
$ scripts/config --enable CONFIG_VIRTIO_CONSOLE

# 依存関係を反映し、新しい項目には既定値を設定
$ make olddefconfig

# 次の6項目がすべて =y になっていることを確認
$ grep -E '^CONFIG_(VIRTIO_MENU|VIRTIO|VIRTIO_MMIO|VIRTIO_MMIO_CMDLINE_DEVICES|VIRTIO_CONSOLE|HVC_DRIVER)=' .config

$ make -j$(nproc) bzImage
$ cp arch/x86/boot/bzImage ~/microkvm/bzImage
```

確認する設定:

```text
CONFIG_HVC_DRIVER=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_VIRTIO=y
CONFIG_VIRTIO_MENU=y
CONFIG_VIRTIO_MMIO=y
CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES=y
```

`VIRTIO_MMIO_CMDLINE_DEVICES` はコマンドラインからのデバイス登録、`VIRTIO_CONSOLE` は DeviceID=3 のドライバに必要。`VIRTIO` と `HVC_DRIVER` は依存関係で有効になる。リング処理は `CONFIG_VIRTIO` とともにビルドされるため、独立した `CONFIG_VIRTIO_RING` の設定は不要。

### VMM のビルドと起動

initramfs は Step 10–11 のものをそのまま使う。

```bash
$ cd ~/microkvm
$ make clean
$ make
$ ./microkvm
```

## 出力

```
virtio-mmio: Registering device virtio-mmio.0 at 0xd0000000-0xd00001ff, IRQ 5.
[virtio-mmio] read  offset=0x000 → 0x74726976
[virtio-mmio] read  offset=0x004 → 0x1
[virtio-mmio] read  offset=0x008 → 0x3
[virtio-mmio] read  offset=0x00c → 0x4d4b564d
[virtio-mmio] write offset=0x028 ← 0x1000
[virtio-mmio] write offset=0x070 ← 0x0
[virtio-mmio] device reset
[virtio-mmio] read  offset=0x070 → 0x0
[virtio-mmio] write offset=0x070 ← 0x1
[virtio-mmio] read  offset=0x070 → 0x1
[virtio-mmio] write offset=0x070 ← 0x3
...
[virtio-mmio] write offset=0x030 ← 0x0
[virtio-mmio] read  offset=0x040 → 0x0
[virtio-mmio] read  offset=0x034 → 0x0
[virtio-mmio] write offset=0x040 ← 0x0
[virtio-mmio] read  offset=0x070 → 0x3
[virtio-mmio] write offset=0x070 ← 0x83
virtio_console virtio0: Error -2 initializing vqs
virtio_console virtio0: probe with driver virtio_console failed with error -2
```

識別レジスタを通じたデバイス認識には成功するが、virtio-console ドライバの初期化は失敗する。`QueueNumMax` (`0x034`) はキューの最大サイズを示すレジスタで、このタグでは未実装のため `0` (キューが使えない) を返す。Status の `0x83` は、`0x03` (ACKNOWLEDGE | DRIVER) に `0x80` (FAILED) が加わった値。`Error -2 initializing vqs` はこの段階では想定通りで、キュー設定は Step 14 で実装する。

シェルの入出力には引き続き UART を使う。終了するにはホスト端末で `Ctrl-C` を押す。

## 重要な知見

デバイスの位置はコマンドラインで Linux に伝え、種類は MMIO レジスタへの応答で識別させる。識別できることと、ドライバの初期化が完了してデータ転送できることは別。このステップは前者までを実装する。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| virtio-mmio トランスポート | 固定 GPA のレジスタ領域、PCI バス不要 |
| デバイス識別 | Magic/Version/DeviceID/VendorID の読み取り |
| MMIO ディスパッチ | Step 5 の KVM_EXIT_MMIO を実プロトコルに活用 |
| Status 状態遷移 | ACKNOWLEDGE → DRIVER (virtio spec の初期化シーケンス) |
| kernel command line | `virtio_mmio.device=size@base:irq` でデバイス位置を宣言 |
| メモリスロット外の MMIO | KVM がアクセスを VMM に渡し、レジスタ値で応答 |

## 変わったこと

Step 11 からの変更:

- **新規ファイル**: `virtio_mmio.c`, `virtio_mmio.h`
- **CMDLINE**: `virtio_mmio.device=0x200@0xd0000000:5` を追加
- **KVM_EXIT_MMIO ハンドラ**: アドレスを判定して virtio レジスタの read/write にディスパッチ
- **Kernel**: CONFIG_VIRTIO_* オプションを追加して再ビルド
- **Makefile**: `virtio_mmio.c` をビルド対象に追加

## 次のステップ

[Step 13: virtio feature negotiation](step13_virtio-features.md) — HostFeatures/GuestFeatures レジスタを実装し、kernel が feature negotiation を完了できるようにする。

その後、Step 14 でキュー設定、Step 15 で送信処理を実装する。
