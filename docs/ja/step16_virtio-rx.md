# Step 16: virtio-console RX (host → guest)

## 目的

ホスト端末の入力を receiveq (queue 0) のバッファに書き込み、IRQ 5 で Linux に受信を通知する。入力先を切り替えるモニター操作を追加し、`/dev/hvc0` から文字を読み取れるようにする。

## 背景

### RX は TX の逆方向

Step 15 (TX) では guest がバッファにデータを配置して VMM に kick した。RX では役割が逆:

- **guest** が空バッファを提供する（virtio-console ドライバが初期化時に receive バッファを確保して receiveq に投入 — Step 14 末尾の 128 回の QueueNotify kick がこれ）
- **VMM** がそのバッファにデータを書き込む
- **VMM** が IRQ で guest に「データが来た」と通知する（IRQ 5 — デバイス setup 時に kernel command line で設定した割り込み線）

### なぜ RX には IRQ が必要か

ホストからの入力はゲストとは独立したタイミングで発生する。この実装では、VMM が Used Ring を更新した後に IRQ 5 を注入し、Linux の virtio-mmio ドライバに完了バッファの確認を促す。データは共有メモリで渡し、割り込みはその到着を知らせる。

### RX フロー概要

```
Host stdin → stdin_thread → virtio_console_rx():
  1. receiveq から空バッファを見つける (VRING_DESC_F_WRITE フラグ)
  2. guest バッファにデータを書き込む (guest RAM に memcpy)
  3. used ring に descriptor を記録（実バイト数付き）
  4. interrupt_status |= 0x1 をセット

stdin_thread (virtio_console_rx() が成功した場合):
  5. KVM_IRQ_LINE で IRQ 5 を注入

Guest IRQ ハンドラ:
  6. InterruptStatus を読む → 0x1 (used buffer 通知)
  7. InterruptACK に 0x1 を書く（pending クリア）
  8. used ring を確認 → 完了した descriptor を発見
  9. バッファからデータを読む → /dev/hvc0 に配送
```

### Ctrl-A v モード切替

microkvm の stdin は UART (ttyS0) と virtio (hvc0) で共有。**Ctrl-A を押して離し、その後 `v` を押す**と入力先が切り替わる。

- デフォルト: UART モード → `uart_rx()` が ttyS0 に配送（シェル）
- `Ctrl-A v` 後: Virtio モード → `virtio_console_rx()` が hvc0 に配送

再度同じ操作をすると UART に戻る。切替操作は VMM が消費し、ゲストには渡さない。UART に戻る際は改行を1文字送り、シェルのプロンプトを再表示させる。

### InterruptStatus / InterruptACK

| レジスタ | 方向 | 役割 |
|----------|-----------|---------|
| InterruptStatus (0x060) | Read | VMM が guest にどの割り込みが pending か伝える (bit 0 = used buffer) |
| InterruptACK (0x064) | Write | guest が処理済み割り込みをクリア |

Linux の IRQ ハンドラは InterruptStatus を読み、InterruptACK に書いたビットをクリアする。これはデバイス内の保留状態の更新で、`KVM_IRQ_LINE` による IRQ 線の上げ下げとは別の操作。InterruptStatus の読み取りと InterruptACK の書き込みは、ログ出力を抑制している。

## 実行フロー

```text
Guest (Linux)               KVM                         VMM
     | receiveq に空バッファを登録                          |
     |                       |                           | virtio モードで文字入力
     |                       |                           | virtio_console_rx()
     |                       |                           | WRITE バッファへコピー
     |                       |                           | Used Ring を更新
     |                       |                           | interrupt_status |= 1
     |                       |<-- KVM_IRQ_LINE (5, 1→0)--| stdin_thread
     |<-- IRQ 5 配送 ---------|                           |
     |-- InterruptStatus 読み>|-- KVM_EXIT_MMIO --------->|
     |                       |                           | 保留ビットを返す
     |                       |<-- KVM_RUN ---------------|
     |-- InterruptACK 書き -->|-- KVM_EXIT_MMIO --------->|
     |                       |                           | 指定ビットをクリア
     |                       |<-- KVM_RUN ---------------|
     | Used Ring と受信バッファを確認                        |
     | /dev/hvc0 へ配送       |                           |
```

IRQ は入力データを運ばない。Linux は Used Ring の `id` と `len` を使って、どのバッファに何バイト届いたかを確認する。

## 実装

### virtio_mmio.h の追加

```c
uint32_t interrupt_status;  /* pending 割り込みビット */

/* RX: バッファと Used Ring を更新し、割り込み保留ビットを立てる */
int virtio_console_rx(struct virtio_mmio_dev *dev, const uint8_t *data, size_t len);
```

### virtio_mmio.c — virtio_console_rx()

receiveq の Available Ring から次の Descriptor を取得する。以下は、その Descriptor の確認から受信完了を記録するまでの抜粋:

