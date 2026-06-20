# Step 5: MMIO デバイスエミュレーション

## 目的

ゲスト物理アドレス空間に**ホール**を作ることで、メモリマップドデバイスをエミュレートする。ゲストがバッキングメモリのないアドレスに書き込むと、KVM は VMM に `KVM_EXIT_MMIO` を配送する — 通常のメモリアクセス命令によるデバイスエミュレーションを可能にする。

## 背景

### PIO から MMIO へ

Step 2 では、ゲストはポート I/O (`out` 命令) でホストと通信した。PIO は独立した 16-bit アドレス空間と専用命令を使う。MMIO (Memory-Mapped I/O) は異なる: デバイスレジスタがゲスト物理アドレス空間の**通常のメモリアドレス**として現れる。ゲストは通常の load/store 命令 (`mov`) でアクセスする。

ほとんどの現代デバイスが PIO ではなく MMIO を使う理由:
- アドレス空間がはるかに大きい (64-bit vs 16-bit)
- 標準メモリ命令が使える — 特殊な `in`/`out` 不要
- キャッシュコヒーレンシと順序付けをページ単位で制御可能

### MMIO トラッピングの仕組み

Step 4 で `KVM_SET_USER_MEMORY_REGION` を使ってゲストメモリを登録した。KVM はこれらのメモリスロットを使い、どのゲスト物理アドレスにバッキングホストメモリがあるかを判定する。ゲストが登録済みメモリスロットにバッキングされていない GPA にアクセスすると、KVM はそのアクセスを MMIO として扱い、ユーザー空間に exit する:

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

これは実際のハイパーバイザーがデバイスレジスタをエミュレートするのに使うメカニズム。
QEMU のデバイスモデル (virtio, e1000, AHCI 等) は全てこの方法で MMIO アクセスを受け取る。

### PIO vs MMIO の比較

| | PIO | MMIO |
|---|---|---|
| ゲスト命令 | `out` / `in` (専用) | `mov` (通常の load/store) |
| アドレス空間 | 16-bit ポート番号 | ゲスト物理アドレス空間 |
| トラップメカニズム | I/O 命令インターセプト | RAM バッキングなしの GPA へのアクセス |
| kvm_run 内のデータ位置 | `run + run->io.data_offset` | `run->mmio.data[8]` に直接 |
| 実世界での用途 | レガシー (シリアル, PIC, PIT) | 現代デバイス (NIC, GPU, NVMe) |

### run->mmio のフィールド

`exit_reason == KVM_EXIT_MMIO` の場合:

| フィールド | 意味 |
|-----------|------|
| `run->mmio.phys_addr` | アクセスされたゲスト物理アドレス |
| `run->mmio.data[8]` | 書き込まれたデータ (write) またはバッファ (read) |
| `run->mmio.len` | アクセス幅 (1, 2, 4, または 8 バイト) |
| `run->mmio.is_write` | 1 = ゲストがアドレスに書いた、0 = ゲストが読んだ |

PIO ではデータがオフセットにあるのと違い、MMIO データは `run->mmio` 構造体に直接埋め込まれている。

## 実行フロー

```
VMM (microkvm.c)                         Guest (guest.S)
────────────────                         ───────────────
メモリスロット登録:
  slot 0: GPA 0x00000–0xCFFFF (RAM)
  slot 1: GPA 0xD1000–0xFFFFF (RAM)
  [ホール]: GPA 0xD0000–0xD0FFF (スロットなし)
                                         [リアルモード] 'R' via PIO
                                         [プロテクトモード] 'P' via PIO
                                         [ロングモード]
                                           mov rbx, 0xD0000
                                           mov [rbx], al  ← MMIO ホールへの store
                                                │
                                                ▼
KVM_EXIT_MMIO                            (ゲスト一時停止)
  phys_addr = 0xD0000
  is_write = 1
  data[0] = 'M'
printf("[MMIO write] M")
ioctl(KVM_RUN)                           ← ゲスト再開
                                           hlt
KVM_EXIT_HLT
```

## 実装

### VMM: MMIO ホールの作成

Step 4 では、ゲストメモリ全体をカバーする1つの連続メモリスロットを登録した。
Step 5 では、ギャップのある2つのスロットに分割する:

```c
/* Slot 0: GPA 0x00000 – 0xCFFFF (832 KB) */
struct kvm_userspace_memory_region region1 = {
    .slot = 0,
    .guest_phys_addr = 0,
    .memory_size = 0xD0000,
    .userspace_addr = (unsigned long)mem,
};

/* Slot 1: GPA 0xD1000 – 0xFFFFF (188 KB) */
struct kvm_userspace_memory_region region2 = {
    .slot = 1,
    .guest_phys_addr = 0xD1000,
    .memory_size = GUEST_MEM_SIZE - 0xD1000,
    .userspace_addr = (unsigned long)mem + 0xD1000,
};

/* GPA 0xD0000 – 0xD0FFF: スロット未登録 → MMIO */
```

