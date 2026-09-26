# Step 43: Execution controls — 命令が VM exit を起こす条件

**CPUID は無条件、HLT は control bit、RDMSR は MSR ごとの bitmap で VM exit が決まる。** 同じ命令の interception でも、設定の粒度は異なる。

## このステップの問い

CPUID・HLT・RDMSR は、それぞれなぜ VM exit するのか? トレースで exit を確認し、Linux v7.2 の KVM 実装と照合する。

| 命令 | VM exit を決める仕組み | 今回の観察 |
|------|----------------------|------------|
| CPUID | VMX non-root では無条件に exit | boot 中の CPUID exit |
| HLT | `HLT exiting` control | idle 中の HLT exit |
| RDMSR | `Use MSR bitmaps` と対象 MSR の read bit | IA32_APIC_BASE（0x1b）の読み出し |

exit の発生条件と、exit 後に KVM が userspace へ処理を返すかは別の判断である。ここでは前者を中心に追い、KVM の処理先まで確認する。

## microkvm と KVM の役割

このステップではモニタコマンドや実装を追加せず、既存の Linux guest と KVM のトレース機能を使う。

microkvm は CPUID の登録や MSR filter などを KVM API で設定する。VMCS の execution controls や hardware 用 MSR bitmap を構成するのは KVM の VMX 実装である。Step 8 の userspace 用 MSR filter と、今回扱う hardware の MSR bitmap は異なる層の仕組みになる。

## 観察手順

### 1. ターミナル B（ホスト）で記録を開始する

Intel VMX のホストで、ほかの VM を停止して計測する。まず必要なイベントが使えることを確認する。

```bash
trace_dir=/sys/kernel/tracing
sudo ls "$trace_dir/events/kvm/kvm_exit/enable" "$trace_dir/events/kvm/kvm_msr/enable"
```

microkvm の起動前に記録を開始する。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
echo nop | sudo tee "$trace_dir/current_tracer"
echo | sudo tee "$trace_dir/set_event"
echo | sudo tee "$trace_dir/set_event_pid"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_exit/filter"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_msr/filter"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_exit/enable"
echo 1 | sudo tee "$trace_dir/events/kvm/kvm_msr/enable"
echo | sudo tee "$trace_dir/trace"
echo 1 | sudo tee "$trace_dir/tracing_on"
```

### 2. ターミナル A で VM を起動する

microkvm のディレクトリで実行し、guest のプロンプトが出るまで待つ。

```bash
./microkvm
```

### 3. ターミナル B で記録を止め、保存して読む

boot 中の記録が idle 中のイベントで上書きされないよう、プロンプト到達後に記録を止める。

```bash
echo 0 | sudo tee "$trace_dir/tracing_on"
sudo cat "$trace_dir/trace" > ~/kvm-controls.txt
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_exit/enable"
echo 0 | sudo tee "$trace_dir/events/kvm/kvm_msr/enable"

grep -E 'reason (CPUID|HLT|MSR_READ)' ~/kvm-controls.txt | head -30
grep -c 'reason MSR_READ' ~/kvm-controls.txt
grep -E 'msr_read[[:space:]]+1b[[:space:]]' ~/kvm-controls.txt
```

`kvm_exit` は KVM の exit 処理中の観察点で、hardware の exit reason を記録する。`kvm_msr` は MSR 処理の index と結果を記録する。以下の件数は既存の計測例であり、再計測で一致させる必要はない。

## CPUID: VMX の仕様で exit する

Step 41 では次の CPUID exit を観測した。トレースの主要フィールドを抜粋する。

```text
reason CPUID rip 0x1e32c9
```

CPUID は VMX non-root で実行すると無条件に VM exit する。HLT のように exit を有効・無効にする専用 control はない。`KVM_SET_CPUID2` が指定するのは guest に返す情報であり、CPUID exit を発生させるかどうかではない。

KVM は `EXIT_REASON_CPUID` を `kvm_emulate_cpuid()` に振り分け、登録された CPUID を基に応答する。Step 41 で確認したとおり、処理後に RIP を進めて guest を再開する。

## HLT: execution-control bit で exit する

idle 中の Linux guest では次の exit が観測された。

```text
reason HLT rip 0xffffffff8138bcd5
```

HLT は primary processor-based execution controls の `HLT exiting` が 1 なら VM exit する。Linux KVM では `CPU_BASED_HLT_EXITING` がその bit に対応し、今回の構成では有効になっている。

v7.2 の KVM は `CPU_BASED_HLT_EXITING` を required control に含めるが、[vmx_exec_control()](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L4661) では次のように調整する（抜粋）。

```c
if (kvm_hlt_in_guest(vmx->vcpu.kvm))
    exec_control &= ~CPU_BASED_HLT_EXITING;
