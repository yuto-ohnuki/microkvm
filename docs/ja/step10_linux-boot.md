# Step 10: ★ 最小 Linux ブート

## 目的

microkvm 上で実際の Linux カーネルをブートする。Step 1–9 の全てを統合する**マイルストーン**: モード遷移、メモリ管理、デバイスエミュレーション、割り込み配送、準仮想化。

このステップ以降、microkvm は無改造の Linux カーネルを実行する。

## 背景

### Linux がブートに必要とするもの

Linux カーネル (bzImage) は VMM に以下を要求する:

1. **メモリ** — 十分な RAM と、それを記述する有効な e820 マップ
2. **ブートプロトコル** — boot_params (ゼロページ) にコマンドライン、メモリマップ、ローダーメタデータ
3. **CPU 状態** — フラットセグメントの 32-bit プロテクトモード (カーネルが自身でロングモードに遷移)
4. **シリアルコンソール** — I/O ポート 0x3F8 の 8250 UART で出力
5. **割り込み配送** — KVM のカーネル内 irqchip (PIC, IOAPIC, LAPIC) が提供
6. **CPUID** — KVM フィルタ済み CPU 機能情報 (カーネルは初期ブートでロングモードサポートを確認)
7. **時刻ソース** — タイムキーピング用の kvmclock またはキャリブレーション済み TSC

### Linux ブートプロトコル

Linux ブートプロトコルの要件:
- CPU は 32-bit プロテクトモード、ページングは無効
- CS/DS/ES/FS/GS/SS にフラットセグメント (base=0, limit=4GB)
- `%esi` が `boot_params` (「ゼロページ」) を指す
- カーネルが物理アドレス 0x100000 (1MB) にロード済み

VMM は `RIP = 0x100000` に設定 — 圧縮カーネルのエントリポイント。そこからカーネルは自身を解凍し、ロングモードに遷移し、最終的に `start_kernel` に到達する。

### bzImage 構造

```
┌──────────────────────┬─────────────────────────────────┐
│  Setup (リアルモード)  │  プロテクトモードカーネル           │
│  ~16KB               │  (圧縮済み、1MB にロード)          │
└──────────────────────┴─────────────────────────────────┘
         ↑                          ↑
    setup ヘッダ              圧縮カーネルエントリ
    (プロトコルバージョン,      (自身を解凍,
     loadflags 等)             ロングモードに入る,
                               start_kernel に到達)
```

VMM は setup ヘッダを解析してカーネルオフセットを見つけ、プロテクトモード部分を 0x100000 にコピーする。

### 8250 UART エミュレーション

Linux のシリアルコンソールドライバ (`8250`) は PIO の 0x3F8–0x3FF で通信する。
VMM は出力をサポートするのに十分な UART をエミュレートする必要がある:

| レジスタ | ポート | Read | Write |
|---------|--------|------|-------|
| THR/RBR | 0x3F8 | 受信バッファ | 送信データ |
| IER | 0x3F9 | — | 割り込み有効化 |
| IIR | 0x3FA | 割り込み ID | — |
| LCR | 0x3FB | — | ライン制御 (DLAB) |
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

Linux は CPUID 経由で KVM を検出し、タイムキーピングに **kvmclock** を使用する。
Linux は KVM CPUID リーフ (ベンダ文字列 "KVMKVMKVM" と機能フラグ) で kvmclock を発見する。高コストな TSC キャリブレーションループを回避。ホストの CPUID をパススルー (LAPIC タイマー問題を避けるため TSC-deadline モードは隠す) し、固定 TSC 周波数を設定:

```c
ioctl(vcpufd, KVM_SET_TSC_KHZ, 1000000UL);  /* 1 GHz */
```

## 実行フロー

```
VMM (microkvm.c)                         Linux カーネル
────────────────                         ──────────────
1. bzImage setup ヘッダ解析
2. カーネルを GPA 0x100000 にコピー
3. boot_params を 0x7000 に設定:
     - e820 マップ (0–640KB, 1MB–128MB)
     - コマンドラインポインタ
     - initramfs アドレス/サイズ
4. initramfs を 0x4000000 にロード
5. KVM_CREATE_IRQCHIP + KVM_CREATE_PIT2
6. KVM_SET_CPUID2 (ホスト CPUID パススルー)
7. KVM_SET_TSC_KHZ (1 GHz)
8. vCPU 設定: 32-bit プロテクトモード
     RIP=0x100000, ESI=0x7000
9. KVM_RUN
                                         startup_32:
                                           verify_cpu (CPUID チェック)
                                           ページテーブル構築
                                           ロングモード有効化
                                           カーネル解凍
                                         start_kernel:
                                           KVM 検出 (CPUID)
                                           kvmclock 初期化
                                           シリアルコンソール初期化 (8250)
                                           initramfs マウント
                                           /init を実行
                                              ↓
                                         "=== Hello from microkvm guest! ==="
```

## 実装

### VMM: bzImage ローダー

