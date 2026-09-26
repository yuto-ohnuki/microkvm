# Step 23: Live migration (socket) — TCP streaming と source/destination モデル

## 目的

Step 22 の file-based migration を **socket-based** に拡張し、source と destination が**同時に動作**する live migration を実現する。migration の transport を「seek 可能なファイル」から「TCP の順序付きバイトストリーム」に変え、両者の本質的な違いを protocol 設計で吸収する。

## 背景

### Step 22 の何が足りないか

Step 22 は動作する live migration だったが、**file-based simulator** だった:

```
Source:      ./microkvm            → migration.bin に書く → プロセス終了
Destination: ./microkvm --restore-migration migration.bin   （source 終了後）
```

source が migration file を書き終える（=プロセスが終了する）まで destination は起動できない。これは real live migration ではない — 本来は source と destination が**ネットワーク越しに同時に生き**、pre-copy 中も source guest は走り続け、stop-and-copy の一瞬だけ止まって destination が引き継ぐ。

このステップで transport をファイルから TCP socket に変え、その「同時実行モデル」を実現する。

### 核心の問い: なぜ file format をそのまま TCP で送れないのか

これがこのステップの学習テーマそのもの。答えは transport の性質の違いにある:

| | File (Step 22) | TCP socket (Step 23) |
|---|---|---|
| I/O 完了 | 部分完了する場合がある | **partial I/O** — 1 回で一部しか転送されない |
| seek | `lseek()` で任意位置に戻れる | **seek 不可** — 一方向の順序ストリーム |
| メタデータ後追い | 末尾で header に `lseek` して書き戻せる | **書いたバイトは取り消せない** |

Step 22 は、最後に `lseek(0)` で header の `num_iterations` を書き戻し、destination がその回数だけ dirty block を読む方式だった。TCP では先頭へ戻れないため、この終了判定を変更する。部分読み書きには、既存の `writen()` / `readn()` で対応する。

### 解決策 1: phase marker（seek 依存の除去）

「後で header に回数を書き戻す」代わりに、**各 dirty block の直前に 1 byte の phase marker を前置**する:

```
[0x01] [dirty block]   ← MIG_PHASE_DIRTY: まだ iteration が続く
[0x01] [dirty block]
[0x02] [dirty block]   ← MIG_PHASE_FINAL: これが最後（この後 CPU state）
[cpu/device state]
```

destination はマーカーを見て「次が iterative dirty か、final か」をその場で判断できる。**事前に回数を知る必要がない** = seek 不要。final の後に CPU/device state が続くことが marker で確定する。

これは「ストリームは自己記述的（self-describing）でなければならない」という stream protocol の基本原則。長さやフレーム境界を後追いのメタデータに頼れないので、データ自身に境界を埋め込む。

### 解決策 2: writen / readn（partial I/O の吸収）

socket の `write()`/`read()` は要求バイト数の一部しか処理しないことがある（カーネルの socket buffer の空き次第）。全量が完了するまでループする既存の helper を利用する:

```c
static ssize_t writen(int fd, const void *buf, size_t n);  /* n バイト全部書くまでループ */
static ssize_t readn(int fd, void *buf, size_t n);         /* n バイト全部読むまでループ */
```

ファイルでも部分読み書きは起こりうるため、同じ helper を使う。これらは Step 21 で導入され、Step 22 の migration でも使用している。

### transport 抽象化: path → fd

既存の migration コアロジックからファイルの open を呼び出し側へ移し、**関数の引数を `const char *path` から `int fd` に変える**。phase marker と seek 可否への対応を組み合わせ、file と socket で同じ転送処理を使う:

```c
/* Before (Step 22): 関数内で open する */
int migrate_precopy(const char *path, ...);

/* After (Step 23): fd を受け取る（呼び出し側が file/socket を決める） */
int migrate_precopy(int fd, ...);
```

呼び出し側で fd を用意する:
- file: `open("migration.bin", ...)`
- socket source: `connect_to("tcp:host:port")`
- socket dest: `listen_on("tcp:port")` → `accept()`

migration の**アルゴリズムは transport を知らない**。これは Unix の "everything is a file descriptor" 抽象がそのまま効く好例。

## 実行フロー

