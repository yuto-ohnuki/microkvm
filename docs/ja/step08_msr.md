# Step 8: MSR ハンドリング

## 目的

`KVM_EXIT_X86_WRMSR` と `KVM_EXIT_X86_RDMSR` を使って **Model-Specific Register** (MSR) アクセスをトラップ・エミュレートする。
MSR exit を導入 — 準仮想化のゲスト-ハイパーバイザー間通信に使われる重要な VM exit のクラス。

> **Note:** このステップには `KVM_CAP_X86_USER_SPACE_MSR` と `KVM_CAP_X86_MSR_FILTER` をサポートするカーネルが必要。

## 背景

### MSR とは

Model-Specific Register は専用命令 (`wrmsr` / `rdmsr`) でアクセスする CPU レジスタの大きなセット。CPU 内部の機能を制御・公開する:

| MSR | 目的 |
|-----|------|
| `IA32_TSC` (0x10) | タイムスタンプカウンタ |
| `IA32_APIC_BASE` (0x1B) | Local APIC ベースアドレス |
| `IA32_EFER` (0xC0000080) | 拡張機能 (LME, NXE) |
| `MSR_KVM_*` (0x4B564Dxx) | KVM 準仮想化インターフェース |

MSR は**準仮想化**の主要メカニズム — ゲストとハイパーバイザーは実ハードウェアに存在しないシンセティック MSR を通じて通信できる。例えば KVM は kvmclock などの機能に、ASCII "KVM" に対応する `0x4B564Dxx` 範囲を予約している。

microkvm では、こうした標準・KVM 定義の範囲を避け、教育用ゲスト専用の任意の実験的インデックス `0x20000000` (`MSR_CUSTOM`) を使う。これはアーキテクチャ的にも KVM 的にも定義されていない値で、「ゲストとハイパーバイザーが自分たちだけの取り決めで MSR を使う」ことを示すためのもの。

### デフォルトの KVM 動作

デフォルトでは、KVM はほとんどの MSR アクセスをユーザー空間に exit せず内部で処理する (TSC, APIC, EFER 等)。KVM で処理されずユーザー空間にもルーティングされない MSR に対しては、ゲストは通常 #GP (General Protection Fault) を受ける。

この実装では、フィルタで拒否した MSR アクセスをユーザー空間へ渡すため、次の2段階を設定する:

1. **`KVM_CAP_X86_USER_SPACE_MSR`** — `KVM_MSR_EXIT_REASON_FILTER` を指定し、フィルタで拒否したアクセスを #GP の代わりにユーザー空間へ通知
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

この step のカスタム MSR は、VMM のファイルスコープ変数 `msr_store` に保存する。自動的に vCPU ごとの状態が作られるわけではなく、保存範囲は VMM の実装で決まる。

## 実行フロー

```
VMM (microkvm)                                  Guest
──────────────                                  ─────
FILTER 理由の MSR exit を有効化
MSR filter: 0x20000000 の read/write を拒否
KVM_RUN
                                                （Step 7 と同じモード遷移・PIO・MMIO）
                                                ECX = 0x20000000
                                                EDX = 0, EAX = 0x42
                                                wrmsr
KVM_EXIT_X86_WRMSR
  msr_store = 0x42
  run->msr.error = 0
KVM_RUN                                         （wrmsr を完了して再開）
                                                rdmsr
KVM_EXIT_X86_RDMSR
  run->msr.data = msr_store
  run->msr.error = 0
KVM_RUN                                         （rdmsr を完了: EDX=0, EAX=0x42）
                                                add al, '0' → 'r'
                                                out 0x10, al
KVM_EXIT_IO: 'r' を表示
KVM_RUN                                         （再開）
                                                改行を PIO 出力
                                                Step 7 と同じ割り込み処理・終了
```

## 実装

`microkvm.h` に `MSR_CUSTOM` を定義し、`microkvm.c` にフィルタ設定と MSR の保存・応答処理、`guest.S` に書き込みと読み戻しを追加する。

### VMM: MSR トラッピングの有効化 (2段階セットアップ)

VM 作成後、ゲスト実行前に設定する。以下はエラー処理を省略した抜粋。

```c
/* ユーザー空間 MSR exit を有効化 */
struct kvm_enable_cap msr_cap = {
    .cap = KVM_CAP_X86_USER_SPACE_MSR,
    .args[0] = KVM_MSR_EXIT_REASON_FILTER,
};
ioctl(vmfd, KVM_ENABLE_CAP, &msr_cap);

/* フィルタ設定 — カスタム MSR を拒否 */
uint8_t msr_bitmap[] = {0x00};  /* bit=0 は拒否 (トラップ) を意味 */
struct kvm_msr_filter filter = {
    .flags = KVM_MSR_FILTER_DEFAULT_ALLOW,
    .ranges = {{
        .flags = KVM_MSR_FILTER_READ | KVM_MSR_FILTER_WRITE,
        .nmsrs = 1,
        .base = MSR_CUSTOM,       /* 0x20000000 */
        .bitmap = msr_bitmap,
    }},
};
ioctl(vmfd, KVM_X86_SET_MSR_FILTER, &filter);
```

