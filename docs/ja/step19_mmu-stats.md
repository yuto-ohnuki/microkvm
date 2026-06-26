# Step 19: KVM MMU stats — メモリ仮想化の観察

## 目的

`KVM_GET_STATS_FD` で per-VM / per-vCPU の MMU カウンタを boot 前後で取得する。demand paging、EPT fault の解決、exit の内訳を実際の数値で観察する。

## 背景

### なぜメモリ仮想化を観察するのか

Step 1-18 で hypervisor を構築した。Phase D では KVM のメモリサブシステムの内部動作を理解する。dirty page tracking (Step 20) や snapshot (Step 21) の前に、KVM が guest メモリをどう管理しているかを可視化する必要がある。

このステップで答える問い:
- boot 中に実際にマッピングされる guest ページはいくつか？
- EPT fault は何回発生し、どう解決されるか？
- exit の内訳は I/O / メモリ / HLT のどれが支配的か？

### KVM のバイナリ stats インターフェース

Linux 5.15 以前は debugfs (`/sys/kernel/debug/kvm/`) でしか KVM stats を取得できず、全 VM の合計値しか見えなかった。`KVM_GET_STATS_FD` は per-VM / per-vCPU の stats をバイナリ fd で提供する:

```
ioctl(vmfd, KVM_GET_STATS_FD, NULL)    → VM レベルの stats fd
ioctl(vcpufd, KVM_GET_STATS_FD, NULL)  → vCPU レベルの stats fd
```

fd 内のレイアウト:
```
┌──────────────────────┐  offset 0
│ kvm_stats_header     │  num_desc, name_size, desc_offset, data_offset
├──────────────────────┤  offset = desc_offset
│ kvm_stats_desc[N]    │  name + type + data block 内の offset
├──────────────────────┤  offset = data_offset
│ uint64_t data[]      │  カウンタの実際の値
└──────────────────────┘
```

各 descriptor の `flags` フィールドが stat の種類を示す:
- **Cumulative (type=0)**: 単調増加カウンタ（例: `pf_taken`）
- **Instant (type=1)**: 現在値のスナップショット（例: `pages_4k`）
- **Peak (type=2)**: 過去最大値

### EPT demand paging

KVM は guest メモリ全体の EPT (Extended Page Table) を事前に作成しない。guest が初めてページにアクセスすると EPT violation が発生し、KVM がオンデマンドでマッピングする:

```
Guest が未マップの GPA にアクセス
  → EPT violation (VM exit)
  → KVM page fault handler
  → host ページを割り当て、EPT エントリを作成
  → pf_fixed++, pages_4k++
  → guest を再開（リトライでアクセス成功）
```

つまり boot 中に guest が実際にタッチしたページだけが EPT エントリを持つ — 128MB 全体ではない。

## 実行フロー

```
Host (microkvm main)         KVM                    Guest
────────────────────         ───                    ─────
KVM_GET_STATS_FD (vm+vcpu)
kvm_stats_capture(before)
                                                    boot 開始
                                                    kernel がページをタッチ
                             EPT violation
                             → pf_fixed++
                             → pages_4k++
                                                    boot 完了
                                                    シェルプロンプト
Ctrl-C → stop_requested
kvm_stats_capture(after)
kvm_stats_print_delta()
  → pages_4k: 4325 (current)
  → pf_taken: +4327
  → pf_fixed: +4325
  → exits: +27665
```

## 実装

### kvm_stats.h

stats capture インターフェースを定義する新規ヘッダ:

```c
#define KVM_STATS_MAX_ENTRIES 64

struct kvm_stat_entry {
    char name[48];
    uint64_t value;
    uint32_t flags;
};

struct kvm_stats_reading {
    unsigned int count;
    struct kvm_stat_entry entries[KVM_STATS_MAX_ENTRIES];
};

int kvm_stats_capture(int stats_fd, struct kvm_stats_reading *snap);
void kvm_stats_print_delta(const char *label,
    const struct kvm_stats_reading *before,
    const struct kvm_stats_reading *after);
```

### kvm_stats.c — capture

バイナリ stats fd のレイアウトを読み取る: header → descriptor → data block。

```c
int kvm_stats_capture(int stats_fd, struct kvm_stats_reading *snap) {
    struct kvm_stats_header hdr;
    pread(stats_fd, &hdr, sizeof(hdr), 0);

    /* 各 descriptor は可変長: 固定構造体 + name_size 分のパディング */
    size_t one_desc = sizeof(struct kvm_stats_desc) + hdr.name_size;
    char *descs = malloc(one_desc * hdr.num_desc);
    pread(stats_fd, descs, one_desc * hdr.num_desc, hdr.desc_offset);

    char *data = malloc(8 * 1024);
    pread(stats_fd, data, 8 * 1024, hdr.data_offset);

    /* scalar stat のみ抽出 (histogram の size > 1 はスキップ) */
    /* Histogram stat は単一の uint64_t ではなく配列を含むため、簡潔さのために省略 */
    for (unsigned int i = 0; i < hdr.num_desc; i++) {
        struct kvm_stats_desc *d = (void *)(descs + i * one_desc);
        if (d->size != 1) continue;
        /* name, value, flags を保存 */
    }
}
```

