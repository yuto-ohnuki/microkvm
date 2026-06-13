# Step 7: 割り込み注入 (IRQ)

## 目的

`KVM_INTERRUPT` を使って**ホストからゲスト**に割り込みを配送する。
**IDT** (Interrupt Descriptor Table)、割り込みゲート、`iretq` を導入 — ホストとゲスト間の3番目の通信方向を完成させる。

## 背景

### 新しい通信方向

これまで全ての VM exit はゲストが実行した命令が原因だった: `out`、`mov [addr]`、`hlt`、`wrmsr`。いつ通信するかはゲストが決めていた。

割り込みは根本的に異なる。ホストはゲストが能動的に通信していない時でも、いつでもゲストに通知できる。ゲストは無関係な計算の最中に中断される可能性がある。実ハードウェアもこう動作する: NIC は CPU がポーリングするのを待たず、パケット到着時に割り込みを発生させる。

### これまでの通信方向

| Step | 方向 | メカニズム |
|------|------|-----------|
| 2 | Guest → Host | PIO (`out` → `KVM_EXIT_IO`) |
| 5 | Guest → Host | MMIO write (`mov [addr], val` → `KVM_EXIT_MMIO`) |
| 6 | Guest ↔ Host | MMIO read (ゲストが要求、VMM が値で応答) |
| **7** | **Host → Guest** | **割り込み注入 (`KVM_INTERRUPT`)** |

MMIO read はゲストがデータを*ポーリング*する。割り込みはホストがゲストに非同期に*通知*する — 「何か起きた、今すぐ処理せよ」。実デバイスがイベントを通知する方法: NIC がパケット準備完了、ディスクが転送完了、タイマー発火。

### IDT (Interrupt Descriptor Table)

IDT は割り込みベクタ番号 (0–255) をハンドラアドレスにマップする。
割り込み到着時、CPU は:
1. `IDT[vector]` を参照
2. RIP、CS、RFLAGS (64-bit モードでは RSP、SS も) をスタックにプッシュ
3. ハンドラアドレスにジャンプ
4. IF をクリア (割り込みゲートの場合) してネスト割り込みを防止

ハンドラが実行され、`iretq` で保存された状態を復元して中断されたコードに戻る。

### 64-bit 割り込みゲート (16 バイト)

```
┌──────────────────────────────────────────────────────────────┐
│ offset_low (15:0) │ selector │ IST │ type_attr │ offset_mid  │
├──────────────────────────────────────────────────────────────┤
│ offset_high (63:32)              │ reserved                  │
└──────────────────────────────────────────────────────────────┘

type_attr = 0x8E:
  present=1, DPL=0, type=interrupt gate (0xE)
```

ハンドラアドレスは歴史的理由から3つのフィールド (offset_low, offset_mid, offset_high) に分割される。

### KVM_INTERRUPT

```c
struct kvm_interrupt irq = { .irq = 32 };
ioctl(vcpufd, KVM_INTERRUPT, &irq);
```

ゲストに割り込みベクタ 32 の配送を要求する。割り込みはゲストが割り込み配送を許可する状態 (IF=1 かつ割り込みシャドウ内でない) のときに注入される。

KVM_INTERRUPT は学習に適したシンプルなメカニズム。本番 VMM は通常、仮想 LAPIC、IOAPIC、irqfd、MSI/MSI-X で割り込みを配送する — 後のステップで扱うトピック。

### なぜゲストにスタックが必要か

割り込み発火時、CPU は割り込みフレームをスタックにプッシュする。
ゲストは割り込み有効化前に有効な RSP を持つ必要がある。64-bit モードでは割り込みフレームに SS、RSP、RFLAGS、CS、RIP が含まれる。有効なスタックポインタなしではこのプッシュがフォルトする。このステップでは割り込み有効化前に `RSP = 0x60000` を設定する。

### なぜ `sti; hlt` か

