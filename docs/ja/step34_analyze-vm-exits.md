# Step 34: VM exit を分析する — 測定時間の意味を読む

## このステップのポイント

**exit から次の entry までの時間には、KVM の処理だけでなく、userspace 往復や待機も含まれる。** Step 33 の回数・Time% に加え、Min / Avg / Max を見て、負荷による違いを確認する。

```text
kvm_exit (T1) → KVM の処理・userspace 往復・待機 → 次の kvm_entry (T2)
              └──── exit-to-reentry latency = T2 - T1 ────┘
```

この時間は hardware の VM exit 遷移単体のコストではない。今回の問いは「長い測定時間を、そのまま処理が重いと解釈してよいか」である。

## 何を使って観察するか

- `perf kvm stat live`: 負荷を変えたときの回数と時間を画面で確認する。
- `perf record` + `perf script`: exit / entry のイベント列を保存し、同じ記録から時間を集計する。

microkvm は1プロセス、vCPU は1個の構成を使う。以下は既存の測定例であり、時間や回数は環境・操作によって変わる。

## 観察手順と出力

### 1. live で負荷による違いを見る

ターミナル A で VM を起動する。以下の (1)〜(3) は、VM を起動し直して別々に計測する。

```bash
cd ~/microkvm
./microkvm
```

ターミナル B では、各パターンを次のコマンドで観察する。

```bash
sudo perf kvm stat live -p $(pgrep -x microkvm)
```

ターミナル A（guest）での操作:

**(1) idle**: `/ #` が表示されたら、何も入力せずに待つ。

**(2) 出力なし負荷**: 次を実行し、出力を `/dev/null` に捨てる。

```sh
/ # while true; do ls /proc >/dev/null; cat /proc/cpuinfo >/dev/null; done
```

**(3) 出力あり負荷**: 次を実行し、UART に出力し続ける。

```sh
/ # while true; do cat /proc/cpuinfo; done
```

(1) idle の出力例:

```text
                VM-EXIT  Samples  Samples%  Time%   Min Time   Max Time    Avg time
                    HLT      113   100.00%  100.00% 3851.97us  4092.26us  3977.68us ( +- 0.07% )
```

(2) 出力なし負荷 の出力例:

```text
                VM-EXIT  Samples  Samples%  Time%   Min Time   Max Time    Avg time
     EXTERNAL_INTERRUPT      106    80.30%   96.95%    9.90us    14.21us    10.88us ( +- 0.87% )
       PREEMPTION_TIMER       25    18.94%    1.88%    0.70us     1.14us     0.89us ( +- 2.15% )
     EXTERNAL_INTERRUPT        1     0.76%    1.17%   13.87us    13.87us    13.87us ( +- 0.00% )
```

(3) 出力あり負荷 の出力例:

```text
                VM-EXIT  Samples  Samples%  Time%   Min Time   Max Time    Avg time
         IO_INSTRUCTION     1459    98.98%   99.97%   10.02us  6265.74us   258.98us ( +- 11.94% )
     EXTERNAL_INTERRUPT        9     0.61%    0.02%    9.19us    10.34us     9.61us ( +- 1.31% )
       PREEMPTION_TIMER        5     0.34%    0.00%    0.53us     0.62us     0.55us ( +- 3.31% )
         IO_INSTRUCTION        1     0.07%    0.00%   10.33us    10.33us    10.33us ( +- 0.00% )
```

入力待ちでは HLT、UART に出力し続ける負荷では IO_INSTRUCTION が大半を占める。出力ありの IO_INSTRUCTION は Min 10.02 us に対して Max 6265.74 us と幅があり、Avg 258.98 us だけでは個々の処理時間を代表できない。

### 2. record でイベント列を保存して集計する

次は `dd` を使う。転送データは UART に出さないが、コマンド入力と終了時の表示には UART を使う。

```bash
# ターミナル B: 10 秒記録
sudo perf record -e 'kvm:kvm_exit,kvm:kvm_entry' -p $(pgrep -x microkvm) -o ~/kvm-dd.data -- sleep 10

# 記録開始直後、ターミナル A（guest）で実行
/ # dd if=/dev/zero of=/dev/null bs=1M count=500
```

1 vCPU の exit と次の entry を対にして集計する。記録末尾などで entry と対にならない exit は含めない。

