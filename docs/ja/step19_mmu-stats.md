# Step 19: KVM MMU stats — メモリ仮想化の観察

> **Phase D: Memory State Management**
>
> Phase A–C は CPU 実行、デバイスエミュレーション、I/O 性能に焦点を当てた。
> Phase D はメモリ状態に移る: KVM が guest ページをどうマッピングし、変更を追跡し、最終的に snapshot と live migration を可能にするか。

## 目的

`KVM_GET_STATS_FD` で per-VM / per-vCPU の MMU カウンタをゲスト実行開始前と停止後に取得する。demand paging、EPT fault の解決、exit の内訳を実際の数値で観察する。

## 背景

### なぜメモリ仮想化を観察するのか

Step 1-18 で hypervisor を構築した。Phase D ではKVM のメモリサブシステムの内部動作を理解する。dirty page tracking (Step 20) や snapshot (Step 21) の前に、KVM が guest メモリをどう管理しているかを可視化する必要がある。

このステップで答える問い:
- 停止時点でマッピングされている guest ページはいくつか？
- EPT fault は何回発生し、どう解決されるか？
- exit の内訳は I/O / メモリ / HLT のどれが支配的か？

### KVM のバイナリ stats インターフェース

`KVM_GET_STATS_FD` は、VM fd または vCPU fd を指定して、その VM / vCPU の統計を読み取るバイナリ fd を取得する API:

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

各 descriptor の `flags` の下位4ビットが stat の種類を示す:
- **Cumulative (type=0)**: 単調増加カウンタ（例: `pf_taken`）
- **Instant (type=1)**: 現在値のスナップショット（例: `pages_4k`）
- **Peak (type=2)**: 過去最大値

### EPT demand paging

KVM は guest メモリ全体の EPT (Extended Page Table) を事前に作成しない。guest が初めてページにアクセスすると EPT violation が発生し、KVM がオンデマンドでマッピングする:

```
Guest が未マップの GPA にアクセス
  → EPT violation (VM exit)
  → KVM page fault handler
  → GPA に対応する host ページを取得し、EPT エントリを作成
  → pf_fixed++、4 KiB マッピングなら pages_4k++
  → guest を再開（リトライでアクセス成功）
```

必要なマッピングをアクセスに応じて作るため、128 MiB 全体を事前に EPT へ登録する必要はない。host ページは VMM がカーネルなどをロードした時点で確保済みの場合もある。

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
  → pages_4k: 4004 (current)
  → pf_taken: +4006
  → pf_fixed: +4004
  → exits: +22173
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

バイナリ stats fd のレイアウトを読み取る: header → descriptor → data block。以下は読み取りの流れを示す抜粋で、エラー処理・保存処理・解放処理は省略している。

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
    for (unsigned int i = 0, n = 0; i < hdr.num_desc && n < KVM_STATS_MAX_ENTRIES; i++) {
        struct kvm_stats_desc *d = (void *)(descs + i * one_desc);
        if (d->size != 1) continue;
        /* entries[n] に name, value, flags を保存 */
        n++;
    }
}
```

設計上の選択:
- `pread()` で明示的 offset 指定 — スレッドセーフ、seek 状態を持たない
- histogram (`d->size > 1`) をスキップ — scalar カウンタのみ
- 最大64個の scalar を保存し、data block は固定8 KiB を読み取る
- 表示時に名前の部分一致・完全一致でフィルタ（`pf_`, `pages_`, `tlb`, `exits` 等）
- cumulative は差分、instant / peak は取得値を表示し、0 の項目は省略する

### microkvm.c — before/after capture

stats は2つの時点で取得する: `KVM_RUN` 開始前と vCPU 停止後。差分には boot 後のシェル操作や待機中の動作も含まれる。

```c
/* KVM_RUN の前 */
int vm_stats_fd = ioctl(vmfd, KVM_GET_STATS_FD, NULL);
int vcpu_stats_fd = ioctl(vcpus[0].fd, KVM_GET_STATS_FD, NULL);
struct kvm_stats_reading vm_before = {0}, vcpu_before = {0};
if (vm_stats_fd >= 0)
    kvm_stats_capture(vm_stats_fd, &vm_before);
