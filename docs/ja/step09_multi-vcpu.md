# Step 9: 複数 vCPU

## 目的

**pthread** を使って2つの vCPU を並列実行し、共有デバイス状態と同期の必要性を導入する。
実行順序が非決定的になる最初のステップ。

## 背景

### 1 vCPU = 1 スレッド

`KVM_RUN` はゲストが exit するまで呼び出しスレッドをブロックする。複数 vCPU を並行実行するには、各 vCPU に独自のスレッドが必要 — QEMU が vCPU 実行を構成する方法と同じ:

```
main thread
  ├── pthread_create → vCPU 0 thread: for(;;) { KVM_RUN; handle exit; }
  ├── pthread_create → vCPU 1 thread: for(;;) { KVM_RUN; handle exit; }
  ├── pthread_join(vCPU 0)
  └── pthread_join(vCPU 1)
```

### 共有状態は同期が必要

両方の vCPU スレッドが同じデバイス状態 (`device_counter`、`msr_store`、`stdout`) にアクセスする。同期なしでは並行アクセスがデータ競合を引き起こす:

```
vCPU 0: device_counter を読む (=0)
vCPU 1: device_counter を読む (=0)    ← まだ書き戻されていない
vCPU 0: device_counter を書く (=1)
vCPU 1: device_counter を書く (=1)   ← 2 であるべきが 1 (更新消失)
```

mutex が共有状態へのアクセスを直列化する。概念的に QEMU の BQL (Big QEMU Lock) と類似 — どちらも並行 vCPU スレッドからの共有デバイス状態へのアクセスを直列化する。

### BSP と AP の初期化

実マルチプロセッサハードウェアでは:
- **BSP** (Bootstrap Processor) が最初にブートしモード遷移を実行
- **AP** (Application Processor) は BSP が準備済みの環境に後から起動

このパターンを模倣:
- vCPU 0 はリアルモードから開始し全モードを遷移 (Step 3–4)
- vCPU 1 は `KVM_SET_SREGS` で直接ロングモードから開始 — VMM が vCPU 0 がゲストコードで達成したのと同じレジスタを全て設定

実 AP はリセット状態から開始しブートストラップトランポリンを実行 (INIT IPI → SIPI → 16-bit リアルモード → トランポリン → ロングモード) するが、簡略化のため vCPU 1 を直接ロングモードで初期化する。

### 非決定的出力

2スレッドが並行実行するため、出力順序はスケジューリングに依存する。vCPU 1 の出力が vCPU 0 の任意の2行間に現れる可能性がある。これはバグではなく、実際の並行実行を示している。

## 実行フロー

```
main                    vCPU 0 thread              vCPU 1 thread
────                    ─────────────              ─────────────
pthread_create(0)       KVM_RUN
pthread_create(1)       [リアルモード] 'R'         KVM_RUN
                        [プロテクト] 'P'           [ロングモード]
                        [ロングモード]               MMIO read → 0
                          MMIO write 'M'             out '0'
                          MMIO read → 1              hlt → 完了
                          MSR write/read
                          'I' (割り込み)
                        hlt → 完了
pthread_join(0)
pthread_join(1)

出力順序は非決定的 — vCPU 1 が vCPU 0 の前後どちらでカウンタを読むかは不定。
```

## 実装

### VMM: struct vcpu

```c
struct vcpu {
    int fd;
    struct kvm_run *run;
    int id;
    size_t mmap_size;
};
```

各 vCPU は独自の fd、kvm_run ページ、識別子を持つ。kvm_run ページは vCPU ごと — 各スレッドは自身の exit 情報のみを読む。

### VMM: vCPU 作成ループ

```c
for (int i = 0; i < NUM_VCPUS; i++) {
    vcpus[i].fd = ioctl(vmfd, KVM_CREATE_VCPU, i);
    vcpus[i].run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, vcpus[i].fd, 0);
    /* ... レジスタ初期化 ... */
}
```

`KVM_CREATE_VCPU` の第2引数は vCPU インデックス。KVM はこれを APIC ID 割り当てや per-CPU 状態に内部的に使用する。

### VMM: vCPU 1 — 直接ロングモード初期化

```c
/* vCPU 1: 直接ロングモードで開始 */
sregs.cr0 = 0x80000011;          /* PG | PE | ET */
sregs.cr3 = 0x70000;             /* vCPU 0 と同じページテーブル */
sregs.cr4 = 0x20;                /* PAE */
sregs.efer = (1 << 8) | (1 << 10); /* LME | LMA */

sregs.cs.selector = 0x18;        /* GDT[3]: 64-bit コード */
sregs.cs.l = 1;                  /* ロングモード */
sregs.cs.present = 1;
sregs.cs.s = 1;
sregs.cs.type = 11;              /* 実行/読み取り, accessed */

regs.rip = VCPU1_ENTRY;          /* 0x1100 */
regs.rsp = 0x50000;              /* vCPU 0 とは別のスタック */
```

