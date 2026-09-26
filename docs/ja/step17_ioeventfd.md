# Step 17: ioeventfd — TX kick のユーザー空間への exit を省く

## 目的

transmitq の QueueNotify を KVM から eventfd に通知し、TX 処理を専用の `txkick_thread` に移す。vCPU スレッドが `KVM_EXIT_MMIO` を受け取り、その場で TX を処理する経路を省く。

## 背景

### Step 16 から変わる処理経路

これまでは、QueueNotify への書き込みで `KVM_RUN` がユーザー空間へ戻り、vCPU スレッドが TX を処理してからゲストを再開していた。

ioeventfd は、登録条件に一致する MMIO/PIO 書き込みを KVM 内で eventfd への通知に変える機能。TX は通知を受けた別スレッドが処理するため、vCPU スレッドは TX 処理を終えてから `KVM_RUN` を呼び直す必要がなくなる。

ただし、ゲストから KVM に制御が移るハードウェアの VM exit は残る。省くのは **KVM からユーザー空間へ `KVM_EXIT_MMIO` を返す処理**であり、「vCPU が一切停止しない」という意味ではない。

### eventfd の基本

ここでは `eventfd(0, EFD_CLOEXEC)` で、初期値0のカウンタを持つ通知用 fd を作る:

- `write(fd, &val, 8)` → 内部カウンタに val を加算
- `read(fd, &val, 8)` → カウンタを読んで 0 にリセット（0 の間はブロック）

複数回の kick が1回の read にまとまることがある。例えば3回分の通知を `val=3` として受け取っても、スレッドは TX 関数を1回呼ぶ。処理対象は通知の回数ではなく、Available Ring の `last_avail_idx` から、その呼び出しで読み取った `avail_idx` までで決まる。

KVM の ioeventfd はこれを MMIO trap パスに組み込む: guest がマッチするアドレス+値を書くと、KVM が内部で `eventfd_signal()` を呼ぶ。

### datamatch

登録条件は、GPA `0xD0000050`・4バイト書き込み・値 `1`。`KVM_IOEVENTFD_FLAG_DATAMATCH` で書き込み値も比較する:

- Guest が QueueNotify = **1** (transmitq) を書く → eventfd 発火
- Guest が QueueNotify = **0** (receiveq) を書く → eventfd 発火しない、通常の MMIO exit

transmitq kick だけを選択的に高速化し、receiveq kick は通常の exit のまま残す。

## 実行フロー

```text
Guest (Linux)               KVM                         VMM
     | Descriptor と Available Ring を準備                | txkick_thread が read() で待機
     |-- QueueNotify=1 ----->|                           |
     |                       | アドレス・幅・値が一致        |
     |                       |-- eventfd に通知 --------->| read() が返る
     |<-- ゲスト実行を再開 -----|                           | virtio_console_tx()
     |                       |                           | RAM からデータを読む
     |                       |                           | stdout へ出力
     |                       |                           | Used Ring を更新
```

ゲストの再開と TX スレッドの実行は、ホストのスケジューリングに従って進む。TX スレッドはユーザー空間で動き、データ処理が KVM 内に移るわけではない。

## 実装

### microkvm.c の追加

1. eventfd 作成と KVM への登録:

```c
txkick_fd = eventfd(0, EFD_CLOEXEC);
if (txkick_fd < 0) {
    perror("eventfd");
    return 1;
}

struct kvm_ioeventfd ioeventfd = {
    .addr = VIRTIO_MMIO_BASE + VIRTIO_MMIO_QUEUE_NOTIFY,
    .len = 4,
    .datamatch = 1,     /* transmitq のみ */
    .fd = txkick_fd,
    .flags = KVM_IOEVENTFD_FLAG_DATAMATCH,
};
if (ioctl(vmfd, KVM_IOEVENTFD, &ioeventfd) < 0) {
    perror("KVM_IOEVENTFD");
    return 1;
}
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

`main()` は vCPU スレッドを起動する前に、`pthread_create()` で `txkick_thread` を起動する。eventfd 作成や KVM への登録が失敗した場合は終了し、従来の MMIO 処理へ切り替える経路はない。

### virtio_mmio.c の変更

- `virtio_console_tx` を `static` から公開に変更（txkick_thread から呼ぶため）
- QueueNotify case から直接 TX を呼ぶ処理を削除。条件に一致する TX 通知は eventfd に届き、`txkick_thread` が処理する

### virtio_mmio.h の追加

```c
void virtio_console_tx(struct virtio_mmio_dev *dev, uint8_t *ram, size_t ram_size);
```

## 出力

ゲストで `/dev/hvc0` に書き込み、TX が動作することを確認する:

```
/ # echo hello > /dev/hvc0
hello
```

この表示は送信が動作することの確認であり、exit 回数の計測ではない。QueueNotify のログは以前から抑制しているため、ログが出ないことだけでは ioeventfd の効果は判断できない。

## 重要な知見

ioeventfd は、ゲストからの通知と VMM のデータ処理を分離する。KVM は一致する書き込みを eventfd へ通知し、ユーザー空間の専用スレッドが共有メモリ上のキューを処理する。減らすのは TX kick に伴うユーザー空間への往復で、ハードウェアの VM exit 全体ではない。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| ioeventfd | KVM がカーネル内で MMIO write をインターセプトし eventfd にシグナル |
| eventfd | 軽量スレッド間通知 (write でシグナル、read で待機) |
| datamatch | 特定の書き込み値のみを選択的にトリガー |
| 非同期処理 | vCPU スレッドから TX 処理を分離 |
| ユーザー空間への exit 削減 | 登録条件に一致する TX kick で KVM_EXIT_MMIO を返さない |

## 変わったこと

Step 16 からの変更:

- **microkvm.c**: `#include <sys/eventfd.h>`、`txkick_fd` + `txkick_thread`、`KVM_IOEVENTFD` 登録、スレッド作成
- **virtio_mmio.c**: `virtio_console_tx` を公開、QueueNotify case が直接 TX を呼ばなくなる
- **virtio_mmio.h**: `virtio_console_tx()` プロトタイプ追加

## 次のステップ

[Step 18: irqfd](step18_irqfd.md) — RX の IRQ 5 通知を、`KVM_IRQ_LINE` の呼び出しから eventfd への書き込みに置き換える。
