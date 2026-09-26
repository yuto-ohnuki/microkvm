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

Step 1 では `KVM_RUN` を1回だけ呼んだが、このステップでは I/O を処理するたびに再度呼び、ゲストの実行を継続する。`KVM_EXIT_HLT` を受け取ったらループを終了する。

### run->io のフィールド

`exit_reason == KVM_EXIT_IO` の場合、共有 kvm_run ページには以下が含まれる:

| フィールド | 意味 |
|-----------|------|
| `run->io.port` | アクセスされたポート |
| `run->io.direction` | `KVM_EXIT_IO_OUT` (1) または `KVM_EXIT_IO_IN` (0) |
| `run->io.size` | データ幅 (1, 2, または 4 バイト) |
| `run->io.count` | データの個数（このゲストでは1） |
| `run->io.data_offset` | kvm_run 先頭からデータまでのオフセット |

## 実行フロー

```
VMM (microkvm)                                  Guest
──────────────                                  ─────
                                                mov al, 'H'
                                                out 0x10, al
KVM_EXIT_IO
  port=0x10, direction=OUT
  data at run + data_offset = 'H'
  putchar('H')
KVM_RUN                                         （I/O を完了して再開）
                                                mov al, 'i'
                                                out 0x10, al
KVM_EXIT_IO: putchar('i')
KVM_RUN                                         （再開）
                                                mov al, '\n'
                                                out 0x10, al
KVM_EXIT_IO: putchar('\n')
KVM_RUN                                         （再開）
                                                hlt
KVM_EXIT_HLT: ループ終了
```

このゲストの命令に対応して、VMM は `KVM_EXIT_IO` を3回、`KVM_EXIT_HLT` を1回受け取る。

## 実装

`microkvm.c` のゲストコードと実行ループを変更し、`microkvm.h` に文字出力用ポート `PIO_PORT`（`0x10`）を追加する。以下のコードはエラー処理などを省略した抜粋。

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

データのアドレスは `(char *)run + run->io.data_offset`。オフセットはバイト単位なので、`char *` に変換してから加算する。このゲストの `out` は `size = 1`、`count = 1` のため、1バイトを読み出して `putchar` に渡す。共有領域からデータを読むための追加 syscall は不要。

## 出力

```
$ ./microkvm
Starting guest...
Hi
Guest halted.
```

`Starting guest...` は実行開始前の表示、`Hi` はゲストの3回の `out` を VMM が文字と改行にしたもの、`Guest halted.` は `KVM_EXIT_HLT` を受け取った VMM の表示。

### なぜポート I/O を最初に使うのか

このステップのポート I/O は、ポート番号と出力方向を確認して1文字表示するだけで実装できる。KVM がポート番号やデータを `KVM_EXIT_IO` と共有領域を通じて VMM に渡す。

メモリ空間を使う MMIO（memory-mapped I/O）は、Step 5 で導入する。

### パフォーマンスの注記

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

この構成では1文字ごとにユーザー空間との往復が発生する。Step 12–18 では virtio の共有キューや ioeventfd、irqfd を導入し、この往復の削減を学ぶ。

## 重要な知見

通常のゲスト命令は CPU 上で直接実行される。VM exit ではまず KVM に制御が戻り、KVM 内で処理できる場合はユーザー空間へ戻らず実行を継続する。このステップのポート出力は VMM が担当するため、`KVM_RUN` が `KVM_EXIT_IO` を返し、VMM が文字を出力してから再び `KVM_RUN` を呼ぶ。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|--------------|
| exit handler ループ | I/O 処理後に `KVM_RUN` を再度呼び、HLT で終了 |
| ポート I/O インターセプト | `out` がこの構成で `KVM_EXIT_IO` をトリガー |
| kvm_run データアクセス | `(char *)run + run->io.data_offset` で出力データを読む |
| ゲスト → ホスト通信 | 最もシンプルなチャネルとしての PIO |

## 変わったこと

- ゲストコードを `hlt` のみから、`Hi\n` を出力する3組の `mov` / `out` と `hlt` に変更。
- `PIO_PORT`（`0x10`）を定義し、`KVM_EXIT_IO` の文字出力処理を追加。
- `KVM_RUN` の1回の呼び出しをループに変更し、HLT で終了する。

## 次のステップ

[Step 3: リアルモード → プロテクトモード](step03_protected-mode.md) — ゲストコードをアセンブリファイルに分離し、GDT を設定し、CPU を 16-bit リアルモードから 32-bit プロテクトモードに遷移させる。