```bash
$ sudo perf script -i ~/kvm-dd.data | awk '
  /kvm_exit/ { for(i=1;i<=NF;i++) if($i=="reason"){reason=$(i+1);break}
    for(i=1;i<=NF;i++) if($i ~ /^[0-9]+\.[0-9]+:$/){ts=$i;sub(/:$/,"",ts);exit_ts=ts+0}
    have_exit=1; next }
  /kvm_entry/ && have_exit {
    for(i=1;i<=NF;i++) if($i ~ /^[0-9]+\.[0-9]+:$/){ts=$i;sub(/:$/,"",ts);entry_ts=ts+0}
    dt=(entry_ts-exit_ts)*1e6; n[reason]++; sum[reason]+=dt
    if(mn[reason]==""||dt<mn[reason])mn[reason]=dt; if(dt>mx[reason])mx[reason]=dt; have_exit=0 }
  END {
    printf "%-20s %8s %10s %10s %10s\n","REASON","count","min_us","avg_us","max_us"
    for(r in n) printf "%-20s %8d %10.2f %10.2f %10.2f\n",r,n[r],mn[r],sum[r]/n[r],mx[r] | "sort -k2 -rn"
  }'
```

集計結果（単位 us）:

```text
REASON                  count     min_us     avg_us     max_us
HLT                      2496    1050.00    3974.76    4208.00
IO_INSTRUCTION            505       9.00      10.81      27.00
EPT_VIOLATION              56       5.00       6.36      45.00
EXTERNAL_INTERRUPT         17       8.00      10.12      17.00
MSR_READ                   10       6.00       7.30       8.00
PREEMPTION_TIMER            5       1.00       1.20       2.00
```

## 出力のどこを見るか

### 1. HLT の約 4 ms は待機を含む

HLT の Avg 3974.76 us は、vCPU が再開するまでの待機を含む。KVM の HLT 処理だけに約 4 ms かかったという意味ではなく、他の reason と分けて読む。

### 2. IO の時間差だけでは原因は分からない

record の IO_INSTRUCTION は Min 9 / Avg 10.81 / Max 27 us だった。live の出力あり負荷より短いが、**測定方法と負荷を同時に変えているため、差を端末 I/O や測定方法のせいと断定できない。** 原因を調べるには条件を揃えた比較が必要になる。

また、Min / Avg / Max で時間の幅は分かるが、「大多数が最小値付近だった」など分布の形までは分からない。

## KVM の実装と照合する

Step 33 と同じく、Linux v7.2 の [`vmx_vcpu_run()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L7505) を参照する。今回は二つのトレース地点と guest 実行の位置関係に注目する。

```c
fastpath_t vmx_vcpu_run(struct kvm_vcpu *vcpu, u64 run_flags)
{
    bool force_immediate_exit = run_flags & KVM_RUN_FORCE_IMMEDIATE_EXIT;
    struct vcpu_vmx *vmx = to_vmx(vcpu);

    /* entry 前の確認などを省略 */
    trace_kvm_entry(vcpu, force_immediate_exit);

    /* guest state・タイマなどの準備を省略 */
    vmx_vcpu_enter_exit(vcpu, __vmx_vcpu_enter_flags(vmx));

    /* host state の復元・失敗時の分岐などを省略 */
    trace_kvm_exit(vcpu, KVM_ISA_VMX);

    /* exit 後の処理を省略 */
    return vmx_exit_handlers_fastpath(vcpu, force_immediate_exit);
}
```

`trace_kvm_entry()` は guest 実行の前、`trace_kvm_exit()` は制御が KVM に戻った後に置かれている。今回集計するのは、**ある呼び出しの exit から、次の呼び出しの entry まで**の時刻差である。同じ呼び出し内の entry → exit ではない。

この配置から、計測区間には exit 後の処理や待機が入り、hardware の切り替え時刻そのものを測っているわけではないと分かる。

## このステップで分かったこと

- exit-to-reentry latency は処理と待機を含む経過時間である。
- 平均だけで判断せず、時間の幅と負荷の内容を合わせて読む。
- 遅延原因を切り分けるには、測定方法と負荷の条件を揃える。

## 次のステップ

[Step 35: KVM 内部をトレースする](step35_trace-kvm-internals.md) では、ftrace で PIO exit の処理経路を追い、時間の内側でどの関数が実行されるかを確認する。
