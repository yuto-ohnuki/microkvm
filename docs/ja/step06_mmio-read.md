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

このデバイスでは、同じ MMIO アドレス 0xD0000 に対し、write は文字を受け取ってカウンタを増やし、read は現在のカウンタ値を返す。

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

このステップでは、VMM がシンプルな `device_counter` 変数を保持する。0xD0000 への MMIO write ごとにカウンタをインクリメントし、read は現在値を返す。read 自体ではカウンタは変化しない。デバイスモデルが複数のゲストアクセスにまたがって持続する**状態**を保持できることを示す — あらゆるデバイスエミュレータの基盤。

## 実行フロー

以下は、ロングモードでの MMIO アクセス以降の流れ。

```
VMM (microkvm)                                  Guest
──────────────                                  ─────
                                                RBX = 0xD0000
                                                mov al, 'M'
                                                mov [rbx], al
KVM_EXIT_MMIO (write)
  device_counter: 0 → 1
  'M' のログを表示
KVM_RUN                                         （write を完了して再開）
                                                mov al, '\n'
                                                mov [rbx], al
KVM_EXIT_MMIO (write)
  device_counter: 1 → 2
  改行のログは省略
KVM_RUN                                         （再開）
                                                mov al, [rbx]
KVM_EXIT_MMIO (read)
  is_write=0, len=1
  run->mmio.data[0] = 2
  returning 2 を表示
KVM_RUN                                         （read を完了: AL = 2）
                                                add al, '0' → '2'
                                                out 0x10, al
KVM_EXIT_IO: '2' を表示
KVM_RUN                                         （再開）
                                                改行を out
KVM_EXIT_IO: ログを省略
KVM_RUN                                         （再開）
                                                hlt
KVM_EXIT_HLT: 終了
```

## 実装

変更するのは `microkvm.c` と `guest.S`。メモリスロットとページテーブルは Step 5 のまま。

### VMM: read サポート付き MMIO ハンドラ

```c
/* microkvm.c のファイルスコープで保持 */
static uint8_t device_counter = 0;

/* ... exit handler ループ内: */
case KVM_EXIT_MMIO:
    if (run->mmio.phys_addr == MEM_GAP_START) {
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
    .byte 0xB0, '\n'       /* mov al, '\n' */
    .byte 0xE6, 0x10       /* out 0x10, al */
```

ゲストは書き込んだのと同じアドレスから読む。`mov al, [rbx]` 命令が `is_write=0` で `KVM_EXIT_MMIO` をトリガーする。VMM が `data[0]` を埋め、次の `KVM_RUN` で KVM が read を完了すると、ゲストは AL で値を受け取る。`add al, '0'` は0〜9の値を1桁の ASCII 数字にする処理で、今回は2を文字 `2` にして PIO で出力する。

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

`returning 2` は VMM が返す値、続く PIO の `2` はゲストが受け取った値を文字に変換した結果。ログに表示されない改行の write も数えるため、カウンタは2になる。

## 重要な知見

この MMIO アドレスを read すると、最後に書いた文字ではなく、write の回数が返る。VMM がアクセスをまたいで状態を保持し、read の結果を決めることで、単なる RAM とは異なるデバイスの振る舞いを実現する。read はカウンタを変更せず、追加の write がなければ何度読んでも同じ値を返す。

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

## 変わったこと

- `microkvm.c`: ファイルスコープに `device_counter` を追加し、MMIO write で増加、read で現在値を返す。
- `guest.S`: MMIO read と、取得した値を数字として PIO 出力する処理を追加。

## 次のステップ

[Step 7: 割り込み注入](step07_irq.md) — これまで全てのインタラクションはゲスト主導だった。次のステップでは、ホストが割り込みを使ってゲストに非同期に通知する — 逆方向の通信 (host → guest) を導入する。