```c
/* setup ヘッダ解析 */
uint8_t setup_sects = hdr[0x1F1];
if (setup_sects == 0) setup_sects = 4;
uint32_t setup_size = (setup_sects + 1) * 512;
uint32_t kernel_size = file_size - setup_size;

/* プロテクトモードカーネルを 1MB にコピー */
memcpy((char *)mem + KERNEL_ADDR, bzimage + setup_size, kernel_size);
```

### VMM: boot_params (ゼロページ)

```c
/* e820 メモリマップ */
e820[0]: 0x0 – 0x9FC00        (type=RAM)
e820[1]: 0x100000 – 128MB     (type=RAM)
```

簡略化のため microkvm は2つの RAM 領域のみを公開する。実システムはファームウェア予約領域 (EBDA, ACPI 等) を含むより詳細な e820 マップを提供する。

```c
/* コマンドライン */
strcpy(mem + CMDLINE_ADDR, "console=ttyS0 earlyprintk=serial rdinit=/init");
boot_params->cmd_line_ptr = CMDLINE_ADDR;

/* ローダーメタデータ */
boot_params->type_of_loader = 0xFF;
boot_params->loadflags |= LOADED_HIGH | CAN_USE_HEAP;
```

### VMM: vCPU 初期化 (Linux ブートプロトコル)

```c
sregs.cr0 = 0x11;              /* PE | ET (ページングなし) */
sregs.cs.db = 1;               /* 32-bit */
sregs.cs.g = 1;                /* 4KB 粒度 */
sregs.cs.limit = 0xFFFFFFFF;   /* フラット 4GB */
/* DS/ES/FS/GS/SS: 同じフラットデータセグメント */

regs.rip = 0x100000;           /* startup_32 */
regs.rsi = 0x7000;             /* boot_params ポインタ */
```

Step 9 の vCPU 1 (ロングモード) と比較: ここでは Linux ブートプロトコルが要求するため 32-bit プロテクトモードを使用。カーネルは自身でロングモード遷移を行う — Step 4 で実装したのと同じシーケンス。

### VMM: UART (uart.c)

UART は DLAB 対応のレジスタディスパッチを持つステートマシン:

```c
void uart_out(struct uart8250 *u, uint16_t port, uint8_t val, int vmfd) {
    int reg = port - UART_BASE;
    if (u->lcr & UART_LCR_DLAB) {
        if (reg == 0) { u->dll = val; return; }
        if (reg == 1) { u->dlm = val; return; }
    }
    switch (reg) {
    case UART_THR:  /* 送信 */
        putchar(val);
        if (u->ier & UART_IER_THRI) {
            u->pending_thre = 1;
            /* KVM_IRQ_LINE 経由で IRQ 4 を注入 */
            /* IRQ 4 は COM1 (0x3F8) の伝統的な割り込みライン */
        }
        break;
    case UART_IER:
        u->ier = val;
        break;
    case UART_LCR:
        u->lcr = val;
        break;
    /* ... */
    }
}
```

## 前提条件

### カーネルのビルド (bzImage)

microkvm はシリアルコンソールと KVM 準仮想化に必要なオプションを加えた `tinyconfig` で最小 Linux カーネルをビルドする:

```bash
# カーネルソースの取得
git clone --depth 1 https://github.com/torvalds/linux.git ~/linux-src
cd ~/linux-src

# 最小構成から必要なオプションを追加
make tinyconfig

scripts/config --enable CONFIG_64BIT
scripts/config --enable CONFIG_PRINTK
scripts/config --enable CONFIG_TTY
scripts/config --enable CONFIG_SERIAL_8250
scripts/config --enable CONFIG_SERIAL_8250_CONSOLE
scripts/config --enable CONFIG_EARLY_PRINTK
scripts/config --enable CONFIG_KVM_GUEST
scripts/config --enable CONFIG_HYPERVISOR_GUEST
scripts/config --enable CONFIG_PARAVIRT
scripts/config --enable CONFIG_PARAVIRT_CLOCK
scripts/config --enable CONFIG_BLK_DEV_INITRD
scripts/config --enable CONFIG_BINFMT_ELF
scripts/config --enable CONFIG_BINFMT_SCRIPT
scripts/config --enable CONFIG_DEVTMPFS
scripts/config --enable CONFIG_DEVTMPFS_MOUNT
scripts/config --enable CONFIG_PROC_FS
scripts/config --enable CONFIG_SYSFS
scripts/config --disable CONFIG_VT

make olddefconfig
make -j$(nproc) bzImage

cp arch/x86/boot/bzImage ~/microkvm/
```

**各オプションの理由:**
- `SERIAL_8250` + `SERIAL_8250_CONSOLE`: カーネル出力をエミュレートした UART に送る
- `KVM_GUEST` + `PARAVIRT_CLOCK`: kvmclock を有効化（TSC キャリブレーション hang を回避）
- `DEVTMPFS_MOUNT`: `/dev` エントリを自動作成（mknod 不要で init が実行可能）
- `VT=n`: カーネルが `/dev/console` を仮想端末に接続するのを防ぐ（シリアルを使用）