```

required の定義だけで「常に有効」とは判断せず、VM ごとの最終設定まで読む。今回の HLT exit は、この bit が有効な経路の観測である。

今回の HLT は KVM が受け取り、割り込みなどによる再開まで vCPU を待機させる処理につながる。hardware に HLT の実行を任せるか、exit させて KVM が待機を管理するかを control で選べる点が CPUID との違いである。

## RDMSR: bitmap で MSR ごとに選ぶ

### 二段階の判定

RDMSR の interception は `Use MSR bitmaps` と read bitmap で決まる。

```text
Use MSR bitmaps = 0
    → RDMSR は VM exit

Use MSR bitmaps = 1
    → 対象 MSR が bitmap の範囲内か?
         ├─ 範囲外 → VM exit
         └─ 範囲内 → read bit が 1 なら VM exit
                     read bit が 0 なら、この条件では exit しない
```

bitmap の対象は `0x00000000–0x00001fff` と `0xc0000000–0xc0001fff` で、read と write に別の bit がある。VMCS は 4 KiB の bitmap のアドレスを保持する。bitmap を無効にすることは「すべて直接実行させる」ことではなく、すべて intercept する設定になる。

### IA32_APIC_BASE（0x1b）を追う

boot 計測では `MSR_READ` exit が **97 件**あり、`kvm_msr` には以下の読み出しが複数回記録された。

```text
msr_read 1b = 0xfee00900
```

97 件は MSR_READ 全体の件数であり、0x1b だけの件数ではない。`kvm_msr` の行は、KVM が IA32_APIC_BASE の読み出しを処理したことを示す。

[alloc_loaded_vmcs()](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L3080) は bitmap を全 bit 1 で初期化する（抜粋）。

```c
memset(loaded_vmcs->msr_bitmap, 0xff, PAGE_SIZE);
```

その後、[vmx_set_intercept_for_msr()](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c#L4109) が、直接アクセスを許可する MSR の bit を解除する。read 側の分岐は次のとおりである（抜粋）。

```c
if (type & MSR_TYPE_R) {
    if (!set && kvm_msr_allowed(vcpu, msr, KVM_MSR_FILTER_READ))
        vmx_clear_msr_bitmap_read(msr_bitmap, msr);
    else
        vmx_set_msr_bitmap_read(msr_bitmap, msr);
}
```

IA32_APIC_BASE は今回の構成では解除対象にならず、read interception が残る。0x1b の位置は low MSR read 領域（4 KiB bitmap の先頭）から次のように計算できる。

```text
byte offset = 0x1b / 8 = 3
bit         = 0x1b % 8 = 3
mask        = 1 << 3   = 0x08
```

この bit が 1 なので、guest の RDMSR は次の経路へ進む。

```text
Guest: RDMSR IA32_APIC_BASE（ECX = 0x1b）
    ↓ Use MSR bitmaps + 対象 read bit = 1
hardware: EXIT_REASON_MSR_READ
    ↓
KVM: kvm_emulate_rdmsr()
    ↓ guest に提示する APIC base の値を取得
RAX / RDX に結果を反映、RIP を更新
    ↓
Guest を再開
```

これは VMCS の bitmap を直接読み出した結果ではなく、トレースと KVM の初期化・設定処理を対応づけた説明である。この 0x1b の読み出しは KVM 内で処理できるため、その exit を理由に microkvm へ戻る必要はない。

### exit の有無と処理結果を分ける

RDMSR が intercept された後、KVM は値を返すほか、受け付けられないアクセスには guest の #GP を発生させる。対象の reason を userspace に返す設定なら `KVM_EXIT_X86_RDMSR` になる。**VM exit は KVM に制御が移るイベント、#GP は guest に見える例外**であり、同じものではない。

また、ある MSR がログにないことだけでは、bitmap により直接実行されたとは判断できない。guest がその RDMSR を実行していない可能性もある。今回確認したのは 0x1b を intercept して処理する経路である。

## このステップで分かったこと

- CPUID は無条件、HLT は control bit、RDMSR は bitmap を含む判定で exit する。
- MSR bitmap は read / write と MSR index ごとに interception を指定する。
- hardware の interception と、KVM の処理結果・userspace 復帰は別の判断である。

## 参照

- [Linux KVM VMX 実装](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c): `vmx_exec_control()`、MSR interception、exit handler
- [Linux KVM VMX control 定義](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.h): required / optional controls と MSR bitmap 操作
- [Linux KVM MSR 処理](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c): `kvm_emulate_rdmsr()`、結果のレジスタ反映、例外・userspace 処理への分岐
- [Intel Software Developer’s Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): Volume 3 の VM-Execution Controls、Instructions That Cause VM Exits、MSR Bitmaps

## 次のステップ

[Capstone](capstone.md) では RDMSR 0x1b を題材に、State・Memory・Control の視点を合わせ、microkvm から guest の実行と再開までを一本の流れとして説明する。
