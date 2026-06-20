# Step 11: 対話型シェル

## 目的

シリアル **RX** (受信) サポートを追加し、ゲストがホスト端末からの入力を受け付けられるようにする。busybox initramfs と組み合わせて、VM 内で対話型シェルを実行する — 「使える VMM」マイルストーンの完成。

## 背景

### TX vs RX

Step 10 はシリアル TX (送信) を実装: ゲストが THR に書き、VMM が stdout に出力。
Step 11 は逆方向を追加:

| 方向 | UART の役割 | フロー |
|------|------------|--------|
| TX (Step 10) | Guest → Host | ゲストが THR に書く → VMM が stdout に出力 |
| RX (Step 11) | Host → Guest | ユーザーが入力 → VMM が RBR を設定 → IRQ 4 → ゲストが読む |

### なぜ別の stdin スレッドが必要か

vCPU スレッドはほとんどの時間 `ioctl(KVM_RUN)` 内でブロックされている。同時にホストキーボード入力を待つことはできない。専用の **stdin リーダースレッド**がこれを解決:

```
[ホスト端末]  →  [stdin_thread]  →  [UART RBR + IRQ 4]  →  [ゲストカーネル]
  キー入力      read(stdin)        uart_rx()               シリアルドライバ
```

概念的に QEMU のキャラクタデバイスバックエンドに類似 — vCPU 実行とは独立に I/O を処理する。

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

7. ドライバが 'a' を tty レイヤーに渡す → シェルが処理
   シェルが TX 経由で 'a' をエコーバック → ホスト端末に表示
