# Step 5: MMIO デバイスエミュレーション

## 目的

ゲスト物理アドレス空間に**ホール**を作ることで、メモリマップドデバイスをエミュレートする。ゲストがバッキングメモリのないアドレスに書き込むと、KVM は VMM に `KVM_EXIT_MMIO` を配送する — 通常のメモリアクセス命令によるデバイスエミュレーションを可能にする。

## 背景

### PIO から MMIO へ

Step 2 では、ゲストはポート I/O (`out` 命令) でホストと通信した。PIO は独立した 16-bit アドレス空間と専用命令を使う。MMIO (Memory-Mapped I/O) は異なる: デバイスレジスタがゲスト物理アドレス空間の**通常のメモリアドレス**として現れる。ゲストは通常の load/store 命令 (`mov`) でアクセスする。

ほとんどの現代デバイスが PIO ではなく MMIO を使う理由:
- 16-bit のポート番号に制限されず、広い物理アドレス空間を使える
- 標準メモリ命令が使える — 特殊な `in`/`out` 不要

MMIO は RAM と同じように自由にキャッシュできるとは限らない。実際のドライバでは、デバイスに合ったメモリ属性とアクセス順序を扱う必要がある。

### MMIO トラッピングの仕組み

Step 1 から、`KVM_SET_USER_MEMORY_REGION` でゲスト RAM とホストメモリの対応を登録してきた。このステップでは、GPA（ゲスト物理アドレス）0xD0000–0xD0FFF を登録範囲から外す。ゲストがこの領域へデータを書き込むと、KVM は通常の RAM として処理できず、`KVM_EXIT_MMIO` で VMM に処理を渡す:

```
ゲスト実行:  mov [0xD0000], al
                    │
                    ▼
        ゲストページテーブル参照
        present=1 (アイデンティティマップ)
                    │
                    ▼
        ゲスト物理アドレス: 0xD0000
                    │
                    ▼
        メモリスロット検索: バッキング RAM なし
                    │
                    ▼
        KVM は通常のゲストメモリでは
        アクセスを解決できない
                    │
                    ▼
        KVM_EXIT_MMIO がユーザー空間に配送
                    │
                    ▼
        microkvm デバイスモデルが処理
```

これはユーザー空間でデバイスをエミュレートする基本的な仕組み。MMIO をカーネル内で処理したり、通知を高速化したりする構成では、毎回 VMM に戻るとは限らない。

### PIO vs MMIO の比較

| | PIO | MMIO |
|---|---|---|
| ゲスト命令 | `out` / `in` (専用) | `mov` (通常の load/store) |
| アドレス空間 | 16-bit ポート番号 | ゲスト物理アドレス空間 |
| トラップメカニズム | I/O 命令インターセプト | RAM バッキングなしの GPA へのアクセス |
| kvm_run 内のデータ位置 | `(char *)run + run->io.data_offset` | `run->mmio.data`（8バイト配列）に直接 |
| 実世界での用途 | レガシー (シリアル, PIC, PIT) | 現代デバイス (NIC, GPU, NVMe) |

### run->mmio のフィールド

`exit_reason == KVM_EXIT_MMIO` の場合:

| フィールド | 意味 |
|-----------|------|
| `run->mmio.phys_addr` | アクセスされたゲスト物理アドレス |
| `run->mmio.data` | 8バイト配列。write では書き込まれたデータ、read では VMM が返すデータを格納 |
| `run->mmio.len` | 今回通知されたデータのバイト数（このゲストでは1） |
| `run->mmio.is_write` | 1 = ゲストがアドレスに書いた、0 = ゲストが読んだ |

PIO ではデータがオフセットにあるのと違い、MMIO データは `run->mmio` 構造体に直接埋め込まれている。

## 実行フロー

```
VMM (microkvm)                                  Guest
──────────────                                  ─────
メモリスロットを登録:
  slot 0: 0x00000–0xCFFFF (RAM)
  slot 1: 0xD1000–0xFFFFF (RAM)
  0xD0000–0xD0FFF: 未登録
KVM_RUN
                                                'R', 'P', 'L' と各改行を PIO 出力
                                                （各 out の処理後に KVM_RUN）
                                                mov rbx, 0xD0000
                                                mov al, 'M'
                                                mov [rbx], al
KVM_EXIT_MMIO (write)
  phys_addr=0xD0000, len=1
  is_write=1, data[0]='M'
  'M' のログを表示
KVM_RUN                                         （write を完了して再開）
                                                mov al, '\n'
                                                mov [rbx], al
KVM_EXIT_MMIO (write)
  改行のログは省略
KVM_RUN                                         （再開）
                                                hlt
KVM_EXIT_HLT: ループ終了
```

VMM が受け取る通知は PIO が6回、MMIO write が2回、HLT が1回。ログに改行を表示しなくても、そのアクセスによる通知は発生する。

## 実装

`microkvm.h` にメモリ配置の定数を追加し、`microkvm.c` のスロット登録と exit handler、`guest.S` のロングモード部分を変更する。ゲストページテーブルは Step 4 のまま。以下は主要部分の抜粋。

### VMM: MMIO ホールの作成

Step 4 では、ゲストメモリ全体をカバーする1つの連続メモリスロットを登録した。
Step 5 では、ギャップのある2つのスロットに分割する。範囲を表す定数は `microkvm.h` で定義し、`MEM_GAP_START=0xD0000`、`MEM_GAP_END=0xD1000` とする:

