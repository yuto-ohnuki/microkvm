# Step 37: IRQ delivery — GSI から guest の vector まで

## このステップのポイント

**microkvm が渡す GSI 番号と、guest CPU が受ける vector は別である。** UART の IRQ4 を例に、KVM が割り込みを配送する経路を確認し、irqfd ではその入口がどう変わるかを見る。

```text
microkvm: GSI 4 を通知
  → KVM の routing table → IOAPIC の pin 4
  → guest が設定した vector・宛先に従って LAPIC へ配送
```

GSI は割り込み線の論理番号、vector は CPU が割り込み処理に使う番号である。microkvm は `KVM_CREATE_IRQCHIP` で割り込みコントローラを KVM に用意させ、UART の注入では `KVM_IRQ_LINE` に GSI 4 と level を渡す。vector を指定するのは UART の注入コードではなく、IOAPIC を設定する guest 側である。

## 観察の準備

ターミナル A は microkvm と guest の操作、ターミナル B はホストのトレース操作に使う。他の VM と perf は停止しておく。

ターミナル A で通常起動し、guest の `/ #` まで待つ。

```bash
cd ~/microkvm
unset USE_IOEVENTFD USE_IRQFD
./microkvm
```

ターミナル B で対象イベントと関数を確認する。以降も同じターミナル B を使う。

```bash
trace_dir=/sys/kernel/tracing
for e in kvm_set_irq kvm_ioapic_set_irq kvm_apic_accept_irq; do
    sudo ls "$trace_dir/events/kvm/$e/enable"
done
sudo grep -w kvm_set_irq "$trace_dir/available_filter_functions"
```

三つの `enable` ファイルと `kvm_set_irq` が表示されることを確認する。記録の設定は Step 35 と同様に `sudo` で各ファイルを操作する。

## 観察手順と出力

### 1. UART の GSI と vector を見る

ターミナル B で tracepoint を設定し、記録を開始する。PID フィルタは、後で kworker の処理も拾えるよう外す。vector は環境によって変わるため、特定の番号を除外せず記録する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
for e in kvm_set_irq kvm_ioapic_set_irq kvm_apic_accept_irq; do
    echo 0 | sudo tee "$trace_dir/events/kvm/$e/filter"
    echo 1 | sudo tee "$trace_dir/events/kvm/$e/enable"
done
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

ターミナル A の guest で `ls` を実行する。入力・エコー・出力に伴って UART の割り込みが発生する。

```sh
/ # ls
```

プロンプトが戻ったらターミナル B で停止・保存し、GSI 4 周辺を読む。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-irq-uart.txt
grep -A 4 'kvm_set_irq: gsi 4 level 1' ~/kvm-irq-uart.txt | head -30
```

出力例（抜粋）:

```text
microkvm [012] 98222.234315: kvm_set_irq: gsi 4 level 1 source 0
microkvm [012] 98222.234319: kvm_apic_accept_irq: apicid 0 vec 33 (Fixed|edge)
microkvm [012] 98222.234321: kvm_ioapic_set_irq: pin 4 dst 0 vec 33 (Fixed|physical|edge)
microkvm [012] 98222.234322: kvm_set_irq: gsi 4 level 0 source 0
microkvm [012] 98222.234322: kvm_ioapic_set_irq: pin 4 dst 0 vec 33 (Fixed|physical|edge)
```

この例では **GSI 4 → pin 4 → vector 33（0x21）** と対応している。`level 1 → 0` は、UART の注入コードが線を上げてから下げる操作に対応する。edge / level のトリガ方式自体は guest の IOAPIC 設定による。

`kvm_apic_accept_irq` が IOAPIC のイベントより先に出る理由は後述する。タイマなど別の割り込みも混ざるため、常に同じ5行が並ぶとは限らない。抜粋だけで判断しにくい場合は保存ファイルを `less` で読む。

### 2. function_graph で呼び出し関係を見る

ターミナル B で tracepoint を止め、`kvm_set_irq` 以下の関数を記録する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_ftrace_pid"
echo | sudo tee "$trace_dir/set_ftrace_filter"
echo | sudo tee "$trace_dir/set_ftrace_notrace"
echo | sudo tee "$trace_dir/set_graph_function"
echo | sudo tee "$trace_dir/set_graph_notrace"
echo function_graph | sudo tee "$trace_dir/current_tracer"
echo kvm_set_irq | sudo tee "$trace_dir/set_graph_function"
echo 8 | sudo tee "$trace_dir/max_graph_depth"
sudo cat "$trace_dir/set_graph_function"
```

最後の出力が `kvm_set_irq` であることを確認してから記録を開始する。

```bash
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

ターミナル A の guest で再び `ls` を実行する。

```sh
/ # ls
```

ターミナル B で停止・保存する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-irq-functions.txt
head -80 ~/kvm-irq-functions.txt
```

観測した関数ツリーの主要部分（CPU・時間の列と一部の呼び出しを省略）:

```text
kvm_set_irq {
  kvm_irq_map_gsi
  kvm_ioapic_set_irq {
    ioapic_set_irq {
      ...
      ioapic_service {
        __kvm_irq_delivery_to_apic {
          __kvm_irq_delivery_to_apic_fast {
            __apic_accept_irq {
              vt_deliver_interrupt [kvm_intel]
            }
          }
        }
      }
    }
  }
  kvm_pic_set_irq { ... }
}
```

