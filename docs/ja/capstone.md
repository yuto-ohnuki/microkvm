# Capstone: RDMSR を guest から KVM、再開まで追う

**RDMSR 0x1b は VM exit を起こすが、KVM が値を返せるため、この処理で `KVM_RUN` は microkvm へ戻らない。**

## このステップの問い

Part 3 の State・Memory・Control を、一つの `RDMSR IA32_APIC_BASE (0x1b)` に結びつける。なぜ exit し、誰が結果を返し、どこから guest を再開するのかを追う。

## 観測したこと

[Step 43](step43_execution-controls.md) の boot 計測では、`kvm_exit` に MSR_READ が 97 件あり、`kvm_msr` に次の読み出しが記録された。

```text
msr_read 1b = 0xfee00900
```

97 件は MSR_READ 全体の件数であり、0x1b の件数ではない。ここではこの記録と KVM の実装を使って、0x1b の成功経路をたどる。以下の図は処理の説明であり、一回分の生トレースを並べたものではない。再確認には Step 43 の記録手順を使う。

## RDMSR の往復

```text
microkvm
    │ ioctl(KVM_RUN)                     userspace → KVM
    ▼
KVM: guest state を準備
    │ VM entry
    ▼
Guest: ECX = 0x1b、RDMSR を実行
    │ Use MSR bitmaps が有効、対象 read bit が 1
    ▼
hardware VM exit                         guest → KVM
    │ VMCS が扱う guest state と exit information を保存
    │ host state をロード
    ▼
KVM: kvm_emulate_rdmsr()
    │ guest の IA32_APIC_BASE を取得
    │ RAX / RDX に結果を反映、RIP を更新
    │ VM entry
    ▼
Guest: RDMSR の次の命令から再開
```

### 1. microkvm が vCPU を実行する

microkvm の vCPU スレッドが `ioctl(vcpu->fd, KVM_RUN, NULL)` を呼び、KVM に実行を渡す。KVM は必要な guest state を VMCS に同期し、VM entry を行う。

[Step 41](step41_vmcs-explorer.md) で確認したように、RIP / RSP などの VMCS state と、RAX など KVM がソフトウェアで管理するレジスタ状態を合わせて guest の実行状態を用意する。

### 2. hardware が RDMSR を intercept する

RDMSR は ECX で指定した MSR を読み、64-bit の結果を EDX:EAX に返す命令である。今回の index は `0x1b`。

今回の構成では `Use MSR bitmaps` が有効で、0x1b の read bit が 1 になっている。CPU は命令を intercept し、`EXIT_REASON_MSR_READ` を記録して KVM に制御を移す。これが **Control** の役割である。

VM exit 時の guest RIP は RDMSR 命令を指す。hardware は VMCS が扱う guest state と exit 情報を保存し、host state をロードする。これが **State** の切り替えになる。

### 3. KVM が guest に返す値を用意する

v7.2 の VMX exit handler は `EXIT_REASON_MSR_READ` を `kvm_emulate_rdmsr()` に振り分ける。値の取得先を [x86.c](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L4440) まで追うと、guest の APIC 状態から返していることが分かる（抜粋）。

```c
case MSR_IA32_APICBASE:
    msr_info->data = vcpu->arch.apic_base;
    break;
```