if (vcpu_stats_fd >= 0)
    kvm_stats_capture(vcpu_stats_fd, &vcpu_before);

/* vCPU 停止後 */
struct kvm_stats_reading vm_after = {0}, vcpu_after = {0};
if (vm_stats_fd >= 0) {
    kvm_stats_capture(vm_stats_fd, &vm_after);
    kvm_stats_print_delta("KVM VM stats", &vm_before, &vm_after);
    close(vm_stats_fd);
}
if (vcpu_stats_fd >= 0) {
    kvm_stats_capture(vcpu_stats_fd, &vcpu_after);
    kvm_stats_print_delta("KVM vCPU 0 stats", &vcpu_before, &vcpu_after);
    close(vcpu_stats_fd);
}
```

## 出力

今回の execution report の KVM 統計部分を抜粋する:

```
--- KVM VM stats ---
  pages_4k                         4004 (current)

--- KVM vCPU 0 stats ---
  pf_taken                         +4006
  pf_fixed                         +4004
  pf_emulate                       +2
  pf_mmio_spte_created             +2
  tlb_flush                        +4
  exits                            +22173
  io_exits                         +15674
  mmio_exits                       +163
  halt_exits                       +1270
  irq_injections                   +98
```

数値の読み方:

- **pages_4k = 4004**: 現在の4 KiB マッピングは約15.64 MiB（guest RAM 128 MiB の約12.2%）。累積アクセス数ではなく、マッピングが解除されれば減りうる。
- **pf_taken = 4006、pf_fixed = 4004**: KVM の page fault 処理の大部分がマッピングの作成・更新などで解決されている。guest OS 自身の page fault 回数ではない。
- **pf_emulate = 2、pf_mmio_spte_created = 2**: エミュレーションへ進んだ fault と MMIO 用 SPTE（KVM が管理するページテーブルエントリ）の作成が各2回。`pf_taken - pf_fixed` とも一致するが、この統計だけではアクセス先アドレスまでは特定できない。
- **exits = 22173**: 計測期間中に KVM が数えた VM exit。KVM 内で処理したものも含み、すべてがユーザー空間へ戻るわけではない。
- **io_exits = 15674（約71%）**: I/O port の exit が多い。Linux の起動ログを出力する UART などが該当する。
- **mmio_exits = 163**: Userspace counters の `MMIO exits total: 163` と一致する。
- **irq_injections = 98**: タイマーなどを含む KVM の割り込み注入数。Userspace counters の IRQ inject は virtio RX の通知だけを数えるため、今回の0回と矛盾しない。

今回の Mode は両方 OFF で、TX / IRQ latency はいずれもデータなし。virtio の TX / RX ワークロードを実行していない場合でも、起動時のデバイス初期化で MMIO exit は発生する。

## 重要な知見

KVM はアクセスに応じて guest メモリのマッピングを作成し、今回は停止時点で4,004個の4 KiB マッピングが存在した。現在のマッピング数と fault の累積回数を区別すると、メモリ管理の動作を読み取れる。Step 20 では、さらに書き込みを追跡する dirty page tracking を扱う。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| KVM_GET_STATS_FD | per-VM/vCPU カウンタのバイナリ stats API |
| EPT demand paging | アクセスに応じてマッピングを作成し、pages_4k で現在数を観察 |
| pf_fixed vs pf_emulate | マッピング等で解決した fault とエミュレーションへ進んだ fault |
| pread() での stats fd 読み取り | スレッドセーフな明示的 offset 指定 |
| Exit の内訳 | I/O (UART) が boot を支配、MMIO (virtio) はごくわずか |

## 変わったこと

Step 18 後のベンチマーク実装からの変更:
- **新規ファイル**: `kvm_stats.h`, `kvm_stats.c` — stats capture とフィルタ付き delta 表示
- **microkvm.c**: `#include "kvm_stats.h"`, KVM_RUN 前後で capture
- **Makefile**: `kvm_stats.c` をビルドに追加

## 次のステップ

[Step 20: Dirty page tracking](step20_dirty-tracking.md) では `KVM_MEM_LOG_DIRTY_PAGES` と `KVM_GET_DIRTY_LOG` を使い、guest がどのページに書き込んだかを追跡する — live migration の基盤技術。