**IOAPIC の処理から LAPIC の `__apic_accept_irq` に進む**親子関係を確認する。この計測では PIC への routing entry もあり、`kvm_pic_set_irq` が続いた。

この1回では全体 38.830 us のうち `vt_deliver_interrupt` が 26.118 us だった。ただし、この値だけから posted-interrupt や物理 IPI の内訳は分からない。関数が深さ制限で見えない場合は `max_graph_depth` を `12` にして再計測する。

### 3. irqfd では注入するタスクがどう変わるかを見る

ターミナル A の VM を終了し、irqfd を有効にして起動し直す。

```bash
USE_IRQFD=1 ./microkvm
```

guest で virtio の受信側を用意し、入力先を切り替える。

```text
/ # cat /dev/hvc0 &
/ # < Ctrl-A v >
[monitor] input → hvc0 (virtio)
```

ターミナル B で手順1と同じ tracepoint 設定を行い、記録を開始する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
for e in kvm_set_irq kvm_ioapic_set_irq kvm_apic_accept_irq; do
    echo 0 | sudo tee "$trace_dir/events/kvm/$e/filter"
    echo 1 | sudo tee "$trace_dir/events/kvm/$e/enable"
done
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

ターミナル A で数文字入力して Enter を押し、UART 入力へ戻す。

```text
abc

< Ctrl-A v >
[monitor] input → ttyS0 (UART)
```

ターミナル B で停止・保存し、GSI 4 / 5 と左側のタスク名を確認する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-irq-irqfd.txt
grep -E 'kvm_set_irq: gsi (4|5) ' ~/kvm-irq-irqfd.txt | head -30
```

出力例（抜粋）:

```text
kworker/6:1-137  99143.183804: kvm_set_irq: gsi 5 level 1 source 0
kworker/6:1-137  99143.183808: kvm_set_irq: gsi 5 level 0 source 0
microkvm-30492   99143.183994: kvm_set_irq: gsi 4 level 1 source 0
microkvm-30492   99143.183996: kvm_set_irq: gsi 4 level 0 source 0
```

UART の GSI 4 は ioctl を呼んだ microkvm、virtio の GSI 5 は kworker のコンテキストで記録された。irqfd は GSI 5 に登録されているので、UART 入力のままではこの比較ができない。irqfd が常に kworker を通るかどうかは、次のソースで確認する。

## KVM の実装と照合する

### IOAPIC は guest が設定した vector を使う

Linux v7.2 の [`ioapic_service()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/ioapic.c#L457) は redirection table から配送情報を取り出す。以下は関数内の抜粋。

```c
union kvm_ioapic_redirect_entry *entry = &ioapic->redirtbl[irq];
/* 宣言・mask などの確認を省略 */
irqe.dest_id = entry->fields.dest_id;
irqe.vector = entry->fields.vector;
/* その他の配送情報の設定・分岐を省略 */
```

この情報で LAPIC へ配送した後、呼び出し元の [`ioapic_set_irq()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/ioapic.c#L240) がトレースを記録する。

```c
ret = ioapic_service(ioapic, irq, line_status);

out:
trace_kvm_ioapic_set_irq(entry.bits, irq, ret == 0);
```

これが、手順1で LAPIC のイベントが IOAPIC のイベントより先に見えた理由である。**トレース地点の記録順と、関数の呼び出し順は同じとは限らない。**

### irqfd は直接注入できなければ workqueue に回す

Linux v7.2 の [`irqfd_wakeup()`](https://github.com/torvalds/linux/blob/v7.2/virt/kvm/eventfd.c#L203) は、eventfd の通知を受けて次の分岐を通る（関数内の抜粋）。

```c
if (unlikely(!irqfd_is_active(irqfd)) ||
    kvm_arch_set_irq_inatomic(&irq, kvm,
                              KVM_USERSPACE_IRQ_SOURCE_ID, 1,
                              false) == -EWOULDBLOCK)
    schedule_work(&irqfd->inject);
```

注入をその場で処理できない場合などに workqueue へ渡し、その処理が `kvm_set_irq` を呼ぶ。今回の kworker の記録はこの経路と整合する。したがって、kworker が出ること自体を irqfd の必須条件にはしない。

irqfd により virtio の IRQ ごとの `KVM_IRQ_LINE` は不要になるが、microkvm の `write(eventfd)` は system call である。タスク名の違いだけから高速化は判断しない。また execution report の IRQ counter は virtio RX の IRQ5 が対象で、UART IRQ4 の ioctl は含まない。

Step 36 の `vt_sync_pir_to_irr()` は、posted-interrupt の保留状態を LAPIC の IRR に同期する vCPU 側の処理である。今回見た注入経路の入口ではなく、配送を受ける側に位置する。

## トレースを終了する

ターミナル B で設定を解除する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_graph_function"
echo 0 | sudo tee "$trace_dir/max_graph_depth"
```

## このステップで分かったこと

- microkvm が通知する GSI と、guest が受ける vector は別である。
- KVM は routing と guest の IOAPIC 設定に従って割り込みを配送する。
- irqfd は eventfd を注入経路につなぎ、今回の workqueue 経路では共通の GSI routing に合流した。

## 次のステップ

[Step 38: MMIO exit](step38_mmio-exit.md) では、guest のメモリアクセスが KVM の MMIO 処理を経て `KVM_EXIT_MMIO` として返る経路を追う。
