# Step 18: irqfd — IRQ 注入の exit elimination

## 目的

IRQ 5 注入の `ioctl(KVM_IRQ_LINE)` を eventfd write に置き換える。KVM が割り込み配送をカーネル空間内で完結させる — VMM からの syscall が不要になる。

## 背景

### Step 17 後に残っている問題

Step 17 で TX kick の exit は排除した。しかし RX にはまだ overhead がある:

```
Step 16-17 の RX パス:
  stdin_thread が 'a' を受信
  → virtio_console_rx() が guest バッファに書き込み
  → ioctl(KVM_IRQ_LINE, irq=5, level=1)    ← syscall #1
  → ioctl(KVM_IRQ_LINE, irq=5, level=0)    ← syscall #2
```

RX 文字ごとに edge-triggered IRQ 注入に 2 回の syscall が必要。高スループット入力では ioctl overhead が顕著になる。

### irqfd とは?

irqfd は ioeventfd の IRQ 方向版:

| | ioeventfd (Step 17) | irqfd (Step 18) |
|---|---|---|
| 方向 | Guest → Host (kick) | Host → Guest (IRQ) |
| トリガー | Guest MMIO write | VMM eventfd write |
| 置き換え対象 | KVM_EXIT_MMIO | ioctl(KVM_IRQ_LINE) |
| 効果 | vCPU が停止しない | VMM が syscall しない |

irqfd では eventfd に write するだけで KVM がカーネル内で直接割り込みを注入する — edge-triggered、level 管理不要。

### Before vs After

```
Step 16 (ioctl):
  stdin_thread:
    virtio_console_rx()
    ioctl(KVM_IRQ_LINE, level=1)    ← user→kernel→user
    ioctl(KVM_IRQ_LINE, level=0)    ← user→kernel→user
  合計: 1文字あたり 2 syscall

Step 18 (irqfd):
  stdin_thread:
    virtio_console_rx()
    write(irq5_fd, &1, 8)           ← 1 syscall、KVM が自動注入
  合計: 1文字あたり 1 syscall、auto edge-trigger
```

## 実行フロー

```
Host (stdin_thread)              KVM (kernel)              Guest
───────────────────              ────────────              ─────
virtio_console_rx() 完了
write(irq5_fd, &1, 8)
                                 eventfd シグナル
                                 → KVM が IRQ 5 を注入
                                   (自動 edge-trigger:
                                    assert + deassert)
                                                          IRQ ハンドラ起動
                                                          InterruptStatus を読む
                                                          InterruptACK を書く
                                                          used ring を処理
                                                          /dev/hvc0 に配送
```

ioctl なし。手動 level=1/level=0 なし。KVM が edge-trigger シーケンス全体を内部処理する。

> KVM はどうやって eventfd を「見ている」のか？ `KVM_IRQFD` 呼び出し時に KVM が eventfd に poll handler を登録する。カウンタが非ゼロになると（= 誰かが write すると）、カーネルコンテキストから IRQ が注入される — userspace の関与なし。

**注意:** irqfd が置き換えるのは *通知パス* のみ。guest 側は依然として InterruptStatus を読んで確認し、InterruptACK で割り込みを acknowledge する — Step 16 の virtio プロトコルは変わらない。

## 実装

### microkvm.c の変更

1. eventfd 作成と irqfd 登録:
```c
irq5_fd = eventfd(0, EFD_CLOEXEC);

struct kvm_irqfd irqfd = {
    .fd = irq5_fd,
    .gsi = 5,       /* IRQ 5 = virtio-mmio 割り込み線 */
    .flags = 0,     /* デフォルトで edge-triggered */
};
ioctl(vmfd, KVM_IRQFD, &irqfd);
```

2. stdin_thread の ioctl 呼び出しを置き換え:
```c
/* Before (Step 16): edge trigger に 2 ioctl */
struct kvm_irq_level irq = { .irq = 5, .level = 1 };
ioctl(g_vmfd, KVM_IRQ_LINE, &irq);
irq.level = 0;
ioctl(g_vmfd, KVM_IRQ_LINE, &irq);

/* After (Step 18): 1 write で KVM が edge trigger 処理 */
uint64_t val = 1;
write(irq5_fd, &val, sizeof(val));
```

### 依存関係

`KVM_IRQFD` は `KVM_CREATE_IRQCHIP` の後に呼ぶ必要がある。irqchip が存在しないと IRQ 線を eventfd に接続できない。

## 出力

```
/ # cat /dev/hvc0 &
/ # 
[monitor] input → hvc0 (virtio)
abc
abc
```

Step 16 と同じ動作 — ただし IRQ 注入に ioctl が不要になる。出力では見えないが latency で計測可能。実測値は [benchmark.md](benchmark.md) を参照。

## 重要な知見

ioeventfd (Step 17) + irqfd (Step 18) で、virtio の notification path 全体がカーネル内に収まる:

```
TX 通知: Guest MMIO write → ioeventfd → カーネルがシグナル → スレッドが処理
RX 通知: VMM eventfd write → irqfd → カーネルが IRQ 注入 → guest が処理
```

TX kick も RX IRQ も、full VM exit や ioctl round-trip を必要としない。vCPU は中断なしに走り続け、VMM は不要な syscall を避ける。通知処理が vCPU パスから完全に外れる — vhost-net がカーネルスレッドとハードウェアオフロードで実現するのと同じ原則。

### 最適化の全体像

| Step | メカニズム | 操作あたりの exit/syscall |
|------|-----------|------------------------------|
| 11 | UART PIO | 1バイトごとに 1 VM exit |
| 15 | virtio QueueNotify | バッチごとに 1 VM exit |
| 17 | ioeventfd | **0 VM exit** (カーネル eventfd) |
| 16 | ioctl KVM_IRQ_LINE | IRQ ごとに 2 syscall |
| 18 | irqfd | **1 write** (カーネルが自動注入) |

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| irqfd | eventfd write → KVM カーネルが IRQ 注入 (ioctl 不要) |
| ioeventfd との対称性 | ioeventfd = guest→host 高速化、irqfd = host→guest 高速化 |
| Edge-trigger 自動化 | KVM が assert+deassert を内部処理 |
| GSI (Global System Interrupt) | `.gsi = 5` で eventfd を IRQ 線 5 にマッピング |
| Notification path 最適化 | 両方向がカーネル内で完結 |

## 変わったこと

Step 17 からの変更:
- **microkvm.c のみ**: `irq5_fd` eventfd 作成、`KVM_IRQFD` 登録、stdin_thread が `ioctl(KVM_IRQ_LINE)` × 2 を `write(irq5_fd)` × 1 に置き換え

`virtio_mmio.c` / `virtio_mmio.h` の変更なし。

## 次のステップ

Phase C 完了。virtio I/O パスは最適化された通知と共に完全に機能する。

[Step 19: KVM MMU stats](../step19_mmu-stats.md) で Phase D (Memory State Management) が始まる — EPT 内部の観察、dirty page tracking、snapshot、live migration。
