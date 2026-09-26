# Step 42: EPT explorer — guest memory と host backing を対応づける

## このステップのポイント

**RAM slot に登録済みでも、EPT mapping が用意されているとは限らない。** `< Ctrl-A e >` で GPA と microkvm のメモリの対応を調べ、boot 中の EPT fault と照合する。

区別するのは、VMM によるメモリの登録、CPU によるアドレス変換、fault 時の KVM の処理である。

## 背景

### guest paging と EPT

guest のページングが有効なとき、メモリアクセスには二段階の変換がある。

```text
GVA（guest virtual address）
    │ guest page tables：guest OS が構築、基点は guest CR3
    ▼
GPA（guest physical address）
    │ EPT：KVM が構築、基点は EPT pointer
    ▼
HPA（host physical address）
```

CPU は guest page tables と EPT、および変換キャッシュを使ってアクセスする。必要な変換と権限が揃っていれば、そのメモリアクセスのために KVM へ戻る必要はない。

一方、microkvm が `mmap()` で得るのは HVA である。ホストのページテーブルが HVA を host backing page に結びつけ、KVM はその backing を使って GPA→HPA の EPT mapping を構築する。EPT が HVA を経由して変換するわけではない。

```text
                  memory slot の登録
                 GPA             HVA
                  │ EPT           │ host page tables
                  ▼               ▼
                      host backing memory
```

### Part 1・Part 2 とのつながり

Step 5 では RAM の登録範囲に穴を設け、MMIO を処理した。Step 19・20 では MMU stats と dirty page を取得し、Step 38 では RAM fault と MMIO の処理経路を観察した。今回は memory slot の情報を加え、これらをアドレス変換の視点からつなぐ。

## 今回追加する実装

変更は `microkvm.c` にまとまっている。登録用の memory region を保持する配列、GPA の検索関数、モニタの表示処理を追加する。

### 登録と表示で同じ構造体を使う

```c
#define MAX_MEMSLOTS 2

static struct kvm_userspace_memory_region g_memslots[MAX_MEMSLOTS];
static size_t g_nr_memslots = 0;
```

`g_memslots[0]` と `[1]` に GPA、サイズ、HVA、`KVM_MEM_LOG_DIRTY_PAGES` を設定し、同じ配列を KVM への登録と表示に使う。

```c
g_nr_memslots = 0;
for (size_t i = 0; i < MAX_MEMSLOTS; i++) {
    if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &g_memslots[i]) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return 1;
    }
    g_nr_memslots++;
}
```

KVM から slot 一覧を取得する処理ではなく、microkvm 自身が登録した情報を保持して表示する。この構成では登録完了後に slot 情報を変更しないため、`< Ctrl-A e >` の表示は入力スレッド内で行う。

### GPA から slot と HVA を求める

```c
static const struct kvm_userspace_memory_region *find_memslot(uint64_t gpa)
{
    for (size_t i = 0; i < g_nr_memslots; i++) {
        uint64_t start = g_memslots[i].guest_phys_addr;
        uint64_t end   = start + g_memslots[i].memory_size;
        if (gpa >= start && gpa < end)
            return &g_memslots[i];
    }
    return NULL;
}
```

slot の範囲は始点を含み、終点を含まない。slot が見つかれば、次の式で HVA を計算する。

```text
HVA = slot.userspace_addr + (GPA - slot.guest_phys_addr)
```

モニタは登録済み slot を列挙し、隣接する slot 間の GPA の隙間を計算する。配列は GPA の昇順で保持する。さらに `0x00100000` と `0x000d0000` を検索し、slot と HVA、または未登録であることを表示する。この検索は guest にメモリアクセスを発行するものではない。

## 観察A: memory slot を表示する

ターミナル A で Linux guest を起動し、プロンプトからモニタを呼ぶ。

```bash
cd ~/microkvm
./microkvm
```

```text
/ # < Ctrl-A e >
```

既存の計測値を、現在の表示形式に合わせると次のようになる。HVA は実行ごとに変わり得る。

```text
=== KVM Memory Slots (Ctrl-A e) ===
Slot 0
  GPA  : [0x00000000, 0x000d0000)
  size : 0xd0000
  HVA  : [0x7f619e400000, 0x7f619e4d0000)
Slot 1
  GPA  : [0x000d1000, 0x08000000)
  size : 0x7f2f000
  HVA  : [0x7f619e4d1000, 0x7f61a6400000)
Unregistered GPA gap
  GPA  : [0x000d0000, 0x000d1000)  (no RAM memslot)
Query GPA 0x00100000
  slot : 1
  HVA  : 0x7f619e500000
Query GPA 0x000d0000
  slot : none (unregistered)
===================================
```

