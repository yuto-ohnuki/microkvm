# Step 9: 複数 vCPU

> **Phase B: Linux Boot**
>
> Phase A はベアメタル guest コードで CPU 仮想化の基礎を構築した。
> Phase B では実際の Linux kernel を boot する: マルチ vCPU、bzImage ロード、シリアルコンソール、対話シェル。終了時には guest が busybox による完全な Linux userspace を実行する。

## 目的

**pthread** を使って2つの vCPU を並列実行し、共有デバイス状態と同期の必要性を導入する。実行順序が非決定的になる最初のステップ。

## 背景

### 1 vCPU = 1 スレッド

この実装では各 vCPU にホストスレッドを割り当て、`KVM_RUN` の中でゲストを実行し、戻ったら exit を処理する。2スレッドがそれぞれ `KVM_RUN` を呼ぶことで、複数 vCPU を並行実行できる:

```
main thread
  ├── pthread_create → vCPU 0 thread: for(;;) { KVM_RUN; handle exit; }
  ├── pthread_create → vCPU 1 thread: for(;;) { KVM_RUN; handle exit; }
  ├── pthread_join(vCPU 0)
  └── pthread_join(vCPU 1)
```

### 共有状態は同期が必要

このゲストでは、vCPU 0 が `device_counter` を更新し、vCPU 1 はその値を読む。書き手が1つでも、別スレッドの読み取りと同期しなければデータ競合になる。

`dev_lock` でカウンタの更新・読み取りを保護する。`msr_store` と vCPU ごとの通常ログも同じロックで保護するが、今回 MSR にアクセスするゲストコードは vCPU 0 だけにある。ロックは処理を直列化しても、どちらの vCPU が先に進むかは決めない。

### BSP と AP の初期化

実マルチプロセッサハードウェアでは:
- **BSP** (Bootstrap Processor) が最初にブートしモード遷移を実行
- **AP** (Application Processor) は BSP が準備済みの環境に後から起動

この step では、異なる開始状態を用意する:
- vCPU 0 はリアルモードから開始し全モードを遷移 (Step 3–4)
- vCPU 1 は VMM がレジスタを設定し、直接ロングモードから開始

実機の AP 起動で使う INIT/SIPI は実装しない。ページテーブルの準備、ゲストバイナリのロード、両 vCPU の初期化は、スレッド起動前に VMM が済ませる。vCPU 1 は vCPU 0 のブート完了を待たずに実行できる。

### 非決定的出力

2スレッドが並行実行するため、vCPU 間のログ順序はスケジューリングに依存する。vCPU 1 の読み取り値は、vCPU 0 の2回の write に対して読み取りがいつ実行されるかにより、0・1・2のいずれかになる。vCPU 0 自身の read は2回の write の後なので2になる。

## 実行フロー

```
Guest                         KVM                         VMM (microkvm)
─────                         ───                         ──────────────
                                                          共有メモリ・両 vCPU を初期化
                                                          pthread_create × 2
vCPU 0: リアルモード ←─────── vCPU 0 を実行 ←────────── thread 0: KVM_RUN
vCPU 1: ロングモード ←─────── vCPU 1 を実行 ←────────── thread 1: KVM_RUN

  各 vCPU は独立して進む:
vCPU 0: MMIO write × 2 ─────→ KVM_EXIT_MMIO ────────────→ dev_lock 内で counter++
vCPU 1: MMIO read ──────────→ KVM_EXIT_MMIO ────────────→ dev_lock 内で現在値を返す
vCPU 1: 値を PIO 出力、hlt ─→ KVM_EXIT_HLT ─────────────→ thread 1 終了
vCPU 0: MMIO read、MSR、IRQ
        最後の hlt ─────────→ KVM_EXIT_HLT ─────────────→ thread 0 終了
                                                          pthread_join × 2
                                                          リソース解放
```

図の read/write の並びやスレッドの終了順は固定ではない。`KVM_RUN` 自体は `dev_lock` の外で呼ぶため、ゲスト実行全体を直列化しない。

## 実装

`microkvm.c` の exit ループを `vcpu_thread` に移し、`microkvm.h` に `NUM_VCPUS=2` と `VCPU1_ENTRY=0x1100` を追加する。`guest.S` に vCPU 1 用コードを追加し、`Makefile` では `-lpthread` をリンクする。以下は主要部分の抜粋。

### VMM: struct vcpu

```c
struct vcpu {
    int fd;
    int id;
    struct kvm_run *run;
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

`ioctl(vmfd, KVM_CREATE_VCPU, i)` の第3引数 `i` が vCPU ID（x86 では APIC ID）。返された fd と共有領域は vCPU ごとに保持する。

### VMM: vCPU 1 — 直接ロングモード初期化

```c
/* vCPU 1: 直接ロングモードで開始 */
sregs.cr0 = 0x80000011;             /* PG | PE | ET */
sregs.cr3 = 0x70000;                /* vCPU 0 と同じページテーブル */
sregs.cr4 = 0x20;                   /* PAE */
sregs.efer = (1 << 8) | (1 << 10);  /* LME | LMA */

sregs.cs.selector = 0x18;           /* GDT[3]: 64-bit コード */
sregs.cs.l = 1;                     /* ロングモード */
sregs.cs.present = 1;
sregs.cs.s = 1;
sregs.cs.type = 11;                 /* 実行/読み取り, accessed */

