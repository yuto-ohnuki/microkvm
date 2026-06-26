# Step 20: Dirty page tracking

## 目的

`KVM_MEM_LOG_DIRTY_PAGES` と `KVM_GET_DIRTY_LOG` を使い、guest がどのページに書き込んだかを追跡する。`Ctrl-A d` モニタコマンドで dirty page 数をオンデマンド表示する。

## 背景

### Dirty page とは何か

「dirty」とは「書き込まれた」という意味。KVM の文脈では:
- **dirty page** = 前回の `KVM_GET_DIRTY_LOG` 呼び出し以降に guest が書き込んだ 4KB ページ
- **clean page** = 書き込まれていないページ（またはリセット後に書き込まれていないページ）

### なぜ dirty tracking が必要か

2つの主要ユースケース:
1. **Live migration (Step 22)**: 各 iteration で 128MB 全体をコピーするのではなく、前回から変わったページだけを送る
2. **Incremental snapshot**: RAM 全体ではなく変更ページだけ保存で高速化

どちらも「どのページが変わったか」を知る必要がある。それが dirty tracking。

### KVM dirty tracking の仕組み

KVM は EPT の write-protection を利用して書き込みを検出する:

```
1. KVM_MEM_LOG_DIRTY_PAGES フラグをメモリスロットに設定
   → KVM がスロット内の全 EPT エントリから write 権限を剥奪

2. Guest がページ X に書き込み
   → EPT violation (write fault, VM exit)
   → KVM が dirty bitmap[X] = 1 をセット
   → KVM がページ X の EPT エントリに write 権限を復元
   → Guest を再開（リトライで write 成功）

3. Guest が再度ページ X に書き込み
   → EPT エントリに既に write 権限あり → exit なし（ゼロコスト）

4. VMM が KVM_GET_DIRTY_LOG を呼ぶ
   → KVM が bitmap を userspace にコピー
   → KVM が bitmap をクリア（全ビット → 0）
   → KVM が再度 write 権限を剥奪（tracking を再 arm）
   → 次のサイクル開始
```

各 `KVM_GET_DIRTY_LOG` 呼び出し後の最初の write だけが EPT write fault を起こす。同じサイクル内での同一ページへの後続 write はゼロコスト。

重要: `KVM_GET_DIRTY_LOG` は bitmap を返すと同時にリセットする。次回呼んだときは「前回以降に dirtied されたページだけ」が返る。これが iterative pre-copy migration に必要な正確なセマンティクス。

### 2つの API

| API | 役割 |
|-----|------|
| `KVM_MEM_LOG_DIRTY_PAGES` | `kvm_userspace_memory_region` のフラグ — スロットの tracking を有効化 |
| `KVM_GET_DIRTY_LOG` | ioctl — bitmap を返却 + アトミックにクリア |

## 実行フロー

```
Host (microkvm)              KVM                         Guest
───────────────              ───                         ─────
SET_USER_MEMORY_REGION
  flags = KVM_MEM_LOG_DIRTY_PAGES
                             EPT: 全ページ write-protect
                                                         boot (ページに書き込み)
                             EPT violation → bitmap[X]=1
                             write 権限を復元
                                                         シェルプロンプト
Ctrl-A d
  KVM_GET_DIRTY_LOG (slot 0)
  KVM_GET_DIRTY_LOG (slot 1)
                             bitmap → userspace にコピー
                             bitmap クリア、write-protect 再設定
  popcount → 表示
                                                         echo hello
Ctrl-A d (再度)
  KVM_GET_DIRTY_LOG
                             前回呼び出し以降に dirty された
                             ページのみ返す
  popcount → 表示
  (今回は ~188 pages のみ)
```

## 実装

### メモリスロットに dirty logging を有効化

```c
struct kvm_userspace_memory_region region1 = {
    .slot = 0,
    .flags = KVM_MEM_LOG_DIRTY_PAGES,   /* dirty tracking 有効化 */
    .guest_phys_addr = 0,
    .memory_size = 0xD0000,
    .userspace_addr = (unsigned long)mem,
};
```

両スロット（MMIO hole を挟んだ slot 0 と slot 1）にこのフラグを設定。

### print_dirty_log() — 取得と表示

