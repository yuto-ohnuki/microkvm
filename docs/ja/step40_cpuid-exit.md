# Step 40: CPUID exit — 提示する CPU 機能が guest の動作を変える

## このステップのポイント

**CPUID は guest が利用する CPU 機能を決める情報であり、応答を変えると guest の初期化処理も変わる。** 起動時の CPUID を観察し、PMU（性能監視機能）の提示を止めた場合に、Step 39 の MSR アクセスがどう変わるか比較する。

```text
microkvm が KVM_SET_CPUID2 で応答を登録
  → guest の cpuid → VM exit → KVM が登録内容を基に応答
  → guest が提示された機能に応じて初期化
```

microkvm は `KVM_GET_SUPPORTED_CPUID` で KVM が提供可能な応答を取得し、TSC-Deadline の bit を落として vCPU に登録する。ホストの生の CPUID をそのまま渡しているわけではない。今回の通常設定では PMU leaf `0xA` を残している。

CPUID の実行時の処理は KVM 内で完結する。hardware VM exit は起こるが、microkvm に CPUID 用の exit handler は不要である。

## 観察の準備

ターミナル A は microkvm のビルド・起動、ターミナル B はホストの記録操作に使う。他の VM と perf を止め、microkvm も停止した状態から始める。ホストの KVM モジュールはロード済みとする。

ターミナル B で二つのイベントを確認する。

```bash
trace_dir=/sys/kernel/tracing
sudo cat "$trace_dir/events/kvm/kvm_cpuid/format"
sudo ls "$trace_dir/events/kvm/kvm_msr/enable"
```

`kvm_cpuid` の `function` は leaf、`index` は subleaf、`rax/rbx/rcx/rdx` は応答、`found` はエントリが見つかったかを示す。以下では CPUID と MSR を同時に記録する。

## 観察手順と出力

### 1. 通常設定の起動を記録する

ターミナル B で設定し、記録を開始する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
for e in kvm_cpuid kvm_msr; do
    echo 0 | sudo tee "$trace_dir/events/kvm/$e/filter"
    echo 1 | sudo tee "$trace_dir/events/kvm/$e/enable"
done
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

ターミナル A で起動する。

```bash
cd ~/microkvm
./microkvm
```

guest の `/ #` が表示されたら、ターミナル B で停止・保存する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-cpuid-baseline.txt
grep 'kvm_cpuid:' ~/kvm-cpuid-baseline.txt | head -30
```

応答部分の出力例（抜粋）:

```text
func 0        rax 24 rbx 756e6547 rcx 6c65746e rdx 49656e69  found
func 1        rax a06d1 rbx 2100800 rcx f6fab223 rdx f8bfbff found
func a idx 0  rax 8300802 rbx 0 rcx 0 rdx 8602               found
func 40000000 rax 40000001 rbx 4b4d564b rcx 564b4d56 rdx 4d  found
```

注目するのは次の二つである。

- `func a` の EAX（表示は `rax`）`0x08300802`: 下位8 bit が PMU version `2`、次の8 bit が汎用カウンタ数 `8` を示す。
- `func 40000000`: KVM の署名 `KVMKVMKVM` を返す。guest は次の leaf `0x40000001` の機能情報と合わせて KVM の準仮想化機能を検出する。

起動ログではすべて `found` だった。要求したエントリが見つかったという意味であり、この表示だけで個々の機能の動作まで検証したことにはならない。

必要なら、同じ保存ファイルから leaf ごとの回数を集計できる。

```bash
grep 'kvm_cpuid:' ~/kvm-cpuid-baseline.txt \
    | grep -oE 'func [0-9a-fA-F]+' | sort | uniq -c | sort -rn
```

### 2. PMU の提示だけを変更して再ビルドする

ターミナル A の VM を終了する。`microkvm.c` の CPUID 設定部分で、leaf 1 の TSC-Deadline bit を落とす既存ループの後、`KVM_SET_CPUID2` を呼ぶ直前に、次のループを一時的に追加する。

```c
/* 実験用: PMU を guest に提示しない */
for (int j = 0; j < (int)cpuid.header.nent; j++) {
    if (cpuid.entries[j].function == 0xA) {
        cpuid.entries[j].eax = 0;
        cpuid.entries[j].ebx = 0;
        cpuid.entries[j].ecx = 0;
        cpuid.entries[j].edx = 0;
    }
}
```

既存の leaf 1 用ループには `break` があるため、別のループとして追加する。他の CPUID 設定や guest kernel は変更せず、ターミナル A で再ビルドする。

```bash
make
```

この時点ではまだ起動しない。次の記録開始を先に行う。

### 3. 変更後の起動を同じ条件で記録する

ターミナル B で、手順1のイベント設定を使って記録を開始する。

```bash
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

ターミナル A で変更後の VM を起動する。

```bash
./microkvm
```

guest の `/ #` が表示されたら、ターミナル B で停止して別ファイルに保存する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-cpuid-no-pmu.txt
grep -E 'kvm_cpuid:.*func a([[:space:]]|$)' ~/kvm-cpuid-no-pmu.txt
```

まず leaf `a` の `rax/rbx/rcx/rdx` がすべて `0` になったことを確認する。行がない場合や値が変わっていない場合は、記録開始のタイミング、追加位置、ビルドと起動した実行ファイルを確認する。

### 4. MSR の #GP を比較する

ターミナル B で二つのファイルを同じ条件で集計する。

```bash
$ for log in ~/kvm-cpuid-baseline.txt ~/kvm-cpuid-no-pmu.txt; do
    echo "$log"
    grep 'kvm_msr:.*#GP' "$log" \
        | grep -Ec 'msr_(read|write)[[:space:]]+(1a6|1a7|3f6|3f7|1d9)[[:space:]]'