```
Destination (先に起動: ./microkvm --incoming tcp:4444)      Source (./microkvm → Ctrl-A t)
──────────────────────────────────────────────────         ──────────────────────────────
main:                                                        stdin_thread          vCPU thread
  listen_on("tcp:4444")                                      ────────────          ───────────
    socket / bind / listen
    accept() でブロック  ←──────────────────────────────┐
                                                      │    Ctrl-A t 検出
                                                      └──  connect_to("tcp:127.0.0.1:4444")
  migrate_restore(fd, ...):                                  migrate_precopy(fd, ...):
    readn header                ←──────[migrate_header]───    writen header
    readn 128MB base RAM        ←──────[full RAM]─────────    writen full RAM (128MB)  [guest 稼働]
    for (;;):                                                 100ms 待機               [guest が dirty に]
      readn phase marker        ←──────[0x01][dirty]──────    writen 0x01 + dirty pages
      readn dirty block                                       （収束するまで繰り返す）
                                                             stop_requested = 1
                                                                                    次の VM exit → break
                                                             main (join 後):
                                                             migrate_stop_and_copy():
      phase==FINAL → break      ←──────[0x02][dirty]──────      writen 0x02 + final dirty
      readn CPU/device state    ←──────[cpu state]────────      writen CPU/device state
    state 適用
  KVM_RUN → guest 再開
                                                                  close(fd) → プロセス終了
```

ポイント: destination の `accept()` が source の `connect()` を待ち、以降は同じ TCP コネクション上で header → RAM → dirty blocks → CPU state が**ストリームとして**流れる。source guest は pre-copy 中ずっと生きていて、止まるのは stop-and-copy の一瞬だけ。

## 実装

### snapshot.h — protocol version と phase marker

```c
#define MIG_MAGIC   0x4D4B4D47  /* "MKMG" */
#define MIG_VERSION 2           /* Step 22 は 1 — stream format 変更のため bump */

/* dirty block の直前に置く 1 byte のフェーズマーカー */
#define MIG_PHASE_DIRTY     0x01    /* まだ iteration が続く */
#define MIG_PHASE_FINAL     0x02    /* 最後の dirty（この後 CPU state） */
```

`MIG_VERSION` を 1 → 2 に上げる。stream format が変わったので、古い migration.bin（version 1）を誤って読まないよう header の version check で弾く。

関数シグネチャを path から fd に変更:

```c
int migrate_precopy(int fd, int vmfd, void *mem, size_t mem_size,
    struct migrate_context *ctx);
int migrate_restore(int fd, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio,
    void *mem, size_t mem_size);
```

### snapshot.c — writen / readn helper

```c
/* 部分書き込みを繰り返し、EINTR は再試行する。
 * write が 0 を返して進まない場合もエラーにする。*/
static ssize_t writen(int fd, const void *buf, size_t n)
{
    size_t left = n;
    const char *p = buf;
    while (left > 0) {
        ssize_t nw = write(fd, p, left);
        if (nw < 0) {
            if (errno == EINTR) continue;   /* シグナルで中断 → 再試行 */
            return -1;
        }
        if (nw == 0) return -1;             /* 書き込みが進まない */
        p += nw; left -= nw;
    }
    return (ssize_t)n;
}
/* readn も対称的に実装 */
```

`readn()` は必要量を受け取る前の EOF をエラーにする。socket への送信では `SIGPIPE` を無視し、切断を `write()` のエラーとして受け取れるようにする:

```c
signal(SIGPIPE, SIG_IGN);
```

既存の戻り値チェックで失敗を呼び出し側へ伝え、pre-copy 失敗時には VM を続行する。

### snapshot.c — phase marker の送受信

送信側（source）は各 dirty iteration の前にマーカーを書く:

```c
/* migrate_precopy: iteration ループ内 */
uint8_t phase = MIG_PHASE_DIRTY;
writen(fd, &phase, sizeof(phase));
migrate_write_dirty(fd, vmfd, mem, mem_size, &dirty_count);

/* migrate_stop_and_copy: 最終ブロックの前 */
uint8_t phase = MIG_PHASE_FINAL;
writen(fd, &phase, sizeof(phase));
migrate_write_dirty(fd, vmfd, mem, mem_size, &final_dirty);
```

受信側（destination）は Step 22 の「`num_iterations` 回ループ」を**マーカー駆動ループ**に置き換える:

```c
/* migrate_restore: Step 22 は for(i < hdr.num_iterations) だった */
for (;;) {
    uint8_t phase;
    if (readn(fd, &phase, sizeof(phase)) != sizeof(phase))
        goto fail;
    if (phase != MIG_PHASE_DIRTY && phase != MIG_PHASE_FINAL) {
        fprintf(stderr, "[migration] invalid phase: %#x\n", phase);
        goto fail;   /* 壊れた stream を検出 */
    }
    int dirty = migrate_read_dirty(fd, mem, mem_size);
    if (dirty < 0) goto fail;
    if (phase == MIG_PHASE_FINAL) break;   /* final の後は CPU state */
}
```

