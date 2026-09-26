# Step 20: Dirty page tracking

## 目的

`KVM_MEM_LOG_DIRTY_PAGES` と `KVM_GET_DIRTY_LOG` を使い、guest がどのページに書き込んだかを追跡する。`Ctrl-A d` モニタコマンドで dirty page 数をオンデマンド表示する。

## 背景

### Dirty page とは何か

「dirty」とは「書き込まれた」という意味。このステップの設定では:
- **dirty page** = 前回の `KVM_GET_DIRTY_LOG` 呼び出し以降に guest が書き込んだ 4KB ページ
- **clean page** = 書き込まれていないページ（またはリセット後に書き込まれていないページ）

### なぜ dirty tracking が必要か

2つの主要ユースケース:
1. **Live migration (Step 22)**: 各 iteration で 128MB 全体をコピーするのではなく、前回から変わったページだけを送る
2. **Incremental snapshot**: RAM 全体ではなく変更ページだけ保存で高速化

どちらも「どのページが変わったか」を知る必要がある。それが dirty tracking。

### KVM dirty tracking の仕組み

書き込み検出の基本を、EPT の write-protection を使う方式で説明する。実際の検出方式は CPU と KVM の設定に依存する:

```
1. KVM_MEM_LOG_DIRTY_PAGES フラグをメモリスロットに設定
   → KVM がスロット内の全 EPT エントリから write 権限を剥奪

2. Guest がページ X に書き込み
   → EPT violation (write fault, VM exit)
   → KVM が dirty bitmap[X] = 1 をセット
   → KVM がページ X の EPT エントリに write 権限を復元
   → Guest を再開（リトライで write 成功）

3. Guest が再度ページ X に書き込み
   → EPT エントリに既に write 権限あり → dirty 検出のための write fault は不要

4. VMM が KVM_GET_DIRTY_LOG を呼ぶ
   → KVM が bitmap を userspace にコピー
   → KVM が bitmap をクリア（全ビット → 0）
   → KVM が再度 write 権限を剥奪（tracking を再 arm）
   → 次のサイクル開始
```

この方式では、dirty として記録して書き込みを許可したページへの後続 write は、dirty 検出のための fault を必要としない。

この実装では手動クリア機能を有効にしていないため、`KVM_GET_DIRTY_LOG` は bitmap を取得し、ioctl が戻るまでに dirty bit をクリアする。次回呼んだときは「前回以降に dirtied されたページだけ」が返る。これが iterative pre-copy migration に必要な正確なセマンティクス。

### 2つの API

| API | 役割 |
|-----|------|
| `KVM_MEM_LOG_DIRTY_PAGES` | `kvm_userspace_memory_region` のフラグ — スロットの tracking を有効化 |
| `KVM_GET_DIRTY_LOG` | ioctl — bitmap を取得し、dirty bit をクリア（この実装の設定） |

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
  (今回は 191 pages)
