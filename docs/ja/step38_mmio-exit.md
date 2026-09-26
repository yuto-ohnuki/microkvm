# Step 38: MMIO exit — EPT の exit から KVM_EXIT_MMIO まで

## このステップのポイント

**`kvm_page_fault` が見えなくなっても、MMIO の VM exit が消えたとは限らない。** 今回の virtio-mmio では、初回の EPT violation と、MMIO SPTE 作成後の EPT misconfiguration を観察する。

```text
guest の MMIO アクセス
  → EPT violation / EPT misconfiguration（hardware VM exit）
  → KVM が MMIO を識別・命令をエミュレート
  → KVM_EXIT_MMIO（userspace への復帰理由）
  → microkvm のデバイス処理
```

EPT は guest 物理アドレス（GPA）から host 物理アドレスへの変換を担う。virtio-mmio の `0xd0000000` は RAM memslot として登録されておらず、今回のアクセスは KVM の MMIO 処理へ進む。RAM へのアクセスでも EPT mapping の作成などで violation は起こるため、EPT violation と MMIO は同義ではない。

## 観察の準備

Intel VMX / EPT 環境を使う。ターミナル A は microkvm と guest の操作、ターミナル B はホストのトレース操作用とする。他の VM と perf を止め、**最初は microkvm も停止した状態**にする。起動前から記録して初回アクセスを捕捉するためである。

ターミナル B で必要なイベントと関数を確認する（ホストの KVM モジュールはロード済みとする）。

```bash
trace_dir=/sys/kernel/tracing
for e in kvm_mmio kvm_page_fault kvm_emulate_insn vcpu_match_mmio; do
    sudo ls "$trace_dir/events/kvm/$e/enable"
done
sudo grep 'handle_ept_misconfig' "$trace_dir/available_filter_functions"
```

四つのイベントと `handle_ept_misconfig` またはサフィックス付きの関数名が見えることを確認する。以降も同じターミナル B を使う。

## 観察手順と出力

### 1. 起動時の初回アクセスを記録する

ターミナル B で tracepoint を設定し、記録を開始する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
for e in kvm_mmio kvm_page_fault kvm_emulate_insn vcpu_match_mmio; do
    echo 0 | sudo tee "$trace_dir/events/kvm/$e/filter"
    echo 1 | sudo tee "$trace_dir/events/kvm/$e/enable"
done
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

その後、ターミナル A で起動する。今回の観察では ioeventfd / irqfd を使わない。

```bash
cd ~/microkvm
unset USE_IOEVENTFD USE_IRQFD
./microkvm
```

guest の `/ #` が表示されたら、ターミナル B で停止・保存する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-mmio-boot.txt
grep -iE 'd000[0-9a-f]{4}' ~/kvm-mmio-boot.txt | head -40
```

出力例（virtio-mmio の初回アクセスを抜粋）:

```text
kvm_page_fault: rip 0xffffffff8134c508 address 0xd0000000 error_code 0x181
vcpu_match_mmio: gpa 0xd0000000 Read GPA
kvm_mmio: mmio unsatisfied-read len 4 gpa 0xd0000000 val 0x0
kvm_mmio: mmio read len 4 gpa 0xd0000000 val 0x74726976
```

`0x74726976` は Part 1 で実装した virtio の MagicValue である。この記録では初回に `kvm_page_fault` が現れ、同じページの `0xd0000004` などへの後続アクセスでは現れなかった。

起動時には RAM 側の fault も多いため、virtio のアドレスに絞って読む。初回の行が見つからない場合は、記録開始が VM 起動より前だったかを確認する。保存ファイルのヘッダで `entries-written` が `entries-in-buffer` を上回っていれば、古い記録が上書きされた可能性もある。

### 2. 起動後の MMIO 読み書きを記録する

VM は起動したまま、ターミナル A の guest で virtio の受信側を用意する。

```text
/ # cat /dev/hvc0 &
/ # < Ctrl-A v >
[monitor] input → hvc0 (virtio)
```

ターミナル B で、手順1のイベント設定を使って新しく記録する。

```bash
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

ターミナル A で `abc` と入力して Enter を押す。ターミナル B で停止・保存する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-mmio-runtime.txt
head -80 ~/kvm-mmio-runtime.txt
```

出力例（1 回の read と後続の write を抜粋）:

```text
microkvm-30942 kvm_emulate_insn: 0:ffffffff8134be37:44 8b 60 60 (prot64)
microkvm-30942 vcpu_match_mmio:  gva 0xffffc90000005060 gpa 0xd0000060 Read GPA
microkvm-30942 kvm_mmio: mmio unsatisfied-read len 4 gpa 0xd0000060 val 0x0
microkvm-30942 kvm_mmio: mmio read len 4 gpa 0xd0000060 val 0x1
microkvm-30942 kvm_emulate_insn: 0:ffffffff8134be42:44 89 60 64 (prot64)
microkvm-30942 vcpu_match_mmio:  gva 0xffffc90000005064 gpa 0xd0000064 Write GPA
microkvm-30942 kvm_mmio: mmio write len 4 gpa 0xd0000064 val 0x1
```

見る箇所は三つある。

- `kvm_emulate_insn`: guest の命令バイト。今回の read は `mov 0x60(%rax), %r12d` で、KVM の命令エミュレーションを通っている。
- `unsatisfied-read → read`: KVM 内で満たせなかった読み取りを microkvm が処理し、値 `0x1` を返している。`0xd0000060` は InterruptStatus、後続の `0xd0000064` は InterruptACK への書き込みである。
- `vcpu_match_mmio`: vCPU に保持された MMIO 情報との照合。GVA/GPA が表示されるが、この関数がアドレス変換を行ったという意味ではない。

この計測では対象ページの `kvm_page_fault` が見えなかった。次は別の入口を記録し、VM exit が続いているかを確認する。

### 3. EPT misconfiguration の経路を確認する

ターミナル B で function_graph に切り替える。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_ftrace_pid"
echo | sudo tee "$trace_dir/set_ftrace_filter"
echo | sudo tee "$trace_dir/set_ftrace_notrace"
echo | sudo tee "$trace_dir/set_graph_function"
echo | sudo tee "$trace_dir/set_graph_notrace"
echo function_graph | sudo tee "$trace_dir/current_tracer"
echo 'handle_ept_misconfig*' | sudo tee "$trace_dir/set_graph_function"
echo 10 | sudo tee "$trace_dir/max_graph_depth"
sudo cat "$trace_dir/set_graph_function"
```