### snapshot.c — lseek fallback（file と socket の共用）

`migrate_stop_and_copy` の header 書き戻しは、socket では失敗するので**成功時のみ**実行する:

```c
if (lseek(fd, 0, SEEK_SET) >= 0) {   /* file: 成功 → header 更新 */
    struct migrate_header hdr = { ... };
    writen(fd, &hdr, sizeof(hdr));
}
/* socket: lseek が -1 → skip（phase marker があるので header の num_iterations は不要） */
```

destination は file / socket のどちらでも phase marker で終端を判断し、`num_iterations` は使わない。file 版では反復数を header に書き戻すが、形式は version 2 となる。

### snapshot.c — bounds check（stream の防御的検証）

Step 22 の `migrate_read_dirty` にある範囲チェックを、socket 受信でも利用する:

```c
if (dirty_count > mem_size / 4096) { /* 異常な count → 中断 */ return -1; }
...
if (page_idx >= mem_size / 4096) {   /* 範囲外 page index → 中断 */ return -1; }
```

既存の dirty_count / page_idx / MSR 数の検証に加え、このステップでは不正な phase marker も検出する。

### microkvm.c — connect_to / listen_on

source 側（TCP client）:

```c
/* spec: "tcp:<ipv4>:<port>" または "tcp:<port>"（省略時 127.0.0.1） */
static int connect_to(const char *spec)
{
    /* "tcp:" を skip、strrchr(':') で host/port を分離、strtol で port を範囲検証 */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  /* 後述 */
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { /* 不正アドレス検出 */ }
    if (connect(sock, ...) < 0) { perror("connect"); ... }
    return sock;
}
```

destination 側（TCP server）:

```c
/* spec: "tcp:<port>" */
static int listen_on(const char *spec)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, ...);   /* 再起動時に "Address already in use" 回避 */
    bind(srv, ...); listen(srv, 1);
    fprintf(stderr, "[migration] waiting for connection on port %ld...\n", port);
    int sock = accept(srv, NULL, NULL);     /* source の connect をブロックして待つ */
    close(srv);                             /* listening socket は不要になる */
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, ...);
    return sock;
}
```

### microkvm.c — キー割り当てと --incoming

file 版と socket 版を**別キー**で共存させる:

- `Ctrl-A m` = **file-based**（`migration.bin` に書く。形式は version 2）
- `Ctrl-A t` = **socket-based**（destination に connect して送信。t は tcp / transport の意）

```c
/* stdin_thread: socket 送信 */
if (c == 't') {
    fprintf(stderr, "\n[monitor] starting live migration (socket)...\n");
    int mig_fd = connect_to("tcp:127.0.0.1:4444");
    if (mig_fd < 0) {
        fprintf(stderr, "[monitor] migration aborted, VM continues\n");
    } else if (migrate_precopy(mig_fd, g_vmfd, virtio_dev.ram,
        GUEST_MEM_SIZE, &g_migrate_ctx) == 0) {
        /* Stop the vCPU for the final transfer */
        g_migrate_active = 1;
        stop_requested = 1;
    } else {
        fprintf(stderr, "[monitor] migration aborted, VM continues\n");
    }
    continue;
}
```

destination の起動フラグ:

```c
/* main: 引数検証で --incoming tcp:PORT を受け取る */
char *incoming_spec = NULL;
/* --restore / --restore-migration と共通で argc == 3 を確認し、
 * --incoming の場合に incoming_spec = argv[2] を設定 */

/* incoming の場合は bzImage を読まない（RAM は migration で送られてくる） */
if (!restore_path && !migrate_restore_path && !incoming_spec)
    load_bzimage("bzImage", mem, CMDLINE);

/* Accept incoming migration via socket */
if (incoming_spec) {
    int mig_fd = listen_on(incoming_spec);
    if (mig_fd < 0) return 1;
    if (migrate_restore(mig_fd, vcpus[0].fd, vmfd, &uart, &virtio_dev,
        mem, GUEST_MEM_SIZE) < 0)
        return 1;
}
```

## 出力

