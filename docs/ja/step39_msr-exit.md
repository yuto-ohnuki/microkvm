# Step 39: MSR exit — exit の条件と処理先を分けて読む

## このステップのポイント

**MSR アクセスで VM exit が起きても、microkvm に処理が返るとは限らない。** 起動時の MSR 処理を記録し、KVM 内での処理・guest への例外・userspace 復帰を区別する。

MSR は CPU の制御・状態レジスタで、guest は `rdmsr` / `wrmsr` でアクセスする。通常の `rdmsr` は ECX で MSR 番号を指定し、EDX:EAX に値を受け取る。

```text
guest の rdmsr / wrmsr
  → hardware の intercept 設定
      ├─ intercept しない → MSR アクセスによる VM exit を省く
      └─ VM exit → KVM の MSR 処理
                     ├─ 成功 → 値や状態を反映して guest 再開
                     ├─ 拒否 → guest に #GP
                     └─ 設定された条件に一致 → userspace へ返す
```

VMX の MSR bitmap は read / write ごとの intercept を指定する仕組みで、KVM が管理する。一方、Step 8 の MSR filter と userspace MSR handling は、KVM がアクセスをどう扱うかの設定である。**VM exit の条件と、その後の処理先は別の判断になる。** bitmap の構造は Step 43 で扱う。

## 観察の準備

ターミナル A は microkvm、ターミナル B はホストのトレース操作に使う。他の VM と perf を止め、microkvm も停止しておく。今回は起動前から記録する。

ターミナル B で tracepoint を確認する。ホストの KVM モジュールはロード済みとする。

```bash
trace_dir=/sys/kernel/tracing
sudo cat "$trace_dir/events/kvm/kvm_msr/format"
```

`write`（read / write）、`ecx`（MSR 番号）、`data`、`exception` のフィールドを確認する。ログ中の MSR 番号は `1b` など、`0x` なしの16進表記になる。以降も同じターミナル B を使う。

## 観察手順と出力

### 1. 起動時の MSR 処理を保存する

ターミナル B で、以前のイベントを無効にして `kvm_msr` だけを記録する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_msr/filter"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_msr/enable"
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

その後、ターミナル A で起動する。

```bash
cd ~/microkvm
./microkvm
```

guest の `/ #` が表示されたら、ターミナル B で停止・保存する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-msr-boot.txt
head -60 ~/kvm-msr-boot.txt
```

### 2. MSR 番号と read / write を集計する

ターミナル B で保存したファイルを集計する。

```bash
grep -oE 'msr_(read|write)[[:space:]]+[0-9a-fA-Fx]+' ~/kvm-msr-boot.txt \
    | sort | uniq -c | sort -rn | head -30
