# Step 35: KVM 内部をトレースする — PIO が userspace に届くまで

## このステップのポイント

**microkvm の UART への PIO は、KVM 内の I/O バスで処理されず、`KVM_EXIT_IO` として userspace に返る。** この経路を、出来事を記録する tracepoint と、関数の呼び出し関係を記録する function_graph で確認する。

```text
guest の UART アクセス → KVM の PIO handler → in-kernel I/O バス
                                               ↓ 処理されない
                           KVM_EXIT_IO → microkvm の uart_in() / uart_out()
```

Step 34 では時間を測った。今回は「その間にどの関数を通り、なぜ microkvm に戻るのか」を追う。

## 観察の準備

Intel VMX 環境で microkvm を1プロセスだけ起動する。ターミナル A は guest 操作用、ターミナル B はホストでのトレース操作用とする。Step 34 の perf は終了しておく。

ターミナル A で起動し、guest の `/ #` まで待つ。

```bash
cd ~/microkvm
./microkvm
```

ターミナル B で tracefs と必要な機能を確認する。一般ユーザーではディレクトリに入れない場合があるため、`cd` は使わず、各ファイルを `sudo` で操作する。以降も同じターミナル B を使う。

```bash
trace_dir=/sys/kernel/tracing
sudo cat "$trace_dir/available_tracers"
sudo ls "$trace_dir/events/kvm/kvm_pio/enable" "$trace_dir/events/kvm/kvm_userspace_exit/enable"
sudo grep -w handle_io "$trace_dir/available_filter_functions"
```

`function_graph`、二つのイベントの `enable` ファイル、`handle_io` が見えれば先に進める。見つからない場合は、ホストの tracefs のマウント、KVM モジュールのロード、カーネルの tracing 設定を確認する。

以下では vCPU スレッドを取りこぼさないよう PID フィルタを外す。他の VM のイベントも対象になるため、観察中は他の VM を動かさない。

## 観察手順と出力

### 1. tracepoint で PIO と userspace 復帰を見る

ターミナル B で以前の設定とバッファをクリアし、二つのイベントだけを記録する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_pio/enable"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_userspace_exit/enable"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

記録開始後、ターミナル A の guest で1回実行する。

```sh
/ # echo hello world
```

プロンプトが戻ったらターミナル B で記録を止め、ファイルに保存して読む。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-pio-events.txt
head -60 ~/kvm-pio-events.txt
```

出力例（抜粋。`...` は省略）:

```text
microkvm-25070 [006] 95526.463450: kvm_pio: pio_read  at 0x3fd size 1 val 0x60
microkvm-25070 [006] 95526.463470: kvm_userspace_exit: reason KVM_EXIT_IO (2)
microkvm-25070 [006] 95526.463495: kvm_pio: pio_read  at 0x3f8 ...
microkvm-25070 [006] 95526.463691: kvm_pio: pio_write at 0x3f8 size 1 val 0xd
microkvm-25070 [006] 95526.463769: kvm_pio: pio_write at 0x3f8 size 1 val 0xa
microkvm-25070 [006] 95526.463789: kvm_pio: pio_write at 0x3f9 size 1 val 0x5
```

見るのはポートと復帰理由である。`0x3fd` は UART の状態、`0x3f8` は文字データ、`0x3f9` は割り込み許可のレジスタに対応する。`KVM_EXIT_IO` は、PIO の処理を userspace に返したことを示す。

同じ保存ファイルで回数も確認できる。

```bash
grep -c 'kvm_pio:' ~/kvm-pio-events.txt
grep -c 'kvm_userspace_exit:.*KVM_EXIT_IO' ~/kvm-pio-events.txt
```

計測では両方とも 398 回だった。文字以外のレジスタアクセスや、入力のエコー・プロンプト出力も含むため、`hello world` の文字数とは一致しない。read の `kvm_pio` は値を受け取った完了側で記録されるので、直後の `kvm_userspace_exit` と同じアクセスだとは限らない。

### 2. function_graph で PIO handler の中を見る

次は `handle_io` 以下の呼び出しを記録する。ターミナル B で tracepoint を無効にし、関数トレースへ切り替える。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_ftrace_pid"
echo | sudo tee "$trace_dir/set_ftrace_filter"
echo | sudo tee "$trace_dir/set_ftrace_notrace"
echo | sudo tee "$trace_dir/set_graph_notrace"
echo function_graph | sudo tee "$trace_dir/current_tracer"
echo handle_io | sudo tee "$trace_dir/set_graph_function"
echo 10 | sudo tee "$trace_dir/max_graph_depth"
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

ターミナル A の guest で1回実行する。

```sh
/ # echo hello
```

プロンプトが戻ったらターミナル B で停止・保存する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-pio-functions.txt
head -80 ~/kvm-pio-functions.txt
```