`nmsrs=1` なので、ビットマップの bit 0 だけが `MSR_CUSTOM` の read/write を制御する。値0は拒否を意味し、`KVM_MSR_EXIT_REASON_FILTER` を有効にしているため VMM に通知される。この exit 設定がなければ、拒否したアクセスはゲストの #GP になる。

`DEFAULT_ALLOW` は範囲外の MSR をフィルタで拒否しないという意味。未対応 MSR まで使えるようにする設定ではなく、範囲外は KVM の通常の処理に従う。

### VMM: MSR exit の処理

```c
/* microkvm.c のファイルスコープ */
static uint64_t msr_store = 0;

/* exit handler 内（ログ出力は省略） */
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

次の `KVM_RUN` で、`error=0` なら KVM が MSR 操作を完了する。read の結果は `data` から EDX:EAX に反映され、`error=1` ならゲストに #GP が注入される。

### ゲスト: wrmsr と rdmsr

```asm
    /* wrmsr: MSR 0x20000000 に 0x42 を書く */
    .byte 0xB9, 0x00, 0x00, 0x00, 0x20      /* mov ecx, 0x20000000 */
    .byte 0x31, 0xD2                        /* xor edx, edx */
    .byte 0xB8, 0x42, 0x00, 0x00, 0x00      /* mov eax, 0x42 */
    .byte 0x0F, 0x30                        /* wrmsr */

    /* rdmsr: 同じ MSR から読み戻す */
    .byte 0x0F, 0x32                        /* rdmsr → eax = 0x42 */
    .byte 0x04, 0x30                        /* add al, '0' → 'r' */
    .byte 0xE6, 0x10                        /* out 0x10, al */
    .byte 0xB0, '\n'                        /* mov al, '\n' */
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
[MSR write] 0x20000000 = 0x42
[MSR read] 0x20000000 -> 0x42
[PIO out port 0x10] r
[PIO out port 0x10] I
Guest halted.
```

MSR の write/read ログで、0x42 を保存して同じ値を返したことを確認する。PIO の `r` は `0x42 + 0x30 = 0x72` の結果で、数値を十進表記したものではない。最後の `I` と `Guest halted.` は Step 7 から継続する割り込み処理の出力。

## 重要な知見

MSR フィルタで対象を選び、VMM が値の保存と読み戻しを担当することで、独自の MSR インターフェースを実装できる。準仮想化かどうかはゲストと VMM の取り決めで決まり、MMIO と MSR の違いだけで分類できるものではない。

### KVM の準仮想化 MSR との関係

kvmclock は MSR で共有メモリの場所などを登録し、ゲストがそのメモリの時刻情報を読む仕組み。この step のように `rdmsr` で値そのものを返す方式とは異なり、標準の KVM 機能は KVM 内部で処理される。

### これまでに導入された主要 VM exit タイプ (Step 1–8)

| Exit タイプ | Step | トリガー |
|-------------|------|---------|
| `KVM_EXIT_HLT` | 1, 7 | `hlt` 命令 |
| `KVM_EXIT_IO` | 2 | `out` / `in` 命令 |
| `KVM_EXIT_MMIO` | 5, 6 | メモリスロットなしの GPA へのアクセス |
| `KVM_EXIT_X86_WRMSR` | 8 | 拒否された MSR への `wrmsr` |
| `KVM_EXIT_X86_RDMSR` | 8 | 拒否された MSR からの `rdmsr` |

ここまでで扱った主要な exit の一覧であり、KVM の全 exit タイプを網羅するものではない。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| MSR フィルタ | 2段階セットアップ: capability 有効化 + フィルタビットマップ設定 |
| wrmsr / rdmsr | ECX = アドレス、EDX:EAX = 値 (64-bit 分割) |
| 準仮想化 | ゲスト-ハイパーバイザー通信のためのシンセティック MSR |
| 選択的トラッピング | DEFAULT_ALLOW + 特定 MSR を拒否 |
| エラー注入 | `run->msr.error = 1` でゲストに #GP を発生 |

## 変わったこと

- `microkvm.h`: 教育用の独自 MSR インデックス `MSR_CUSTOM`（0x20000000）を追加。
- `microkvm.c`: MSR exit の有効化、フィルタ、`msr_store` と read/write 処理を追加。
- `guest.S`: 0x42 の書き込み・読み戻しと、結果の PIO 出力を追加。

## 次のステップ

[Step 9: 複数 vCPU](step09_multi-vcpu.md) — これまで VM は単一の仮想 CPU で構成されていた。実システムはマルチコア。次のステップでは、同じゲストメモリとデバイス状態を共有する複数 vCPU を導入し、ホストスレッド間の同期が必要になる。