ホスト側の `mmap` は1つの連続 1MB 割り当てのまま。KVM に2つの非連続範囲を伝え、4KB ギャップを MMIO デバイスアドレスにする。

```
ゲスト物理アドレス空間:

0x00000              0xCFFFF  0xD0000  0xD0FFF  0xD1000              0xFFFFF
┌────────────────────┬────────────────────┬────────────────────────────────┐
│   slot 0 (RAM)     │   MMIO hole (4KB)  │   slot 1 (RAM)                 │
│   normal access    │   no backing store │   normal access                │
│   → no exit        │   → KVM_EXIT_MMIO  │   → no exit                    │
└────────────────────┴────────────────────┴────────────────────────────────┘
```

### VMM: KVM_EXIT_MMIO の処理

```c
case KVM_EXIT_MMIO:
    if (run->mmio.phys_addr == 0xD0000 && run->mmio.is_write) {
        char c = run->mmio.data[0];
        if (c != '\n')
            printf("[MMIO write @ 0x%llx] %c\n", run->mmio.phys_addr, c);
    }
    break;
```

データは `run->mmio.data[]` に直接利用可能 — (PIO の `data_offset` と違い) オフセット計算不要。

### ゲスト: MMIO アドレスへの書き込み

ロングモードで、ゲストは通常の `mov` で GPA 0xD0000 にバイトを格納する:

```asm
long_mode:
    .byte 0x48, 0xC7, 0xC3, 0x00, 0x00, 0x0D, 0x00     /* mov rbx, 0xD0000 */
    .byte 0xB0, 'M'                                    /* mov al, 'M' */
    .byte 0x88, 0x03                                   /* mov [rbx], al */
```

命令自体にはこれがデバイスアクセスであることを示すものはない。CPU は通常の store 命令を実行する。アドレスが RAM かデバイスかは、完全にアドレス変換レイヤーによって決定される。

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

出力に PIO と MMIO の exit が両方表示され、2つのメカニズムが並べて見える。

## なぜゲストページテーブルを MMIO に使わないのか

自然な疑問: ゲストページテーブルで GPA 0xD0000 を「not present」にすればよいのでは？

できない — それは MMIO exit ではなく**ゲスト内部のページフォルト** (#PF) を引き起こす。KVM はゲストに #PF を注入し、IDT がないため triple fault になる。

MMIO トラッピングは**メモリスロットレイヤー** (第2段階変換) で動作し、ゲストページテーブルレイヤーではない。ゲストページテーブルは「このアドレスは有効」と言い (アイデンティティマップで present=1)、しかしバッキングするメモリスロットがない — それがユーザー空間への exit をトリガーする。

```
ゲストページテーブル:  GPA 0xD0000   → present (アイデンティティマップ)
メモリスロット:        GPA 0xD0000   → スロット未登録
結果:                 KVM_EXIT_MMIO → ユーザー空間が処理
```

## 重要な知見

ゲストは特定のアドレスが RAM を指すのかデバイスレジスタを指すのか区別できない。どちらも通常のメモリ命令でアクセスする。この抽象化が、OS が RAM、PCIe デバイス、フレームバッファ、MMIO レジスタに同じ load/store 命令を使えることを可能にする。

MMIO エミュレーションは Step 4 で導入した2段階変換の直接的な帰結。VMM がどのゲスト物理アドレスにバッキングメモリがあり、どれにないかを制御する。バッキングメモリのないアドレスがデバイスレジスタになる — VMM は全アクセスをインターセプトしてデバイスの振る舞いをエミュレートする。

### 実世界の例

PCIe デバイスは BAR (Base Address Register) と呼ばれる MMIO 領域でレジスタを公開する。Linux ドライバが MMIO レジスタに書き込むとき:

```c
writel(value, bar + offset);
```

CPU は物理アドレスへの通常の store 命令を実行する。このステップでは、この store がメモリスロットのホールにヒットし、デバイスエミュレーションのために VMM に exit する。本番 VMM も同じ基本的なアイデアを使うが、一部のデバイスはカーネル内で処理されたり他のメカニズムで加速されたりする。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| MMIO | ゲスト物理アドレスとしてのデバイスレジスタ |
| メモリスロットホール | 未登録 GPA 範囲 → KVM_EXIT_MMIO |
| run->mmio | phys_addr, data[], len, is_write — 1つの構造体に全て |
| 透過的トラッピング | ゲストは通常の `mov` を使う — trap を認識しない |
| 第2段階 vs ゲスト PT | MMIO はメモリスロットの性質であり、ゲストページテーブルではない |

## 次のステップ

[Step 6: MMIO read + デバイス状態](step06_mmio-read.md) — MMIO read サポートを追加し、ゲストがデバイス状態を問い合わせられるようにして、双方向デバイスモデルを完成させる。
