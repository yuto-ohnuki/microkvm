# Step 3: リアルモード → プロテクトモード

## 目的

ゲスト CPU を 16-bit リアルモードから 32-bit プロテクトモードに遷移させる。
**GDT** (Global Descriptor Table)、**CR0.PE**、**far jump** を使い、ゲスト自身がモードを切り替える仕組みを学ぶ。

## 背景

### x86 CPU モード

```
リアルモード (16-bit)  →  プロテクトモード (32-bit)  →  ロングモード (64-bit)
     Step 1–2                 このステップ                   Step 4
```

Step 1 と 2 では、vCPU はリアルモード（CR0.PE=0）で動作していた。このステップでは CR0.PE を1にし、32-bit コードセグメントへ移る。リアルモードでも一部の32-bit 命令は使えるが、ここではデフォルトのオペランド・アドレスサイズを32-bit に切り替える。

### GDT (Global Descriptor Table)

プロテクトモードでは、命令やデータへのアクセスに**セグメントディスクリプタ**の設定が使われる。
GDT はこれらのディスクリプタのテーブル。各エントリは以下を定義する:
- ベースアドレス (セグメントの開始位置)
- リミット (アクセスできる最大オフセット)
- タイプ (コードまたはデータ、読み取り/書き込み可能か)
- 特権レベル (ring 0–3)

```
GDT[0] = null ディスクリプタ  (有効なセグメントとして使用しない)
GDT[1] = コード: base=0, limit=0xFFFFFFFF（範囲は4 GiB）, 32-bit, 実行/読み取り
GDT[2] = データ: base=0, limit=0xFFFFFFFF（範囲は4 GiB）, 32-bit, 読み書き
```

### フラットメモリモデル

このゲストでは、コードとデータの両セグメントを base=0、limit=0xFFFFFFFF に設定する。このように同じアドレス範囲を使う構成を**フラットメモリモデル**と呼び、セグメントのベース加算によるアドレスのずれをなくす。

セグメントの範囲が4 GiB でも、ゲスト RAM が4 GiB に増えるわけではない。VMM が登録するメモリは Step 2 と同じ1 MiB で、このステップではゲストのページングも無効のまま。

### セグメントセレクタ

セレクタは、ディスクリプタのインデックス、GDT/LDT の選択、要求特権レベル（RPL）を含む16-bit 値。このゲストでは GDT を選び、RPL=0 とするため、値はインデックス × 8 になる:
- セレクタ `0x08` → GDT[1] (コード)
- セレクタ `0x10` → GDT[2] (データ)

CPU はセレクタを使ってディスクリプタを参照し、そのフィールド (base, limit, type) を内部にキャッシュする。このキャッシュされたコピーは**ディスクリプタキャッシュ**と呼ばれ — セグメントレジスタの隠された部分で、明示的にリロードされるまで持続する。

### なぜ far jump が必要か

`CR0.PE = 1` を設定するとプロテクトモードが有効になるが、CS のディスクリプタキャッシュはまだ古いリアルモードの値を保持している。**far jump** (`ljmp $selector, $offset`) は CPU に以下を強制する:
1. 新しいセレクタを CS にロード
2. 対応する GDT エントリを読む
3. 32-bit 属性でディスクリプタキャッシュを更新

これがないと、命令のデフォルトのオペランド・アドレスサイズは16-bit のままになる。

far jump はブート中に CS をリロードして遷移を完了するための最も一般的なメカニズム。(far call、`iret`、タスクスイッチでも CS をリロードできるが、`ljmp` がモード遷移の標準的な選択。)

## 実行フロー

```
Guest: リアルモード (.code16)                   Guest: プロテクトモード (.code32)
───────────────────────                         ─────────────────────────
'R' と改行を PIO 出力
lgdt gdt_desc
CR0.PE = 1
ljmp $0x08, $protected_mode ──→                 DS / SS に 0x10 をロード
                                                'P' と改行を PIO 出力
                                                hlt
```

文字と改行はそれぞれ別の `out` で出力し、その都度 `KVM_RUN` を呼び直す。VMM は `KVM_EXIT_IO` を4回、`KVM_EXIT_HLT` を1回受け取る。

モード遷移を指示するのはゲストコードであり、VMM に専用の処理は追加しない。ただし、制御レジスタ操作などがハードウェアの VM exit を起こすかは CPU や KVM の設定に依存する。KVM 内部での処理と、`KVM_RUN` がユーザー空間へ戻ることは区別する。

## 実装

`guest.S` と `boot.c` / `boot.h` を追加し、`microkvm.c` のインラインバイト配列をファイル読み込みに置き換える。`Makefile` にゲストバイナリのビルドを追加する。

### ビルドパイプライン

このステップ以降、ゲストコードは独立したアセンブリファイルに存在する:

```
guest.S → as --32 → guest.o → ld -m elf_i386 -Ttext 0x0 --oformat binary → guest.bin
```

VMM は実行時に `guest.bin` をゲスト物理アドレス（GPA）0 にロードする。`as --32` は32-bit ELF オブジェクトを生成し、命令のエンコードは `.code16` / `.code32` で切り替える。これらの指示自体は CPU の動作モードを変更しない。

