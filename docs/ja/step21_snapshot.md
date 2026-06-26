# Step 21: VM snapshot — VM 状態の保存と復元

## 目的

`Ctrl-A s` で VM の全状態（CPU レジスタ、デバイス状態、guest RAM）をファイルに保存し、`./microkvm --restore snapshot.bin` で復元して保存した瞬間から実行を再開する。

## 背景

### VM snapshot とは何か

VM snapshot は「実行中の仮想マシンのチェックポイント」。ある瞬間の全状態（CPU レジスタ、メモリ内容、デバイス状態）をファイルに凍結保存し、後から同じ状態に復元できる仕組み。

物理マシンのハイバネーションに似ているが、仮想化ならではの強力さがある:
- 物理マシン: ディスクに書いて電源オフ → *同じ*マシンで復帰
- 仮想マシン: ファイルに書く → 同じ or *別の*ホストで復帰（= live migration の基盤）

Snapshot が保存するのは**実行状態**であり、メモリ内容だけではない。RAM だけでは CPU がどこを実行中か (RIP)、どのモードか (CR0/CR4/EFER)、どの割り込みが pending かが分からず、復元しても意味がない。

ユースケース: デバッグ（クラッシュ再現）、テスト環境の高速起動（boot スキップ）、live migration (Step 22)、ロールバック（変更の取り消し）。

### 何を保存する必要があるか

実行中 VM の状態は複数の層にまたがる:

| 層 | 内容 | KVM ioctl |
|----|------|-----------|
| CPU architectural | RAX-R15, RIP, RFLAGS, セグメント, CR0-CR4, EFER | KVM_GET_REGS, KVM_GET_SREGS |
| FPU/SSE/AVX | x87 + SSE + AVX レジスタ（kernel が内部的に使用） | KVM_GET_FPU |
| XCRs | XCR0 — どの XSAVE 機能が有効か | KVM_GET_XCRS |
| LAPIC | Local APIC レジスタ + timer 状態 | KVM_GET_LAPIC |
| Pending events | pending 例外/割り込み、NMI 状態 | KVM_GET_VCPU_EVENTS |
| PIT | 8254 timer チャネル（system tick） | KVM_GET_PIT2 |
| IRQ chips | PIC master + PIC slave + IOAPIC | KVM_GET_IRQCHIP (×3) |
| Clock | kvmclock offset（guest の時間基準） | KVM_GET_CLOCK |
| MSRs | TSC, APICBASE, syscall entry, kvmclock | KVM_GET_MSRS |
| デバイス | UART レジスタ、virtio 状態 | 構造体を直接コピー |
| メモリ | Guest RAM (128MB) | mmap 領域を直接 write |

### なぜ restore の順序が重要か

KVM の `SET` ioctl には副作用がある:

| ioctl | 副作用 |
|-------|--------|
| KVM_CREATE_PIT2 | PIT timer を即座に arm（カウント開始） |
| KVM_SET_MSRS (kvmclock) | KVM が guest RAM の shared clock page にアクセス |
| KVM_SET_LAPIC | LAPIC timer を re-arm（deadline が過去なら即発火） |
| KVM_SET_SREGS | KVM が CR3 経由で page table を検証 |
| KVM_SET_XCRS | CR4.OSXSAVE と整合しないと VMCS invalid |

順序を間違えると timer storm（nested interrupt による overflow）、triple fault、VMCS invalid state が発生する。正しい restore 順序:

```
RAM を先にロード（guest メモリにアクセスする ioctl の前に）
  → PIT → Clock → IRQchip → XCRs → SREGS → MSRs → LAPIC → Events → FPU → REGS
```

PIT が最初なのは `KVM_CREATE_PIT2`（VM セットアップ中に呼ぶ）がデフォルト timer を arm するため — 発火する前に保存した state で即上書きする必要がある。

## 実行フロー

```
Save (Ctrl-A s):
─────────────────────────────────────────────────────────────────
stdin_thread          vCPU thread              main
────────────          ───────────              ────
Ctrl-A s 検出
  stop_requested=1
                      次の VM exit
                      stop_requested チェック
                      → break → return
                                               pthread_join()
                                               snap_save()
                                                 → header
                                                 → save_cpu_state()
                                                 → write(RAM, 128MB)
                                               exit

Restore (--restore snapshot.bin):
─────────────────────────────────────────────────────────────────
main
────
--restore をパース
VM 作成, irqchip, PIT (通常セットアップ)
メモリ割り当て、スロット登録
load_bzimage スキップ、レジスタ初期化スキップ
snap_restore()
  → header 検証
  → ファイルから state を読む
  → ファイルから RAM を読む
  → apply: PIT → clock → irqchip → XCRs → SREGS → MSRs → LAPIC → events → FPU → REGS
  → virtio デバイス状態を復元
KVM_RUN → guest が保存した RIP から再開
```

## 実装

### snapshot.h — 定義

```c
#define SNAP_MAGIC   0x4D4B564D   /* "MKVM" */
#define SNAP_VERSION 1
#define SNAP_NUM_MSRS 12

struct snap_header {
    uint32_t magic;
    uint32_t version;
    uint64_t mem_size;
};

/* ホストポインタを除外した virtio 状態 (ram, ram_size は含めない) */
struct virtio_snap { ... };
```

### ファイルレイアウト