`sti` の後、x86 は一時的に1命令分割り込み配送を遅延する (**割り込みシャドウ**)。
シーケンス:

```asm
sti
hlt
```

により、保留中の割り込みが配送される前に CPU がアトミックに停止状態に入れる。この保証がないと、`sti` と `hlt` の間に割り込みが到着し、`hlt` が永遠にスリープする可能性がある (割り込みは既に処理済み)。このパターンは OS で一般的に使われる — Linux の idle ループも同じ技法を使用。

## 実行フロー

```
VMM (microkvm.c)                         Guest (guest.S)
────────────────                         ───────────────
                                         [ロングモード]
                                           mov rsp, 0x60000
                                           MMIO write 'M'
                                           MMIO read → '0'
                                           lidt [idt_desc]
                                           sti          (IF=1)
                                           hlt          (割り込み待ち)
                                                │
KVM_EXIT_HLT                                    ▼
  irq_injected == 0 なので:
  ioctl(KVM_INTERRUPT, {.irq=32})
  irq_injected = 1
  ioctl(KVM_RUN)
                                                │
                                         CPU がベクタ 32 を配送:
                                           割り込みフレームをプッシュ
                                           IDT[32] → irq_handler
                                           mov al, 'I'
                                           out 0x10, al
                                                │
KVM_EXIT_IO                                     ▼
  printf("[PIO out] I")
ioctl(KVM_RUN)
                                           iretq
                                           (RIP, CS, RFLAGS, RSP, SS をポップ)
                                           ← hlt の次の命令に復帰
                                           hlt (2回目)
                                                │
KVM_EXIT_HLT                                    ▼
  irq_injected == 1 → 完了
  printf("Guest halted.")
```

## 実装

### VMM: HLT 時の割り込み注入

```c
int irq_injected = 0;

case KVM_EXIT_HLT:
    if (!irq_injected) {
        struct kvm_interrupt irq = { .irq = 32 };
        ioctl(vcpufd, KVM_INTERRUPT, &irq);
        irq_injected = 1;
    } else {
        printf("Guest halted.\n");
        goto done;
    }
    break;
```

最初の `hlt` でゲスト CPU が停止状態になる。注入された割り込みが CPU を起こし、割り込みハンドラに制御を移す。2回目の `hlt` (ハンドラが `iretq` で戻った後) は「完了」を意味する。

### ゲスト: IDT 設定と割り込み待ち

```asm
    /* スタック設定 (割り込み配送に必要) */
    .byte 0x48, 0xC7, 0xC4, 0x00, 0x00, 0x06, 0x00  /* mov rsp, 0x60000 */

    /* IDT ロード */
    .byte 0x48, 0xC7, 0xC1                            /* mov rcx, imm32 */
    .long idt_desc
    .byte 0x0F, 0x01, 0x19                            /* lidt [rcx] */

    /* 割り込み有効化して待機 */
    .byte 0xFB                                         /* sti */
    .byte 0xF4                                         /* hlt */

    /* iretq がここに戻った後 */
    .byte 0xF4                                         /* hlt (完了) */
```

ゲストはスタックを設定し (割り込みフレームプッシュに必要)、`lidt` で IDT をロードし、`sti` で割り込み配送を有効にして停止する。`hlt` 命令は割り込み到着まで CPU をサスペンドする。

### ゲスト: 割り込みハンドラ

```asm
.align 16
irq_handler:
    .byte 0xB0, 'I'       /* mov al, 'I' */
    .byte 0xE6, 0x10      /* out 0x10, al */
    .byte 0xB0, '\n'      /* mov al, '\n' */
    .byte 0xE6, 0x10      /* out 0x10, al */
    .byte 0x48, 0xCF      /* iretq */
```

ハンドラは PIO で 'I' を出力し、`iretq` で戻る。`iretq` は CPU が以前プッシュした割り込みフレームを消費し、そこから実行状態を復元して `hlt` の次の命令で再開する。