```

### Raw ターミナルモード

デフォルトではホスト端末は行単位で入力をバッファリングする (canonical モード)。
対話型シェルには各キー入力を即座に配送する必要がある:

```c
struct termios raw = orig_termios;
raw.c_lflag &= ~(ICANON | ECHO);    /* 行バッファリングなし、ローカルエコーなし */
raw.c_cc[VMIN] = 1;                 /* 1 バイトで read が返る */
tcsetattr(STDIN_FILENO, TCSANOW, &raw);
```

端末は `atexit(restore_terminal)` で終了時に復元される。

### ESC シーケンスフィルタリング

カーネルのシリアルドライバは初期化中にカーソル位置を問い合わせる (`ESC[6n`)。
ホスト端末が `ESC[row;colR` で応答する。フィルタリングなしでは、この応答がゲストへのゴミ入力として現れる。stdin スレッドは `0x1b` (ESC) で始まるシーケンスを終端文字が見つかるまで破棄する。ホスト端末が生成する ANSI エスケープ応答の簡略化フィルタ — 完全なエスケープシーケンスパーサーではない。

## 実行フロー

```
VMM                                      Guest (Linux + busybox)
───                                      ───────────────────────
set_raw_terminal()
pthread_create(stdin_thread)
pthread_create(vcpu_thread)
                                         カーネルブート...
                                         init: open /dev/ttyS0
                                         execve("/bin/sh")
                                         sh: プロンプト書き込み "~ # "
                                              ↓ (THR 経由の TX)
stdout: "~ # "

ユーザーが 'l' を入力:
  stdin_thread: read() → 'l'
  uart_rx(): rbr='l', lsr|=DR
  KVM_IRQ_LINE(4, 1→0)
                                         IRQ 4 → serial8250_interrupt
                                         IIR 読み取り → 0x04 (RDI)
                                         RBR 読み取り → 'l'
                                         tty: 'l' をエコー (TX)
stdout: "l"

ユーザーが '\n' を入力:
  (同じフロー)
                                         sh: "ls" を実行
                                         出力書き込み (TX)
stdout: "bin  dev  init  proc  sys\n"
```

## 実装

### stdin_thread

```c
static void *stdin_thread(void *arg) {
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

### uart_rx (uart.c 内)

```c
void uart_rx(struct uart8250 *u, uint8_t c, int vmfd) {
    u->rbr = c;
    u->lsr |= UART_LSR_DR;          /* Data Ready */
    if (u->ier & UART_IER_RDI) {    /* RX 割り込み有効? */
        u->pending_rdi = 1;
        /* IRQ 4 をアサート・デアサートして、カーネル内 PIC に
           見える割り込みリクエストを作成 */
        struct kvm_irq_level irq = { .irq = 4, .level = 1 };
        ioctl(vmfd, KVM_IRQ_LINE, &irq);
        irq.level = 0;
        ioctl(vmfd, KVM_IRQ_LINE, &irq);
    }
}
```

`uart_rx()` は stdin スレッドから呼ばれるが、vCPU スレッドが `uart_in()` で読むのと同じ UART 状態にアクセスする。microkvm は Step 9 で導入したデバイスロックを使って UART 状態を保護する。

### initramfs init スクリプト

```sh
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
echo "=== Hello from microkvm guest! ==="
exec /bin/sh
```

init スクリプトが必須ファイルシステムをマウントし busybox sh を exec する。カーネルコマンドラインの `console=ttyS0` と `CONFIG_DEVTMPFS_MOUNT=y` により、カーネルが自動的に stdin/stdout/stderr をシリアルコンソールに接続する — 手動で `/dev/ttyS0` を open する必要はない。

## initramfs について

Step 10 で作成した initramfs に既に busybox と対話シェル（`exec /bin/sh` の init）が含まれている。このステップでは initramfs の変更は不要 — VMM に `stdin_thread` を追加するだけでホスト入力がゲストに届くようになる。

## 出力

```
/ # uname -r
7.1.0+
/ # ls /
bin   dev   init  lib   proc  root  sys
/ # echo hello
hello
/ # cat /proc/cmdline
console=ttyS0 earlyprintk=serial rdinit=/init
```

ゼロから構築した VMM 上で、無改造 Linux カーネル上で動作する完全に対話型のシェル。

## 重要な知見

### なぜ対話的入力が重要か

Step 10 まで Linux は出力のみを生成していた。Step 11 は入力をゲストに戻すことでループを閉じる。初めて、VM 内のソフトウェアが VM 外のユーザーのアクションに反応できる。

### 完全な I/O ループ

Step 11 はシリアルデバイスの**完全な I/O ループ**を完成:

```
ユーザーキー入力 → stdin_thread → UART RBR → IRQ 4 → ゲストドライバ
     ↑                                                    │
     │                                                    ↓
ホスト端末 ← stdout ← UART THR ← ゲストドライバ ← シェル出力
```

完全なキャラクタデバイス: 割り込み駆動 RX 通知 (RDI がゲストにデータ到着を通知) 付き双方向データフロー。

同じパターン — データプレーン + 割り込み通知 — は全ての I/O デバイスに現れる。microkvm の次のフェーズ (Step 12–14) はこの文字ごとの割り込みモデルを共有メモリリングバッファ (virtio) に置き換え、I/O 操作あたりの VM exit 数を劇的に削減する。

1文字の入力で少なくとも1回の割り込みと複数の VM exit が発生する。大きな I/O ワークロード (大きなファイルの `cat` 等) ではこれが法外に高コストになる — Step 12 での virtio への移行の動機。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| UART RX | RBR, LSR.DR, IER.RDI, IIR=0x04 |
| 独立 I/O スレッド | stdin_thread が vCPU とは独立にホスト入力を読む |
| 通知としての IRQ | IRQ 4 がデータ到着時にゲストドライバを起こす |
| Raw ターミナル | ICANON/ECHO 無効で文字単位の配送 |
| ESC フィルタリング | 端末制御応答を破棄 |
| initramfs + init | PID 1 が ttyS0 を接続し shell を exec |
| 完全な I/O ループ | TX + RX + 割り込み = 完全なキャラクタデバイス |

## 次のステップ

[Step 12: Virtio リングバッファ](step12_virtio-discovery.md) — 文字ごとの MMIO/PIO を共有メモリリングバッファに置き換え、virtio アーキテクチャを導入して I/O 操作あたりの VM exit を劇的に削減する。