```

## 実装

### メモリスロットに dirty logging を有効化

```c
struct kvm_userspace_memory_region region1 = {
    .slot = MEM_SLOT0_ID,
    .flags = KVM_MEM_LOG_DIRTY_PAGES,   /* dirty tracking 有効化 */
    .guest_phys_addr = MEM_SLOT0_GPA,
    .memory_size = MEM_SLOT0_SIZE,
    .userspace_addr = (unsigned long)mem + MEM_SLOT0_GPA,
};
```

両スロット（MMIO hole を挟んだ slot 0 と slot 1）にこのフラグを設定。

### print_dirty_log() — 取得と表示

slot 0 の処理を抜粋する。実装では slot 1 も取得し、割り当て・ioctl のエラー処理、合計の表示、bitmap の解放を行う。

```c
static void print_dirty_log(int vmfd, size_t mem_size) {
    /* bitmap 割り当て: 1ページ = 1ビット、uint64_t 境界に切り上げ */
    size_t slot0_pages = MEM_SLOT0_SIZE / 4096;
    size_t slot0_bitmap_sz = (slot0_pages + 63) / 64 * 8;
    uint64_t *bitmap0 = calloc(1, slot0_bitmap_sz);

    struct kvm_dirty_log log0 = { .slot = MEM_SLOT0_ID, .dirty_bitmap = bitmap0 };

    /* bitmap を取得し、dirty bit をクリア */
    ioctl(vmfd, KVM_GET_DIRTY_LOG, &log0);

    /* popcount で dirty page 数をカウント */
    uint64_t dirty0 = 0;
    for (size_t i = 0; i < slot0_bitmap_sz / 8; i++)
        dirty0 += __builtin_popcountll(bitmap0[i]);
}
```

bitmap の bit 0 は各スロットの先頭ページに対応する。bit n の GPA は `スロットの開始 GPA + n × 4096` になる:
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
/ # < Ctrl-A d >
=== Dirty page report (Ctrl-A d) ===
  Slot 0 [0x0-0xd0000]:        8 / 208 pages dirty
  Slot 1 [0xd1000-0x8000000]:  3930 / 32559 pages dirty
  Total:                       3938 pages (15752 KB)

/ # echo hello > /dev/hvc0
hello
/ # < Ctrl-A d >
=== Dirty page report (Ctrl-A d) ===
  Slot 0 [0x0-0xd0000]:        0 / 208 pages dirty
  Slot 1 [0xd1000-0x8000000]:  191 / 32559 pages dirty
  Total:                       191 pages (764 KB)

/ # mkdir /tmp
/ # dd if=/dev/zero of=/tmp/test bs=4K count=1024
1024+0 records in
1024+0 records out
4194304 bytes (4.0MB) copied, 0.013352 seconds, 299.6MB/s
/ # < Ctrl-A d >
=== Dirty page report (Ctrl-A d) ===
  Slot 0 [0x0-0xd0000]:        0 / 208 pages dirty
  Slot 1 [0xd1000-0x8000000]:  1241 / 32559 pages dirty
  Total:                       1241 pages (4964 KB)
```

結果の読み方:

- **1回目 (boot 後)**: 3938 pages (15752 KB) — tracking 有効化以降に dirty になったページ。
- **2回目 (echo 後)**: 191 pages (764 KB) — 1回目以降の書き込み。echo に伴う処理だけでなく、その間の guest の動作も含む。
- **3回目 (dd 4 MiB 後)**: 1241 pages (4964 KB) — ファイルデータ4 MiBは1024ページ相当。mkdir・dd・ファイルシステムや kernel の処理も書き込みを伴うが、差の217ページの内訳はこの集計だけでは特定できない。
- **Slot 0 が2・3回目に0**: 今回の各計測区間では、低メモリ領域への書き込みが記録されなかった。

表示の `KB` は1ページを4 KiBとして計算した値。同じページへ何度書いても、その計測区間では1ページとして数える。2回目の値が初回の累積値ではないことは、取得時にクリアされる動作と整合する。

レポート取得中も vCPU は動作し、2つのスロットは順番に取得するため、VM 全体を同一時刻で止めたスナップショットではない。

## 重要な知見

この設定の `KVM_GET_DIRTY_LOG` は、dirty bitmap の取得とクリアを行う。live migration では、bitmap を取得・クリアして対象ページをコピーし、その間の新たな書き込みを次回の取得で追跡する。最後に VM を止めて残りを転送することで、コピー中の更新も反映できる。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| KVM_MEM_LOG_DIRTY_PAGES | メモリスロット単位で dirty tracking を有効化するフラグ |
| KVM_GET_DIRTY_LOG | bitmap を取得し、dirty bit をクリア（この実装の設定） |
| EPT write-protect | 書き込みを fault で検出する方式。Step 19 の未マップページへの fault と目的が異なる |
| Dirty bitmap の構造 | 1ビット/4KB ページ、uint64_t アライメント |
| __builtin_popcountll | set bit を効率的にカウント（dirty page 数） |
| Iterative delta | 2回目の呼び出しは1回目以降の変更のみ表示 |

## 変わったこと

Step 19 からの変更:
- **microkvm.c のみ**: `print_dirty_log()` 関数、`Ctrl-A d` ハンドラ、両メモリスロットに `KVM_MEM_LOG_DIRTY_PAGES` フラグ

新規ファイルなし。

## 次のステップ

[Step 21: VM snapshot](step21_snapshot.md) では VM の全状態（CPU レジスタ + デバイス状態 + RAM）をファイルに保存し、`--restore` で復元する — 保存した瞬間から実行を再開する。
