# Step 41: VMCS explorer — KVM 越しに guest の CPU 状態を読む

> **Part 3: Explore VT-x Through the KVM Interface**
>
> Part 3 では、microkvm の KVM API と既存のトレース機能を使い、VT-x の状態管理・メモリ変換・実行制御を理解する。観測した値を Linux KVM のソースと Intel SDM に対応づけ、guest の命令実行から VM exit、再開までを追う。

## このステップのポイント

**KVM API が返す guest の CPU 状態は、raw VMCS のダンプではない。** モニタで取得した状態を VMCS の役割と対応づけ、CPUID exit の前後で RIP を更新するのが hardware と KVM のどちらかを確認する。

```text
KVM API が公開する guest state ← KVM の変換・キャッシュ・同期 → VMCS
                                ↓
                      CPUID の応答と RIP を更新
```

## 背景

### VMCS の役割

VMCS（Virtual Machine Control Structure）は、Intel VT-x が guest の実行と VM entry / exit を制御するために使う構造である。KVM の VMX 実装が VMREAD / VMWRITE などを通じて管理する。

| 分類 | 役割 |
|------|------|
| Guest-state area | guest の RIP、RSP、制御レジスタ、セグメントなど |
| Host-state area | VM exit 後に実行を戻す host の RIP、RSP、CR3 など |
| VM-execution controls | guest 実行中の動作や、選択可能な exit 条件 |
| VM-entry / VM-exit controls | entry / exit 時の状態の扱い |
| VM-exit information | exit reason、命令長など、exit の処理に必要な情報 |

VM entry では guest state をロードし、VM exit では guest state の保存と host state のロードを行う。対象のフィールドや controls に従って hardware が状態を切り替え、KVM がその前後の準備と処理を担う。RAX などの汎用レジスタは VMCS に保存されないため、KVM 側の保存・復元も必要になる。

### Part 1・Part 2 とのつながり

Step 21 の snapshot は `KVM_GET_REGS` / `KVM_GET_SREGS` で CPU 状態を取得した。今回は同じ API で取得した値を画面に表示する。Step 36 で追った VM entry / exit の経路に、Step 40 の CPUID 処理を重ね、状態がどこで変わるかを見る。

## 今回追加する実装

変更箇所は `microkvm.c`、`snapshot.c`、`snapshot.h` の 3 ファイルである。

### モニタから取得を要求する

`stdin_thread()` は `< Ctrl-A p >` を受けると要求フラグを立てる。

```c
if (c == 'p') {
    fprintf(stderr, "\n[monitor] dumping guest state (Ctrl-A p)\n");
    dump_requested = 1;
    continue;
}
```

`dump_requested` は `static volatile sig_atomic_t` として宣言する。vCPU スレッドはループ先頭で要求を確認し、次の `KVM_RUN` より前に取得する。

```c
if (dump_requested) {
    dump_cpu_state(vcpu->fd);
    dump_requested = 0;
}
if (ioctl(vcpu->fd, KVM_RUN, NULL) < 0) {
    perror("KVM_RUN");
    return NULL;
}
```

状態の取得は vCPU スレッドが `KVM_RUN` から戻った後に行う。これにより、入力スレッドから同じ vCPU の ioctl を呼んで待ち合わせることを避ける。表示されるのはキーを押した瞬間ではなく、要求を処理した時点の状態である。hardware VM exit だけではこのループ先頭に戻らず、`KVM_RUN` の userspace 復帰が必要になる。

### KVM が公開する状態を表示する

`snapshot.c` の `dump_cpu_state()` は次の二つの API を呼ぶ。

```c
struct kvm_regs regs;
ioctl(vcpufd, KVM_GET_REGS, &regs);

struct kvm_sregs sregs;
ioctl(vcpufd, KVM_GET_SREGS, &sregs);
```

`regs` から RIP / RSP / RFLAGS と RAX / RBX / RCX / RDX、`sregs` から CR0 / CR3 / CR4 / EFER と CS / SS を表示する。EFER の LMA は `(sregs.efer >> 10) & 1` で取り出す。このステップの表示処理は `KVM_GET_MSRS` を使わない。

## 観察A: guest state を読む

ターミナル A で Linux guest を起動し、プロンプトでモニタコマンドを入力する。

```bash
cd ~/microkvm
./microkvm
```

