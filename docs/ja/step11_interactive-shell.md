# Step 11: 対話型シェル

## 目的

ホスト端末の入力を、Step 10 で用意した UART の **RX** (受信) 処理に接続する。既存の busybox initramfs を使い、ゲストのシェルでコマンドを実行する。

## 背景

### TX vs RX

Step 10 はシリアル TX (送信) を実装: ゲストが THR に書き、VMM が stdout に出力。
Step 11 は逆方向を追加:

| 方向 | UART の役割 | フロー |
|------|------------|--------|
| TX (Step 10) | Guest → Host | ゲストが THR に書く → VMM が stdout に出力 |
| RX (Step 11) | Host → Guest | ユーザーが入力 → VMM が RBR を設定 → IRQ 4 → ゲストが読む |

### なぜ別の stdin スレッドが必要か

vCPU スレッドは `KVM_RUN` でゲストを実行し、ユーザー空間へ戻ると I/O を処理する。そこでホスト入力を待つとゲストの実行も止まるため、専用の **stdin リーダースレッド**で入力を待つ:

```
[ホスト端末]  →  [stdin_thread]  →  [UART RBR + IRQ 4]  →  [ゲストカーネル]
  キー入力      read(stdin)        uart_rx()               シリアルドライバ
```

### UART RX レジスタ

| レジスタ | 役割 |
|---------|------|
| RBR (0x3F8, read, DLAB=0) | Receive Buffer — ゲストがここから文字を読む |
| LSR bit 0 (DR) | Data Ready — 「RBR に未読データがある」 |
| IER bit 0 (RDI) | Receive Data Interrupt 有効化 |
| IIR = 0x04 | 割り込み識別: 「原因は受信データ」 |

### RX シーケンス

```
1. ユーザーが 'a' を押す
   stdin_thread: read(STDIN_FILENO) が 'a' を返す

2. stdin_thread が UART 状態を更新:
   uart.rbr = 'a'
   uart.lsr |= DR (Data Ready)

3. stdin_thread が IRQ 4 を注入 (IER.RDI が有効な場合):
   KVM_IRQ_LINE {irq=4, level=1}  (assert)
   KVM_IRQ_LINE {irq=4, level=0}  (deassert)

4. ゲストカーネルが IRQ 4 を受信:
   serial8250_interrupt() が実行

5. ゲストが IIR (0x3FA) を読む → VMM が 0x04 (RDI) を返す
   ドライバが把握: 「割り込みの原因は受信データ」

6. ゲストが RBR (0x3F8) を読む → VMM が 'a' を返す
   VMM がクリア: lsr &= ~DR, pending_rdi = 0

7. ドライバが 'a' を tty レイヤーに渡す → シェルへ入力
   ゲストの端末処理によるエコーやシェル出力が TX 経由でホスト端末に戻る
```

### ホスト端末の入力設定 (Step 10 から継続)

デフォルトではホスト端末は行単位で入力をバッファリングする (canonical モード)。
対話型シェルには各キー入力を即座に配送する必要がある:

```c
struct termios raw = orig_termios;
raw.c_lflag &= ~(ICANON | ECHO);    /* 行バッファリングなし、ローカルエコーなし */
raw.c_cc[VMIN] = 1;                 /* 1 バイトで read が返る */
raw.c_cc[VTIME] = 0;                /* タイムアウトなし */
tcsetattr(STDIN_FILENO, TCSANOW, &raw);
```

`ISIG` は無効化していないため、通常の端末設定では `Ctrl-C` はゲストへ渡らず、ホスト側の VMM を終了する。`atexit(restore_terminal)` による復元は通常終了時のみで、シグナル終了後に端末表示が戻らない場合はホストで `stty sane` を実行する。

### ESC シーケンスフィルタリング

ホスト端末は、出力にカーソル位置を問い合わせる制御文字列が含まれると、位置を表す応答を返す。この応答はキー入力と同じく VMM の標準入力に届くため、そのまま `uart_rx()` に渡すと、ユーザーが打っていない文字列がゲストのシェルに入力されてしまう。ESC フィルタは、この端末応答を取り除くためにある。

実装は ESC (`0x1b`) から次の英字までを破棄するだけで、端末応答とキー入力を区別しない。そのため矢印キーも通らず、ESC 単独の入力でも後続の英字まで読み捨てる。

## 実行フロー

```text
Guest (Linux + busybox)      KVM                         VMM
     |                       |                           | stdin_thread を開始
     |                       |<-- KVM_RUN ---------------| vcpu_thread
     | ブート → /init → sh    |                           |
     |                       |                           | ホストで 'a' を入力
     |                       |                           | read() → uart_rx()
     |                       |                           | RBR='a', LSR.DR=1
     |                       |<-- KVM_IRQ_LINE (IRQ 4) --| IER.RDI 有効時
     |<-- IRQ 4 配送 ---------|                           |
     |-- IIR / RBR 読み取り ->|-- KVM_EXIT_IO ----------->|
     |                       |                           | uart_in() が応答
     |                       |<-- KVM_RUN ---------------|
     |<-- 入力を取得 ----------|                           |
     | tty → シェル           |                           |
     |-- エコー・出力 (THR) -->|-- KVM_EXIT_IO ----------->|
     |                       |                           | uart_out() → stdout
```

