# Step 1: `hlt` の実行

## 目的

KVM API を使って、1つのゲスト命令 (`hlt`) を実際の CPU 上で実行する。
I/O もモード遷移もない — ゲストコードがハードウェア仮想化を通じてネイティブに実行されることの最小限の証明。

## 背景

### KVM API の階層構造

KVM API は3段階のファイルディスクリプタに対する `ioctl` 呼び出しで構成される:

```
/dev/kvm  (system fd)
  └── VM fd        (ioctl KVM_CREATE_VM)
        └── vCPU fd  (ioctl KVM_CREATE_VCPU)
```

各レベルで制御するスコープが異なる:
- **System fd**: 機能の問い合わせ、グローバルパラメータの取得
- **VM fd**: メモリ領域の管理、vCPU の作成
- **vCPU fd**: レジスタの設定、ゲストコードの実行、exit 情報の読み取り

### struct kvm_run (共有ページ)

vCPU を作成すると、その fd を `mmap` して共有ページ (`struct kvm_run`) を取得する。
`KVM_RUN` が返った後、このページがゲストが exit した*理由*を教えてくれる:

```
┌─────────────────────────┐
│  exit_reason            │  ← VM が exit した理由 (HLT, IO, MMIO, …)
│  io / mmio / …          │  ← exit 固有の詳細情報
└─────────────────────────┘
```

exit ごとに `read()`/`write()` syscall を避ける — データは共有マッピング経由で既にユーザー空間にある。

### `hlt` 命令

`hlt` (opcode `0xF4`) は次の割り込みまで CPU を停止する。
この最小ゲストでは、`hlt` の実行により `KVM_RUN` が `KVM_EXIT_HLT` (値 5) で返る。ハイパーバイザーが対処を決める — 今回は成功メッセージを表示して終了する。

## 実行フロー

```
microkvm (host)                              KVM                    Hardware
───────────────                              ───                    ────────
1. open("/dev/kvm")
2. ioctl(KVM_CREATE_VM)
3. mmap(1MB anonymous)
4. ioctl(KVM_SET_USER_MEMORY_REGION)
     → KVM はこのメモリスロットを使ってゲスト物理メモリアクセスを変換する
5. memcpy(0xF4 to GPA 0)
6. ioctl(KVM_CREATE_VCPU)
7. mmap(vcpufd) → struct kvm_run
8. KVM_SET_SREGS  (CS.base=0)
9. KVM_SET_REGS   (RIP=0, RFLAGS=2)
10. ioctl(KVM_RUN) ─────────────→  VMLAUNCH ──────→  guest mode
                                                      │
                                                      │ GPA 0 で hlt 実行
                                                      ↓
11. run->exit_reason == 5 ←─────  kvm_run 充填 ←───  VMEXIT (HLT)
12. printf("Success!")
```

## 実装

### 1. /dev/kvm のオープンと VM の作成

```c
kvmfd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
vmfd = ioctl(kvmfd, KVM_CREATE_VM, 0);
```

`kvmfd` はシステムレベルのハンドル。`KVM_CREATE_VM` は VM fd を返す — CPU もメモリもない空のコンテナ。

### 2. ゲストメモリの確保と登録

```c
mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

struct kvm_userspace_memory_region region = {
    .slot = 0,
    .guest_phys_addr = 0,
    .memory_size = GUEST_MEM_SIZE,   /* 1 MB */
    .userspace_addr = (unsigned long)mem,
};
ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);
```

KVM に伝える: 「ゲスト物理アドレス 0 をこのホスト仮想アドレスにマップせよ」。
KVM はこのメモリスロットをゲスト物理メモリのバッキングストアとして使う。
ハードウェア仮想化では、最終的に EPT/NPT などの第2段階アドレス変換に参加する。

### 3. ゲストコードのロード

```c
static const unsigned char guest_code[] = { 0xf4 /* hlt */ };
memcpy(mem, guest_code, sizeof(guest_code));
```

`hlt` 命令を GPA 0 に配置 — vCPU が実行を開始する正確な場所。

### 4. vCPU の作成と kvm_run のマップ

```c
vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, 0);
mmap_size = ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, 0);
run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
```

`mmap` により exit 情報へのゼロコピーアクセスを得る。サイズは実行時に問い合わせる（カーネルバージョンにより異なる可能性があるため）。

### 5. レジスタの初期化

```c
/* 特殊レジスタ — まず取得し、CS のみ変更して戻す */
ioctl(vcpufd, KVM_GET_SREGS, &sregs);
sregs.cs.base = 0;
sregs.cs.selector = 0;
ioctl(vcpufd, KVM_SET_SREGS, &sregs);

/* 汎用レジスタ — ゼロから開始 */
memset(&regs, 0, sizeof(regs));
regs.rip = 0;
regs.rflags = 0x2;   /* x86 では bit 1 は常にセットされている必要がある */
ioctl(vcpufd, KVM_SET_REGS, &regs);
```

実効命令アドレスは `CS.base + RIP = 0 + 0 = 0` で、`hlt` 命令を指す。

sregs に対して GET してから SET する理由: sregs には多くのフィールド (DS, SS, CR0, CR4, EFER…) がある。全てをゼロにすると CPU がクラッシュする。デフォルトを保持し、必要な部分のみ変更する。

### 6. 実行と exit reason の確認

```c
ioctl(vcpufd, KVM_RUN, NULL);

switch (run->exit_reason) {
case KVM_EXIT_HLT:
    printf("Guest executed HLT. Success!\n");
    break;
}
```

## 出力

```
$ ./microkvm
Starting guest...
Exit reason: 5
Guest executed HLT. Success!
```

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| KVM fd 階層 | system → VM → vCPU、それぞれ固有の ioctl セット |
| ゲストメモリの設定 | `mmap` + `KVM_SET_USER_MEMORY_REGION` → ゲスト物理メモリのバッキングストア |
| struct kvm_run | ゼロコピー exit 情報のための共有ページ |
| VM exit | ゲスト `hlt` → ハードウェア VMEXIT → KVM が kvm_run を充填 → ioctl が返る |
| レジスタ初期化 | sregs (モード/セグメント) vs regs (実行状態) |

## このステップが重要な理由

最小の KVM 実行ループを確立する: ホストのユーザー空間が VM 状態を準備し、`KVM_RUN` でゲストに入り、ゲストが exit したときに制御を取り戻す。

以降の全ステップはこの構造を維持し、一度に1つの新しい概念のみを追加する。

## 次のステップ

[Step 2: I/O ポート文字出力](step02_io-port.md) — exit handler ループを追加し、PIO (`out` 命令) を使ってゲストからホストに文字を送る。
