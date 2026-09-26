# Step 7: 割り込み注入 (IRQ)

## 目的

`KVM_INTERRUPT` を使って**ホストからゲスト**に割り込みを配送する。
**IDT** (Interrupt Descriptor Table)、割り込みゲート、`iretq` を使い、ゲストが割り込みハンドラを実行して元の処理へ戻る流れを学ぶ。

## 背景

### 新しい通信方向

Step 2–6 の PIO と MMIO は、ゲストが命令を実行して通信を始める仕組みだった。

割り込みでは、ゲストがデバイスを読み取りに来るのを待たずに通知できる。この step では外部イベントを待つ代わりに、最初の `KVM_EXIT_HLT` を契機として VMM が割り込みを1回注入する。

### これまでの通信方向

| Step | 方向 | メカニズム |
|------|------|-----------|
| 2 | Guest → Host | PIO (`out` → `KVM_EXIT_IO`) |
| 5 | Guest → Host | MMIO write (`mov [addr], val` → `KVM_EXIT_MMIO`) |
| 6 | Guest ↔ Host | MMIO read (ゲストが要求、VMM が値で応答) |
| **7** | **Host → Guest** | **割り込み注入 (`KVM_INTERRUPT`)** |

MMIO read を繰り返して状態変化を確認する方法をポーリングと呼ぶ。割り込みはホスト側からの通知であり、ゲストが割り込みを受け付けられる状態になったときに配送される。

### IDT (Interrupt Descriptor Table)

IDT は割り込みベクタ番号 (0–255) をハンドラアドレスにマップする。
割り込み到着時、CPU は:
1. `IDT[vector]` を参照
2. 64-bit モードでは SS、RSP、RFLAGS、CS、RIP をこの順でスタックに保存
3. 割り込みゲートの場合は IF をクリアし、マスク可能な外部割り込みの追加配送を抑止
4. ハンドラアドレスにジャンプ

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

`irq=32` は割り込み線の番号ではなく、IDT のベクタ番号。この step はカーネル内の割り込みコントローラを作らず、`KVM_INTERRUPT` でベクタ32を保留状態にする。次の `KVM_RUN` で、IF（RFLAGS の割り込み許可フラグ）が1、かつ割り込みシャドウ内でない状態で配送される。

KVM_INTERRUPT は学習に適したシンプルなメカニズム。本番 VMM は通常、仮想 LAPIC、IOAPIC、irqfd、MSI/MSI-X で割り込みを配送する — 後のステップで扱うトピック。

### なぜゲストにスタックが必要か

割り込み発火時、CPU は割り込みフレームをスタックにプッシュする。
ゲストは割り込み有効化前に有効な RSP を持つ必要がある。64-bit モードでは割り込みフレームに SS、RSP、RFLAGS、CS、RIP が含まれる。有効なスタックポインタなしではこのプッシュがフォルトする。このステップでは割り込み有効化前に `RSP = 0x60000` を設定する。

### なぜ `sti; hlt` か

このゲストのように IF=0 から `sti` で IF=1 にすると、次の命令が完了するまでマスク可能な外部割り込みの配送を抑止する（**割り込みシャドウ**）。
シーケンス:

```asm
sti
hlt
```

により、割り込みを先に処理した直後に `hlt` で停止し、次の通知を待ち続ける競合を避けられる。この step では `hlt` が `KVM_EXIT_HLT` を返した後、VMM が割り込みを要求してゲストを再実行する。

## 実行フロー

```
Guest                         KVM                         VMM (microkvm)
─────                         ───                         ──────────────
ロングモード:
  RSP = 0x60000
  MMIO write 'M' と改行、read → 2（Step 6 と同じ）
  lidt [idt_desc]
  sti
  hlt ──────────────────────→ KVM_EXIT_HLT ─────────────→ KVM_INTERRUPT (vector=32)
                                                          irq_injected = 1
割り込み受け入れ  ←───────── ベクタ32を配送 ←────────── KVM_RUN
  復帰状態をスタックに保存
  IF=0、IDT[32] → irq_handler
  'I' と改行を out ─────────→ KVM_EXIT_IO（各1回） ─────→ 'I' を表示して再実行
  iretq
  最初の hlt の次の命令へ復帰
  hlt ──────────────────────→ KVM_EXIT_HLT ─────────────→ irq_injected == 1
                                                          Guest halted. を表示して終了
```

## 実装

