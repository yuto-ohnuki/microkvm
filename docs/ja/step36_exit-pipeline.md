# Step 36: KVM の exit パイプライン — guest 実行と exit 処理の全体像

## このステップのポイント

**KVM は VM exit 後に fastpath で処理できるかを判定し、その結果に応じて再突入や通常の exit 処理へ進む。** Step 35 の `handle_io` が、この全体のどこに位置するかを確認する。

```text
vcpu_enter_guest()
  → entry 準備（レジスタ・割り込み状態など）
  → guest 実行 → VM exit → fastpath 判定
       ↑                    ├─ 再突入可能 → 同じループへ
       └────────────────────┘
                            └─ それ以外 → host 側の状態を復元
                                           → handle_exit()
                                             結果の確認／必要なら通常 dispatch
```

`KVM_RUN` 1 回の中で `vcpu_enter_guest()` は繰り返し呼ばれ得る。また、1 回の `vcpu_enter_guest()` の中でも guest へ再突入する場合がある。**fastpath / 通常経路の区別と、userspace に戻るかどうかの区別は別である。**

## 観察の準備

Step 35 と同じく、ターミナル A で microkvm の Linux guest を起動し、ターミナル B でホストの tracefs を操作する。他の VM と perf は停止しておく。

ターミナル B で、対象関数がトレース可能かを確認する。

```bash
trace_dir=/sys/kernel/tracing
sudo grep 'vcpu_enter_guest' "$trace_dir/available_filter_functions"
```

`vcpu_enter_guest` または `vcpu_enter_guest.constprop.0` などが表示されることを確認する。以降の `vcpu_enter_guest*` は、このサフィックスにも対応する指定である。

## 観察手順と出力

### 1. 記録対象を設定する

ターミナル B で Step 35 の設定をクリアし、`vcpu_enter_guest` 以下を対象にする。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_ftrace_pid"
echo | sudo tee "$trace_dir/set_ftrace_filter"
echo | sudo tee "$trace_dir/set_ftrace_notrace"
echo | sudo tee "$trace_dir/set_graph_function"
echo | sudo tee "$trace_dir/set_graph_notrace"
echo function_graph | sudo tee "$trace_dir/current_tracer"
echo 'vcpu_enter_guest*' | sudo tee "$trace_dir/set_graph_function"
echo 6 | sudo tee "$trace_dir/max_graph_depth"
sudo cat "$trace_dir/set_graph_function"
```

最後の出力に対象関数名があることを確認する。`all functions enabled` と表示される場合は絞り込みができていないため、記録を始めず関数名と設定時のエラーを確認する。

### 2. guest を操作して記録を保存する

ターミナル B でバッファを空にして記録を開始する。

```bash
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

ターミナル A の guest で1回実行する。

```sh
/ # echo hi
```