### RAM として登録した範囲

microkvm は 128 MiB の連続した仮想アドレス範囲を `mmap()` で確保し、4 KiB の穴を除いた二つの範囲を KVM に登録する。host の物理ページまで連続しているという意味ではない。

GPA `0x00100000` は slot 1 に含まれる。表示例では次の計算になる。

```text
slot 内の offset = 0x00100000 - 0x000d1000 = 0x2f000
HVA = 0x7f619e4d1000 + 0x2f000 = 0x7f619e500000
```

GPA `0x000d0000` に対応する位置も元の `mmap()` 範囲内にはあるが、guest RAM としては登録されていない。**HVA の領域が存在することと、GPA が RAM slot に含まれることは別である。** この穴は virtio-mmio の `0xd0000000` とも別のアドレスである。

この表示で分かるのは GPA→slot→HVA の対応であり、HPA や EPT entry の値ではない。

## 観察B: boot 中の EPT fault を追う

RAM mapping と MMIO の処理を boot 中のトレースで確認する。これは観察A の二つの GPA にアクセスを発行する実験ではなく、Linux の起動で自然に発生する fault の観察である。

観察A の VM を終了する。ターミナル A は VM の起動、ターミナル B はホストのトレース操作用とし、他の VM と perf は停止しておく。

### 1. 起動前にトレースを設定する

ターミナル B でイベントを確認する。

```bash
trace_dir=/sys/kernel/tracing
sudo ls "$trace_dir/events/kvm/kvm_exit/enable" "$trace_dir/events/kvm/kvm_page_fault/enable"
```

Step 41 の CPUID filter を解除し、EPT を含む exit と page-fault イベントを記録する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_exit/filter"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_page_fault/filter"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_exit/enable"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_page_fault/enable"
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

### 2. 起動後に記録を停止・保存する

ターミナル A で起動する。

```bash
./microkvm
```

guest のプロンプトが表示されたら、idle 中のイベントで記録が上書きされる前に、ターミナル B で停止・保存する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-ept-boot.txt
```

ターミナル A では `< Ctrl-A e >` で今回の slot 表示を確認してから VM を終了し、終了レポートの MMU stats も控える。HVA は観察Aとは異なる場合がある。

### 3. exit reason と fault の GPA を確認する

ターミナル B で保存ファイルを読む。

```bash
grep -E 'reason EPT_VIOLATION|reason EPT_MISCONFIG' ~/kvm-ept-boot.txt | head -20
grep -c 'reason EPT_VIOLATION' ~/kvm-ept-boot.txt
grep -c 'reason EPT_MISCONFIG' ~/kvm-ept-boot.txt
grep 'kvm_page_fault:' ~/kvm-ept-boot.txt | head -20
```

`kvm_exit` の reason と、`kvm_page_fault` の `address`（この VMX 経路では GPA）を読む。後者を slot の GPA 範囲と照合する。`head` は先頭の確認なので、全体を調べる場合は保存ファイルを `less` で読む。

既存の boot 計測では、トレースに次の件数が記録された。

```text
EPT_VIOLATION  1463
EPT_MISCONFIG  774
```

同じ実行の終了レポートでは、以下の値が得られた。

```text
pf_taken               +4321
pf_fixed               +4318
pf_emulate             +3
pf_mmio_spte_created   +3
mmio_exits             +163
```

トレースバッファに残る範囲と stats の集計期間は一致するとは限らないため、二つの件数を一対一に対応づけない。

### RAM fault: mapping を構築・更新する

記録された `kvm_page_fault` の GPA は `0x1eb800`〜`0x07ffffff` の範囲で、slot 1 に含まれていた。RAM として登録済みでも、EPT mapping がまだない場合やアクセス権限の処理が必要な場合には EPT violation が発生する。

```text
EPT violation
    ↓
KVM: GPA に対応する memory slot を探す
    ↓
HVA から host backing page を解決
    ↓