### ゲスト: ベクタ 32 の IDT エントリ

```asm
idt:
    .fill 64, 8, 0                /* ベクタ 0-31: null (512 バイト) */
    /* ベクタ 32: 割り込みゲート → irq_handler */
    .word irq_handler             /* offset_low */
    .word 0x18                    /* selector: 64-bit コードセグメント */
    .byte 0x00                    /* IST = 0 (現在のスタックを使用) */
    .byte 0x8E                    /* present=1, DPL=0, interrupt gate */
    .word 0x0000                  /* offset_mid = 0 */
    .long 0x00000000              /* offset_high = 0 */
    .long 0x00000000              /* reserved */
```

IDT の位置がベクタ番号を決定する。32個の null エントリ (ベクタ 0–31) を埋めてから、位置 32 にハンドラを配置する。

簡略化のため、ハンドラは低メモリ (64KB 以下) に配置されているので、上位オフセットフィールド (`offset_mid`、`offset_high`) はゼロ。一般的な実装では完全な 64-bit ハンドラアドレスを3つのオフセットフィールドに分割する必要がある。

## 出力

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
[PIO out port 0x10] R
[PIO out port 0x10] P
[MMIO write @ 0xd0000] M
[MMIO read  @ 0xd0000] returning 0
[PIO out port 0x10] 0
[PIO out port 0x10] I
Guest halted.
```

'I' が割り込み配送とハンドラ実行を確認する。

## 重要な知見

割り込み注入はデバイスエミュレーションを**非同期**にするメカニズム。割り込みなしでは、ゲストはイベントを確認するために MMIO レジスタを継続的にポーリングしなければならない。割り込みにより、VMM は何か起きた時に正確にゲストに通知できる。

### ポーリング (Step 6) vs 割り込み (Step 7)

```
ポーリング:                        割り込み:
  while (status == 0)               hlt  (CPU スリープ、消費電力ゼロ)
      read_mmio();                  ← 割り込み到着
  // CPU サイクルを浪費              ハンドラが即座に実行
```

割り込みが存在する理由はポーリングのスケーラビリティが低いため。デバイスをポーリングするゲストは CPU 時間を浪費し、繰り返し VM exit を引き起こす。割り込みでは、ゲストはスリープ (または有用な作業) し、処理すべきことがある時のみ起こされる。このトレードオフ — レイテンシ vs CPU 効率 — は I/O 仮想化の中核。

VMM が注入する割り込みベクタを選ぶ。ゲストが IDT を構築してそのベクタの意味を決める。KVM は配送メカニズムに過ぎない — IDT の内容やハンドラコードの知識を持たない。

### 完成したデバイスモデル

Step 5–7 で、デバイスモデルの3コンポーネント全てが揃った:

| コンポーネント | Step | メカニズム |
|--------------|------|-----------|
| デバイスへのコマンド/データ | 5 | MMIO write |
| デバイスからのステータス/データ | 6 | MMIO read |
| 非同期通知 | 7 | 割り込み注入 |

全ての実デバイスが従うパターン: ドライバがコマンドを書き、ステータスを読み、イベント発生時に割り込みを受ける。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| KVM_INTERRUPT | ベクタ番号による Host → Guest 通知 |
| IDT | ベクタ番号をハンドラアドレスにマップ |
| 割り込みゲート | 状態保存、ハンドラへジャンプ、IF クリア |
| iretq | スタックから RIP/CS/RFLAGS/RSP/SS を復元 |
| スタック要件 | CPU が復帰状態をプッシュ — RSP が有効である必要 |
| 非同期通知 | VMM がいつゲストに割り込むかを決定 |

## 次のステップ

[Step 8: MSR ハンドリング](step08_msr.md) — `KVM_EXIT_X86_WRMSR` / `KVM_EXIT_X86_RDMSR` を使って Model-Specific Register アクセスをトラップ・エミュレートし、VM exit タイプのセットを完成させる。
