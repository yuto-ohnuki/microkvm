# Step 16: virtio-console RX (host → guest)

## 目的

host stdin を receive queue と IRQ 注入で guest に配送する。ユーザーが virtio モードで入力すると、データは host → guest RAM → IRQ → ドライバ → `/dev/hvc0` と流れる。これで双方向 virtio-console が完成する。

## 背景

### RX は TX の逆方向

Step 15 (TX) では guest がバッファにデータを配置して VMM に kick した。RX では役割が逆:
- **guest** が空バッファを提供する（virtio-console ドライバが初期化時に receive バッファを確保して receiveq に投入 — Step 14 末尾の 128 回の QueueNotify kick がこれ）
- **VMM** がそのバッファにデータを書き込む
- **VMM** が IRQ で guest に「データが来た」と通知する（IRQ 5 — デバイス setup 時に kernel command line で設定した割り込み線）

### なぜ RX には IRQ が必要か

guest は host からの入力がいつ来るか知りようがない。TX は guest が主導するが、RX は非同期 — VMM が能動的に guest に通知する必要がある。割り込みがなければ、guest は receive queue を確認しない。

```
IRQ なし: VMM がデータを書く → guest は気づかない → /dev/hvc0 は空のまま
IRQ あり: VMM がデータを書く → IRQ 5 → ドライバが起床 → used ring 確認 → /dev/hvc0 に配送
```

ポーリング（guest が繰り返し used ring を確認）でも動作するが、CPU 時間を浪費する。割り込みを使えばデータが届くまで guest はスリープできる — NIC、ブロックデバイス、シリアル、NVMe など全ての I/O デバイスが割り込みを使う根本的な理由がここにある。

### RX フロー概要

```
Host stdin → stdin_thread → virtio_console_rx():
  1. receiveq から空バッファを見つける (VRING_DESC_F_WRITE フラグ)
  2. guest バッファにデータを書き込む (guest RAM に memcpy)
  3. used ring に descriptor を記録（実バイト数付き）
  4. interrupt_status |= 0x1 をセット
  5. KVM_IRQ_LINE で IRQ 5 を注入

Guest IRQ ハンドラ:
  6. InterruptStatus を読む → 0x1 (used buffer 通知)
  7. InterruptACK に 0x1 を書く（pending クリア）
  8. used ring を確認 → 完了した descriptor を発見
  9. バッファからデータを読む → /dev/hvc0 に配送
```

### Ctrl-A v モード切替

microkvm の stdin は UART (ttyS0) と virtio (hvc0) で共有。`Ctrl-A v` で入力先を切替:
- デフォルト: UART モード → `uart_rx()` が ttyS0 に配送（シェル）
- `Ctrl-A v` 後: Virtio モード → `virtio_console_rx()` が hvc0 に配送

### InterruptStatus / InterruptACK

| レジスタ | 方向 | 役割 |
|----------|-----------|---------|
| InterruptStatus (0x060) | Read | VMM が guest にどの割り込みが pending か伝える (bit 0 = used buffer) |
| InterruptACK (0x064) | Write | guest が処理済み割り込みをクリア |

guest の IRQ handler が InterruptStatus を読んで割り込み源を特定し、InterruptACK に書いてクリアする。クリアしないと割り込みが pending のまま残る。

## 実行フロー

```
Host (stdin_thread)              KVM                Guest (virtio-console driver)
───────────────────              ───                ────────────────────────────
ユーザーが virtio モードで 'a' 入力
read(stdin) → 'a'

virtio_console_rx(&dev, 'a', 1):
  avail ring: 空バッファを探す
  descriptor: addr=X, flags=WRITE
  memcpy(ram + X, "a", 1)
  used ring: {id, len=1} を記録
  interrupt_status |= 0x1

ioctl(KVM_IRQ_LINE, irq=5, 1)
ioctl(KVM_IRQ_LINE, irq=5, 0)
                                 PIC が IRQ 5 を
                                 guest CPU に配送
                                                    IRQ ハンドラ起動
                                                    MMIO read InterruptStatus → 0x1
                                                      (KVM_EXIT_MMIO → VMM が値を返す)
                                                    MMIO write InterruptACK ← 0x1
                                                      (KVM_EXIT_MMIO → VMM がクリア)
                                                    used ring 確認 → descriptor 完了
                                                    addr X のバッファを読む → 'a'
                                                    /dev/hvc0 に配送
                                                    cat /dev/hvc0 が 'a' を表示
```