regs.rip = VCPU1_ENTRY;             /* 0x1100 */
regs.rsp = 0x50000;                 /* vCPU 0 とは別のスタック */
```

VMM は制御レジスタとセグメント属性を直接設定する。`cs.selector=0x18` を代入するだけで GDT が読み込まれるわけではないため、CS の属性も設定する。上の抜粋以外に DS/SS も初期化しており、両 vCPU の全レジスタが同一になるわけではない。ゲストスタックの先頭は vCPU 0 が0x60000、vCPU 1 が0x50000で、互いに重ならない領域を使う。

### VMM: mutex 保護された exit ハンドラ

```c
static pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;

/* vcpu_thread 内: */
case KVM_EXIT_MMIO:
    if (run->mmio.phys_addr == MEM_GAP_START) {
        pthread_mutex_lock(&dev_lock);
        if (run->mmio.is_write) {
            device_counter++;
        } else {
            run->mmio.data[0] = device_counter;
        }
        /* 実コードのログ出力もこのロック内（ここでは省略） */
        pthread_mutex_unlock(&dev_lock);
    }
    break;
```

共有状態へのアクセスと対応するログを同じロック内で処理する。別 vCPU の処理がその途中に入ることは防ぐが、ログ行の前後関係までは固定しない。

### VMM: 起動・終了の管理

```c
for (int i = 0; i < NUM_VCPUS; i++) {
    pthread_create(&threads[i], NULL, vcpu_thread, &vcpus[i]);
}
for (int i = 0; i < NUM_VCPUS; i++) {
    pthread_join(threads[i], NULL);
}
```

両スレッドを起動してから終了を待ち、最後に共有メモリなどを解放する。割り込み注入は vCPU 0 の最初の HLT のみで、vCPU 1 は最初の HLT で終了する。`irq_injected` は各 `vcpu_thread` のローカル変数として保持する。

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

`.org 0x1100` はバイナリ内の固定オフセットにこのコードを配置する。VMM が vCPU 1 の RIP をこのアドレスに設定する。vCPU 1 は vCPU 0 と同じ MMIO デバイスから読み取り、`device_counter` が真に共有であることを示す — どちらの vCPU がどの時点で read するかはスケジューリング次第で、返る値はその瞬間のカウンタ状態に依存する（非決定的）。

## 出力

```
$ ./microkvm
Loaded guest: 8232 bytes
Starting guest...
[vCPU 0][PIO out port 0x10] R
[vCPU 1][MMIO read  @ 0xd0000] returning 0
[vCPU 0][PIO out port 0x10] P
[vCPU 1][PIO out port 0x10] 0
[vCPU 1] halted.
[vCPU 0][PIO out port 0x10] L
[vCPU 0][MMIO write @ 0xd0000] M
[vCPU 0][MMIO read  @ 0xd0000] returning 2
[vCPU 0][PIO out port 0x10] 2
[vCPU 0][MSR write] 0x20000000 = 0x42
[vCPU 0][MSR read] 0x20000000 -> 0x42
[vCPU 0][PIO out port 0x10] r
[vCPU 0][PIO out port 0x10] I
[vCPU 0] halted.
```

この実行では vCPU 1 が vCPU 0 の write より先に読み、0を返している。`Loaded guest: 8232 bytes` と両 vCPU の終了を確認できる。再実行時にログ順序や vCPU 1 の値が変わっても、上記の実行タイミングによる違いであれば正常。

## 重要な知見

各 vCPU は固有のレジスタと `kvm_run` を持ち、ゲストメモリと VMM のデバイス状態は共有する。mutex は共有状態のデータ競合を防ぐが、vCPU 間の実行順序を固定しない。そのため、正しく同期していても vCPU 1 の読み取り値は実行タイミングによって変わる。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| Thread-per-vCPU | 各 vCPU に `pthread_create` でホストスレッドを割り当てる |
| 共有デバイス状態 | vCPU 0 がカウンタを更新し、vCPU 1 が同じカウンタを読む |
| Mutex | 共有状態の更新・読み取りを排他制御 |
| BSP/AP パターン | vCPU 0 はリアルモードからブート; vCPU 1 はロングモードで開始 |
| 非決定性 | 出力順序がスケジューリングに依存 |
| Per-vCPU 状態 | 各 vCPU に独立した fd、kvm_run、スタック、RIP |

## 変わったこと

- exit ループを `vcpu_thread` に分離し、vCPU ごとの fd・共有領域・識別子を `struct vcpu` にまとめる。
- 2つの vCPU を別スレッドで実行し、共有デバイス状態を mutex で保護する。
- vCPU 1 のロングモード初期化と、0x1100 に配置するゲストコードを追加。
- ログに vCPU ID を付け、全スレッドの終了後にリソースを解放する。

## 次のステップ

[Step 10: 最小 Linux ブート](step10_linux-boot.md) — これまで全てのゲストは手書きアセンブリだった。次のステップではトイゲストを実際の OS に置き換え、Linux が Step 1–9 で導入したメカニズムをどう使うか観察する: モード遷移、デバイスエミュレーション、割り込み配送、MSR ベースの準仮想化。
