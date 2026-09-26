# Step 10: ★ 最小 Linux ブート

## 目的

手書きのゲストを Linux カーネルに置き換え、1 vCPU・128MB の構成でブートする。UART に起動ログとシェルのプロンプトを表示するところまでを扱う。ホスト端末からの入力は Step 11 で接続する。

このステップ以降、microkvm は無改造の Linux カーネルを実行する。

## 背景

### Linux がブートに必要とするもの

このステップでは、Linux カーネル (bzImage) の起動に以下を用意する:

1. **メモリ** — 十分な RAM と、それを記述する有効な e820 マップ
2. **ブートプロトコル** — boot_params (ゼロページ) にコマンドライン、メモリマップ、ローダーメタデータ
3. **CPU 状態** — フラットセグメントの 32-bit プロテクトモード (カーネルが自身でロングモードに遷移)
4. **シリアルコンソール** — I/O ポート 0x3F8 の 8250 UART で出力
5. **割り込み配送** — KVM のカーネル内 irqchip (PIC, IOAPIC, LAPIC) が提供
6. **CPUID** — KVM フィルタ済み CPU 機能情報 (カーネルは初期ブートでロングモードサポートを確認)
7. **時刻ソース** — タイムキーピング用の kvmclock またはキャリブレーション済み TSC

### Linux ブートプロトコル

ここで使う 32-bit エントリの主な初期状態:

- CPU は 32-bit プロテクトモード、ページングは無効
- CS/DS/ES/FS/GS/SS にフラットセグメント (base=0, limit=0xFFFFFFFF)
- `%esi` が `boot_params` (「ゼロページ」) を指す
- カーネルが物理アドレス 0x100000 (1MB) にロード済み

VMM は `RIP = 0x100000` に設定 — 圧縮カーネルのエントリポイント。そこから 64-bit カーネルはロングモードに遷移して自身を解凍し、最終的に `start_kernel` に到達する。

### bzImage 構造

```
┌──────────────────────┬─────────────────────────────────┐
│  Setup (リアルモード)  │  プロテクトモードカーネル           │
│  ~16KB               │  (圧縮済み、1MB にロード)          │
└──────────────────────┴─────────────────────────────────┘
         ↑                          ↑
    setup ヘッダ              圧縮カーネルエントリ
    (プロトコルバージョン,      (ロングモードに入る,
     loadflags 等)             自身を解凍,
                               start_kernel に到達)
```

VMM は setup ヘッダを解析してカーネルオフセットを見つけ、プロテクトモード部分を 0x100000 にコピーする。

### 8250 UART エミュレーション

Linux のシリアルコンソールドライバ (`8250`) は PIO の 0x3F8–0x3FF で通信する。
VMM は出力をサポートするのに十分な UART をエミュレートする必要がある:

| レジスタ | ポート | Read | Write |
|---------|--------|------|-------|
| THR/RBR | 0x3F8 | 受信バッファ | 送信データ |
| IER | 0x3F9 | 有効化状態 | 割り込み有効化 |
| IIR | 0x3FA | 割り込み ID | — |
| LCR | 0x3FB | ライン制御状態 | ライン制御 (DLAB) |
| MCR | 0x3FC | モデム制御状態 | モデム制御 |
| LSR | 0x3FD | ライン状態 | — |

LCR の DLAB (Divisor Latch Access Bit) がポート 0x3F8 と 0x3F9 の意味を変える — DLAB=1 の場合、ボーレート分周器レジスタになる。DLAB ハンドリングなしでは、ドライバのボーレート初期化が文字出力と誤解される。

### カーネル内 PIC と PIT

割り込みコントローラをユーザー空間でエミュレートする代わりに、KVM 内蔵のエミュレーションを使う:

```c
ioctl(vmfd, KVM_CREATE_IRQCHIP, 0);     /* PIC (8259) + IOAPIC + LAPIC */
ioctl(vmfd, KVM_CREATE_PIT2, &pit);     /* PIT (8254) タイマー */
```

これでユーザー空間の関与なしに動作するタイマー割り込みがカーネルに提供される — KVM が割り込み配送パス全体を内部的に処理。

### kvmclock と CPUID

`KVM_GET_SUPPORTED_CPUID` で KVM がゲストに提供できる CPU 機能を取得し、TSC-deadline のビットを落として `KVM_SET_CPUID2` で設定する。ホストの CPUID をそのまま渡すわけではない。