EPT mapping を構築・更新して guest を再開
```

`pf_fixed +4318` は、多くの fault が KVM の MMU 処理で解決されたことを示す。RAM として登録する操作と、実際の EPT mapping の構築は別の段階である。

### MMIO: EPT misconfiguration を使った識別

Step 38 で見たように、KVM は MMIO 用の SPTE に特殊な設定を使い、既知の MMIO アクセスを識別する。Intel EPT ではこの設定へのアクセスが EPT misconfiguration を起こし、`handle_ept_misconfig()` から MMIO 処理へ進む。

今回のように RAM memslot に属さない GPA へのアクセスを KVM が MMIO として処理し、MMIO caching が有効な場合、MMIO 用 SPTE が作られる。その entry が有効な間の後続アクセスでは、EPT violation ではなく EPT misconfiguration の経路を利用できる。これは exit をなくす仕組みではなく、exit 後の MMIO 判定を効率化する仕組みである。

`pf_mmio_spte_created +3` と `EPT_MISCONFIG 774` はこの経路を理解する材料になる。ただし、この集計だけで観察A の穴 `0x000d0000` がアクセスされたとは判断しない。

## KVM の実装と照合する

### slot から backing を探すのは KVM

Linux v7.2 の [`gfn_to_hva()`](https://github.com/torvalds/linux/blob/v7.2/virt/kvm/kvm_main.c#L2741) は、guest のページ番号（GFN）から slot を探して HVA を求める。

```c
unsigned long gfn_to_hva(struct kvm *kvm, gfn_t gfn)
{
    return gfn_to_hva_many(gfn_to_memslot(kvm, gfn), gfn, NULL);
}
```

これは KVM のソフトウェア処理であり、CPU がメモリアクセスのたびにこの関数を呼ぶわけではない。RAM fault の処理では slot と guest のページ番号から host backing のページを解決し、EPT mapping の構築に使う。

[`kvm_mmu_faultin_pfn()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/mmu/mmu.c#L4704) では、slot がない場合を別経路へ分けている（関数内の抜粋）。

```c
struct kvm_memory_slot *slot = fault->slot;
/* 宣言・状態の確認を省略 */
if (unlikely(!slot))
    return kvm_handle_noslot_fault(vcpu, fault, access);
```

### page-fault イベントと hardware exit を区別する

[`handle_ept_violation()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L5967) は `trace_kvm_page_fault()` を呼ぶ。一方、[`handle_ept_misconfig()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L5983) は別の入口で MMIO 処理につながる。Step 38 で見たように、同じ `kvm_mmu_page_fault()` に進んでも、同じ tracepoint が記録されるわけではない。

## EPT と KVM の役割分担

```text
Guest のメモリアクセス
    ├─ 有効な変換・権限がある
    │      → CPU が EPT / 変換キャッシュを使ってアクセス
    │
    ├─ EPT violation
    │      → KVM が RAM mapping の構築・更新、または MMIO 処理
    │
    └─ MMIO 用 SPTE による EPT misconfiguration
           → KVM が MMIO を処理
                 → userspace のデバイス処理が必要なら KVM_EXIT_MMIO
```

`kvm_page_fault` は KVM の処理内に置かれたトレース地点である。VMX の EPT violation と、KVM が userspace に返す `KVM_EXIT_MMIO` は異なる段階のイベントになる。CPU は memory slot を参照せず、EPT の変換・権限・形式を検査する。登録情報を使って RAM やデバイスへのアクセスとして処理するのは KVM と VMM である。

## トレースを終了する

ターミナル B でイベントを無効にする。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
```

## このステップで分かったこと

- memory slot は GPA と userspace の backing memory の対応を定義する。
- HVA と HPA、guest paging と EPT は別のアドレス・変換である。
- 有効な EPT mapping があれば、そのメモリアクセスごとに KVM は介入しない。
- EPT violation は RAM でも発生する。MMIO では EPT misconfiguration を利用する経路もある。

## 参照

- [KVM API: KVM_SET_USER_MEMORY_REGION](https://docs.kernel.org/virt/kvm/api.html#kvm-set-user-memory-region)
- [Linux KVM MMU: Translation / Memory](https://docs.kernel.org/virt/kvm/x86/mmu.html): GPA、HVA、HPA と mapping の関係
- [Linux v7.2 KVM VMX 実装](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c): `handle_ept_violation()` / `handle_ept_misconfig()`
- [Linux v7.2 KVM MMU 実装](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/mmu/mmu.c): `kvm_mmu_page_fault()` と MMIO の判定
- [Linux v7.2 KVM SPTE 実装](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/mmu/spte.c): `make_mmio_spte()` による MMIO 用 entry の作成
- [Intel Software Developer’s Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): Volume 3 の EPT、EPT Violations、EPT Misconfigurations

## 次のステップ

[Step 43](step43_execution-controls.md) では CPUID・HLT・RDMSR を比較し、命令の実行が VM exit を起こす条件を VMX の実行制御から確認する。