vCPU 0 が Step 3–4 を実行して達成するのと同じ状態を、VMM が直接設定する。並行実行時の破損を避けるため、各 vCPU は独自のスタックを持つ。

### VMM: mutex 保護された exit ハンドラ

```c
static pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;

/* vcpu_thread 内: */
case KVM_EXIT_MMIO:
    pthread_mutex_lock(&dev_lock);
    if (run->mmio.is_write) {
        /* ... write 処理 ... */
    } else {
        run->mmio.data[0] = device_counter++;
    }
    pthread_mutex_unlock(&dev_lock);
    break;
```

共有状態 (`device_counter`、`msr_store`、`printf`) への全アクセスを mutex で包む。更新消失とインターリーブ出力を防止する。

### ゲスト: vCPU 1 エントリポイント

```asm
.org 0x1100
vcpu1_entry:
    .byte 0x48, 0xC7, 0xC3, 0x00, 0x00, 0x0D, 0x00     /* mov rbx, 0xD0000 */
    .byte 0x8A, 0x03                                   /* mov al, [rbx] */
    .byte 0x04, 0x30                                   /* add al, '0' */
    .byte 0xE6, 0x10                                   /* out 0x10, al */
    .byte 0xF4                                         /* hlt */
```

`.org 0x1100` はバイナリ内の固定オフセットにこのコードを配置する。VMM が vCPU 1 の RIP をこのアドレスに設定する。vCPU 1 は vCPU 0 と同じ MMIO デバイスから読み取り、`device_counter` が真に共有であることを示す — 先に読んだ vCPU が 0 を得、もう一方が 1 を得る。

## 出力

```
$ ./microkvm
Loaded guest: 8232 bytes
Starting guest with 2 vCPUs...
[vCPU 0][PIO out port 0x10] R
[vCPU 1][MMIO read] returning 0
[vCPU 1][PIO out port 0x10] 0
[vCPU 1] halted.
[vCPU 0][PIO out port 0x10] P
[vCPU 0][MMIO write] M
[vCPU 0][MMIO read] returning 1
[vCPU 0][PIO out port 0x10] 1
[vCPU 0][MSR write] 0x4b564d00 = 0x42
[vCPU 0][MSR read] 0x4b564d00 -> 0x42
[vCPU 0][PIO out port 0x10] r
[vCPU 0][PIO out port 0x10] I
[vCPU 0] halted.
```

注意: vCPU 1 が先に `device_counter` を読んだ (0 を返した) ので、vCPU 0 の後の read は 1 を返した。vCPU 0 が MMIO read に先に到達していれば、値は逆になる。この非決定性は並行実行に固有のもの。

## 重要な知見

マルチ vCPU VM は根本的にホスト上の**マルチスレッドプログラム**。
各 vCPU はゲストコードの実行 (`KVM_RUN` 内) とユーザー空間での exit 処理を交互に行うホストスレッド。

並行プログラミングの全ての課題が当てはまる:
- 共有可変状態に同期が必要
- 出力順序が非決定的
- 各 vCPU に独自のスタックと per-CPU 状態が必要

MMIO カウンタが具体例を提供: 両 vCPU が同じ仮想デバイスにアクセスする。先に読んだ vCPU が 0 を受け取り、もう一方が 1。mutex なしでは `device_counter` の read-modify-write が競合で破損する可能性。

QEMU も正確にこう動作する:
- 各 vCPU は `qemu_thread`
- デバイス状態は BQL (Big QEMU Lock) で保護
- per-vCPU 状態 (`CPUState`) はスレッドローカル

BQL アプローチは単純だがスケーラビリティを制限 — 一度に1スレッドのみがデバイス状態にアクセスできる。現代の QEMU はパフォーマンスクリティカルなパスで細粒度ロックへの移行を徐々に進めている。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| Thread-per-vCPU | 各 vCPU に `pthread_create` — QEMU と同じ |
| 共有デバイス状態 | `device_counter`、`msr_store` に両スレッドがアクセス |
| Mutex | `pthread_mutex_t` が共有状態を保護 — BQL に類似 |
| BSP/AP パターン | vCPU 0 はリアルモードからブート; vCPU 1 はロングモードで開始 |
| 非決定性 | 出力順序がスケジューリングに依存 |
| Per-vCPU 状態 | 各 vCPU に独立した fd、kvm_run、スタック、RIP |

## 次のステップ

[Step 10: 最小 Linux ブート](step10_linux-boot.md) — これまで全てのゲストは手書きアセンブリだった。次のステップではトイゲストを実際の OS に置き換え、Linux が Step 1–9 で導入したメカニズムをどう使うか観察する: モード遷移、デバイスエミュレーション、割り込み配送、MSR ベースの準仮想化。
