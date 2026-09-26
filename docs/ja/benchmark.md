# ベンチマーク: ioeventfd / irqfd レイテンシ計測

## 概要

microkvm には ioeventfd (Step 17) と irqfd (Step 18) の性能影響を計測する組み込みベンチマークがある。環境変数でこれらの機能を切り替え、4つの構成を比較して通知方式が処理時間とユーザー空間への MMIO exit 回数にどう影響するかを観察できる。

## 前提条件

ベンチマーク計測コードは Step 18 の上に別コミットとして存在し、`benchmark` タグで参照・使用できる。Step 18 のタグにはベンチマークコードは含まれない。

```bash
$ git checkout benchmark
$ make
```

## 使い方

```bash
# パターン 1: ベースライン (Step 15-16 style — MMIO exit + ioctl IRQ)
./microkvm

# パターン 2: ioeventfd のみ (Step 17 — TX kick は eventfd、IRQ は ioctl)
USE_IOEVENTFD=1 ./microkvm

# パターン 3: irqfd のみ (Step 18 の IRQ パス — TX kick は MMIO exit)
USE_IRQFD=1 ./microkvm

# パターン 4: 両方有効 (Step 17+18 の通知方式)
USE_IOEVENTFD=1 USE_IRQFD=1 ./microkvm
```

環境変数は値ではなく、設定されているかで判定する。`USE_IRQFD=0` でも ON になるため、比較前にホストで `unset USE_IOEVENTFD USE_IRQFD` を実行する。

## ワークロード

TX は `bench.sh` で2,000行を送り、RX は `abcdefghij` と Enter の計11文字を送る。

### bench.sh の準備

既存の initramfs を新しい作業ディレクトリに展開し、`/bin/bench.sh` を追加する。`/dev/console` の復元には権限が必要なため、展開は `sudo` で行う。

```bash
$ initramfs_work=$(mktemp -d /tmp/microkvm-bench.XXXXXX)
$ cd "$initramfs_work"
$ zcat ~/microkvm/initramfs.gz | sudo cpio -idmv

$ sudo tee bin/bench.sh > /dev/null << 'EOF'
#!/bin/sh
N=2000
echo "[bench] TX start: $N lines -> /dev/hvc0"
i=1
while [ $i -le $N ]; do
    echo "microkvm benchmark line $i"
    i=$((i + 1))
done > /dev/hvc0
echo "[bench] TX done: $N lines"
EOF
$ sudo chmod +x bin/bench.sh

$ sudo sh -c 'find . | cpio -o -H newc' | gzip > ~/microkvm/initramfs.gz
$ cd ~/microkvm
```

`~/microkvm` はホスト上のプロジェクトパスに合わせる。

### 各パターン共通の操作

「使い方」の各設定で起動し、同じ手順を実行する:

```text
1. ゲストで cat /dev/hvc0 &
2. ゲストで bench.sh
3. [bench] TX done: 2000 lines を待つ
4. < Ctrl-A v > → [monitor] input → hvc0 (virtio) を確認
5. abcdefghij と Enter を入力し、受信内容の表示を待つ
6. < Ctrl-A v > → UART に戻す
7. Ctrl-C → stderr に execution report 表示
```

## 計測結果

以下は上記ワークロードによる4パターンの実測結果。全パターンで TX Count は4,011、IRQ Count は11となった。

### パターン 1: ベースライン (ioeventfd=OFF, irqfd=OFF)

```
==== microkvm execution report ====
Mode: ioeventfd=OFF, irqfd=OFF

--- Userspace counters ---
MMIO exits total:           4207
QueueNotify MMIO exits:
  RX queue 0:               139
  TX queue 1:               4011
ioeventfd TX kicks:         0
IRQ inject (ioctl):         11
IRQ inject (irqfd):         0

--- TX processing latency ---
  Method:   MMIO exit handler
  Count:    4011
  Avg:      1809 ns (1.81 us)
  Min:      798 ns (0.80 us)
  Max:      11582 ns (11.58 us)

--- IRQ injection latency ---
  Method:   ioctl (KVM_IRQ_LINE x2)
  Count:    11
  Avg:      10368 ns (10.37 us)
  Min:      9352 ns (9.35 us)
  Max:      12019 ns (12.02 us)
==================================
```

### パターン 2: ioeventfd のみ (USE_IOEVENTFD=1)

```
==== microkvm execution report ====
Mode: ioeventfd=ON, irqfd=OFF

--- Userspace counters ---
MMIO exits total:           196
QueueNotify MMIO exits:
  RX queue 0:               139
  TX queue 1:               0
ioeventfd TX kicks:         4011
IRQ inject (ioctl):         11
IRQ inject (irqfd):         0

--- TX processing latency ---
  Method:   ioeventfd thread
  Count:    4011
  Avg:      3452 ns (3.45 us)
  Min:      669 ns (0.67 us)
  Max:      28525 ns (28.52 us)

--- IRQ injection latency ---
  Method:   ioctl (KVM_IRQ_LINE x2)
  Count:    11
  Avg:      10533 ns (10.53 us)
  Min:      6834 ns (6.83 us)
  Max:      27552 ns (27.55 us)
==================================
```

### パターン 3: irqfd のみ (USE_IRQFD=1)