## 実装

### virtio_mmio.h の追加

```c
uint32_t interrupt_status;  /* pending 割り込みビット */

/* RX: receiveq にデータを書き込み割り込みを発生させる */
int virtio_console_rx(struct virtio_mmio_dev *dev, const uint8_t *data, size_t len);
```

### virtio_mmio.c — virtio_console_rx()

```c
int virtio_console_rx(struct virtio_mmio_dev *dev, const uint8_t *data, size_t len)
{
    int qidx = 0;  /* receiveq */

    /* avail ring から次の空バッファを見つける */
    /* VRING_DESC_F_WRITE フラグを確認（デバイスがこのバッファに書き込む） */
    /* guest バッファにデータを memcpy */
    /* used ring に実バイト数付きで記録 */
    /* interrupt_status |= 0x1 をセット */
    return 0;  /* 成功; 呼び出し側が IRQ を注入する */
}
```

TX との違い: `used_elem.len` に実際に書き込んだバイト数を返す。guest はこれでバッファに何バイトあるか知る。

### virtio_mmio.c — InterruptStatus / InterruptACK

```c
case VIRTIO_MMIO_INTERRUPT_STATUS:
    val = dev->interrupt_status;    /* guest が読んで割り込み源を特定 */
    break;

case VIRTIO_MMIO_INTERRUPT_ACK:
    dev->interrupt_status &= ~value;  /* guest が処理済みビットをクリア */
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

```
/ # cat /dev/hvc0 &
/ #
[monitor] input → hvc0 (virtio)
abc
abc

[monitor] input → ttyS0 (UART)

/ #
```

virtio モードで入力した文字が `/dev/hvc0` に配送され、`cat` で表示される。

## 重要な知見

RX で virtio I/O ループが完成する。TX (Step 15) は guest → host を shared memory で示した。RX は同じ仕組みの逆方向 — ただし guest はポーリングできないため IRQ 通知が加わる。

| | TX (Step 15) | RX (Step 16) |
|---|---|---|
| 誰が開始 | Guest (write + kick) | Host (write + IRQ) |
| Queue | transmitq (1) | receiveq (0) |
| Descriptor flag | 0 (デバイスが読む) | VRING_DESC_F_WRITE (デバイスが書く) |
| used_elem.len | 0 (確認されない) | 実バイト数 |
| 通知 | guest → VMM (QueueNotify) | VMM → guest (IRQ 5) |

非対称性が存在する理由: **I/O 開始の主体** が異なる。TX は guest 主導（データがある時を知っている）、RX は host 主導（guest は入力がいつ来るか知らない）。IRQ がこのギャップを埋める。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| Shared memory RX | VMM が guest 提供のバッファに直接書き込む |
| VRING_DESC_F_WRITE | 「デバイスがここに書いてよい」を示すフラグ |
| IRQ 注入 | KVM_IRQ_LINE で guest の割り込みハンドラを起動 |
| InterruptStatus/ACK | guest が割り込み源の特定とクリアを行う |
| Edge-triggered IRQ | assert (level=1) → deassert (level=0) |
| Ctrl-A v モード切替 | 1つの stdin を UART と virtio で共有 |
| 双方向 virtio | TX + RX = 完全なコンソールデバイス |

## 変わったこと

Step 15 からの変更:
- **virtio_mmio.h**: `interrupt_status` フィールド、`virtio_console_rx()` プロトタイプ
- **virtio_mmio.c**: `virtio_console_rx()` 実装、`INTERRUPT_STATUS` が live 値を返す、`INTERRUPT_ACK` がビットクリア、InterruptStatus/ACK のログ抑制
- **microkvm.c**: stdin_thread に `Ctrl-A v` モード切替、virtio モードで `virtio_console_rx()` + IRQ 5 注入

## 次のステップ

[Step 17: ioeventfd](step17_ioeventfd.md) — TX kick の QueueNotify VM exit を elimination する。KVM が eventfd 経由でカーネル空間内で通知を完結させる。