IIR と RBR の読み取りは、それぞれ PIO として処理される。改行でコマンドを確定すると、シェルが実行結果を同じ TX 経路で出力する。

## 実装

### stdin_thread

```c
static void *stdin_thread(void *arg) {
    (void)arg;
    uint8_t c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        /* ESC シーケンスフィルタ (端末応答) */
        if (c == 0x1b) {
            while (read(STDIN_FILENO, &c, 1) == 1) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                    break;
            }
            continue;
        }
        uart_rx(&uart, c, g_vmfd);
    }
    return NULL;
}
```

`main()` では、vCPU スレッドの起動前に stdin スレッドを作成する:

```c
pthread_t stdin_tid;
int ret = pthread_create(&stdin_tid, NULL, stdin_thread, NULL);
if (ret != 0) {
    fprintf(stderr, "pthread_create(stdin): %s\n", strerror(ret));
    exit(1);
}
```

### uart_rx (Step 10 から継続)

```c
void uart_rx(struct uart8250 *u, uint8_t c, int vmfd) {
    u->rbr = c;
    u->lsr |= UART_LSR_DR;          /* Data Ready */
    if (u->ier & UART_IER_RDI) {    /* RX 割り込み有効? */
        u->pending_rdi = 1;
        inject_irq4(vmfd);
    }
}
```

`uart_rx()` と vCPU スレッドの `uart_in()` / `uart_out()` は UART 状態を共有するが、このタグにはロックによる保護がない。また RBR は 1 バイトのみで、未読データを待たずに上書きするため、連続入力や貼り付けでは文字を取りこぼす可能性がある。

### initramfs init スクリプト

```sh
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
echo "=== Hello from microkvm guest! ==="
exec /bin/sh
```

`/init` は proc・sysfs・devtmpfs をマウントし、busybox sh を起動する。Step 10 で用意した `/dev/console` と `console=ttyS0` により、シェルはシリアルコンソールにつながる標準入出力を引き継ぐ。initramfs では `CONFIG_DEVTMPFS_MOUNT` による自動マウントは行われない。

## initramfs について

Step 10 で作成した initramfs に既に busybox と対話シェル（`exec /bin/sh` の init）が含まれている。このステップでは initramfs の変更は不要 — VMM に `stdin_thread` を追加するだけでホスト入力がゲストに届くようになる。

## 出力

Step 10 の `bzImage` と `initramfs.gz` をそのまま使う。

```bash
$ make clean
$ make
$ ./microkvm
```

プロンプトが表示されたら、以下のコマンドを一つずつ入力する:

```
/ # uname -r
7.3.0-rc4+
/ # ls /
bin   dev   init  lib   proc  root  sys
/ # echo hello
hello
/ # cat /proc/cmdline
console=ttyS0 earlyprintk=serial rdinit=/init
```

コマンドと結果が表示されれば、ホスト入力 → UART RX → ゲストでの実行 → UART TX の往復を確認できる。終了するにはホスト端末で `Ctrl-C` を押す。

## 重要な知見

受信データは UART の RBR に置き、IRQ 4 はその到着を知らせる。stdin スレッドを vCPU 実行から分けることで、ゲストを動かしながらホスト入力を届けられる。ただし、この最小実装には共有状態の同期と受信キューがない。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| UART RX | RBR, LSR.DR, IER.RDI, IIR=0x04 |
| 独立 I/O スレッド | stdin_thread が vCPU とは独立にホスト入力を読む |
| 通知としての IRQ | IRQ 4 がデータ到着時にゲストドライバを起こす |
| ホスト端末の設定 | ICANON/ECHO を無効化し、ISIG は維持 |
| ESC フィルタリング | ESC から次の英字までを破棄 |
| initramfs + init | シェルが /init の標準入出力を引き継ぐ |
| 双方向 I/O | TX に加え、RX データと割り込みをゲストへ届ける |

## 変わったこと

- `microkvm.c` に `stdin_thread()` と、その起動処理を追加。
- stdin スレッドで ESC シーケンスを読み捨て、それ以外を既存の `uart_rx()` に渡す。
- `uart.c`、端末設定、initramfs は Step 10 から変更なし。

## 次のステップ

[Step 12: virtio-mmio デバイスの発見](step12_virtio-discovery.md) — virtio-mmio の識別レジスタを実装し、Linux にデバイスを認識させる。