観測した読み取り経路（時刻・所要時間の列と一部の呼び出しを省略）:

```text
handle_io [kvm_intel]() {
  kvm_fast_pio [kvm]() {
    emulator_pio_in_out [kvm]() {
      kvm_io_bus_read [kvm]() {
        kvm_io_bus_get_first_dev [kvm]() {
          kvm_io_bus_sort_cmp [kvm]();
          kvm_io_bus_sort_cmp [kvm]();
          kvm_io_bus_sort_cmp [kvm]();
        }
      }
    }
    kvm_get_linear_rip [kvm]() { ... }
  }
}
```

`handle_io → kvm_fast_pio → emulator_pio_in_out → kvm_io_bus_read` をたどる。書き込みなら `kvm_io_bus_write` が現れる。先頭に読み取り経路がなければ、保存ファイル内の `kvm_io_bus_read` を検索する。

`kvm_get_linear_rip()` は現在の命令の線形アドレスを取得する。この経路では、userspace 復帰後の PIO 完了処理に備えて保存している。

このツリーから I/O バスを探索したことは分かるが、返り値までは分からない。userspace に戻る条件は、次のソースで確認する。なお、function_graph の範囲は kernel 内の `handle_io` 以下であり、microkvm の端末出力時間は含まない。

## KVM の実装と照合する

Linux v7.2 の [`emulator_pio_in_out()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L8390) では、I/O バスの処理結果を見て userspace 用の情報を設定する。以下は分岐に注目した抜粋で、省略箇所はコメントで示す。

```c
/* 関数冒頭の変数宣言・確認処理を省略 */
for (i = 0; i < count; i++) {
    if (in)
        r = kvm_io_bus_read(vcpu, KVM_PIO_BUS, port, size, data);
    else
        r = kvm_io_bus_write(vcpu, KVM_PIO_BUS, port, size, data);

    if (r) {
        if (i == 0)
            goto userspace_io;
        /* 途中から処理できなくなった場合の処理を省略 */
    }
    /* data の更新を省略 */
}
return 1;
userspace_io:
/* PIO データの準備を省略 */
vcpu->run->exit_reason = KVM_EXIT_IO;
/* direction / size / count / port などの設定を省略 */
return 0;
```

今回の UART は microkvm がエミュレートしており、in-kernel I/O バスで処理されない。そのため `userspace_io` に進み、`KVM_EXIT_IO` が設定される。これが tracepoint で見た復帰理由につながる。

microkvm の `case KVM_EXIT_IO:` は UART のポート範囲を判定し、読み取りを `uart_in()`、書き込みを `uart_out()` に渡す。書き込みを例に一往復を並べると次のようになる（処理の対応図であり、一回分の生トレースではない）。

```text
guest の out 命令
  → VM exit (IO_INSTRUCTION)
  → handle_io → kvm_fast_pio → kvm_io_bus_write
  → KVM_EXIT_IO で KVM_RUN が戻る
  → microkvm の uart_out() が処理
  → 次の KVM_RUN → guest 再開
```

## トレースを終了する

ターミナル B でトレース設定を解除する。保存した二つのファイルは後から読み直せる。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_graph_function"
echo | sudo tee "$trace_dir/set_event"
echo 0 | sudo tee "$trace_dir/max_graph_depth"
```

記録が空なら、guest 操作中に `tracing_on` が `1` だったかと、準備で確認したイベント・関数が利用可能かを確認する。設定項目の詳細は [ftrace のドキュメント](https://www.kernel.org/doc/html/latest/trace/ftrace.html) を参照。

## このステップで分かったこと

- tracepoint は PIO と userspace 復帰、function_graph は関数の呼び出し経路を示す。
- I/O バスで処理されない UART アクセスは、`KVM_EXIT_IO` で microkvm に届く。
- UART は文字以外のレジスタも操作するため、文字数と PIO 回数は一致しない。

## 次のステップ

[Step 36: KVM の exit パイプライン](step36_exit-pipeline.md) では、`handle_io` に至る前段も含め、VM entry・exit・handler 呼び出しの全体を追う。