### initramfs のビルド

カーネルにはルートファイルシステムが必要。busybox とシェルスクリプト init で最小 initramfs を作成:

```bash
# busybox スタティックバイナリ取得
cd /tmp
curl -o busybox https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox
chmod +x busybox

# ディレクトリ構造作成
mkdir -p /tmp/initramfs_root/{bin,dev,proc,sys,lib/modules}
cp /tmp/busybox /tmp/initramfs_root/bin/

# シェルコマンドの symlink 作成
cd /tmp/initramfs_root/bin
for cmd in sh echo cat ls mount mkdir uname head ps \
           dd wc sync free mv cp rm \
           hexdump devmem lspci \
           insmod rmmod lsmod dmesg grep; do
    ln -sf busybox $cmd
done

# init スクリプト作成
cat > /tmp/initramfs_root/init << 'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
echo "=== Hello from microkvm guest! ==="
exec /bin/sh
EOF
chmod +x /tmp/initramfs_root/init

# 圧縮 cpio アーカイブとしてパック
cd /tmp/initramfs_root
find . | cpio -o -H newc | gzip > ~/microkvm/initramfs.gz
```

VMM が `initramfs.gz` をゲストメモリの GPA 0x4000000 にロードし、`boot_params` 経由でアドレスとサイズを渡す。カーネルはこれをルートファイルシステムとして展開し、PID 1 として `/init` を実行する。init は必須ファイルシステムをマウントしてから対話シェルを起動する。

## 出力

```
$ ./microkvm
bzImage: protocol 2.15, setup 16384 bytes, kernel 943104 bytes
Kernel loaded at 0x100000 (943104 bytes)
initramfs loaded at 0x4000000 (699991 bytes)
Starting guest...
Linux version 7.1.0+ ...
Command line: console=ttyS0 earlyprintk=serial rdinit=/init
...
Hypervisor detected: KVM
kvm-clock: Using msrs 4b564d01 and 4b564d00
...
serial8250: ttyS0 at I/O 0x3f8 (irq = 4, base_baud = 115200) is a 8250
...
Run /init as init process
=== Hello from microkvm guest! ===
/ #
```

## 重要な知見

Linux のブートは新しい概念ではない — Step 1–9 の全概念の**応用**:

| Linux が必要とするもの | 提供元 |
|---------------------|--------|
| プロテクトモードエントリ | Step 3 (GDT, CR0.PE, フラットセグメント) |
| ロングモード遷移 | Step 4 (カーネルが自身で行う) |
| シリアル出力 | Step 2 (PIO) + UART ステートマシン (Step 6 パターン) |
| タイマー割り込み | Step 7 (カーネル内 irqchip 経由の IRQ 注入) |
| kvmclock | Step 8 (MSR ベースの準仮想化インターフェース) |
| CPUID | Step 8 (ハイパーバイザー検出) |
| メモリ | Step 4 (メモリスロット → e820 マップ) |

VMM の役割は環境を準備し、カーネルが初期化するのに十分なハードウェアだけをエミュレートすること。Linux が動作すれば、前のステップで手作業で構築したのと同じメカニズムを使う — ただし今度は実際のデバイスドライバとカーネルサブシステムを通じて。

### 実装しなかったもの

microkvm は以下に KVM のカーネル内エミュレーションを依存:
- **PIC/IOAPIC/LAPIC** — `KVM_CREATE_IRQCHIP`
- **PIT タイマー** — `KVM_CREATE_PIT2`
- **kvmclock MSR** — KVM が内部的に処理

QEMU のような本番 VMM は柔軟性のためにこれらをユーザー空間で実装するが、最小カーネルのブートには KVM の内蔵サポートで十分。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| Linux ブートプロトコル | boot_params, e820, cmdline, 32-bit エントリ |
| bzImage 解析 | Setup ヘッダ → カーネルオフセットとサイズ |
| 8250 UART | DLAB, IER, IIR, THR, LSR によるステートマシン |
| カーネル内 irqchip | KVM_CREATE_IRQCHIP + KVM_CREATE_PIT2 |
| CPUID パススルー | KVM_GET_SUPPORTED_CPUID → KVM_SET_CPUID2 |
| kvmclock | シンセティック MSR 経由の準仮想化時刻ソース |
| initramfs | ゲストメモリにロード、boot_params 経由でアドレスを渡す |

## 変わったこと

Step 9 まで、全てのゲスト命令は手書きだった。Step 10 以降、ゲストは独自にページング、割り込み処理、スケジューリング、ドライバ初期化を行う実際の OS。

VMM の役割は「数命令を実行して exit を観察する」から「環境を準備してカーネルに任せる」に移行。QEMU がまさにこれを行う — そしてここからフォーカスは徐々に基本的な正確性から完全性とパフォーマンスへと移る。

## 次のステップ

[Step 11: 対話型シェル](step11_interactive-shell.md) — シリアル RX サポートを追加し、ゲストがホスト端末からの入力を受け付けられるようにして、対話型 busybox シェルを実現する。
