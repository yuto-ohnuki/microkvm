# ベンチマーク: ioeventfd / irqfd レイテンシ計測

## 概要

microkvm には ioeventfd (Step 17) と irqfd (Step 18) の性能影響を計測する組み込みベンチマークがある。環境変数でこれらの機能を切り替え、4つの構成を比較して exit elimination が latency と exit 回数にどう影響するかを観察できる。

## 前提条件

ベンチマーク計測コードは Step 18 の上に別コミットとして存在する:

```
ec8f80d tools: add latency benchmark for ioeventfd and irqfd
```

ベンチマークを使うには、このコミット（または `v2` ブランチ上のこれ以降のコミット）をチェックアウトする。`step18` タグにはベンチマークコードは含まれない — ioeventfd + irqfd のクリーンな実装のみ。

## 使い方

```bash
# パターン 1: ベースライン (Step 15-16 style — MMIO exit + ioctl IRQ)
./microkvm

# パターン 2: ioeventfd のみ (Step 17 — TX kick は eventfd、IRQ は ioctl)
USE_IOEVENTFD=1 ./microkvm

# パターン 3: irqfd のみ (Step 18 の IRQ パス — TX kick は MMIO exit)
USE_IRQFD=1 ./microkvm

# パターン 4: 両方有効 (Step 17+18 — 完全最適化)
USE_IOEVENTFD=1 USE_IRQFD=1 ./microkvm
```

## ワークロード

一貫した結果を得るために、各パターンで同じ手順を実行:

```
1. Guest シェル:  cat /dev/hvc0 &
2. TX テスト:     echo hello > /dev/hvc0   (3回繰り返す)
3. RX テスト:     Ctrl-A v → "abc" 入力 → Ctrl-A v (UART に戻す)
4. 終了:    　    Ctrl-C → stderr に stats 表示
```

## 計測結果 (EC2 c7i.xlarge, Linux 7.1.0+)

### パターン 1: ベースライン (ioeventfd=OFF, irqfd=OFF)

```
MMIO exits total:           185
QueueNotify MMIO exits:
  RX queue 0:               132
  TX queue 1:               10

TX processing latency:
  Method:   MMIO exit handler
  Avg:      5467 ns (5.47 us)

IRQ injection latency:
  Method:   ioctl (KVM_IRQ_LINE x2)
  Avg:      7372 ns (7.37 us)
```

### パターン 2: ioeventfd のみ (USE_IOEVENTFD=1)

```
MMIO exits total:           175
QueueNotify MMIO exits:
  RX queue 0:               132
  TX queue 1:               0
ioeventfd TX kicks:         10

TX processing latency:
  Method:   ioeventfd thread
  Avg:      5575 ns (5.58 us)

IRQ injection latency:
  Method:   ioctl (KVM_IRQ_LINE x2)
  Avg:      6608 ns (6.61 us)
```

### パターン 3: irqfd のみ (USE_IRQFD=1)

```
MMIO exits total:           185
QueueNotify MMIO exits:
  RX queue 0:               132
  TX queue 1:               10

TX processing latency:
  Method:   MMIO exit handler
  Avg:      5680 ns (5.68 us)

IRQ injection latency:
  Method:   irqfd (write)
  Avg:      4670 ns (4.67 us)
```

### パターン 4: 両方有効 (USE_IOEVENTFD=1 USE_IRQFD=1)

```
MMIO exits total:           175
QueueNotify MMIO exits:
  RX queue 0:               132
  TX queue 1:               0
ioeventfd TX kicks:         10

TX processing latency:
  Method:   ioeventfd thread
  Avg:      6676 ns (6.68 us)

IRQ injection latency:
  Method:   irqfd (write)
  Avg:      4428 ns (4.43 us)
```

## まとめ

| 指標 | ベースライン | ioeventfd | irqfd | 両方 |
|--------|----------|-----------|-------|------|
| MMIO exits total | 185 | **175** | 185 | **175** |
| TX QueueNotify exits | 10 | **0** | 10 | **0** |
| TX latency (平均) | 5.47 μs | 5.58 μs | 5.68 μs | 6.68 μs |
| IRQ latency (平均) | 7.37 μs | 6.61 μs | **4.67 μs** | **4.43 μs** |

## 結果の解釈

> **注意:** このワークロードは意図的に小さい (`echo hello` × 3)。実際のネットワークやストレージワークロードでは毎秒数千の通知が発生し、exit 削減の効果はより顕著になる。ここでの価値は絶対値ではなく *仕組み* を実証すること。

### ioeventfd の効果 (パターン 1 → 2)

- TX QueueNotify の MMIO exit が 10 → **0** に減少
- MMIO exits total が 10 減少（排除された TX kick の分）
- **vCPU は TX のために一度も停止しない** — 処理は別スレッドに移動
- TX processing latency はスレッドスケジューリングで微増するが、vCPU 待機時間はゼロになる

### irqfd の効果 (パターン 1 → 3)

- IRQ injection latency が 7.37 μs → **4.67 μs** に改善 (~37% 削減)
- IRQ あたり syscall: 2 (ioctl × 2) → 1 (write × 1)
- MMIO exits は変わらない（irqfd は RX パスの最適化で TX には影響しない）

### 両方有効 (パターン 4)

- 両方の最適化が重なる: TX exit ゼロ + IRQ injection 高速化
- virtio I/O の notification path がカーネル内で完結
- vCPU はどちらの方向でも I/O 通知によってブロックされない

### なぜ TX latency が ioeventfd で増加するか

- ベースライン: vCPU スレッドが TX をインラインで処理（コンテキストスイッチなし）
- ioeventfd: 別スレッドが eventfd 経由で起床 → スレッドスケジューリングが ~1 μs 追加
- **トレードオフ**: TX 処理自体は少し遅くなるが、vCPU は待たないためスループットが向上

## 内部動作

ベンチマークが追加する内容:
- `USE_IOEVENTFD` / `USE_IRQFD` 環境変数フラグ
- `KVM_IOEVENTFD` / `KVM_IRQFD` の条件付き登録
- フラグ OFF 時は MMIO exit handler (TX) / ioctl (IRQ) にフォールバック
- TX 処理と IRQ 注入の周囲に `clock_gettime(CLOCK_MONOTONIC)`
- KVM_EXIT_MMIO ハンドラ内の exit カウンタ
- `SIGINT` ハンドラで VM を停止し stats を表示

同じバイナリで4つの構成を比較可能。

## 核心的な学び

ioeventfd と irqfd の本質は、個々の操作を速くすることではない。通知パスから userspace の往復を排除し、vCPU とデバイス処理が独立して走れるようにすること。vCPU は I/O を待たない — シグナルを発射して続行する。この分離が高性能仮想化 I/O の基盤。