`ld` の `-Ttext 0x0` はコードの配置先をアドレス0とし、`--oformat binary` は ELF ヘッダのないフラットバイナリを出力する。リンク時のアドレスと実際のロード先を一致させることで、GDT やジャンプ先の参照が正しく動く。

### ゲストアセンブリ (guest.S)

```asm
.code16
.global _start
_start:
    /* リアルモード: PIO で 'R' を出力 */
    mov $'R', %al
    out %al, $0x10
    mov $'\n', %al
    out %al, $0x10

    /* GDT をロード */
    lgdt gdt_desc

    /* プロテクトモード有効化: CR0.PE = 1 を設定 */
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0

    /* CS を 32-bit コードセグメントでリロードするための far jump */
    ljmp $0x08, $protected_mode

.code32
protected_mode:
    /* データセグメントを GDT[2] に設定 */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %ss

    /* プロテクトモード: PIO で 'P' を出力 */
    mov $'P', %al
    out %al, $0x10
    mov $'\n', %al
    out %al, $0x10

    hlt
```

### GDT データ

```asm
.align 8
gdt:
    .quad 0                     /* GDT[0]: null */
    .quad 0x00CF9A000000FFFF    /* GDT[1] (0x08): コード, 32-bit, 実行/読み取り */
    .quad 0x00CF92000000FFFF    /* GDT[2] (0x10): データ, 32-bit, 読み書き */

gdt_desc:
    .word gdt_desc - gdt - 1    /* リミット (GDT サイズ - 1) */
    .long gdt                   /* GDT のベースアドレス */
```

### VMM: ファイルから guest.bin をロード

ファイルローダーは `boot.c` に配置し、宣言を `boot.h` に置く。以下は `open` のエラー処理を省略した抜粋:

```c
/* フラットバイナリを GPA 0x0 にロード */
int load_guest(const char *path, void *mem) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    fstat(fd, &st);
    read(fd, mem, st.st_size);
    close(fd);
    printf("Loaded guest: %ld bytes\n", st.st_size);
    return 0;
}
```

VMM (`microkvm.c`) は `load_guest("guest.bin", mem)` を呼び出す。exit handler ループは Step 2 と同じ。

## 出力

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
R
P
Guest halted.
```

`R` はリアルモード、`P` はプロテクトモードへ移った後の出力。`Guest halted.` は、最後の `hlt` を受け取った VMM の表示。

## 重要な知見

ゲストが GDT を用意し、CR0.PE を設定して、far jump で32-bit コードセグメントへ移る。VMM の exit handler は Step 2 と同じで、モード遷移専用の処理を必要としない。ゲスト自身による CPU 状態の変更と、KVM 内部で起こり得る VM exit は別の話である。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| GDT | コードとデータセグメントのフラットモデルディスクリプタ |
| CR0.PE | CPU をプロテクトモードに切り替える単一ビット |
| Far jump | モード遷移を完了するために CS ディスクリプタキャッシュをリロード |
| セグメントセレクタ | コード用 `0x08`、データ用 `0x10` — GDT 選択、RPL=0 |
| 独立ゲストバイナリ | アセンブリ → フラットバイナリ → VMM が実行時にロード |
| ゲストの自律性 | VMM の exit handler を変更せず、ゲストコードでモードを切り替える |

## 変わったこと

- ゲストコードを C のバイト配列から `guest.S` に分離し、`guest.bin` のビルドと読み込みを追加。
- ゲストに GDT、CR0.PE の設定、far jump、DS/SS の更新を追加。
- VMM の exit handler は Step 2 から変更なし。

## 付録: GDT ディスクリプタのデコード

8バイト値 `0x00CF9A000000FFFF` のエンコード:

```
 63       56 55 52 51 48 47       40 39       32
┌───────────┬─────┬──────┬──────────┬──────────┐
│ Base 31:24│Flags│Lim   │  Access  │Base 23:16│
│   0x00    │ C   │19:16 │  0x9A    │  0x00    │
│           │(G=1 │ 0xF  │          │          │
│           │ D=1)│      │          │          │
└───────────┴─────┴──────┴──────────┴──────────┘
 31                16 15                 0
┌────────────────────┬────────────────────┐
│   Base 15:0        │   Limit 15:0       │
│   0x0000           │   0xFFFF           │
└────────────────────┴────────────────────┘

デコード結果:
  Base  = 0x00000000
  Limit = (0xFFFFF << 12) | 0xFFF = 0xFFFFFFFF (G=1)
          最大オフセットは4 GiB - 1、セグメントの範囲は4 GiB
  Access = 0x9A = present, コード, 実行/読み取り, ring 0
  D/B   = 1 → 32-bit デフォルトオペランド/アドレスサイズ
  G     = 1 → リミット粒度は 4KB ページ
```

## 次のステップ

[Step 4: プロテクト → ロングモード](step04_long-mode.md) — ページテーブルを設定し、PAE とページングを有効にし、64-bit ロングモードに遷移する。
