# Step 22: Live migration — iterative pre-copy

## 目的

実行中の VM を停止せずに新しいインスタンスに移動する。dirty page tracking (Step 20) と state save/restore (Step 21) を組み合わせて iterative pre-copy を実装: guest が走り続けたまま RAM をコピーし、最後に短時間停止して残りの dirty pages と CPU state を転送する。

## 背景

### Live migration とは何か

Live migration は実行中の VM をほぼゼロダウンタイムで別のホストに移動する技術。guest は何も気付かない — 何事もなかったかのように実行を続ける。

なぜ重要か:
- **ホストメンテナンス**: kernel update やハードウェア交換を VM 停止なしに実行
- **障害回避**: ハードウェア故障の予兆を検知 → VM を事前に退避
- **リソース最適化**: ホスト間で VM を再配置（bin-packing）

課題: guest RAM は 128MB あり、常に変更され続けている。1回コピーしただけでは不十分 — コピー完了時にはページが変わっている。解決策は iterative convergence。

### Pre-copy アルゴリズム

```
1. 全 RAM をコピー (iteration 0) — guest は走り続ける
2. 100ms 待機 — guest がいくつかのページを dirty にする
3. dirty bitmap 取得 → dirty pages のみコピー (iteration 1)
4. dirty set が十分小さくなるまで繰り返す (< 50 pages)
5. vCPU 停止 → 最終 dirty pages + CPU state をコピー (stop-and-copy)
6. migration file から destination VM を起動
```

重要な洞察: 各 iteration でコピーするページ数は減少する。guest が 100ms で dirty にできるページ数は有限だから。idle guest では収束がほぼ即座。busy guest では複数 iteration が必要。

### 2つのフェーズ

| フェーズ | 実行場所 | VM 実行中？ | 書き込む内容 |
|---------|---------|------------|-------------|
| Pre-copy (`migrate_precopy`) | stdin_thread | はい | Header + full RAM + dirty iterations |
| Stop-and-copy (`migrate_stop_and_copy`) | main (join 後) | いいえ | Final dirty + CPU/device state |

分離が必要な理由: pre-copy は vCPU と並行に走る必要がある（RAM コピー中も guest は動く）。stop-and-copy は vCPU 停止後でないと一貫した CPU state が取れない。

### Migration file 内の dirty page フォーマット

各 dirty iteration は以下を書く:
```
[uint32_t dirty_count]
[uint32_t page_idx, 4096 bytes data] × dirty_count
```

`page_idx` = GPA / 4096。destination は base RAM を先に読み、各 dirty iteration を順番に overlay する — 同じページへの後の iteration が前の値を上書きする。あるページが複数の iteration に出現する場合、最新のコピーが常に勝つ。

## 実行フロー

```
Source VM (Ctrl-A m):
─────────────────────────────────────────────────────────────────
stdin_thread                 vCPU thread
────────────                 ───────────
Ctrl-A m 検出
migrate_precopy():
  dirty log リセット
  full RAM 書き込み (128MB)   [guest は走り続ける]
  100ms 待機                  [guest がページを dirty に]
  KVM_GET_DIRTY_LOG
  dirty pages 書き込み (iter 1)
  dirty_count <= 50 → 完了
  stop_requested = 1
                             次の VM exit
                             stop_requested → break

main (join 後):
  migrate_stop_and_copy():
    KVM_GET_DIRTY_LOG → final dirty 書き込み
    save_cpu_state() → CPU/device state 書き込み
    downtime 計測 (0.4-0.5 ms)
    header を iteration count で更新
    close(fd)

Destination VM (--restore-migration migration.bin):
─────────────────────────────────────────────────────────────────
main:
  migrate_restore():
    full RAM 読み込み (base)
    dirty iteration 1 適用 (overlay)
    final dirty pages 適用 (overlay)
    CPU/device state 読み込み
    state 適用 (PIT → clock → ... → REGS)
  KVM_RUN → guest が再開
```

## 実装

### snapshot.h — migration 構造体

```c
#define MIG_MAGIC   0x4D4B4D47  /* "MKMG" */
#define MIG_VERSION 1

#define MIGRATION_INTERVAL_MS       100
#define MIGRATION_MAX_ITERS         5
#define MIGRATION_THRESHOLD_PAGES   50

struct migrate_header {
    uint32_t magic;
    uint32_t version;
    uint64_t mem_size;
    uint32_t num_iterations;
    uint32_t pad;
};

struct migrate_context {
    int fd;                     /* pre-copy 中の open file */
    uint32_t num_iterations;    /* 完了した iteration 数 */
};
```

threshold (50 pages) はこの教育用実装のために選んだ値。production hypervisor ではネットワーク帯域、dirty rate、許容ダウンタイムに基づいて停止条件を適応的に決定する。

### snapshot.c — migrate_precopy (Phase 1)

```c
int migrate_precopy(const char *path, int vmfd, void *mem, size_t mem_size,
    struct migrate_context *ctx)
{
    /* placeholder header 書き込み */
    /* dirty log リセット (full copy 前に bitmap クリア) */
    /* full RAM 書き込み — iteration 0 */
    /* ループ: sleep → KVM_GET_DIRTY_LOG → dirty pages 書き込み
       dirty_count <= threshold or max_iters に達するまで */
    /* fd と iteration count を ctx に保存 (stop-and-copy 用) */
}
```