[`__kvm_emulate_rdmsr()`](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c#L2107) は読み出しに成功すると `trace_kvm_msr_read()` を呼び、通常の RDMSR では次のように結果を設定する（成功分岐から抜粋）。

```c
kvm_eax_write(vcpu, data);
kvm_edx_write(vcpu, data >> 32);
```

観測された値 `0xfee00900` を RDMSR の結果として返す場合、レジスタへの反映は次のようになる。

```text
EAX = 0xfee00900   （値の下位 32 bit）
EDX = 0x00000000   （値の上位 32 bit）
```

RAX / RDX は VMCS の guest-state field ではない。KVM が保持するレジスタ状態を更新し、entry の経路で CPU に反映する。

### 4. RIP を更新して再開する

読み出しが成功すると、KVM は命令を完了させて RIP を進める。VMX の命令スキップ処理では、hardware が `VM_EXIT_INSTRUCTION_LEN` に記録した命令長を使う。[Step 41](step41_vmcs-explorer.md) の CPUID で確認した、命令長を hardware が提供し、RIP を KVM が更新する分担と同じである。

KVM は更新した状態で再び VM entry を行い、guest は RDMSR の次の命令から続行する。この処理は KVM 内で完了するため、microkvm の `switch (run->exit_reason)` には届かない。同じ `KVM_RUN` は、その後 MMIO やシグナルなど別の理由で戻ることがある。

## Memory はどこに関係するか

[Step 42](step42_ept-explorer.md) で扱った EPT は GPA→HPA の変換を担う。一方、RDMSR の operand はメモリアドレスではなく MSR index である。**今回の MSR_READ exit の原因は、EPT fault ではなく MSR interception** になる。

guest の命令フェッチなどには EPT が使われるが、それと RDMSR を intercept する条件は別である。

## 成功以外の処理結果

### guest に例外を返す

既存の計測では、次の読み出しも記録された。

```text
msr_read 64e = 0x0 (#GP)
```

これは正常に値 0 を返した記録ではなく、KVM がそのアクセスを拒否して guest に #GP を注入する経路である。成功時のように RDMSR の次へ進めず、guest の例外処理につなぐ。

VM exit は guest から KVM への制御移行、#GP はその後 guest に届ける例外である。

### userspace に処理を返す

microkvm は `KVM_CAP_X86_USER_SPACE_MSR` で `KVM_MSR_EXIT_REASON_FILTER` を有効にし、MSR_CUSTOM（`0x20000000`）の read/write を filter で拒否する設定を持つ。このアクセスは `KVM_EXIT_X86_RDMSR` / `KVM_EXIT_X86_WRMSR` として microkvm に返る。

read の場合、microkvm は `run->msr.data` に保存値を設定し、`run->msr.error = 0` として次の `KVM_RUN` を呼ぶ。KVM はその結果を使って命令を完了させる。これは [Step 8](step08_msr.md) で実装した userspace との往復であり、0x1b の KVM 内完結経路との違いになる。

```text
0x1b の読み出し
    guest → VM exit → KVM が値を返す → guest

MSR_CUSTOM の読み出し
    guest → VM exit → KVM_RUN が戻る → microkvm が値を設定
                                      ↓ 次の KVM_RUN
                       guest ← KVM が命令を完了
```

## Part 3 を終えて

Step 41 では KVM が公開する CPU 状態と VMCS の関係、Step 42 では memory slot と EPT、Step 43 では命令の interception を確認した。RDMSR 0x1b では、その関係を次のように説明できる。

- **Control**: MSR bitmap が RDMSR を intercept し、MSR_READ exit を起こす。
- **State**: hardware が VMCS state を切り替え、KVM が読み出し結果と RIP を更新する。
- **Memory**: EPT は命令フェッチなどを支えるが、この exit の原因ではない。
- **userspace との境界**: 0x1b は KVM が処理できるため、その exit を理由に microkvm へ戻らない。

## 参照

- [Linux KVM VMX 実装](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/vmx/vmx.c): MSR interception、exit handler、RIP の更新
- [Linux KVM x86 実装](https://github.com/torvalds/linux/blob/v7.2/arch/x86/kvm/x86.c): `kvm_emulate_rdmsr()` と MSR 処理
- [KVM API](https://docs.kernel.org/virt/kvm/api.html): `KVM_RUN`、`KVM_CAP_X86_USER_SPACE_MSR`、`KVM_X86_SET_MSR_FILTER`
- [Intel Software Developer’s Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html): RDMSR、MSR Bitmaps、VM Exits
