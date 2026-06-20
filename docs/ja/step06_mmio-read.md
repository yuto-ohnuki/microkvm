# Step 6: MMIO read + デバイス状態

## 目的

MMIO **read** サポートを追加し、ゲストがデバイス状態を問い合わせられるようにする。
これで双方向デバイスモデルが完成: write はデバイスにコマンドを送り、read はステータスやデータを返す。

## 背景

### 双方向デバイス通信

Step 5 ではゲストは MMIO アドレスに書くことしかできなかった — 一方通行のチャネル。実デバイスには双方向が必要:

| 方向 | ゲスト操作 | デバイスの役割 | 例 |
|------|-----------|--------------|-----|
| Write | `mov [addr], val` | コマンド/データ受信 | パケット送信、設定変更 |
| Read | `mov val, [addr]` | ステータス/データ返却 | 割り込みステータス読み取り、カウンタ取得 |

1つの MMIO アドレスが read と write で異なる振る舞いをすることがある。これはハードウェアでは一般的: 例えば UART のアドレス 0x3F8 は write 時に Transmit Holding Register、read 時に Receive Buffer Register — 同じアドレスで完全に異なる機能。

### MMIO read の仕組み

ゲストが未登録 GPA から読むと、KVM は `KVM_EXIT_MMIO` かつ `is_write = 0` で exit する。VMM は:
1. `run->mmio.data[]` に返す値を書き込む
2. `KVM_RUN` でゲストを再開する

KVM がメモリアクセスを完了し、返された値を元の命令で指定されたデスティネーションレジスタに配置する。

```
Guest: mov al, [0xD0000]
         │
         ▼
KVM_EXIT_MMIO (is_write=0, len=1)
         │
         ▼
VMM: run->mmio.data[0] = counter_value
VMM: ioctl(KVM_RUN)
         │
         ▼
KVM が load を完了 → al = counter_value
```

### デバイス状態

このステップでは、VMM がシンプルな `device_counter` 変数を保持する。各 MMIO write でカウンタをインクリメントし、各 MMIO read は現在のカウンタ値を返す。デバイスモデルが複数のゲストアクセスにまたがって持続する**状態**を保持できることを示す — あらゆるデバイスエミュレータの基盤。

## 実行フロー

```
VMM (microkvm.c)                         Guest (guest.S)
────────────────                         ───────────────
                                         [ロングモード]
                                           mov rbx, 0xD0000
                                           mov [rbx], al  ('M')
                                                │
KVM_EXIT_MMIO (write)                           ▼
  data[0] = 'M'
  device_counter++ → 1
  printf("[MMIO write] M")
ioctl(KVM_RUN)
                                           mov [rbx], al  ('\n')
                                                │
KVM_EXIT_MMIO (write)                           ▼
  device_counter++ → 2
ioctl(KVM_RUN)
                                           mov al, [rbx]  (read)
                                                │
KVM_EXIT_MMIO (read)                            ▼
  run->mmio.data[0] = 2
  printf("[MMIO read] returning 2")
ioctl(KVM_RUN)
                                           al = 2
                                           add al, '0'  → '2'
                                           out 0x10, al
                                                │
KVM_EXIT_IO                                     ▼
  printf("[PIO out] 2")
ioctl(KVM_RUN)
                                           hlt
```

## 実装

### VMM: read サポート付き MMIO ハンドラ

```c
static uint8_t device_counter = 0;

/* ... exit handler ループ内: */
case KVM_EXIT_MMIO:
    if (run->mmio.phys_addr == 0xD0000) {
        if (run->mmio.is_write) {
            char c = run->mmio.data[0];
            device_counter++;
            if (c != '\n')
                printf("[MMIO write @ 0x%llx] %c\n",
                       run->mmio.phys_addr, c);
        } else {
            run->mmio.data[0] = device_counter;
            printf("[MMIO read  @ 0x%llx] returning %d\n",
                   run->mmio.phys_addr, device_counter);
        }
    }
    break;
```

read では、VMM が `KVM_RUN` を呼ぶ前に `run->mmio.data[]` に返却値を書く。KVM がこの値を受け取り load 命令を完了して、ゲストのデスティネーションレジスタに配置する。

カウンタは write ごとにインクリメントされる。2回の write ('M' と '\n') の後に read すると 2 が返る。

### ゲスト: MMIO アドレスからの読み取り

```asm
    /* MMIO read: 0xD0000 からロード → KVM_EXIT_MMIO (is_write=0) */
    .byte 0x8A, 0x03       /* mov al, [rbx]  — rbx はまだ 0xD0000 */
    .byte 0x04, 0x30       /* add al, '0'    — ASCII 数字に変換 */
    .byte 0xE6, 0x10       /* out 0x10, al   — PIO で出力 */
```

ゲストは書き込んだのと同じアドレスから読む。`mov al, [rbx]` 命令が `is_write=0` で `KVM_EXIT_MMIO` をトリガーする。VMM が `data[0]` を埋めた後、ゲストは AL で値を受け取り、ASCII に変換して PIO で出力する。

## 出力

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
[PIO out port 0x10] R
[PIO out port 0x10] P
[PIO out port 0x10] L
[MMIO write @ 0xd0000] M
[MMIO read  @ 0xd0000] returning 2
[PIO out port 0x10] 2
Guest halted.
```

## 重要な知見

デバイスモデルは write の受動的な受け手ではない。状態を保持し、各 read で異なる値を返す。同じ MMIO アドレスがアクセス方向によって全く異なる意味を持てる。

実際のハードウェアもまさにこう動作する:
- UART のデータレジスタ: write = 送信、read = 受信
- NIC のステータスレジスタ: write = フラグクリア、read = ステータス取得
- タイマーのカウンタレジスタ: write = 値設定、read = 現在カウント取得

アドレス 0xD0000 は格納されたバイトを表さない。デバイスレジスタを表し、その振る舞いはアクセス方向と現在のデバイス状態に依存する。このパターンは仮想化全体で繰り返される: virtio queue notifications、PCI config space、MSI-X テーブルは全てこのように動作する。

VMM が「ハードウェア」— 各アドレスの意味と read/write の振る舞いを決定する。これがデバイスエミュレーションの本質。

```
Guest
  read GPA 0xD0000
        │
        ▼
MMIO ディスパッチャ (アドレスマッチ)
        │
        ▼
デバイスステートマシン
  counter = 0
        │
        ▼
返却値: 0
  counter が 1 になる
```

### RAM との対比

| | RAM | MMIO デバイス |
|---|---|---|
| Write → Read | 書いた値が返る | 全く異なる値が返る可能性 |
| 副作用 | なし | アクション発火の可能性 (パケット送信、IRQ 発火) |
| 状態 | バイトを格納するだけ | 複雑な内部状態を保持 |

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| MMIO read | VMM が値を供給; KVM がゲストの load を完了 |
| デバイス状態 | `device_counter` がアクセスをまたいで持続 |
| 双方向モデル | 同じアドレス、read と write で異なる振る舞い |
| デバイスエミュレーションパターン | アドレス → ディスパッチ → ステートマシン → レスポンス |

## 次のステップ

[Step 7: 割り込み注入](step07_irq.md) — これまで全てのインタラクションはゲスト主導だった。次のステップでは、ホストが割り込みを使ってゲストに非同期に通知する — 逆方向の通信 (host → guest) を導入する。