```
==== microkvm execution report ====
Mode: ioeventfd=OFF, irqfd=ON

--- Userspace counters ---
MMIO exits total:           4207
QueueNotify MMIO exits:
  RX queue 0:               139
  TX queue 1:               4011
ioeventfd TX kicks:         0
IRQ inject (ioctl):         0
IRQ inject (irqfd):         11

--- TX processing latency ---
  Method:   MMIO exit handler
  Count:    4011
  Avg:      2190 ns (2.19 us)
  Min:      1423 ns (1.42 us)
  Max:      15926 ns (15.93 us)

--- IRQ injection latency ---
  Method:   irqfd (write)
  Count:    11
  Avg:      5813 ns (5.81 us)
  Min:      3945 ns (3.94 us)
  Max:      7078 ns (7.08 us)
==================================
```

### パターン 4: 両方有効 (USE_IOEVENTFD=1 USE_IRQFD=1)

```
==== microkvm execution report ====
Mode: ioeventfd=ON, irqfd=ON

--- Userspace counters ---
MMIO exits total:           196
QueueNotify MMIO exits:
  RX queue 0:               139
  TX queue 1:               0
ioeventfd TX kicks:         4011
IRQ inject (ioctl):         0
IRQ inject (irqfd):         11

--- TX processing latency ---
  Method:   ioeventfd thread
  Count:    4011
  Avg:      3467 ns (3.47 us)
  Min:      643 ns (0.64 us)
  Max:      28587 ns (28.59 us)

--- IRQ injection latency ---
  Method:   irqfd (write)
  Count:    11
  Avg:      6326 ns (6.33 us)
  Min:      4977 ns (4.98 us)
  Max:      7297 ns (7.30 us)
==================================
```

## まとめ

| 指標 | ベースライン | ioeventfd | irqfd | 両方 |
|--------|----------|-----------|-------|------|
| MMIO exits total | 4207 | **196** | 4207 | **196** |
| TX QueueNotify exits | 4011 | **0** | 4011 | **0** |
| TX latency (平均) | 1.81 μs | 3.45 μs | 2.19 μs | 3.47 μs |
| IRQ latency (平均) | 10.37 μs | 10.53 μs | **5.81 μs** | **6.33 μs** |

## 結果の解釈

> **注意:** TX は4,011サンプルあるが、IRQ は各11サンプルの単回計測であり、一般的な性能改善率を示すものではない。数値は実行環境やタイミングに依存する。MMIO exit のカウンタはユーザー空間に返された `KVM_EXIT_MMIO` の回数であり、ハードウェアの VM exit 総数ではない。

### ioeventfd の効果 (パターン 1 → 2)

- TX QueueNotify の MMIO exit が 4,011 → **0** に減少
- MMIO exits total が 4,207 → 196 に減少。差の4,011回は TX QueueNotify の削減分と一致する
- TX 処理は vCPU スレッドから別スレッドに移り、vCPU スレッドが TX を終えてからゲストを再開する必要がなくなる
- TX processing latency は `virtio_console_tx()` の実行時間であり、vCPU の停止時間は計測していない

### irqfd の効果 (パターン 1 → 3)

- この計測では IRQ injection latency が 10.37 μs → **5.81 μs** に減少。計測対象はホスト側の通知操作で、ゲストが割り込みを受け取るまでの時間ではない
- IRQ あたり syscall: 2 (ioctl × 2) → 1 (write × 1)
- MMIO exits は変わらない（irqfd は RX パスの最適化で TX には影響しない）

### 両方有効 (パターン 4)

- TX QueueNotify に対するユーザー空間への MMIO exit はゼロになり、この計測では IRQ 通知の所要時間も減少
- KVM が TX kick を eventfd に通知し、RX の eventfd 通知を IRQ に変換
- データ処理は VMM が担当し、RX 通知の write syscall も残る

### TX latency の増加をどう読むか

- ベースライン: vCPU スレッドが TX をインラインで処理（コンテキストスイッチなし）
- ioeventfd: 別スレッドで eventfd の read が返った後、TX 処理の直前から計測する。通知から起床するまでの待ち時間は含まれない
- この結果だけでは増加の原因を特定できず、スループットの向上も判断できない

## 内部動作

ベンチマークが追加する内容:

- `USE_IOEVENTFD` / `USE_IRQFD` 環境変数フラグ
- `KVM_IOEVENTFD` / `KVM_IRQFD` の条件付き登録
- フラグ OFF 時は MMIO exit handler (TX) / ioctl (IRQ) にフォールバック
- TX 処理と IRQ 注入の周囲に `clock_gettime(CLOCK_MONOTONIC)`
- KVM_EXIT_MMIO ハンドラ内の exit カウンタ
- `SIGINT` ハンドラで停止フラグを立て、vCPU スレッド終了後に main が stats を表示

同じバイナリで4つの構成を比較可能。

TX は `virtio_console_tx()` の前後、IRQ は ioctl 2回または write 1回を実行する分岐の前後を計測する。`ioeventfd TX kicks` は eventfd の read が成功した回数で、複数の kick が1回にまとまる場合がある。`IRQ inject (ioctl)` は ioctl 呼び出し数ではなく、assert / deassert の組を1回として数える。

## 核心的な学び

ioeventfd は TX 処理を vCPU スレッドから分離し、irqfd は IRQ 通知を ioctl 2回から write 1回に置き換える。処理時間と通知回数は別の指標であり、何を計測しているかを区別して効果を読む。