Linux は KVM の CPUID リーフから kvmclock を検出する。時刻情報は MSR で登録した共有メモリを介して取得し、この MSR は KVM が処理する。Step 8 の独自 MSR ハンドラは使わない。

TSC 周波数には 1 GHz を指定する。ただし、この設定に失敗してもエラーを表示して起動を続ける。

```c
if (ioctl(vcpus[i].fd, KVM_SET_TSC_KHZ, 1000000UL) < 0) {
    perror("KVM_SET_TSC_KHZ");
}
```

## 実行フロー

```text
Guest (Linux)               KVM                         VMM
     |                       |                           |
     |                       |<-- IRQCHIP / PIT2 作成 ----|
     |                       |<-- メモリスロット登録 -------|
     |                       |                           | bzImage・initramfs を配置
     |                       |                           | boot_params を設定
     |                       |<-- CPUID・TSC・初期状態 ----|
     |                       |<-- KVM_RUN ---------------|
     |<-- 32-bit で実行 ------|                           |
     | ロングモードへ遷移       |                           |
     | カーネル解凍・初期化     |                           |
     |-- UART への PIO ------>|-- KVM_EXIT_IO ----------->|
     |                       |                           | uart_in / uart_out
     |                       |<-- KVM_RUN ---------------|
     |<-- 続行 ---------------|                           |
     | initramfs を展開       |                           |
     | /init → シェル起動      |                           |
     |-- UART への出力 ------>|-- KVM_EXIT_IO ----------->|
     |                       |                           | 挨拶とプロンプトを表示
```

UART の送信割り込みが有効なら、VMM は `KVM_IRQ_LINE` で IRQ 4 を立ち上げて下げる。KVM の irqchip がゲストへの配送を扱う。

## 実装

### VMM: bzImage ローダー

```c
/* setup ヘッダ解析 */
uint8_t setup_sects = hdr[0x1F1];
if (setup_sects == 0) setup_sects = 4;
uint32_t setup_size = (setup_sects + 1) * 512;
uint32_t kernel_size = st.st_size - setup_size;

/* プロテクトモードカーネルを 1MB にコピー */
memcpy((char *)mem + KERNEL_ADDR, (char *)bzimage + setup_size, kernel_size);
```

### VMM: boot_params (ゼロページ)

`boot.c` は GPA `0x7000` に boot_params を配置する。e820 は Linux に使える物理メモリを伝えるもので、KVM のメモリスロット登録とは別の設定。

| 範囲 (終端を含まない) | 種別 |
|----------------------|------|
| `0x0`–`0x9F000` | RAM |
| `0x9F000`–`0x100000` | 予約領域 |
| `0x100000`–128MB | RAM |

RAM 2 領域と予約領域の計 3 エントリを渡す。従来の MMIO ホール `0xD0000`–`0xD1000` も、この予約領域に含まれる。

```c
/* コマンドラインとそのポインタ */
strcpy((char *)mem + CMDLINE_ADDR, cmdline);
*(uint32_t *)((char *)mem + BOOT_PARAMS_ADDR + 0x228) = CMDLINE_ADDR;

/* type_of_loader / loadflags */
*((char *)mem + BOOT_PARAMS_ADDR + 0x210) = 0xFF;
*((char *)mem + BOOT_PARAMS_ADDR + 0x211) |= 0x01 | 0x80;
```

コマンドラインは `console=ttyS0 earlyprintk=serial rdinit=/init`。initramfs は GPA `0x4000000` (64MB) に置き、アドレスとサイズを boot_params の `ramdisk_image` / `ramdisk_size` に書き込む。

### VMM: vCPU 初期化 (Linux ブートプロトコル)

```c
sregs.cr0 = 0x11;              /* PE | ET (ページングなし) */
sregs.cs.db = 1;               /* 32-bit */
sregs.cs.g = 1;                /* 4KB 粒度 */
sregs.cs.limit = 0xFFFFFFFF;   /* フラット 4GB */
/* DS/ES/FS/GS/SS: 同じフラットデータセグメント */

regs.rip = KERNEL_ADDR;        /* 0x100000: startup_32 */
regs.rsi = BOOT_PARAMS_ADDR;   /* 0x7000: boot_params */
```