```
=== Destination (Terminal 1) — 先に起動 ===
$ ./microkvm --incoming tcp:4444
[migration] waiting for connection on port 4444...
        （source が Ctrl-A t を押すまでここでブロック）
[migration] receiving state...
[migration] base RAM loaded (128 MB)
[migration] phase=iter: applied 31 dirty pages
[migration] phase=final: applied 46 dirty pages
[migration] restore complete
Starting guest...
Starting guest [ioeventfd=OFF, irqfd=OFF]...

/ # echo $A
123

=== Source (Terminal 2) ===
$ ./microkvm
/ # export A=123
/ # echo $A
123
/ # < Ctrl-A t >
[monitor] starting live migration (socket)...

=== Live migration simulator ===
Iteration 0: full RAM copy 32768 pages
Iteration 1: 31 dirty pages
Stop-and-copy: 46 dirty pages
Downtime: 0.4 ms
Migration complete: migration.bin
================================
        （execution report を表示してプロセス終了。レポートは省略）
```

読み方:

- `Migration complete: migration.bin` は共通処理の固定メッセージ。`Ctrl-A t` ではファイルではなく TCP socket に送信している
- destination の `accept()` が source の `connect()`（Ctrl-A t）を待ってブロックしている
- `phase=iter` / `phase=final` = destination が **phase marker を読んで**ブロック種別を判定している証拠（Step 22 の「iteration N 回」ログとの違い）
- destination の `echo $A` が `123` を表示し、転送前のシェル変数が保持されている
- Downtime 0.4ms = source の最終 marker・dirty pages・CPU/device state の送信処理時間。destination での復元完了までの時間は含まない
- source 側の `Iteration 1: 31` と destination 側の `phase=iter: 31` が一致 = 同じ dirty block が stream を通って届いている

## 重要な知見

TCP は seek できないため、各 dirty block に phase marker を付け、その場で反復の継続と終了を判断する。部分読み書きは既存の `writen()` / `readn()` が吸収し、migration 処理は呼び出し側から受け取った fd を使う。この分担によって pre-copy と stop-and-copy を file / socket で共用できる。

### スコープ外のトピック

microkvm は educational VMM。以下は production migration の領域なのであえて入れない:
- 帯域制限（`--bandwidth`）、timeout / reconnect / 接続断リカバリ
- TLS / 暗号化、compatibility negotiation
- `Ctrl-A t` の接続先は現状 `tcp:127.0.0.1:4444` にハードコード（同一ホスト localhost 実験用）

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| Transport 抽象化 | `path` → `fd`。migration アルゴリズムは file/socket を区別しない |
| Partial I/O | socket の `write`/`read` は部分完了しうる → `writen`/`readn` でループ |
| Self-describing stream | phase marker (0x01/0x02) で境界と終端を stream 内に埋め込む |
| seekable vs stream | file は末尾 `lseek` で header 書き戻し可、socket は不可 → 設計が変わる |
| source/destination model | 両者が同時に生き、TCP コネクション上で state を stream |
| `--incoming` / `accept` | destination が先に listen、source の connect を待つ |
| TCP_NODELAY | 小さなメタデータ（marker, count）を Nagle 遅延させない |
| SO_REUSEADDR | 再起動時の "Address already in use" 回避 |
| Protocol versioning | stream format 変更で `MIG_VERSION` を bump（1 → 2） |
| Stream の防御的検証 | dirty_count / page_idx の bounds check、不正 phase 検出 |

## 変わったこと

Step 22 からの変更:
- **snapshot.h**: `MIG_VERSION` 1 → 2、`MIG_PHASE_DIRTY`/`MIG_PHASE_FINAL` 追加、`migrate_precopy`/`migrate_restore` のシグネチャを `path` → `fd` に変更
- **snapshot.c**: path → fd 化、phase marker 送受信、マーカー駆動 restore ループ、seek 可能な場合のみ header 更新
- **microkvm.c**: `connect_to()`/`listen_on()` 追加、`--incoming tcp:PORT` パース、`Ctrl-A t`（socket）を `Ctrl-A m`（file）と別に追加、`incoming_spec` 分岐（bzImage / initramfs skip + `migrate_restore`）、SIGPIPE 無視と接続先・引数の検証
- file-based migration の操作も残し、file / socket の両方で version 2 の形式を使用

## 次のステップ

Phase D（メモリ状態管理）完了。全体像:

```
Step 19: 観察 (KVM MMU stats)
Step 20: 追跡 (dirty page logging)
Step 21: 保存 (VM snapshot)
Step 22: 移動 — file transport (migration algorithm)
Step 23: 移動 — socket transport (streaming protocol)
```

Step 22 で migration の**アルゴリズム**を、Step 23 で migration の**transport / protocol** を学んだ。次は [Step 24: PCI config space](step24_pci-config.md) — x86 標準のデバイス発見メカニズム PCI を実装し、Linux がバスを走査してカスタムデバイスを発見できるようにする。