```c
static void print_dirty_log(int vmfd, size_t mem_size) {
    /* bitmap 割り当て: 1ページ = 1ビット、uint64_t 境界に切り上げ */
    size_t slot0_pages = 0xD0000 / 4096;
    size_t slot0_bitmap_sz = (slot0_pages + 63) / 64 * 8;
    uint64_t *bitmap0 = calloc(1, slot0_bitmap_sz);

    struct kvm_dirty_log log0 = { .slot = 0, .dirty_bitmap = bitmap0 };

    /* bitmap を取得し、アトミックに dirty bit をクリア */
    ioctl(vmfd, KVM_GET_DIRTY_LOG, &log0);

    /* popcount で dirty page 数をカウント */
    uint64_t dirty0 = 0;
    for (size_t i = 0; i < slot0_bitmap_sz / 8; i++)
        dirty0 += __builtin_popcountll(bitmap0[i]);
}
```

bitmap は guest ページに直接対応する:
```
Guest pages:  0  1  2  3  4  5  6  7
Bitmap bits:  1  0  1  0  0  1  0  0  → 3 dirty pages (popcount = 3)
```

### Ctrl-A d モニタコマンド

```c
if (c == 'd') {
    print_dirty_log(g_vmfd, GUEST_MEM_SIZE);
    continue;
}
```

## 出力

```
=== Hello from microkvm guest! ===
/ #
=== Dirty page report (Ctrl-A d) ===
  Slot 0 [0x0-0xD0000]:        8 / 208 pages dirty
  Slot 1 [0xD1000-0x8000000]:  4259 / 32559 pages dirty
  Total:                       4267 pages (17068 KB)

/ # echo hello > /dev/hvc0
hello
/ #
=== Dirty page report (Ctrl-A d) ===
  Slot 0 [0x0-0xD0000]:        0 / 208 pages dirty
  Slot 1 [0xD1000-0x8000000]:  188 / 32559 pages dirty
  Total:                       188 pages (752 KB)

/ # mkdir /tmp
/ # dd if=/dev/zero of=/tmp/test bs=4K count=1024
1024+0 records in
1024+0 records out
/ #
=== Dirty page report (Ctrl-A d) ===
  Slot 0 [0x0-0xD0000]:        0 / 208 pages dirty
  Slot 1 [0xD1000-0x8000000]:  1233 / 32559 pages dirty
  Total:                       1233 pages (4932 KB)
```

結果の読み方:
- **1回目 (boot 後)**: 4267 pages (~17MB) — tracking 有効化以降に dirty された全ページ
- **2回目 (echo 後)**: 188 pages (752KB) — *1回目以降に* dirty されたページのみ
- **3回目 (dd 4MB 後)**: 1233 pages (~4.9MB) — 1024 data pages + ~209 pages（ファイルシステムメタデータ、page cache 管理、kernel 内部処理）
- **Slot 0 が1回目以降 0**: 低メモリ領域は boot 後は静的 — ほとんどの write は通常 RAM (Slot 1) で発生し、低メモリ boot 領域 (BDA, boot params — Step 10) にはもう書き込まれない

減少するカウントが `KVM_GET_DIRTY_LOG` のアトミックリセット動作を実証している。

## 重要な知見

`KVM_GET_DIRTY_LOG` は単なる読み取りではない — 読み取り *かつリセット* する。各呼び出しは前回以降に dirty されたページだけを返す。この「取得 + クリア」セマンティクスが live migration に必要なもの: dirty pages をコピー → tracking をクリア → 待機 → *新たに* dirty されたページだけコピー。dirty set がほぼゼロに収束するまで繰り返し、最後に VM を止めて残りの数ページを転送する。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| KVM_MEM_LOG_DIRTY_PAGES | EPT write-protect で write tracking を有効化するフラグ |
| KVM_GET_DIRTY_LOG | bitmap を返却 + アトミックにクリア |
| EPT write-protect | Step 19 の demand paging と同じメカニズム、目的が異なる |
| Dirty bitmap の構造 | 1ビット/4KB ページ、uint64_t アライメント |
| __builtin_popcountll | set bit を効率的にカウント（dirty page 数） |
| Iterative delta | 2回目の呼び出しは1回目以降の変更のみ表示 |

## 変わったこと

Step 19 からの変更:
- **microkvm.c のみ**: `print_dirty_log()` 関数、`Ctrl-A d` ハンドラ、両メモリスロットに `KVM_MEM_LOG_DIRTY_PAGES` フラグ

新規ファイルなし。

## 次のステップ

[Step 21: VM snapshot](step21_snapshot.md) では VM の全状態（CPU レジスタ + デバイス状態 + RAM）をファイルに保存し、`--restore` で復元する — 保存した瞬間から実行を再開する。