```
snapshot.bin
┌────────────────┐
│ Header         │  16 bytes (magic, version, mem_size)
├────────────────┤
│ CPU state      │  ~2.3 KB (regs, sregs, fpu, lapic, xcrs, events)
├────────────────┤
│ Platform state │  ~1.9 KB (pit, irqchip×3, clock, msrs)
├────────────────┤
│ Device state   │  ~72 bytes (uart, virtio_snap)
├────────────────┤
│ Guest RAM      │  128 MB (ファイルサイズの 99.99%)
└────────────────┘
```

### Save と Restore の順序対比

```
Save (sequential write):        Restore (全部読んでから apply):
  Header                          Read: Header → CPU → Platform
  CPU state                              → Device → RAM
  Platform state                  Apply: PIT → Clock → IRQchip
  Device state                           → XCRs → SREGS → MSRs
  RAM                                    → LAPIC → Events → FPU → REGS
                                  (RAM は apply の前にロード — MSRs が guest メモリにアクセスするため)
```

### snapshot.c — save_cpu_state (共通ヘルパー)

```c
static void save_cpu_state(int fd, int vcpufd, int vmfd,
    struct uart8250 *uart, struct virtio_mmio_dev *virtio)
{
    /* vCPU: regs, sregs, fpu, lapic, xcrs, events */
    /* VM-wide: pit, irqchip×3, clock */
    /* MSRs: 12 entries (TSC, APICBASE, SYSENTER, STAR/LSTAR/CSTAR/FMASK,
             KERNEL_GS_BASE, KVM_WALL_CLOCK_NEW, KVM_SYSTEM_TIME_NEW) */
    /* Devices: uart 構造体, virtio_snap 構造体 */
}
```

このヘルパーは Step 22 の `migrate_stop_and_copy()` とも共有する。

### snapshot.c — snap_save

```c
int snap_save(const char *path, ...) {
    write(fd, &hdr, sizeof(hdr));       /* header */
    save_cpu_state(fd, vcpufd, vmfd, uart, virtio);  /* CPU + デバイス */
    write(fd, mem, mem_size);           /* 128MB RAM */
}
```

### snapshot.c — snap_restore

```c
int snap_restore(const char *path, ...) {
    /* 1. ファイルから全 state を読む (save 順) */
    /* 2. RAM を読む */
    /* 3. 正しい順序で KVM に apply:
         PIT → Clock → IRQchip → XCRs → SREGS → MSRs → LAPIC → Events → FPU → REGS */
    /* 4. virtio デバイス状態を復元 (ホストポインタはファイルに含まない) */
}
```

### microkvm.c の変更

- `--restore` 引数パース
- `if (!restore_path)` で bzImage ロードとレジスタ初期化をガード
- `Ctrl-A s` ハンドラ: `stop_requested = 1` を設定
- vCPU join 後: `snap_save("snapshot.bin", ...)`
- KVM_RUN 前: restore 時は `snap_restore(restore_path, ...)`
- `KVM_EXIT_FAIL_ENTRY` case 追加（restore デバッグ用）
- PIT 作成時に `KVM_PIT_SPEAKER_DUMMY` フラグ

## 出力

```
/ # export FOO=bar
/ # echo $FOO
bar
/ #
[monitor] saving snapshot...
[snapshot] saved to snapshot.bin

$ ./microkvm --restore snapshot.bin
[snapshot] restored from snapshot.bin
Starting guest...

/ # echo $FOO
bar
```

シェル変数 `FOO` が save/restore サイクルを生き残っている — guest の全状態（CPU + メモリ + デバイス）が正しく保存・復元された証拠。

## 重要な知見

VM snapshot は「レジスタをファイルにダンプする」だけではない。restore の*順序*が決定的に重要 — KVM の ioctl は副作用としてタイマーを発火させ、guest メモリにアクセスし、CPU 状態を相互に検証する。核心的な発見: `KVM_CREATE_PIT2` がデフォルト timer を arm し、それが restore した LAPIC state に発火すると timer interrupt storm で stack overflow を引き起こす。`KVM_SET_PIT2` で即座に上書きする必要がある。これが教えるのは、ハイパーバイザの state は深く相互接続されており、フラットなレジスタダンプではないということ。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| VM state の完全性 | CPU + platform + デバイス + RAM — どれか1つ欠けても失敗 |
| Restore 順序の依存関係 | PIT が LAPIC より前、XCRs が SREGS より前、RAM が MSRs より前 |
| ホストポインタの除外 | virtio_snap は `uint8_t *ram` を含まない — プロセス固有ポインタはシリアライズ不能 |
| save_cpu_state ヘルパー | snapshot と migration で共有（save は DRY、restore は明示的） |
| KVM_PIT_SPEAKER_DUMMY | boot calibration 中の PIT channel 2 speaker エミュレーション問題を防ぐ |
| KVM_EXIT_FAIL_ENTRY | restore 開発中の VMCS invalid state のデバッグ補助 |

## 変わったこと

Step 20 からの変更:
- **新規ファイル**: `snapshot.h`（構造体、MSR defines、API）、`snapshot.c`（save_cpu_state, snap_save, snap_restore）
- **microkvm.c**: `--restore` 引数、`Ctrl-A s`、`KVM_EXIT_FAIL_ENTRY`、`KVM_PIT_SPEAKER_DUMMY`、boot/regs を `if (!restore_path)` でガード
- **Makefile**: `snapshot.c` 追加、clean に `snapshot.bin` 追加

## 次のステップ

[Step 22: Live migration](step22_live-migration.md) では dirty page tracking (Step 20) と snapshot (Step 21) を組み合わせて iterative pre-copy migration を実装する — guest が走り続けたまま VM 状態を転送する。

Snapshot は**全てを1回コピー**する。Live migration は**全てを1回コピーし、その後は変わった部分だけ**を送る。
