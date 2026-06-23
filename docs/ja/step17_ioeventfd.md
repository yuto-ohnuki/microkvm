# Step 17: ioeventfd — TX kick の exit elimination

## 目的

transmit queue の QueueNotify 書き込みによる VM exit を排除する。KVM が eventfd 経由でカーネル空間内で通知を完結させ、vCPU は TX kick のために一度も停止しなくなる。

## 背景

### Step 15-16 の問題

Step 15-16 では guest が QueueNotify に書き込む（transmitq を kick する）たびに:
```
Guest が QueueNotify に書き込む
  → EPT violation
  → VM exit (KVM_EXIT_MMIO)
  → vCPU 停止
  → VMM が TX 処理
  → KVM_RUN で再開
  → vCPU 再起動
```

`echo hello > /dev/hvc0` のたびに vCPU が TX 処理中に完全に停止する。これは不要 — データは既に shared memory にあるので、vCPU が待つ必要はない。

### ioeventfd とは?

ioeventfd は KVM の機能で、特定の MMIO/PIO write を **カーネル内** でインターセプトし、userspace に exit することなく eventfd シグナルに変換する:

```
Before (Step 15):                        After (Step 17):
Guest が QueueNotify=1 に書く             Guest が QueueNotify=1 に書く
  → EPT violation                          → EPT violation
  → userspace に VM exit        　       　 → KVM が ioeventfd リストを確認
  → vCPU 停止                           　  → addr + value 一致!
  → VMM が TX 処理                          → カーネル内で eventfd_signal()
  → KVM_RUN                                → vCPU 即座に再開
  → vCPU 再開                             　→ txkick_thread が起床して TX 処理
```

vCPU は「~5μs 停止」から「一切停止しない」に変わる。

### eventfd の基本

eventfd は Linux の軽量通知メカニズム:
- `write(fd, &val, 8)` → 内部カウンタをインクリメント（シグナル送信）
- `read(fd, &val, 8)` → カウンタを読んで 0 にリセット（シグナルまでブロック）

複数回の kick は1回の起床に統合 (coalesce) されることがある。eventfd は個別イベントではなくカウンタを保持するため、guest が txkick_thread の起床前に3回 kick すると、thread は `val=3` を受け取るが TX 処理は1回 — 全 pending バッファを1パスで処理する。

KVM の ioeventfd はこれを MMIO trap パスに組み込む: guest がマッチするアドレス+値を書くと、KVM が内部で `eventfd_signal()` を呼ぶ。

### datamatch

`KVM_IOEVENTFD_FLAG_DATAMATCH` + `.datamatch = 1` の意味:
- Guest が QueueNotify = **1** (transmitq) を書く → eventfd 発火
- Guest が QueueNotify = **0** (receiveq) を書く → eventfd 発火しない、通常の MMIO exit

transmitq kick だけを選択的に高速化し、receiveq kick は通常の exit のまま残す。

## 実行フロー

```
Guest                          KVM (kernel)              txkick_thread
─────                          ────────────              ─────────────
echo hello > /dev/hvc0
  ↓
virtio-console ドライバ:
  avail ring + descriptor 準備完了
  writel(1, 0xD0000050)
    ↓ EPT violation
                               ioeventfd リスト確認
                               addr=0xD0000050, val=1 → 一致!
                               eventfd_signal(txkick_fd)
                               vCPU 即座に再開
                               (userspace への exit なし)
                                                        read(txkick_fd) が返る
                                                        virtio_console_tx()
                                                        → "hello" が stdout に表示
```

## 実装

### microkvm.c の追加

1. eventfd 作成と KVM への登録:
```c
txkick_fd = eventfd(0, EFD_CLOEXEC);

struct kvm_ioeventfd ioeventfd = {
    .addr = VIRTIO_MMIO_BASE + VIRTIO_MMIO_QUEUE_NOTIFY,
    .len = 4,
    .datamatch = 1,     /* transmitq のみ */
    .fd = txkick_fd,
    .flags = KVM_IOEVENTFD_FLAG_DATAMATCH,
};
ioctl(vmfd, KVM_IOEVENTFD, &ioeventfd);
```

2. 専用スレッドが eventfd を読んで TX 処理:
```c
static void *txkick_thread(void *arg) {
    (void)arg;
    uint64_t val;
    while (read(txkick_fd, &val, sizeof(val)) == sizeof(val)) {
        virtio_console_tx(&virtio_dev, virtio_dev.ram, virtio_dev.ram_size);
    }
    return NULL;
}
```

### virtio_mmio.c の変更

- `virtio_console_tx` を `static` から公開に変更（txkick_thread から呼ぶため）
- QueueNotify case から直接 TX 呼び出しを削除（ioeventfd がカーネル内で処理）

### virtio_mmio.h の追加

```c
void virtio_console_tx(struct virtio_mmio_dev *dev, uint8_t *ram, size_t ram_size);
```

## 出力

```
/ # echo hello > /dev/hvc0
hello
```

Step 15 と同じ出力 — ただし内部的には TX kick で VM exit が発生しない。vCPU は guest コードの実行を続けながら、`txkick_thread` がデータを非同期処理する。

## 重要な知見

ioeventfd は **通知** と **処理** を分離する。guest の kick は vCPU を止めない fire-and-forget シグナルになる。処理は別スレッドで非同期に行われる。同じアーキテクチャを使うもの:
- **QEMU**: virtio kick acceleration
- **vhost-net**: カーネル側ネットワーキング + ioeventfd

進化の流れ: 1バイトごとの exit (UART) → バッチごとの exit (virtio Step 15) → **zero exit** (ioeventfd Step 17)。

プロダクションでは同じパターンが vhost-net を動かす:
```
Guest kick → ioeventfd → vhost カーネルスレッド → NIC ハードウェア
```
microkvm の `txkick_thread` は vhost のカーネルスレッドの userspace 版。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| ioeventfd | KVM がカーネル内で MMIO write をインターセプトし eventfd にシグナル |
| eventfd | 軽量スレッド間通知 (write でシグナル、read で待機) |
| datamatch | 特定の書き込み値のみを選択的にトリガー |
| 非同期処理 | TX は別スレッドで処理、vCPU は待たない |
| Exit elimination | QueueNotify が transmitq について KVM_EXIT_MMIO を引き起こさなくなる |
| vhost パターン | プロダクション virtio 高速化と同じアーキテクチャ |

## 変わったこと

Step 16 からの変更:
- **microkvm.c**: `#include <sys/eventfd.h>`、`txkick_fd` + `txkick_thread`、`KVM_IOEVENTFD` 登録、スレッド作成
- **virtio_mmio.c**: `virtio_console_tx` を公開、QueueNotify case が直接 TX を呼ばなくなる
- **virtio_mmio.h**: `virtio_console_tx()` プロトタイプ追加

## 次のステップ

[Step 18: irqfd](step18_irqfd.md) — IRQ 注入の ioctl overhead を排除する。eventfd に書き込むだけで IRQ 5 がカーネル空間内で直接注入される。