```text
/ # < Ctrl-A p >
[monitor] dumping guest state (Ctrl-A p)
```

以下は 64-bit Linux guest で取得した値の抜粋である。アドレスとレジスタ値は実行環境や取得時点によって変わる。

```text
RIP    = 0xffffffff8135de91
RSP    = 0xffffc90000003f10
RFLAGS = 0x0000000000000006
RAX=0xffff888000155e00 RBX=0xffffffff81785bc0
RCX=0x0 RDX=0x00000000000003fa
CR0    = 0x80050033
CR3    = 0x0000000004117006
CR4    = 0x00000000007706b0
EFER   = 0x0000000000000d01
CS: sel=0x0010 type=0xb db=0 l=1
SS: sel=0x0018 type=0x3
```

CR0.PG=1、CR4.PAE=1、EFER.LMA=1 から long mode のページングが有効と分かる。CS.l=1 / db=0 は 64-bit code segment を示す。RFLAGS.IF は 0 なので、取得時点では maskable interrupt が禁止されている。

### KVM state と VMCS の対応

| 表示する状態 | 関連する VMCS field | 対応の見方 |
|--------------|-------------------|------------|
| RIP / RSP / RFLAGS | GUEST_RIP / GUEST_RSP / GUEST_RFLAGS | KVM がキャッシュや同期を介して扱う |
| CR0 / CR4 | GUEST_CR0 / GUEST_CR4、READ_SHADOW、guest/host mask | guest に見える論理値と hardware 用の値を区別する |
| CR3 | GUEST_CR3 | guest のページテーブルの基点。EPT pointer とは別 |
| EFER | GUEST_IA32_EFER など | VM entry / exit の EFER 切り替え設定に応じて管理する |
| CS / SS | GUEST_CS_* / GUEST_SS_* | selector、base、limit、access rights に分かれる |
| RAX / RBX / RCX / RDX | 対応する guest-state field なし | KVM のソフトウェア状態と entry / exit のコードが扱う |

この VMCS explorer は、KVM が公開する状態と KVM のソースを対応づけて、VMCS の役割を追う。KVM API は guest の論理的な状態を返し、VMCS との間には変換と同期があるため、表示値は raw VMCS のダンプではない。