Step 9 の vCPU 1 (ロングモード) と比較: ここでは Linux ブートプロトコルが要求するため 32-bit プロテクトモードを使用。VMM はセグメント状態を `KVM_SET_SREGS` で直接設定し、その先のページテーブル構築とロングモード遷移は Linux が行う。

### VMM: UART (uart.c)

`uart_out()` の送信・割り込み有効化部分を抜粋する:

```c
case UART_THR:
    if (u->lcr & UART_LCR_DLAB) {
        u->dll = val;
    } else {
        write(STDOUT_FILENO, &val, 1);
        u->lsr |= UART_LSR_THRE | UART_LSR_TEMT;
        uart_maybe_raise_thre(u, vmfd);
    }
    break;
case UART_IER:
    if (u->lcr & UART_LCR_DLAB) {
        u->dlm = val;
    } else {
        u->ier = val;
        if (u->lsr & UART_LSR_THRE)
            uart_maybe_raise_thre(u, vmfd);
    }
    break;
```

DLAB=0 の送信データはホストの標準出力へ書き出す。送信バッファが空の状態で IER を有効化した場合も、`uart_maybe_raise_thre()` が未通知の送信割り込みを立てる。Linux が IIR を読むと、その送信割り込みの保留状態を解除する。

`uart_rx()` はこのタグに存在するが、ホストの標準入力を読み取って呼び出す処理はまだない。

## 前提条件

### カーネルのビルド (bzImage)

以下は x86_64 Linux ホストで行う。microkvm の `make` とは別に、シリアルコンソールと KVM 準仮想化に必要なオプションを加えた `tinyconfig` で Linux カーネルをビルドする:

```bash
# カーネルソースの取得
$ git clone --depth 1 https://github.com/torvalds/linux.git ~/linux-src
$ cd ~/linux-src

# 最小構成から必要なオプションを追加
$ make tinyconfig

$ scripts/config --enable CONFIG_64BIT
$ scripts/config --enable CONFIG_PRINTK
$ scripts/config --enable CONFIG_TTY
$ scripts/config --enable CONFIG_SERIAL_8250
$ scripts/config --enable CONFIG_SERIAL_8250_CONSOLE
$ scripts/config --enable CONFIG_EARLY_PRINTK
$ scripts/config --enable CONFIG_KVM_GUEST
$ scripts/config --enable CONFIG_HYPERVISOR_GUEST
$ scripts/config --enable CONFIG_PARAVIRT
$ scripts/config --enable CONFIG_PARAVIRT_CLOCK
$ scripts/config --enable CONFIG_BLK_DEV_INITRD
$ scripts/config --enable CONFIG_RD_GZIP
$ scripts/config --enable CONFIG_BINFMT_ELF
$ scripts/config --enable CONFIG_BINFMT_SCRIPT
$ scripts/config --enable CONFIG_DEVTMPFS
$ scripts/config --enable CONFIG_PROC_FS
$ scripts/config --enable CONFIG_SYSFS
$ scripts/config --disable CONFIG_VT

$ make olddefconfig
$ make -j$(nproc) bzImage

$ cp arch/x86/boot/bzImage ~/microkvm/
```

**各オプションの理由:**

- `SERIAL_8250` + `SERIAL_8250_CONSOLE`: カーネル出力をエミュレートした UART に送る
- `KVM_GUEST` + `PARAVIRT_CLOCK`: kvmclock を有効化（TSC キャリブレーション hang を回避）
- `BLK_DEV_INITRD` + `RD_GZIP`: 外部の gzip 圧縮 initramfs を読み込む
- `DEVTMPFS`: `/dev` のデバイスノードを提供する。initramfs では `/init` が明示的にマウントする
- `VT=n`: 使用しない仮想端末を省く。シリアルコンソールは `console=ttyS0` で指定する

### initramfs のビルド

カーネルにはルートファイルシステムが必要。busybox とシェルスクリプト init で最小 initramfs を作成:

```bash
# busybox スタティックバイナリ取得
$ cd /tmp
$ curl -fL -o busybox https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox
$ chmod +x busybox

# ディレクトリ構造作成
$ mkdir -p /tmp/initramfs_root/{bin,dev,proc,sys,lib/modules}
$ cp /tmp/busybox /tmp/initramfs_root/bin/

# /init の標準入出力に使う初期コンソール
$ if [ ! -e /tmp/initramfs_root/dev/console ]; then
    sudo mknod -m 600 /tmp/initramfs_root/dev/console c 5 1
fi

# シェルコマンドの symlink 作成
$ cd /tmp/initramfs_root/bin
$ for cmd in sh echo cat ls mount mkdir uname head ps \
           dd wc sync free mv cp rm touch \
           hexdump devmem lspci \
           insmod rmmod lsmod dmesg grep; do
    ln -sf busybox $cmd
done

# init スクリプト作成
$ cat > /tmp/initramfs_root/init << 'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
echo "=== Hello from microkvm guest! ==="
exec /bin/sh
EOF
$ chmod +x /tmp/initramfs_root/init

# 圧縮 cpio アーカイブとしてパック
$ cd /tmp/initramfs_root
$ find . | cpio -o -H newc | gzip > ~/microkvm/initramfs.gz
```

VMM が `initramfs.gz` をゲストメモリの GPA 0x4000000 にロードし、`boot_params` 経由でアドレスとサイズを渡す。カーネルはこれをルートファイルシステムとして展開し、PID 1 として `/init` を実行する。`/init` は proc・sysfs・devtmpfs をマウントしてからシェルを起動する。アーカイブ内の `/dev/console` は、`/init` が devtmpfs をマウントする前の標準入出力を用意するために必要。

## 出力

`bzImage` と `initramfs.gz` を microkvm のディレクトリに置いて実行する。`make` は VMM のみをビルドする。

```bash
$ cd ~/microkvm
$ make clean
$ make
$ ./microkvm
```

```
$ ./microkvm
bzImage: protocol 2.15, setup 16384 bytes, kernel 939008 bytes
Kernel loaded at 0x100000 (939008 bytes)
initramfs loaded at 0x4000000 (699794 bytes)
Starting guest...
Linux version 7.3.0-rc4+ ...
Command line: console=ttyS0 earlyprintk=serial rdinit=/init
...
Hypervisor detected: KVM
kvm-clock: Using msrs 4b564d01 and 4b564d00
...
clocksource: Switched to clocksource kvm-clock
...
clocksource: Switched to clocksource tsc
...
serial8250: ttyS0 I/O:0x3f8 (irq = 4, base_baud = 115200) is a 8250
...
Run /init as init process
=== Hello from microkvm guest! ===
/bin/sh: can't access tty; job control turned off
/ #
```

`Hypervisor detected: KVM` と `kvm-clock` は KVM の検出と時刻ソースの初期化、挨拶と `/ #` は `/init` とシェルの起動を示す。このステップではプロンプトへの入力はできない。終了するにはホスト端末で `Ctrl-C` を押す。

## 重要な知見

VMM はメモリ配置・CPU 初期状態・ブート情報を準備し、その先の OS 初期化は Linux に任せる。UART の PIO は VMM が処理する一方、PIC/IOAPIC/LAPIC・PIT・kvmclock は KVM が担当する。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| Linux ブートプロトコル | boot_params, e820, cmdline, 32-bit エントリ |
| bzImage 解析 | Setup ヘッダ → カーネルオフセットとサイズ |
| 8250 UART | DLAB, IER, IIR, THR, LSR によるステートマシン |
| カーネル内 irqchip | KVM_CREATE_IRQCHIP + KVM_CREATE_PIT2 |
| CPUID の設定 | KVM_GET_SUPPORTED_CPUID → TSC-deadline を除外 → KVM_SET_CPUID2 |
| kvmclock | MSR で共有メモリを登録する準仮想化時刻ソース |
| initramfs | ゲストメモリにロード、boot_params 経由でアドレスを渡す |

## 変わったこと

- ゲストを `guest.bin` から `bzImage` + `initramfs.gz` に変更し、ローダーと boot_params の設定を追加。
- vCPU を 2 個から 1 個、ゲストメモリを 1MB から 128MB に変更。
- 独自 PIO 出力と MMIO カウンタの処理に代わり、8250 UART を追加。
- CPUID の設定、カーネル内 irqchip / PIT、TSC 周波数の設定を追加。

## 次のステップ

[Step 11: 対話型シェル](step11_interactive-shell.md) — シリアル RX サポートを追加し、ゲストがホスト端末からの入力を受け付けられるようにして、対話型 busybox シェルを実現する。