対象関数名が表示されることを確認して記録を開始する。

```bash
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

ターミナル A は引き続き virtio 入力のまま、再び数文字入力して Enter を押す。ターミナル B で停止・保存する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-mmio-functions.txt
head -100 ~/kvm-mmio-functions.txt
grep -c 'handle_ept_misconfig.*() {' ~/kvm-mmio-functions.txt
```

観測した関数経路（CPU・時間の列と一部の呼び出しを省略）:

```text
handle_ept_misconfig.part.0 [kvm_intel]() {
  kvm_io_bus_write [kvm]() { ... }
  kvm_mmu_page_fault [kvm]() {
    handle_mmio_page_fault [kvm]() {
      get_sptes_lockless → kvm_tdp_mmu_get_walk
    }
    x86_emulate_instruction [kvm]() {
      x86_decode_insn { ... }
    }
  }
}
```

計測では `handle_ept_misconfig` が 36 回記録された。回数は入力などで変わり、この関数の記録対象も virtio だけには限定していない。ここで確認するのは、**起動後も EPT misconfiguration を処理する経路が実行されている**ことである。

## KVM の実装と照合する

### page-fault のイベントがない理由

Linux v7.2 の [`handle_ept_violation()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L5967) 内には `trace_kvm_page_fault()` がある。一方、[`handle_ept_misconfig()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L5983) は別の経路で MMU 処理を呼ぶ。

```c
/* handle_ept_misconfig() 内。前半の確認処理を省略 */
gpa = vmcs_read64(GUEST_PHYSICAL_ADDRESS);
if (!is_guest_mode(vcpu) &&
    !kvm_io_bus_write(vcpu, KVM_FAST_MMIO_BUS, gpa, 0, NULL)) {
    trace_kvm_fast_mmio(gpa);
    return kvm_skip_emulated_instruction(vcpu);
}

return kvm_mmu_page_fault(vcpu, gpa, PFERR_RSVD_MASK, NULL, 0);
```

最初の `kvm_io_bus_write` は `KVM_FAST_MMIO_BUS` の処理を試すもの。ここで処理できなければ `kvm_mmu_page_fault()` へ進む。**関数名に page_fault があっても、`kvm_page_fault` イベントが必ず記録されるわけではない。**

### MMIO SPTE は exit 後の識別を助ける

MMIO SPTE は、既知の MMIO ページを示す特別なページテーブルエントリである。今回の EPT 構成では、そのエントリへのアクセスが misconfiguration を起こす。KVM は exit 後、vCPU の MMIO キャッシュや SPTE を確認する。

[`handle_mmio_page_fault()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/mmu/mmu.c#L4505) の冒頭では、キャッシュに一致すればエミュレーションへ進む結果を返す。

```c
if (mmio_info_in_cache(vcpu, addr, direct))
    return RET_PF_EMULATE;
```

一致しなければ SPTE を調べ、MMIO SPTE として有効なら同じ `RET_PF_EMULATE` を返す。**MMIO SPTE は VM exit を消す仕組みではなく、exit 後に MMIO を識別する処理を短縮する仕組みである。**

その後、microkvm に届くのは `run->mmio` のアドレス・長さ・方向・データである。microkvm は命令をデコードせず、アドレス範囲から `virtio_mmio_read()` / `virtio_mmio_write()` を呼ぶ。Step 35 の PIO と比べると、今回の MMIO 経路には命令デコードが現れるが、これだけで処理全体の速度差は判断しない。

## トレースを終了する

ターミナル A の入力を戻す。

```text
< Ctrl-A v >
[monitor] input → ttyS0 (UART)
```

ターミナル B で設定を解除する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_graph_function"
echo 0 | sudo tee "$trace_dir/max_graph_depth"
```

## このステップで分かったこと

- hardware の EPT exit と、userspace に返す `KVM_EXIT_MMIO` は別の段階である。
- `kvm_page_fault` が見えなくても、EPT misconfiguration の経路で exit は続き得る。
- KVM が MMIO の識別と命令エミュレーションを行い、microkvm はデバイスの読み書きを処理する。

## 次のステップ

[Step 39: MSR exit](step39_msr-exit.md) では、guest の `rdmsr` / `wrmsr` が exit する条件と、KVM 内で処理するか userspace に返すかの違いを追う。