`microkvm.c` の HLT 処理と `guest.S` を変更する。PIO・MMIO の処理は Step 6 のまま。

### VMM: HLT 時の割り込み注入

以下は `main` 内の抜粋（`KVM_INTERRUPT` のエラー処理は省略）。

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

最初の `KVM_EXIT_HLT` では割り込みを要求し、ループ先頭の `KVM_RUN` でゲストを再実行する。2回目の HLT を「完了」とするのは、この VMM の取り決め。`irq_injected` は注入要求済みかを記録する。

### ゲスト: IDT 設定と割り込み待ち

```asm
    /* スタック設定 (割り込み配送に必要) */
    .byte 0x48, 0xC7, 0xC4, 0x00, 0x00, 0x06, 0x00  /* mov rsp, 0x60000 */

    /* IDT ロード */
    .byte 0x48, 0xC7, 0xC1                          /* mov rcx, imm32 */
    .long idt_desc
    .byte 0x0F, 0x01, 0x19                          /* lidt [rcx] */

    /* 割り込み有効化して待機 */
    .byte 0xFB                                      /* sti */
    .byte 0xF4                                      /* hlt */

    /* iretq がここに戻った後 */
    .byte 0xF4                                      /* hlt (完了) */
```

実コードではロングモードに入った直後にスタックを設定し、PIO・MMIO の処理後に `lidt` と `sti; hlt` を実行する。上の抜粋は割り込み関連の部分だけを示している。

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

`iretq` は RAX などの汎用レジスタを復元しない。このハンドラは AL を変更するが、復帰後は HLT のみなので保存を省略している。割り込み前の値を使い続ける場合は、ハンドラ側で保存・復元が必要。

### ゲスト: ベクタ 32 の IDT エントリ

```asm
.align 16
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

idt_desc:
    .word idt_desc - idt - 1      /* IDT のサイズ - 1 */
    .long idt                     /* ベースアドレスの下位32ビット */
    .long 0                       /* ベースアドレスの上位32ビット */
```

IDT の位置がベクタ番号を決定する。`.fill 64, 8, 0` は512バイト、つまり16バイト × 32エントリを埋め、その次がベクタ32になる。`lidt` は10バイトの `idt_desc` から IDT のリミットとベースアドレスを読み込む。

簡略化のため、ハンドラは低メモリ (64KB 以下) に配置されているので、上位オフセットフィールド (`offset_mid`、`offset_high`) はゼロ。一般的な実装では完全な 64-bit ハンドラアドレスを3つのオフセットフィールドに分割する必要がある。

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
[PIO out port 0x10] I
Guest halted.
```

`I` は割り込みハンドラの実行を示す。続く `Guest halted.` は、ハンドラから復帰して2回目の HLT に到達したことを示す。

## 重要な知見

VMM が通知するベクタを選び、ゲストが IDT でその処理先を定める。この step では最初の HLT を契機に通知し、IDT[32] のハンドラ実行と `iretq` による復帰を確認する。外部イベントに応じた非同期通知の基礎となるが、今回はタイマーや別スレッドからの通知は実装していない。

### ポーリングとの違い

ポーリングは状態を繰り返し読み、割り込みは通知を受けて処理する。割り込みにより待機中の反復処理を減らせるが、配送とハンドラ実行のコストはある。この構成の HLT は VMM への復帰を起こすため、ホスト CPU のスリープや消費電力ゼロを意味しない。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| KVM_INTERRUPT | ベクタ番号による Host → Guest 通知 |
| IDT | ベクタ番号をハンドラアドレスにマップ |
| 割り込みゲート | 状態保存、ハンドラへジャンプ、IF クリア |
| iretq | スタックから RIP/CS/RFLAGS/RSP/SS を復元 |
| スタック要件 | CPU が復帰状態をプッシュ — RSP が有効である必要 |
| 通知タイミング | 最初の HLT で VMM が注入を要求し、ゲストが受け入れる |

## 変わったこと

- `microkvm.c`: 最初の HLT でベクタ32を注入し、2回目で終了する処理を追加。
- `guest.S`: スタック、IDT、`sti; hlt`、`I` を出力して `iretq` で戻るハンドラを追加。

## 次のステップ

[Step 8: MSR ハンドリング](step08_msr.md) — `KVM_EXIT_X86_WRMSR` / `KVM_EXIT_X86_RDMSR` を使って Model-Specific Register アクセスをトラップ・エミュレートし、MSR アクセスの処理を学ぶ。
