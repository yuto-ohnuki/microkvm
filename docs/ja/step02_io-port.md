# Step 2: I/O ポート文字出力

## 目的

x86 のポート I/O (`out` 命令) を使って、ゲストからホスト端末に文字を出力する。
これにより**exit handler ループ**を導入する — 全ての VMM の基本構造。

## 背景

### ポート I/O (PIO)

x86 には 16-bit の独立 I/O アドレス空間 (ポート 0x0000–0xFFFF) があり、`in` と `out` 命令でアクセスする。
このステップでは、各ポート I/O 命令が `KVM_EXIT_IO` で `KVM_RUN` を返す原因となる。

```
out 0x10, al    →  AL をポート 0x10 に書く  →  KVM_EXIT_IO (direction=OUT)
in  al, 0x10    →  ポート 0x10 を AL に読む →  KVM_EXIT_IO (direction=IN)
```

実機ではレガシーデバイス (シリアルポート、PIC、PIT) に PIO が使われる。我々のハイパーバイザーでは、ポート `0x10` を単純な文字出力デバイスとして定義する。

### exit handler ループ

実際の VMM は `KVM_RUN` を1回だけ呼ぶのではなく、ループで実行する:

```c
for (;;) {
    ioctl(vcpufd, KVM_RUN, NULL);
    switch (run->exit_reason) {
        case KVM_EXIT_IO:   /* I/O 処理 */  break;
        case KVM_EXIT_HLT:  /* 停止 */      goto done;
    }
}
```

これは QEMU の `kvm_cpu_exec()` と同じ構造。ゲストは CPU 上でネイティブに動作し、VMM はハードウェアが trap した時のみ介入する。

### run->io のフィールド

`exit_reason == KVM_EXIT_IO` の場合、共有 kvm_run ページには以下が含まれる:

| フィールド | 意味 |
|-----------|------|
| `run->io.port` | アクセスされたポート |
| `run->io.direction` | `KVM_EXIT_IO_OUT` (1) または `KVM_EXIT_IO_IN` (0) |
| `run->io.size` | データ幅 (1, 2, または 4 バイト) |
| `run->io.data_offset` | kvm_run 先頭からデータまでのオフセット |

## 実行フロー

```
Guest (CPU)                   Host (microkvm)
───────────                   ───────────────
mov al, 'H'
out 0x10, al  ── VM exit ──→  run->exit_reason = KVM_EXIT_IO
                              run->io.port = 0x10
                              run->io.direction = OUT
                              data at run + data_offset = 'H'
                              putchar('H')
              ←─ KVM_RUN ───
mov al, 'i'
out 0x10, al  ── VM exit ──→  putchar('i')
              ←─ KVM_RUN ───
mov al, '\n'
out 0x10, al  ── VM exit ──→  putchar('\n')
              ←─ KVM_RUN ───
hlt           ── VM exit ──→  KVM_EXIT_HLT → ループ終了
```

合計: 4回の VM exit (3× IO + 1× HLT)。

## 実装

### ゲストコード (インラインバイト配列)

```c
static const unsigned char guest_code[] = {
    0xB0, 'H',     /* mov al, 'H' */
    0xE6, 0x10,    /* out 0x10, al */
    0xB0, 'i',     /* mov al, 'i' */
    0xE6, 0x10,    /* out 0x10, al */
    0xB0, '\n',    /* mov al, '\n' */
    0xE6, 0x10,    /* out 0x10, al */
    0xf4           /* hlt */
};
```

- `0xB0` = `mov al, imm8` (即値バイトを AL にロード)
- `0xE6` = `out imm8, al` (指定ポートに AL を書く)
- AL は RAX の下位 8 ビット — x86 の I/O 命令はデータに AL/AX/EAX を使用

KVM exit に焦点を当てるため、ここではまだ生のバイト配列を使う。
次のステップで、ゲストコードは独立したアセンブリファイルに移動する。

### exit handler ループ

```c
for (;;) {
    ioctl(vcpufd, KVM_RUN, NULL);

    switch (run->exit_reason) {
    case KVM_EXIT_IO:
        if (run->io.port == PIO_PORT && run->io.direction == KVM_EXIT_IO_OUT) {
            putchar(*(char *)((char *)run + run->io.data_offset));
        }
        break;
    case KVM_EXIT_HLT:
        printf("Guest halted.\n");
        goto done;
    }
}
```

データは kvm_run 共有ページ内の `run + data_offset` に存在する。追加の syscall 不要 — kvm_run がコピーではなく mmap されている理由。

## 出力

```
$ ./microkvm
Starting guest...
Hi
Guest halted.
```

## なぜポート I/O を最初に使うのか

ポート I/O は MMIO より単純。ハードウェアがポート番号とデータを `KVM_EXIT_IO` を通じて直接報告してくれる。

MMIO はゲスト物理アドレスに基づくアドレスデコードとデバイスエミュレーションが必要で、Step 5 で導入する。

## パフォーマンスの注記

各 `out` 命令で以下が発生する:

```
ゲスト実行
  ↓
VM exit
  ↓
ユーザー空間デバイスエミュレーション (putchar)
  ↓
VM entry (KVM_RUN)
```

この往復は高コスト。現代の仮想化スタックは共有メモリ (virtio)、カーネル側処理 (ioeventfd)、準仮想化デバイスで exit を削減する — Step 12-14 で扱うトピック。

## 重要な知見

ハイパーバイザーはゲスト実行を継続的に検査しない。
ほとんどの命令はユーザー空間の関与なしに CPU 上で直接実行される。VMM はハードウェアが VM exit をトリガーした時のみ制御を取り戻す。

```
guest   ← CPU 上でネイティブ実行
guest
guest
guest
guest
VM exit ← ハードウェア trap、制御がホストに戻る
host    ← VMM が exit を処理
VM entry
guest   ← ネイティブ実行に復帰
guest
...
```

この実行モデルはプロジェクト全体を通じて変わらない。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| exit handler ループ | `for(;;) { KVM_RUN; switch(exit_reason) }` - QEMU と同じ |
| ポート I/O インターセプト | `out` がこの構成で `KVM_EXIT_IO` をトリガー |
| kvm_run データアクセス | `run + run->io.data_offset` でゼロコピー I/O データ |
| ゲスト → ホスト通信 | 最もシンプルなチャネルとしての PIO |

## 次のステップ

[Step 3: リアルモード → プロテクトモード](step03_protected-mode.md) — ゲストコードをアセンブリファイルに分離し、GDT を設定し、CPU を 16-bit リアルモードから 32-bit プロテクトモードに遷移させる。