### snapshot.c — migrate_stop_and_copy (Phase 2)

```c
int migrate_stop_and_copy(struct migrate_context *ctx, ...) {
    uint64_t t1 = now_ns();

    /* Final dirty pages 書き込み (vCPU 停止済み — consistent) */
    migrate_write_dirty(fd, vmfd, mem, mem_size, &final_dirty);

    /* Step 21 の save_cpu_state() を再利用 */
    save_cpu_state(fd, vcpufd, vmfd, uart, virtio);

    uint64_t t2 = now_ns();
    /* downtime = t2 - t1 (stop-and-copy phase のみ) */

    /* header を最終 iteration count で更新 */
    lseek(fd, 0, SEEK_SET);
    write(fd, &hdr, sizeof(hdr));
}
```

### microkvm.c の変更

- `Ctrl-A m`: stdin_thread から `migrate_precopy()` を呼び、`g_migrate_active = 1` を設定
- `--restore-migration`: 引数をパースし、KVM_RUN 前に `migrate_restore()` を呼ぶ
- vCPU join 後: `if (g_migrate_active)` → `migrate_stop_and_copy()`、else → `snap_save()`
- `now_ns()` を `microkvm.h` に移動（`microkvm.c` と `snapshot.c` で共有）

## 出力

```
/ # export FOO=bar
/ #
[monitor] starting live migration...

=== Live migration simulator ===
Iteration 0: full RAM copy 32768 pages
Iteration 1: 31 dirty pages
Stop-and-copy: 33 dirty pages
Downtime: 0.4 ms
Migration complete: migration.bin
================================

$ ./microkvm --restore-migration migration.bin
[migration] restoring from migration.bin
[migration] base RAM loaded (128 MB)
[migration] iteration 1: applied 31 dirty pages
[migration] final: applied 33 dirty pages
[migration] restore complete
Starting guest...

/ # echo $FOO
bar
```

数値の読み方:
- **Iteration 0**: 128MB 全体コピー (32768 pages × 4KB)
- **Iteration 1**: 100ms 間に 31 pages だけ変更（idle guest）
- **31 < 50 (threshold)**: 1 iteration で収束達成
- **Stop-and-copy**: 最終 33 dirty pages + CPU state
- **Downtime 0.4ms**: guest が実際に停止した時間（state dump のみ）

収束の可視化:
```
転送ページ数:  32768 → 31 → 33 → 完了
               ─────    ──    ──
               full     Δ1    final   (ここで guest 停止)
```

## 重要な知見

Live migration は単一操作ではない — 収束ループ。各 iteration で dirty set が縮小するのは、guest が一定時間内に dirty にできるページ数に上限があるから。idle guest では収束がほぼ即座（100ms で 31 pages）。write-heavy workload ではより多くの iteration が必要で downtime が増える。根本的なトレードオフ: pre-copy 時間（総 migration 所要時間）vs downtime（guest が感じる停止時間）。

### Simulator の制約

これは single host 上の file-based simulator。migration file が完全に書かれる（source プロセスが終了する）まで destination は restore できない。production では source と destination がネットワーク越しに通信し、destination は "migration complete" シグナルを受けてから起動する。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| Iterative pre-copy | Full copy → dirty iterations → stop-and-copy |
| Dirty convergence | 32768 → 31 → 33 pages（idle guest は 1 iteration で収束） |
| Downtime 計測 | stop-and-copy のみ now_ns() で計測 = 0.4ms |
| Two-phase 設計 | precopy (VM live) + stop-and-copy (VM stopped) |
| save_cpu_state 再利用 | Step 21 のヘルパーを stop-and-copy で使用 (DRY) |
| Full copy 前の dirty log リセット | iteration 1 が *新しい* write のみを見ることを保証 |
| File format: base + overlays | destination がレイヤーを順に適用して正しさを保証 |

## 変わったこと

Step 21 からの変更:
- **snapshot.h**: migration 構造体 (`migrate_header`, `migrate_context`)、定数、関数宣言
- **snapshot.c**: `migrate_write_dirty()`, `migrate_read_dirty()`, `migrate_precopy()`, `migrate_stop_and_copy()`, `migrate_restore()`
- **microkvm.c**: `Ctrl-A m` ハンドラ、`--restore-migration`、`g_migrate_ctx`/`g_migrate_active`、join 後の条件分岐
- **microkvm.h**: `now_ns()` をここに移動（共有ユーティリティ）
- **Makefile**: clean に `migration.bin` 追加

## 次のステップ

Phase D 完了。メモリ状態管理の全体像:

```
Step 19: 観察 (KVM MMU stats)
Step 20: 追跡 (dirty page logging)
Step 21: 保存 (VM snapshot)
Step 22: 移動 (live migration)
```

各ステップが前のステップの上に構築され、最終的に動作する pre-copy live migration simulator に到達する。