設計上の選択:
- `pread()` で明示的 offset 指定 — スレッドセーフ、seek 状態を持たない
- histogram (`d->size > 1`) をスキップ — scalar カウンタのみ
- 表示時に name prefix でフィルタ（`pf_*`, `pages_*`, `tlb*`, `exits` 等）

### microkvm.c — before/after capture

stats は2つの時点で取得する: `KVM_RUN` 開始直前と vCPU 停止後。差分が guest 実行中に何が起きたかを示す。

```c
/* KVM_RUN の前 */
int vm_stats_fd = ioctl(vmfd, KVM_GET_STATS_FD, NULL);
int vcpu_stats_fd = ioctl(vcpus[0].fd, KVM_GET_STATS_FD, NULL);
struct kvm_stats_reading vm_before = {0}, vcpu_before = {0};
kvm_stats_capture(vm_stats_fd, &vm_before);
kvm_stats_capture(vcpu_stats_fd, &vcpu_before);

/* vCPU 停止後 */
struct kvm_stats_reading vm_after = {0}, vcpu_after = {0};
kvm_stats_capture(vm_stats_fd, &vm_after);
kvm_stats_print_delta("KVM VM stats (boot delta)", &vm_before, &vm_after);
kvm_stats_capture(vcpu_stats_fd, &vcpu_after);
kvm_stats_print_delta("KVM vCPU 0 stats (boot delta)", &vcpu_before, &vcpu_after);
```

## 出力

```
--- KVM VM stats (boot delta) ---
  pages_4k                         4325 (current)

--- KVM vCPU 0 stats (boot delta) ---
  pf_taken                         +4327
  pf_fixed                         +4325
  pf_emulate                       +2
  pf_mmio_spte_created             +2
  tlb_flush                        +4
  exits                            +27665
  io_exits                         +15047
  mmio_exits                       +163
  halt_exits                       +4316
  irq_injections                   +93
```

数値の読み方:
- **pages_4k = 4325**: EPT に 4325 ページがマップ = guest RAM 128MB 中 ~17MB（demand paging）。これは現在のマッピング数であり累積カウンタではない — ページがアンマップされると減少しうる。
- **pf_taken ≈ pf_fixed**: ほぼ全ての page fault が新規 EPT マッピングで解決。`pf_taken` は処理された全 page fault の数、`pf_fixed` は通常の EPT マッピング作成で解決された数。差分 (4327 - 4325 = 2) は `pf_emulate` と一致 — MMIO special mapping。
- **pf_emulate = 2**: MMIO 領域（virtio-mmio 0xD0000000）— special PTE 作成
- **exits = 27665**: boot 中の全 VM exit
- **io_exits = 15047 (54%)**: UART serial 出力が支配的。Phase B/C と一致 — Linux コンソールは全 boot メッセージに 8250 UART (Step 11) を使用している。
- **mmio_exits = 163 (<1%)**: virtio デバイスアクセスはごくわずか

## 重要な知見

KVM は guest メモリ全体を事前にマッピングしない。128MB の guest RAM のうち、最小 Linux boot で実際にタッチされるのは ~13% だけ。各初回アクセスで EPT violation → KVM が host ページを割り当てて EPT にマッピングする。この demand paging は Step 20 の dirty tracking と同じメカニズム: KVM が EPT 権限を制御して guest のメモリ動作を観察する。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| KVM_GET_STATS_FD | per-VM/vCPU カウンタのバイナリ stats API |
| EPT demand paging | guest がタッチしたページだけ pages_4k が増える |
| pf_fixed vs pf_emulate | 通常マッピング vs MMIO SPTE 作成 |
| pread() での stats fd 読み取り | スレッドセーフな明示的 offset 指定 |
| Exit の内訳 | I/O (UART) が boot を支配、MMIO (virtio) はごくわずか |

## 変わったこと

Step 18 からの変更:
- **新規ファイル**: `kvm_stats.h`, `kvm_stats.c` — stats capture とフィルタ付き delta 表示
- **microkvm.c**: `#include "kvm_stats.h"`, KVM_RUN 前後で capture
- **Makefile**: `kvm_stats.c` をビルドに追加

## 次のステップ

[Step 20: Dirty page tracking](step20_dirty-tracking.md) では `KVM_MEM_LOG_DIRTY_PAGES` と `KVM_GET_DIRTY_LOG` を使い、guest がどのページに書き込んだかを追跡する — live migration の基盤技術。
