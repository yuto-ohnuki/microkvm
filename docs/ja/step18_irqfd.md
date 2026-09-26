# Step 18: irqfd — IRQ 注入を eventfd に置き換える

## 目的

RX 完了時の IRQ 5 通知を、`KVM_IRQ_LINE` の ioctl 2回から eventfd への write 1回に置き換える。KVM が eventfd の通知を受けてゲストへ割り込みを注入する。

## 背景

### Step 17 から変わる処理経路

これまでは `stdin_thread` が1文字を receiveq に書き込むたび、`KVM_IRQ_LINE` で IRQ 5 を assert（level=1）、deassert（level=0）していた。

irqfd は eventfd とゲストの割り込み線を結び付ける仕組み。起動時に `KVM_IRQFD` で登録しておけば、RX 完了時は eventfd に書き込むだけで KVM に割り込み注入を依頼できる。**write も syscall なので、IRQ 通知に必要な syscall は2回から1回になる。**

### ioeventfd との違い

| | ioeventfd (Step 17) | irqfd (Step 18) |
|---|---|---|
| 方向 | Guest → VMM (TX kick) | VMM → Guest (RX IRQ) |
| トリガー | Guest の QueueNotify 書き込み | VMM の eventfd 書き込み |
| 置き換える処理 | TX kick に対する KVM_EXIT_MMIO | IRQ 5 の KVM_IRQ_LINE × 2 |
| 通知先 | TX 用スレッド | ゲストの割り込みコントローラ |

この実装では `.flags = 0` で登録し、KVM が IRQ 5 の assert / deassert を行う。VMM が2回に分けて level を指定する必要はない。

## 実行フロー

```text
Guest (Linux)               KVM                         VMM (stdin_thread)
     |                       |                           | ホストから1文字を受信
     |                       |                           | virtio_console_rx()
     |                       |                           |   受信バッファへコピー
     |                       |                           |   Used Ring を更新
     |                       |                           |   interrupt_status |= 1
     |                       |<-- eventfd に write ------| RX 成功時だけ通知
     |<-- IRQ 5 を注入 -------|                           |
     | InterruptStatus を読む（MMIO）                      |
     | InterruptACK を書く（MMIO）                         |
     | Used Ring から受信データを回収                        |
     | /dev/hvc0 に配送                                   |
```

irqfd が担当するのは IRQ 注入。受信バッファと Used Ring の更新は VMM が行い、ゲストの InterruptStatus / InterruptACK へのアクセスは引き続きユーザー空間で処理する。

## 実装

### microkvm.c — eventfd の作成と登録

IRQ 5 通知用に `irq5_fd` を追加する:

```c
irq5_fd = eventfd(0, EFD_CLOEXEC);
if (irq5_fd < 0) {
    perror("eventfd irq5");
    return 1;
}
```

`KVM_CREATE_IRQCHIP` で割り込みコントローラを作成した後、eventfd を GSI（Global System Interrupt）5 に結び付ける:

```c
struct kvm_irqfd irqfd = {
    .fd = irq5_fd,
    .gsi = 5,   /* IRQ 5 = virtio-mmio の割り込み線 */
    .flags = 0,
};
if (ioctl(vmfd, KVM_IRQFD, &irqfd) < 0) {
    perror("KVM_IRQFD");
    return 1;
}
```

登録時には ioctl を使うが、登録後の RX 通知には eventfd を使う。

### microkvm.c — RX 完了時の通知

```c
if (virtio_mode) {
    if (virtio_console_rx(&virtio_dev, &c, 1) == 0) {
        /* Signal IRQ5 via irqfd (no ioctl needed) */
        uint64_t val = 1;
        write(irq5_fd, &val, sizeof(val));
    }
} else {
    uart_rx(&uart, c, g_vmfd);
}
```

`virtio_console_rx()` が受信バッファと Used Ring を更新できた場合だけ通知する。空きバッファがないなどの理由で失敗した場合は、irqfd へ書き込まない。

## 出力

ゲストで `cat /dev/hvc0 &` を起動し、入力先を virtio に切り替えて `abc` と Enter を入力する。

```text
/ # cat /dev/hvc0 &
/ # < Ctrl-A v >
[monitor] input → hvc0 (virtio)
abc
abc

< Ctrl-A v >
[monitor] input → ttyS0 (UART)

/ #
```

hvc0 の端末エコーが有効なら、最初の `abc` は hvc0 の TX 経由のエコー、次の `abc` は `cat` が UART へ出力した内容。この確認は RX と割り込み配送の動作確認であり、遅延の改善を示すものではない。ioeventfd / irqfd の計測方法と結果は [benchmark.md](benchmark.md) を参照。

## 重要な知見

ioeventfd は TX kick に伴うユーザー空間への戻りを省き、irqfd は RX 完了時の IRQ 通知を ioctl 2回から write 1回に変える。最適化するのは通知の経路で、データ処理は引き続き VMM が担当する。VM exit や syscall 全体がなくなるわけではない。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| irqfd | eventfd の通知を KVM がゲストの IRQ に変換 |
| GSI (Global System Interrupt) | `.gsi = 5` で eventfd を IRQ 5 に結び付ける |
| IRQ 通知の syscall 削減 | assert / deassert の ioctl 2回を write 1回に置き換える |
| データ処理と通知の分担 | VMM が受信データを公開し、KVM が割り込みを配送 |

## 変わったこと

Step 17 からの変更は **microkvm.c**:

- `irq5_fd` の追加、eventfd 作成、`KVM_IRQFD` 登録
- `stdin_thread` の IRQ 5 通知を `KVM_IRQ_LINE` × 2 から `write(irq5_fd)` × 1 に変更
- 終了処理で `irq5_fd` を close

## 次のステップ

[Step 19: KVM MMU stats](step19_mmu-stats.md) — KVM の統計情報を取得し、ゲストメモリを管理する MMU の動作を観察する。