プロンプトが戻ったら、ターミナル B ですぐに停止・保存する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-exit-pipeline.txt
head -120 ~/kvm-exit-pipeline.txt
```

### 3. 関数の並びを確認する

以下は観測した関数の並びから、主要部分を抜き出したもの。時間列と途中の呼び出しを省略し、`...` で示す。

```text
vcpu_enter_guest() {
  ...
  vt_prepare_switch_to_guest() {
    vmx_prepare_switch_to_guest() {
      ...
    }
  }
  vt_sync_pir_to_irr() {
    ...
  }
  ...
  kvm_load_guest_pkru();
  vt_vcpu_run() {
    ...
    vmx_exit_handlers_fastpath() {
      handle_fastpath_hlt() {
        kvm_emulate_halt() {
          ...
        }
      }
    }
  }
  kvm_load_host_pkru();
  ...
  vt_handle_exit() {
    vmx_handle_exit() {
      __vmx_handle_exit() {
        ...
      }
    }
  }
}
```

実際の出力には CPU 番号と `us` の時間列も付く。関数名や見える深さはカーネルの版・ビルドによって変わる。

まず `vt_vcpu_run` の前後を探し、entry 準備と host 側の状態復元を確認する。次に `vmx_exit_handlers_fastpath` と `vt_handle_exit` を探し、exit の処理経路を読む。先頭の120行で呼び出し全体が見えなければ、保存ファイルを続けて読む。

```bash
less ~/kvm-exit-pipeline.txt
```

`echo hi` の計測中でも、入力待ちの HLT が混ざる。上の例も HLT を捉えたものであり、すべての呼び出しに同じ handler が現れるわけではない。必要な内部関数が深さ制限で見えない場合は、`max_graph_depth` を `10` にして手順2から再計測する。

## 出力のどこを見るか

### 1. handler の前後にも処理がある

`vt_prepare_switch_to_guest` や `kvm_load_guest_pkru` は guest 実行の準備、`kvm_load_host_pkru` は host 側の状態復元に対応する。`vt_sync_pir_to_irr` は割り込み状態を同期する処理で、詳細は Step 37 で追う。

計測では `vcpu_enter_guest()` 1 回に約 41.8 us を観測した。この範囲には guest 実行も入るため、Step 34 の exit → 次 entry の時間とは比較しない。

### 2. fastpath の後にも handle_exit は現れる

この例では HLT を `handle_fastpath_hlt → kvm_emulate_halt` で処理している。その後に `vt_handle_exit` があっても、HLT をもう一度通常の handler で処理したという意味ではない。`handle_exit` は fastpath の結果を受け取り、通常 dispatch が必要かを判断する。

Step 35 の `handle_io` は通常 dispatch の先にある。一方、HLT を fastpath で処理したことだけでは、guest へ直ちに再突入したとは言えない。その条件をソースで確認する。

## KVM の実装と照合する

Linux v7.2 における [`vcpu_enter_guest()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L11112) の再突入ループでは、戻り値を次のように判定する（省略箇所はコメントで示す）。

```c
for (;;) {
    /* guest 実行前の処理を省略 */
    exit_fastpath = kvm_x86_call(vcpu_run)(vcpu, run_flags);
    if (likely(exit_fastpath != EXIT_FASTPATH_REENTER_GUEST))
        break;

    if (kvm_lapic_enabled(vcpu))
        kvm_x86_call(sync_pir_to_irr)(vcpu);

    if (unlikely(kvm_vcpu_exit_request(vcpu))) {
        exit_fastpath = EXIT_FASTPATH_EXIT_HANDLED;
        break;
    }
    /* run_flags と統計の更新を省略 */
}
/* host 側の状態復元などを省略 */
r = kvm_x86_call(handle_exit)(vcpu, exit_fastpath);
return r;
```

`EXIT_FASTPATH_REENTER_GUEST` の場合に再突入を試み、それ以外はループを抜ける。VMX では vcpu_run の先で `vmx_vcpu_run()` が呼ばれ、Step 33 の `kvm_exit` トレース後に [vmx_exit_handlers_fastpath()](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L7428) が判定する。

今回見えた [`handle_fastpath_hlt()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L11780) の戻り値は次のとおり。

```c
fastpath_t handle_fastpath_hlt(struct kvm_vcpu *vcpu)
{
    if (!kvm_pmu_is_fastpath_emulation_allowed(vcpu))
        return EXIT_FASTPATH_NONE;

    if (!kvm_emulate_halt(vcpu))
        return EXIT_FASTPATH_EXIT_USERSPACE;

    if (kvm_vcpu_running(vcpu))
        return EXIT_FASTPATH_REENTER_GUEST;

    return EXIT_FASTPATH_EXIT_HANDLED;
}
```

再突入できる状態なら `REENTER_GUEST`、halted 状態などで実行を続けられなければ `EXIT_HANDLED` となる。**fastpath は「VM exit しない」ことでも、「必ず即座に再突入する」ことでもない。**

## トレースを終了する

ターミナル B で設定を解除する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_graph_function"
echo | sudo tee "$trace_dir/set_event"
echo 0 | sudo tee "$trace_dir/max_graph_depth"
```

## このステップで分かったこと

- `KVM_RUN` の内側には、entry 準備・guest 実行・exit 処理の繰り返しがある。
- fastpath の戻り値によって、再突入や通常経路への移行が決まる。
- 関数の処理経路と、userspace に戻るかどうかは区別して読む。

## 次のステップ

[Step 37: IRQ delivery](step37_irq-exit.md) では、microkvm の `KVM_IRQ_LINE` / irqfd から guest に割り込みが届くまでを追い、今回見えた `vt_sync_pir_to_irr()` の位置づけを確認する。
