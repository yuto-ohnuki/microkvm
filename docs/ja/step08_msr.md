# Step 8: MSR ハンドリング

## 目的

`KVM_EXIT_X86_WRMSR` と `KVM_EXIT_X86_RDMSR` を使って **Model-Specific Register** (MSR) アクセスをトラップ・エミュレートする。
MSR exit を導入 — 準仮想化のゲスト-ハイパーバイザー間通信に使われる重要な VM exit のクラス。

> **注:** このステップには `KVM_CAP_X86_USER_SPACE_MSR` とユーザー空間 MSR exit をサポートするカーネルが必要。

## 背景

### MSR とは

Model-Specific Register は専用命令 (`wrmsr` / `rdmsr`) でアクセスする CPU レジスタの大きなセット。CPU 内部の機能を制御・公開する:

| MSR | 目的 |
|-----|------|
| `IA32_TSC` (0x10) | タイムスタンプカウンタ |
| `IA32_APIC_BASE` (0x1B) | Local APIC ベースアドレス |
| `IA32_EFER` (0xC0000080) | 拡張機能 (LME, NXE) |
| `MSR_KVM_*` (0x4B564Dxx) | KVM 準仮想化インターフェース |

16進接頭辞 `0x4B564D` は ASCII 文字列 "KVM" に対応する。KVM はこの範囲をシンセティック (合成) ハイパーバイザー定義 MSR 用に予約している。

MSR は**準仮想化**の主要メカニズム — ゲストとハイパーバイザーは実ハードウェアに存在しないシンセティック MSR を通じて通信できる。KVM は kvmclock などの機能に `0x4B564D00` 範囲を使用。

### デフォルトの KVM 動作

デフォルトでは、KVM はほとんどの MSR アクセスをユーザー空間に exit せず内部で処理する (TSC, APIC, EFER 等)。KVM で処理されずユーザー空間にもルーティングされない MSR に対しては、ゲストは通常 #GP (General Protection Fault) を受ける。

特定の MSR をユーザー空間にトラップするには2段階が必要:

1. **`KVM_CAP_X86_USER_SPACE_MSR`** — 拒否された MSR が #GP 注入ではなくユーザー空間に exit するよう capability を有効化
2. **`KVM_X86_SET_MSR_FILTER`** — どの MSR を拒否 (トラップ) するか指定

### wrmsr / rdmsr 命令

```
wrmsr:  ECX = MSR アドレス,  EDX:EAX = 書き込む 64-bit 値
rdmsr:  ECX = MSR アドレス → EDX:EAX = 読み出した 64-bit 値
```

両方とも CPL=0 (ring 0) が必要。値は歴史的理由から2つの 32-bit レジスタに分割される (これらの命令は 64-bit モード以前から存在)。

### MMIO vs MSR

MMIO はゲスト物理アドレス空間を通じてレジスタを公開 — 通常の load/store 命令でアクセス。MSR は専用 CPU 命令 (`rdmsr`/`wrmsr`) を通じてレジスタを公開。

ゲストの視点からは両方ともレジスタだが、異なるドメインに属する:

| | MMIO (Step 5–6) | MSR (Step 8) |
|---|---|---|
| 所属先 | デバイス | CPU 自体 |
| アクセス | `mov` (メモリ命令) | `rdmsr` / `wrmsr` |
| アドレス空間 | ゲスト物理アドレス | 32-bit MSR インデックス |
| トラップメカニズム | メモリスロットホール | MSR フィルタビットマップ |

ゲストはデバイス列挙 (PCI, ACPI) で MMIO 領域を発見できるが、MSR は CPU アーキテクチャ自体の一部 — 全 CPU コアが独自の MSR 状態を持つ。マルチ vCPU VM では、各 vCPU が独自の MSR 状態を持つ (実マルチコアプロセッサと同様)。

## 実行フロー

```
VMM (microkvm.c)                         Guest (guest.S)
────────────────                         ───────────────
KVM_ENABLE_CAP(USER_SPACE_MSR)
KVM_X86_SET_MSR_FILTER:
  deny MSR 0x4B564D00
                                         [ロングモード]
                                           mov ecx, 0x4B564D00
                                           mov eax, 0x42
                                           xor edx, edx
                                           wrmsr
                                                │
KVM_EXIT_X86_WRMSR                              ▼
  run->msr.index = 0x4B564D00
  run->msr.data  = 0x42
  msr_store = 0x42
  run->msr.error = 0
ioctl(KVM_RUN)
                                           rdmsr
                                                │
KVM_EXIT_X86_RDMSR                              ▼
  run->msr.index = 0x4B564D00
  run->msr.data = msr_store (0x42)
  run->msr.error = 0
ioctl(KVM_RUN)
                                           eax = 0x42
                                           add al, '0' → 'r' (0x42+0x30=0x72)
                                           out 0x10, al
```

## 実装

### VMM: MSR トラッピングの有効化 (2段階セットアップ)

```c
/* Step 1: ユーザー空間 MSR exit を有効化 */
struct kvm_enable_cap msr_cap = {
    .cap = KVM_CAP_X86_USER_SPACE_MSR,
    .args[0] = KVM_MSR_EXIT_REASON_FILTER,
};
ioctl(vmfd, KVM_ENABLE_CAP, &msr_cap);

/* Step 2: フィルタ設定 — カスタム MSR を拒否 */
uint8_t msr_bitmap[] = {0x00};  /* bit=0 は拒否 (トラップ) を意味 */
struct kvm_msr_filter filter = {
    .flags = KVM_MSR_FILTER_DEFAULT_ALLOW,
    .ranges = {{
        .flags = KVM_MSR_FILTER_READ | KVM_MSR_FILTER_WRITE,
        .nmsrs = 1,
        .base = MSR_CUSTOM,       /* 0x4B564D00 */
        .bitmap = msr_bitmap,
    }},
};
ioctl(vmfd, KVM_X86_SET_MSR_FILTER, &filter);
```