```c
/* RX descriptors must have WRITE flag (device writes into guest buffer) */
if (!(desc.flags & VRING_DESC_F_WRITE))
    return -1;

/* Overflow-safe bounds check */
size_t copy_len = len < desc.len ? len : desc.len;
if (desc.addr >= ram_size || copy_len > ram_size - desc.addr)
    return -1;

memcpy(ram + desc.addr, data, copy_len);

/* Post to used ring with actual bytes written */
uint16_t used_idx;
memcpy(&used_idx, ram + used_base + 2, sizeof(uint16_t));
uint16_t used_slot = used_idx % vq->num;

struct vring_used_elem elem = {
    .id = desc_idx,
    .len = (uint32_t)copy_len
};
memcpy(ram + used_base + 4 + used_slot * 8, &elem, sizeof(elem));

used_idx++;
memcpy(ram + used_base + 2, &used_idx, sizeof(uint16_t));

vq->last_avail_idx++;

/* Set interrupt pending (bit 0 = used buffer notification) */
dev->interrupt_status |= 0x1;

return 0;
```

TX と異なり、`used_elem.len` にゲストのバッファへ実際に書き込んだバイト数を返す。RX は先頭 Descriptor 1個だけを使い、chain は辿らない。

stdin スレッドは1文字ずつ渡す。利用可能なバッファがない場合などは `-1` を返し、その文字は再試行せず破棄される。IRQ 注入は成功時に呼び出し側が行う。

### virtio_mmio.c — InterruptStatus / InterruptACK

```c
case VIRTIO_MMIO_INTERRUPT_STATUS:
    val = dev->interrupt_status;        /* guest が読んで割り込み源を特定 */
    break;

case VIRTIO_MMIO_INTERRUPT_ACK:
    dev->interrupt_status &= ~value;    /* guest が処理済みビットをクリア */
    break;
```

### microkvm.c — stdin_thread の変更

```c
if (virtio_mode) {
    if (virtio_console_rx(&virtio_dev, &c, 1) == 0) {
        /* guest に通知: receiveq にデータあり */
        struct kvm_irq_level irq = { .irq = 5, .level = 1 };
        ioctl(g_vmfd, KVM_IRQ_LINE, &irq);
        irq.level = 0;
        ioctl(g_vmfd, KVM_IRQ_LINE, &irq);
    }
} else {
    uart_rx(&uart, c, g_vmfd);
}
```

IRQ 注入は edge trigger (0→1→0) — Step 11 の UART IRQ 4 と同じパターン。

## 出力

ゲストで `cat /dev/hvc0 &` を起動し、入力先を virtio に切り替えて `abc` と Enter を入力する。最後に UART に戻す。

```
/ # cat /dev/hvc0 &
/ # < Ctrl-A v >
[monitor] input → hvc0 (virtio)
abc
abc

< Ctrl-A v >
[monitor] input → ttyS0 (UART)

/ #
```

hvc0 の端末エコーが有効なら、最初の `abc` は hvc0 の TX 経由のエコー、次の `abc` は `cat` が ttyS0 (UART) へ書き出した内容。`cat` の標準出力はシェルから引き継いだままで、モニター操作が切り替えるのはホストからの入力先だけ。

## 重要な知見

RX では、ゲストが用意したバッファに VMM が書き込み、Used Ring で完了を公開してから割り込みで知らせる。共有メモリはデータを、IRQ は確認のきっかけを渡す。InterruptACK はその通知の保留ビットをクリアする。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| Shared memory RX | VMM が guest 提供のバッファに直接書き込む |
| VRING_DESC_F_WRITE | 「デバイスがここに書いてよい」を示すフラグ |
| IRQ 注入 | KVM_IRQ_LINE で guest の割り込みハンドラを起動 |
| InterruptStatus/ACK | guest が割り込み源の特定とクリアを行う |
| Edge-triggered IRQ | assert (level=1) → deassert (level=0) |
| Ctrl-A v モード切替 | 1つの stdin を UART と virtio で共有 |
| 双方向 virtio | transmitq と receiveq で送信・受信を分担 |

## 変わったこと

Step 15 からの変更:

- **virtio_mmio.h**: `interrupt_status` フィールド、`virtio_console_rx()` プロトタイプ
- **virtio_mmio.c**: `virtio_console_rx()` 実装、`INTERRUPT_STATUS` が live 値を返す、`INTERRUPT_ACK` がビットクリア、InterruptStatus/ACK のログ抑制
- **microkvm.c**: stdin_thread に `Ctrl-A v` モード切替、virtio モードで `virtio_console_rx()` + IRQ 5 注入

## 次のステップ

[Step 17: ioeventfd](step17_ioeventfd.md) — TX の QueueNotify を KVM が eventfd に通知し、vCPU スレッドへ `KVM_EXIT_MMIO` を返す経路を省く。