```c
/* Slot 0: GPA 0x00000 – 0xCFFFF (832 KB) */
struct kvm_userspace_memory_region region1 = {
    .slot = MEM_SLOT0_ID,
    .guest_phys_addr = MEM_SLOT0_GPA,
    .memory_size = MEM_SLOT0_SIZE,
    .userspace_addr = (unsigned long)mem + MEM_SLOT0_GPA,
};

/* Slot 1: GPA 0xD1000 – 0xFFFFF (188 KB) */
struct kvm_userspace_memory_region region2 = {
    .slot = MEM_SLOT1_ID,
    .guest_phys_addr = MEM_SLOT1_GPA,
    .memory_size = MEM_SLOT1_SIZE,
    .userspace_addr = (unsigned long)mem + MEM_SLOT1_GPA,
};

/* 各スロットを登録（エラー処理は省略） */
ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region1);
ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region2);

/* GPA 0xD0000 – 0xD0FFF: スロット未登録 → MMIO */
```

ホスト側の `mmap` は連続した1 MiB のまま。そのうち4 KiB をゲスト RAM として登録しないため、登録する RAM は832 KiB + 188 KiB = 1020 KiB になる。

| GPA 範囲 | 用途 | サイズ |
|---|---|---|
| 0x00000–0xCFFFF | slot 0: RAM | 832 KiB |
| 0xD0000–0xD0FFF | 未登録: MMIO ホール | 4 KiB |
| 0xD1000–0xFFFFF | slot 1: RAM | 188 KiB |

RAM へのアクセスでも、変換の初期構築などで VM exit が起こることはある。KVM が内部で処理できるため、通常は `KVM_EXIT_MMIO` として VMM へ戻る必要がない。

### VMM: KVM_EXIT_MMIO の処理

```c
case KVM_EXIT_MMIO:
    if (run->mmio.phys_addr == MEM_GAP_START && run->mmio.is_write) {
        char c = run->mmio.data[0];
        if (c != '\n')
            printf("[MMIO write @ 0x%llx] %c\n", run->mmio.phys_addr, c);
    }
    break;
```

書き込まれた1バイトは `run->mmio.data[0]` から読める。この handler が扱うのはホール先頭の `MEM_GAP_START` への write で、改行はログに表示しない。処理後に `KVM_RUN` を再度呼ぶと、KVM が MMIO 操作を完了してゲストを再開する。

### ゲスト: MMIO アドレスへの書き込み

ロングモードの PIO 出力に続けて、通常の `mov` で `M` と改行を書き込む。命令が指定するのはゲスト仮想アドレスだが、アイデンティティマッピングにより同じ値の GPA 0xD0000 に対応する:

```asm
    /* long_mode 内の PIO 出力に続く部分 */
    .byte 0x48, 0xC7, 0xC3, 0x00, 0x00, 0x0D, 0x00     /* mov rbx, 0xD0000 */
    .byte 0xB0, 'M'                                    /* mov al, 'M' */
    .byte 0x88, 0x03                                   /* mov [rbx], al */
    .byte 0xB0, '\n'                                   /* mov al, '\n' */
    .byte 0x88, 0x03                                   /* mov [rbx], al */
```

命令は通常の store だが、この GPA は RAM として登録されていない。VMM がそのアドレスへの write を文字出力として処理することで、デバイスの動作を実装している。

## 出力

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
[PIO out port 0x10] R
[PIO out port 0x10] P
[PIO out port 0x10] L
[MMIO write @ 0xd0000] M
Guest halted.
```

Step 4 の `putchar` を、ポート番号付きの PIO ログに変更している。`R`・`P`・`L` は PIO、`M` は MMIO による出力。どちらもゲストが送る改行は表示を省略するため、ログの行数と通知回数は一致しない。

### なぜゲストページテーブルに穴を作らないのか

ゲスト仮想アドレスの変換を not present にすると、そのアクセスは MMIO 処理へ進む前にゲストのページフォルト（#PF）になる。このゲストには例外処理用の IDT を用意していないため、例外を処理できず triple fault に至り得る。

さらに、この実装は2 MiB ページを使い、PT レベルのテーブルを持たない。PD[0] の Present ビットを落とすと、0xD0000 だけでなく、実行中のコードを含む最初の2 MiB 全体が無効になる。

```text
ゲストページテーブル: GVA 0xD0000 → GPA 0xD0000（present）
メモリスロット:      GPA 0xD0000 → RAM として未登録
VMM の処理:         このアドレスへの write を文字出力として扱う
```

## 重要な知見

RAM と MMIO は同じメモリアクセス命令を使うが、ゲストソフトウェアはメモリマップやデバイス情報に基づいて用途を区別する。この step では、ゲスト側のアドレス変換を有効に保ちながら、KVM の RAM 登録に穴を作る。そこへの write を VMM が処理することで、文字出力デバイスとして動作させる。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| MMIO | ゲスト物理アドレスとしてのデバイスレジスタ |
| メモリスロットホール | 未登録 GPA 範囲 → KVM_EXIT_MMIO |
| run->mmio | phys_addr, data[], len, is_write — 1つの構造体に全て |
| 通常の命令による I/O | ゲストは `mov` で書き込み、VMM がデバイス処理を行う |
| 第2段階 vs ゲスト PT | MMIO はメモリスロットの性質であり、ゲストページテーブルではない |

## 変わったこと

- RAM の登録を2スロットに分け、0xD0000–0xD0FFF に4 KiB のホールを設ける。
- ゲストに MMIO write を追加し、VMM に `KVM_EXIT_MMIO` の処理を追加。
- PIO 出力をポート番号付きログへ変更し、PIO・MMIO とも改行のログを省略。

## 次のステップ

[Step 6: MMIO read + デバイス状態](step06_mmio-read.md) — MMIO read サポートを追加し、ゲストがデバイス状態を問い合わせられるようにして、双方向デバイスモデルを完成させる。