done
/home/fedora/kvm-cpuid-baseline.txt
8
/home/fedora/kvm-cpuid-no-pmu.txt
0
```

計測では、対象 MSR の #GP が **通常設定で8件、PMU 非公開で0件**になった。件数は環境によって変わるので、再計測では自身の通常設定との差を見る。通常設定から0件なら、この環境では同じ #GP の減少は比較できない。

`#GP` の消失とアクセス自体の減少を区別するため、例外の条件を外して対象 MSR の行も比較する。

```bash
$ for log in ~/kvm-cpuid-baseline.txt ~/kvm-cpuid-no-pmu.txt; do
    echo "$log"
    grep -E 'kvm_msr:.*msr_(read|write)[[:space:]]+(1a6|1a7|3f6|3f7|1d9)[[:space:]]' "$log"
done
/home/fedora/kvm-cpuid-baseline.txt
        microkvm-14196   [001] ..... 15326.957001: kvm_msr: msr_read 1a6 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.957013: kvm_msr: msr_read 1a7 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.957023: kvm_msr: msr_read 3f6 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.957033: kvm_msr: msr_read 3f7 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.957043: kvm_msr: msr_read 3f7 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.957053: kvm_msr: msr_read 3f7 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.957063: kvm_msr: msr_read 3f7 = 0x0 (#GP)
        microkvm-14196   [001] ..... 15326.973043: kvm_msr: msr_read 1d9 = 0x0
        microkvm-14196   [001] ..... 15326.973053: kvm_msr: msr_write 1d9 = 0x4000 (#GP)
/home/fedora/kvm-cpuid-no-pmu.txt
```

比較対象の PMU 関連 MSR と DEBUGCTL の #GP は消えた一方、`0x64e` の #GP は変更後も残った。PMU の提示を止めても、すべての #GP がなくなるわけではない。

```bash
$ grep -E 'kvm_msr:.*msr_read[[:space:]]+64e[[:space:]].*#GP' \
    ~/kvm-cpuid-baseline.txt ~/kvm-cpuid-no-pmu.txt
/home/fedora/kvm-cpuid-baseline.txt:        microkvm-14196   [001] ..... 15327.220312: kvm_msr: msr_read 64e = 0x0 (#GP)
/home/fedora/kvm-cpuid-no-pmu.txt:        microkvm-14589   [002] ..... 15467.774987: kvm_msr: msr_read 64e = 0x0 (#GP)
```

## KVM の実装と照合する

### 登録した CPUID に KVM が応答する

Linux v7.2 の [`kvm_emulate_cpuid()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/cpuid.c#L2160) は、guest の要求を読み取り、応答をレジスタに書き戻す。以下は関数内の抜粋。

```c
/* 変数宣言・CPUID 実行可否の確認を省略 */
eax = kvm_eax_read(vcpu);
ecx = kvm_ecx_read(vcpu);
kvm_cpuid(vcpu, &eax, &ebx, &ecx, &edx, false);
kvm_eax_write(vcpu, eax);
kvm_ebx_write(vcpu, ebx);
kvm_ecx_write(vcpu, ecx);
kvm_edx_write(vcpu, edx);
return kvm_skip_emulated_instruction(vcpu);
```

EAX が leaf、ECX が subleaf で、`kvm_cpuid()` が vCPU の CPUID エントリを基に応答する。その結果を guest に反映して命令を進めるため、microkvm が実行時に応答する必要はない。

### leaf 0xA は KVM 内の PMU 構成にも使われる

CPUID の設定更新は `kvm_pmu_refresh()` にもつながる。[`intel_pmu_refresh()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/pmu_intel.c#L524) は次のように leaf `0xA` を参照する。

```c
/* 関数前半の処理を省略 */
entry = kvm_find_cpuid_entry(vcpu, 0xa);
if (!entry)
    return;

eax.full = entry->eax;
edx.full = entry->edx;

pmu->version = eax.split.version_id;
if (!pmu->version)
    return;
```

したがって、今回の変更は guest の機能検出と KVM 内の仮想 PMU 構成の両方に影響する。比較結果は「PMU の提示によって起動時の MSR アクセスが変わる」ことを示す。PMU を提示しただけで、モデル固有のすべての PMU 関連 MSR に対応するという意味ではない。

## 実験を終了して元に戻す

ターミナル A の VM を終了し、**手順2で追加した実験用ループだけ**を削除して `make` を実行する。

ターミナル B で記録を解除する。比較用のログは保存しておく。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
```

## このステップで分かったこと

- CPUID の応答は起動時に登録し、guest の命令は KVM 内で処理する。
- 提示する CPU 機能を変えると、guest の初期化と MSR アクセスが変わる。
- CPUID は guest に見せる情報だけでなく、KVM 内部の機能構成にも使われる。

## Part 2 を終えて

Step 33〜40 では、hardware VM exit と userspace 復帰を区別し、perf / ftrace の観察を KVM のソースと照合した。回数や時間だけで判断せず、どこで記録された値か、何を変更した比較かを確認することが、KVM と microkvm の役割を理解する手掛かりになる。