Linux v7.2 の [`vmx_set_cr0()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L3448) では、guest に見せる値と hardware 用の値を分けている。

```c
vmcs_writel(CR0_READ_SHADOW, cr0);
vmcs_writel(GUEST_CR0, hw_cr0);
vcpu->arch.cr0 = cr0;
```

read shadow は guest/host mask の対象ビットに使われる。このように KVM は guest に見える状態と hardware の設定を対応づけるため、API の値と raw VMCS が常に一致するとは限らない。

## 観察B: CPUID exit の前後で RIP を追う

ターミナル A の VM を一度終了する。CPUID が多く実行される boot 中を記録するため、ターミナル B で先にトレースを設定する。ホストは Intel VMX 環境とし、他の VM と perf は停止しておく。

### 1. 記録対象を設定して開始する

ターミナル B で必要なイベントと filter のフィールドを確認する。

```bash
trace_dir=/sys/kernel/tracing
sudo cat "$trace_dir/events/kvm/kvm_exit/format"
sudo ls "$trace_dir/events/kvm/kvm_entry/enable"
```

`exit_reason` フィールドを確認し、CPUID の exit と全 entry を記録する。Intel VMX の exit reason `10` は CPUID である。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_entry/filter"
echo 'exit_reason == 10' | sudo tee "$trace_dir/events/kvm/kvm_exit/filter"
sudo cat "$trace_dir/events/kvm/kvm_exit/filter"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_entry/enable"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_exit/enable"
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

### 2. 起動してログを保存する

ターミナル A で起動する。

```bash
./microkvm
```

guest のプロンプトが表示されたら、ターミナル B で停止・保存する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-cpuid-rip.txt
grep -A 1 'kvm_exit:.*reason CPUID' ~/kvm-cpuid-rip.txt | head -40
```

### 3. 同じ vCPU の exit と次の entry を読む

CPUID の exit 行と、その後の同じ vCPU スレッドの entry 行を対応づける。vCPU ID が表示されていればそれも確認する。別スレッドの行が間に入る場合は保存ファイルを `less` で読み、隣り合う行だけで判断しない。CPUID が見つからない場合は、記録開始の順序と、バッファの上書きで起動初期の記録が失われていないかを確認する。

複数回の計測で得た RIP の組は次のとおり。

| CPUID exit の RIP | 次の entry の RIP | 差分 |
|------------------|------------------|------|
| 0x1e32c9 | 0x1e32cb | +2 |
| 0x1e3312 | 0x1e3314 | +2 |
| 0x1e3347 | 0x1e3349 | +2 |

CPUID の命令バイトは `0F A2` で、ここでは RIP が 2 バイト先へ進んでいる。`kvm_entry` / `kvm_exit` は KVM 内のトレース地点であり、この観測をソースと照合して更新の主体を確認する。

### RIP を進めるのは KVM

CPUID の VM exit では、hardware は命令自身を指す guest RIP と命令長を VMCS に記録する。KVM は `kvm_emulate_cpuid()` で応答をレジスタ状態へ反映し、`kvm_skip_emulated_instruction()` を経て VMX 側の `skip_emulated_instruction()` で RIP を進める。

Linux v7.2 の [`skip_emulated_instruction()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L1791) で、CPUID の処理が使う部分を抜き出す。以下のコードは途中の確認・補正処理を省略している。

```c
instr_len = vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
/* 命令長などの確認を省略 */
orig_rip = kvm_rip_read(vcpu);
rip = orig_rip + instr_len;
/* 実行モードに応じた RIP の補正を省略 */
kvm_rip_write(vcpu, rip);
```

更新した RIP は KVM の状態に保持され、[`vmx_vcpu_run()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L7546) 内で次の entry 前に VMCS へ反映される。

```c
if (kvm_register_is_dirty(vcpu, VCPU_REGS_RSP))
    vmcs_writel(GUEST_RSP, vcpu->arch.regs[VCPU_REGS_RSP]);
if (kvm_register_is_dirty(vcpu, VCPU_REG_RIP))
    vmcs_writel(GUEST_RIP, vcpu->arch.rip);
```

命令長を記録するのは hardware、その値を使って RIP を更新するのは KVM である。この CPUID の処理は KVM 内で完結し、その exit を理由に microkvm へ戻ることはない。

## VM entry / exit の状態遷移

```text
microkvm: ioctl(KVM_RUN)
    ↓
KVM: guest state を準備し、必要な値を VMCS へ同期
    ↓ VM entry（VMLAUNCH / VMRESUME）
hardware: VMCS の guest state をロード
    ↓
guest: CPUID を実行
    ↓ VM exit
hardware: VMCS が扱う guest state を保存、exit 情報を記録、host state をロード
    ↓
KVM: CPUID の応答を RAX/RBX/RCX/RDX に反映
     RIP を 0x1e32c9 → 0x1e32cb に更新
    ↓ VM entry
guest: CPUID の次の命令から再開
```

hardware が VMCS に従って状態を切り替え、KVM が exit の意味を解釈して次に実行する guest state を整える。`KVM_RUN` の一回の呼び出しの中で、この往復が繰り返される。

## トレースを終了する

ターミナル B でイベントと CPUID の filter を解除する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo | sudo tee "$trace_dir/set_event"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_exit/filter"
```

## このステップで分かったこと

- KVM API が返す guest state と raw VMCS は、変換・同期を介した関係にある。
- VMCS が扱う状態と、KVM がソフトウェアで保存・復元する汎用レジスタを区別できる。
- CPUID では hardware が命令長を記録し、KVM が応答と RIP の更新を行う。
- hardware VM exit と `KVM_RUN` の userspace 復帰は別の境界である。

## 参照

- [KVM API: KVM_GET_REGS / KVM_GET_SREGS](https://docs.kernel.org/virt/kvm/api.html)
- [Linux v7.2 KVM VMX 実装](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c): レジスタの同期、セグメント変換、命令のスキップ
- [Linux v7.2 KVM CPUID 実装](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/cpuid.c): `kvm_emulate_cpuid()`
- [Intel Software Developer’s Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): Volume 3 の VMCS、VM Entries、VM Exits

## 次のステップ

[Step 42](step42_ept-explorer.md) では memory slot の表示を入口に、guest のアドレスと host の backing memory、EPT の関係を追う。