`DEFAULT_ALLOW` はフィルタに含まれない MSR は KVM が通常通り処理することを意味する。カスタム MSR のみが拒否 (ユーザー空間にトラップ) される。

### VMM: MSR exit の処理

```c
case KVM_EXIT_X86_WRMSR:
    if (run->msr.index == MSR_CUSTOM) {
        msr_store = run->msr.data;
        run->msr.error = 0;    /* 成功 */
    } else {
        run->msr.error = 1;    /* #GP を注入 */
    }
    break;

case KVM_EXIT_X86_RDMSR:
    if (run->msr.index == MSR_CUSTOM) {
        run->msr.data = msr_store;
        run->msr.error = 0;
    } else {
        run->msr.error = 1;
    }
    break;
```

`error = 0` は成功 — KVM はゲストを通常通り再開。
`error = 1` は KVM にゲストに #GP を注入させる。

### ゲスト: wrmsr と rdmsr

```asm
    /* wrmsr: MSR 0x4B564D00 に 0x42 を書く */
    .byte 0xB9, 0x00, 0x4D, 0x56, 0x4B      /* mov ecx, 0x4B564D00 */
    .byte 0x31, 0xD2                        /* xor edx, edx */
    .byte 0xB8, 0x42, 0x00, 0x00, 0x00      /* mov eax, 0x42 */
    .byte 0x0F, 0x30                        /* wrmsr */

    /* rdmsr: 同じ MSR から読み戻す */
    .byte 0x0F, 0x32                        /* rdmsr → eax = 0x42 */
    .byte 0x04, 0x30                        /* add al, '0' → 'r' */
    .byte 0xE6, 0x10                        /* out 0x10, al */
```

ゲストはシンセティック MSR に値を書き、読み戻して PIO で結果を出力する。MSR フィルタがこのアドレスへのアクセスを拒否するため、両命令とも VMM にトラップされる。

## 出力

```
$ ./microkvm
Loaded guest: 4136 bytes
Starting guest...
[PIO out port 0x10] R
[PIO out port 0x10] P
[PIO out port 0x10] L
[MMIO write @ 0xd0000] M
[MMIO read  @ 0xd0000] returning 2
[PIO out port 0x10] 2
[MSR write] 0x4b564d00 = 0x42
[MSR read] 0x4b564d00 -> 0x42
[PIO out port 0x10] r
[PIO out port 0x10] I
Guest halted.
```

## 重要な知見

MSR トラッピングは**準仮想化**を可能にするメカニズム。MMIO (実ハードウェアをエミュレート) と異なり、シンセティック MSR は物理デバイスに対応しないゲストとハイパーバイザー間の直接通信チャネルを作る。

### ハードウェアエミュレーション vs 準仮想化

MMIO は実ハードウェア上に存在し得るデバイスをエミュレートする。シンセティック MSR はゲストがハイパーバイザー下で動作しているからこそ存在するインターフェースを公開する。ゲストはもはや仮想デバイスと話しているのではない — ハイパーバイザーと直接話している。

KVM がこの方法で実装する機能:
- **kvmclock** — ゲストがハードウェアタイマーのキャリブレーションの代わりに MSR 経由で時刻を読む
- **steal time** — ハイパーバイザーがゲストから盗まれた CPU 時間を報告
- **PV spinlocks** — ゲストがロックでスピン中にハイパーバイザーに通知

フィルタメカニズム (`DEFAULT_ALLOW` + 特定 MSR を拒否) により、VMM は必要なものだけをインターセプトする。標準 MSR (TSC, EFER, APIC) はユーザー空間に exit せず KVM 内部で効率的に処理され続ける。

### これまでに導入された主要 VM exit タイプ (Step 1–8)

| Exit タイプ | Step | トリガー |
|-------------|------|---------|
| `KVM_EXIT_HLT` | 1, 7 | `hlt` 命令 |
| `KVM_EXIT_IO` | 2 | `out` / `in` 命令 |
| `KVM_EXIT_MMIO` | 5, 6 | メモリスロットなしの GPA へのアクセス |
| `KVM_EXIT_X86_WRMSR` | 8 | 拒否された MSR への `wrmsr` |
| `KVM_EXIT_X86_RDMSR` | 8 | 拒否された MSR からの `rdmsr` |

これらの exit タイプがあれば、VMM はデバイスエミュレーションと準仮想化インターフェースの驚くほど大きなサブセットを実装できる。以降の全てはこの基盤の上に構築される。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| MSR フィルタ | 2段階セットアップ: capability 有効化 + フィルタビットマップ設定 |
| wrmsr / rdmsr | ECX = アドレス、EDX:EAX = 値 (64-bit 分割) |
| 準仮想化 | ゲスト-ハイパーバイザー通信のためのシンセティック MSR |
| 選択的トラッピング | DEFAULT_ALLOW + 特定 MSR を拒否 |
| エラー注入 | `run->msr.error = 1` でゲストに #GP を発生 |

## 次のステップ

[Step 9: 複数 vCPU](step09_multi-vcpu.md) — これまで VM は単一の仮想 CPU で構成されていた。実システムはマルチコア。次のステップでは、同じゲストメモリとデバイス状態を共有する複数 vCPU を導入し、ホストスレッド間の同期が必要になる。