```

計測では `msr_read 839` が 54 回、`msr_read 1a0` が 6 回だった。主な MSR の対応は次のとおり。回数や種類は guest・ホストの構成によって変わる。

| 観測した MSR 番号 | 内容 |
|------------------|------|
| `839`、`838`、`832` など | x2APIC レジスタ |
| `1a0` | IA32_MISC_ENABLE |
| `1b` | IA32_APIC_BASE |
| `c0000080` | EFER |
| `c0000081`、`c0000082`、`c0000084` | syscall に関する設定 |
| `4b564d07` など | KVM の準仮想化機能 |

この集計は例外となったアクセスも含む。KVM が正常に処理できたかどうかは、次に元の行の `(#GP)` を確認する。

### 3. 同じ記録から #GP を確認する

```bash
grep 'kvm_msr:.*#GP' ~/kvm-msr-boot.txt
```

出力例（抜粋）:

```text
kvm_msr: msr_read  1a6 = 0x0    (#GP)
kvm_msr: msr_read  1a7 = 0x0    (#GP)
kvm_msr: msr_read  3f6 = 0x0    (#GP)
kvm_msr: msr_read  3f7 = 0x0    (#GP)
kvm_msr: msr_write 1d9 = 0x4000 (#GP)
kvm_msr: msr_read  64e = 0x0    (#GP)
```

`(#GP)` は guest に一般保護例外を返す処理を示す。read の `= 0x0` は「正常に 0 を返した」という意味ではない。

この例では PMU 関連の `0x1a6` / `0x1a7` / `0x3f6` / `0x3f7` や DEBUGCTL（`0x1d9`）が現れた。これらは microkvm の MSR filter の対象ではなく、KVM がこの構成で拒否したアクセスである。

## 出力のどこを見るか

### 1. kvm_msr は userspace 復帰の記録ではない

`kvm_msr` は KVM の MSR 処理内に置かれた tracepoint である。APICBASE や EFER の処理が見えても、microkvm の `KVM_EXIT_X86_RDMSR` handler に届いたことにはならない。

microkvm が userspace に返すよう設定したのは `MSR_CUSTOM`（`0x20000000`）である。今回の Linux boot の記録には、この MSR のアクセスは現れていない。

### 2. ログにない MSR を passthrough と決めつけない

今回の集計では IA32_TSC（`0x10`）が見えなかった。ただし通常の時刻読み取りで使う `RDTSC` / `RDTSCP` は `RDMSR 0x10` とは別命令である。**ログにないことだけでは、MSR bitmap により exit を省いたのか、guest がその MSR にアクセスしなかったのかは分からない。**

## KVM の実装と照合する

Linux v7.2 の [`__kvm_emulate_rdmsr()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L2107) で、読み取り後の分岐を見る。以下は関数内の抜粋で、省略箇所はコメントで示す。

```c
/* 変数宣言を省略 */
r = kvm_emulate_msr_read(vcpu, msr, &data);

if (!r) {
    trace_kvm_msr_read(msr, data);
    /* 読み取った値を guest のレジスタへ反映 */
} else {
    if (kvm_msr_user_space(vcpu, msr, KVM_EXIT_X86_RDMSR, 0,
                          complete_rdmsr, r))
        return 0;
    trace_kvm_msr_read_ex(msr);
}

return kvm_x86_call(complete_emulated_msr)(vcpu, r);
```

成功時は値を guest に反映する。失敗時は、まず userspace に返す設定かを確認し、そうでなければ例外側のトレースを記録して完了処理へ進む。手順3の `(#GP)` はこの例外側に対応する。

[`kvm_msr_user_space()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L2085) は、失敗理由が userspace 向けに有効化されているかを確認する。

```c
u64 msr_reason = kvm_msr_reason(r);

if (!(vcpu->kvm->arch.user_space_msr_mask & msr_reason))
    return 0;

vcpu->run->exit_reason = exit_reason;
/* MSR 番号・データ・完了処理などの設定を省略 */
```

Step 8 では `KVM_MSR_EXIT_REASON_FILTER` を有効にし、MSR_CUSTOM の read / write だけを filter で拒否した。この組み合わせで `KVM_EXIT_X86_RDMSR` / `KVM_EXIT_X86_WRMSR` が microkvm に返る。**filter の拒否だけで、無条件に userspace へ返るわけではない。**

## トレースを終了する

ターミナル B でイベントを無効にする。保存ファイルは Step 40 の比較にも使える。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
```

## このステップで分かったこと

- MSR の VM exit 条件と、KVM が処理先を決める設定は区別する。
- `kvm_msr` の記録は userspace 復帰を意味せず、`(#GP)` は正常な読み取りではない。
- guest に提示する CPU 機能によって、起動時にアクセスする MSR も変わる。

## 次のステップ

[Step 40: CPUID exit](step40_cpuid-exit.md) では、guest に提示する PMU 情報を変更して MSR アクセスを比較する。比較では PMU 関連と DEBUGCTL の #GP が 8 件から 0 件になり、`0x64e` の #GP は残った。この差を CPUID と guest の初期化処理から確認する。
