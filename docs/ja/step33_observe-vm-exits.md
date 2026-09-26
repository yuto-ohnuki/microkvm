# Step 33: VM exit を観察する — 3 つの観察点の比較

> **Phase G: Understand KVM Through VM Exits**
>
> Part 2 では、Part 1 で作った microkvm の動作を perf / ftrace で観察し、Linux KVM の実装と照合する。

## このステップのポイント

**hardware VM exit が起きても、`KVM_RUN` が microkvm に戻るとは限らない。** 今回は、自作 counter・KVM binary stats・perf を比較して、この違いを確認する。

Intel VT-x の VM exit は、guest の実行から KVM へ制御が移ることを指す。その後、KVM は自分で処理するか、userspace の VMM に処理を返す。

```text
guest → hardware VM exit → KVM
                            ├─ KVM 内で処理 → guest を再開
                            │   例: 今回観測した HLT、PREEMPTION_TIMER
                            │
                            └─ KVM_RUN が戻る → microkvm が処理
                                例: userspace で扱う MMIO / PIO
```

Part 1 の `switch (run->exit_reason)` に届くのは下側の経路だけである。hardware の exit reason と、userspace に返す `KVM_EXIT_*` は別の分類になる。

## 何を使って観察するか

| 観察手段 | 数えているもの | 今回の使い方 |
|----------|----------------|--------------|
| 自作 MMIO counter | microkvm の `KVM_EXIT_MMIO` 処理回数 | userspace に届いた MMIO を確認 |
| KVM binary stats（Step 19） | KVM 内の各処理地点の累積値 | MMIO に加え、PIO・HLT・exit 全体を確認 |
| perf kvm stat | KVM の exit / entry tracepoint | reason 別の回数と、次の entry までの時間を確認 |

自作 counter は `KVM_RUN` が戻った後に数える。一方、perf は KVM 内の `kvm:kvm_exit` / `kvm:kvm_entry` を使うため、userspace に戻らない exit も観察できる。

## 観察手順と出力

以下は Intel VMX 環境で Linux guest を起動して観察する手順。microkvm は1プロセスだけ起動する。

```bash
# ターミナル A: microkvm を Linux boot で起動 → guest で ls / echo / cat を実行
cd ~/microkvm && ./microkvm

# ターミナル B: 15 秒観察（この間に guest で操作）
sudo perf kvm stat record -p $(pgrep -x microkvm) -- sleep 15
sudo perf kvm stat report
```

### 観察結果

以下は測定例。回数や時間は実行環境・操作によって変わる。

`perf kvm stat report`（15 秒、guest で ls/echo/cat）:

```
             Event name     Samples   Sample%       Time%   Mean Time (ns)
         IO_INSTRUCTION        6747    63.00%       0.00%            11081
                    HLT        3750    35.00%      99.00%          3945206
     EXTERNAL_INTERRUPT          45     0.00%       0.00%            10419
       PREEMPTION_TIMER          32     0.00%       0.00%              678
               MSR_READ          15     0.00%       0.00%             6409
```

microkvm 終了時のレポート:

```text
==== microkvm execution report ====
Mode: ioeventfd=OFF, irqfd=OFF

--- Userspace counters ---
MMIO exits total:           163
QueueNotify MMIO exits:
  RX queue 0:               128
  TX queue 1:               0
ioeventfd TX kicks:         0
IRQ inject (ioctl):         0
IRQ inject (irqfd):         0

--- TX processing latency ---
  (no TX data)

--- IRQ injection latency ---
  (no IRQ data)

--- KVM VM stats ---
  pages_4k                         4318 (current)

--- KVM vCPU 0 stats ---
  pf_taken                         +4321
  pf_fixed                         +4318
  pf_emulate                       +3
  pf_mmio_spte_created             +3
  tlb_flush                        +27
  exits                            +55962
  io_exits                         +23072
  mmio_exits                       +163
  halt_exits                       +26578
  irq_injections                   +1
==================================
```

perf は 15 秒間、自作 counter と binary stats は起動から終了までの値である。**測定期間と集計地点が異なるため、perf の Samples と stats の `exits` を直接突き合わせない。**

## 出力のどこを見るか

### 1. microkvm が処理しない exit も perf に見える

`PREEMPTION_TIMER` が 32 件ある。これは KVM 内で処理され、この exit を理由に `KVM_RUN` は戻らない。microkvm の handler に届かなくても、hardware VM exit は発生している。

### 2. 同じ MMIO を数える counter は一致する

自作 counter の `163` と binary stats の `mmio_exits +163` は、この実行では一致した。どちらも対応する userspace への MMIO 返却を数えている。

一方、binary stats の `exits +55962` は MMIO だけの値ではない。自作 MMIO counter との桁の違いは、数える対象の違いによる。

### 3. HLT の Time% は処理コストではない

回数では `IO_INSTRUCTION` が 63%、時間では `HLT` が 99% を占める。microkvm では UART などの PIO が `IO_INSTRUCTION` として現れる。

perf の時間は exit から次の entry までの経過時間であり、待機やスケジューリングも含む。今回の HLT は vCPU の idle 待機を含むため、**Time% が大きいことを、そのまま処理が重いことと解釈しない。**

## KVM の実装と照合する

Linux v7.2 の [`vmx_vcpu_run()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L7505) を読む。以下は、VM entry / exit とトレース、後続の判定の位置関係を残した抜粋である（省略箇所はコメントで示す）。計測時のカーネルそのものではなく、処理順を確認するための参照版として使う。

```c
fastpath_t vmx_vcpu_run(struct kvm_vcpu *vcpu, u64 run_flags)
{
    bool force_immediate_exit = run_flags & KVM_RUN_FORCE_IMMEDIATE_EXIT;
    struct vcpu_vmx *vmx = to_vmx(vcpu);

    /* 初期化・guest state の準備などを省略 */
    vmx_vcpu_enter_exit(vcpu, __vmx_vcpu_enter_flags(vmx));

    /* host state の復元などを省略 */
    if (unlikely(vmx->fail))
        return EXIT_FASTPATH_NONE;

    trace_kvm_exit(vcpu, KVM_ISA_VMX);
    if (unlikely(vmx_get_exit_reason(vcpu).failed_vmentry))
        return EXIT_FASTPATH_NONE;

    /* 割り込み状態の処理などを省略 */
    return vmx_exit_handlers_fastpath(vcpu, force_immediate_exit);
}
```

読む順序は次の三つである。

1. `vmx_vcpu_enter_exit()` で guest を実行し、KVM に制御が戻る。
2. `trace_kvm_exit()` で exit を記録する。
3. その後に `vmx_exit_handlers_fastpath()` で処理経路を判定する（詳細は Step 36）。

**トレースは userspace に返すかどうかが決まる前にあるため、KVM 内で完結する exit も記録できる。** ただしこれは KVM の処理経路上の観察点であり、hardware の全動作を直接記録するものではない。例えば、抜粋の `vmx->fail` による早期 return はこのトレース地点を通らない。

## このステップで分かったこと

- hardware VM exit と `KVM_RUN` の userspace 復帰は別の境界である。
- perf は KVM 内側、自作 counter は userspace 側を観察する。
- exit の回数と、exit から次の entry までの時間は別の指標である。

## 次のステップ

[Step 34: VM exit を分析する](step34_analyze-vm-exits.md) では、exit から次の entry までの時間分布を調べる。HLT の待機時間を分けて、負荷による違いを確認する。
