# Step 22: Live migration — iterative pre-copy

## 目的

ファイルを使って、live migration の pre-copy と復元を検証する。dirty page tracking (Step 20) と state save/restore (Step 21) を組み合わせて iterative pre-copy を実装: guest が走り続けたまま RAM をコピーし、最後に短時間停止して残りの dirty pages と CPU state を転送する。

## 背景

### Live migration とは何か

Live migration は、VM の停止時間を短く抑えて別のホストへ移動する技術。pre-copy 中は guest を動かし、最終状態の転送時に停止して、移動先で再開する。

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
4. dirty set が50ページ以下になるか、最大5回まで繰り返す
5. vCPU 停止 → 最終 dirty pages + CPU state をコピー (stop-and-copy)
6. migration file から destination VM を起動
```

コピー対象は、前回の bitmap 取得以降に書き込まれたページ。書き込み量によって増減し、必ず減少するわけではない。今回の実行では1回目で閾値以下になった。

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
    stop-and-copy の保存時間を計測
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
    if (migrate_write_dirty(fd, vmfd, mem, mem_size, &final_dirty) < 0) {
        close(fd);
        return -1;
    }

    /* Step 21 の save_cpu_state() を再利用 */
    if (save_cpu_state(fd, vcpufd, vmfd, uart, virtio) < 0) {
        close(fd);
        return -1;
    }

    uint64_t t2 = now_ns();
    /* downtime = t2 - t1 (stop-and-copy phase のみ) */

    /* header を最終 iteration count で更新 */
    lseek(fd, 0, SEEK_SET);
    if (writen(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        close(fd);
        return -1;
    }
    /* close と結果表示は省略 */
}
```

### microkvm.c の変更

- `Ctrl-A m`: stdin_thread から `migrate_precopy()` を呼び、成功時に `g_migrate_active = 1` と `stop_requested = 1` を設定。失敗時は VM を続行
- `--restore-migration`: 引数をパースし、KVM_RUN 前に `migrate_restore()` を呼ぶ
- vCPU join 後: `g_migrate_active` の場合に `migrate_stop_and_copy()` を実行し、失敗を終了コードに反映
- `snapshot.c` から既存の `microkvm.h` の `now_ns()` を利用

## 出力

migration 完了後に source は終了する。以下は migration と復元のログの抜粋で、終了時の execution report は省略している。

```
/ # export FOO=bar
/ # echo $FOO
bar
/ # < Ctrl-A m >
[monitor] starting live migration (file)...

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
Starting guest [ioeventfd=OFF, irqfd=OFF]...

/ # echo $FOO
bar
```

数値の読み方:
- **Iteration 0**: 128MB 全体コピー (32768 pages × 4KB)
- **Iteration 1**: 初回の dirty log リセット以降、全 RAM コピーと100ms待機中などに書き込まれた31ページ
- **31 ≤ 50 (threshold)**: 1 iteration で停止条件を満たした
- **Stop-and-copy**: 前回の bitmap 取得以降の最終33ページと CPU／デバイス状態。別の計測区間なので31ページより多くても矛盾しない
- **Downtime 0.4ms**: vCPU join 後の最終 dirty page 取得・書き込みと CPU／デバイス状態の保存時間。停止待ち、header 更新、close、destination の起動・復元時間は含まない

復元後の `echo $FOO` が `bar` を表示し、シェル変数が保持されていることを確認できる。

コピー量の可視化:
```
転送ページ数:  32768 → 31 → 33 → 完了
               ─────    ──    ──
               full     Δ1    final   (final の前に guest 停止)
```

## 重要な知見

Pre-copy では全 RAM を先にコピーし、コピー中の更新を dirty bitmap で追跡して差分を重ねる。最後に vCPU を停止して残りの差分と CPU／デバイス状態を保存する。反復を増やしても dirty set が小さくなるとは限らず、コピー時間と最終停止中に残る処理量のバランスが重要になる。

### Simulator の制約

これは single host 上の file-based simulator。migration file が完全に書かれる（source プロセスが終了する）まで destination は restore できない。Step 23 で取り扱う socket-based な live migration では source と destination がネットワーク越しに通信し、destination は "migration complete" シグナルを受けてから起動する。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| Iterative pre-copy | Full copy → dirty iterations → stop-and-copy |
| Pre-copy の停止条件 | 今回は31ページで閾値以下となり、最終33ページを停止後にコピー |
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
- **Makefile**: clean に `migration.bin` 追加

## 次のステップ

ここまでで pre-copy live migration の**アルゴリズム**が動いた。ただしこれは single host 上の file-based simulator で、source がファイルを書き終える（プロセス終了）まで destination は起動できない。

[Step 23: Live migration (socket)](step23_live-migration-socket.md) では transport をファイルから TCP socket に変え、source と destination が**同時に生きる**本来の live migration を実現する。migration アルゴリズムは変えず、transport が seekable file から non-seekable stream に変わると protocol 設計がどう変わるかを学ぶ。

Phase D（メモリ状態管理）の全体像:

```
Step 19: 観察 (KVM MMU stats)
Step 20: 追跡 (dirty page logging)
Step 21: 保存 (VM snapshot)
Step 22: 移動 — file transport (migration algorithm)
Step 23: 移動 — socket transport (streaming protocol)
```
