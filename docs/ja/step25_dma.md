# Step 25: DMA シミュレーション — descriptor と doorbell

## 目的

PCI デバイスが guest RAM を直接 read/write する DMA (Direct Memory Access) を実装する。MMIO exit でバイトごとにデータ転送するのではなく、guest が RAM 上に descriptor（転送内容の記述）を配置して doorbell を叩くと、VMM が一括で転送を実行する。

## 背景

### MMIO だけでのデータ転送の問題

Step 24 では `devmem` アクセスごとに1回の KVM_EXIT_MMIO が発生する。4MB のデータを MMIO レジスタ経由で転送するには 100 万回の exit が必要。実用的ではない。

```
DMA なし: 4MB 転送 = 100万 exit（4 bytes/exit）
DMA あり: 4MB 転送 = 2 exit（DESC_LO + DOORBELL）
```

### DMA の仕組み

DMA は制御を逆転させる: CPU がバイトごとにレジスタを操作する代わりに、CPU がデバイスに「データがここにある」と伝え、デバイスがメモリに直接アクセスする:

```
1. ドライバが guest RAM に descriptor を書く (addr + len + direction)
2. ドライバが descriptor の GPA をデバイスレジスタに書く (DESC_LO/HI)
3. ドライバが DOORBELL を kick → 1回だけの MMIO exit
4. デバイス (VMM) が guest RAM から descriptor を読む
5. デバイス (VMM) がデータバッファを直接 read/write (memcpy)
```

核心: データ転送は VMM userspace のメモリ操作（guest RAM の mmap 領域への memcpy）で完結する。バイトごとの VM exit は発生しない。実デバイスも descriptor が guest RAM 範囲内を指しているか検証する必要がある — 実装では `desc.addr + desc.len` を `ram_size` と照合してからアクセスしている。

### Descriptor 構造体

```c
struct dma_desc {
    uint64_t addr;      /* データバッファの GPA */
    uint32_t len;       /* 転送サイズ (bytes) */
    uint32_t flags;     /* 0=device が guest から読む (TX), 1=device が guest に書く (RX) */
};
```

これは高性能デバイス I/O の普遍的パターン:

| microkvm | virtio | NVMe | AHCI |
|----------|--------|------|------|
| dma_desc | vring_desc | SQ entry | PRDT |
| doorbell write | QueueNotify | doorbell | CI register |

全て同じ: 共有メモリ上の descriptor + doorbell kick。

### デバイスレジスタ（拡張）

```
BAR0 + 0x00: STATUS    R    (0x01 = ready)
BAR0 + 0x04: DOORBELL  W    (write → DMA 実行)
BAR0 + 0x08: RESULT    R    (最後の DMA で転送したバイト数)
BAR0 + 0x0C: DESC_LO   W    (descriptor の GPA, 下位 32-bit)
BAR0 + 0x10: DESC_HI   W    (descriptor の GPA, 上位 32-bit)
```

DESC_LO/HI を分けているのは、MMIO レジスタが 32-bit だが GPA は 64-bit になりうるため。

## 実行フロー

```
Guest                        KVM                VMM (microkvm)
─────                        ───                ────────────────
RAM に descriptor 配置
  (GPA 0x07F00000)
RAM に data "hello" 配置
  (GPA 0x07F00100)

devmem BAR0+0x0C ← 0x07F00000
                             KVM_EXIT_MMIO
                                                desc_addr = 0x07F00000

devmem BAR0+0x04 ← 1
                             KVM_EXIT_MMIO
                                                DOORBELL:
                                                1. RAM[0x07F00000] から desc 読み取り
                                                2. desc.addr=0x07F00100, len=5, flags=0
                                                3. memcpy(stdout, RAM+0x07F00100, 5)
                                                → "hello" 出力
                                                last_dma_len = 5

devmem BAR0+0x08
                             KVM_EXIT_MMIO
                                                → 5 を返す (RESULT)
```

## 実装

### pci.h — 新レジスタと descriptor 構造体

```c
#define PCI_DEV_REG_DESC_LO  0x0C
#define PCI_DEV_REG_DESC_HI  0x10

struct dma_desc {
    uint64_t addr;      /* データバッファの GPA */
    uint32_t len;       /* 転送サイズ */
    uint32_t flags;     /* 0=device reads from guest (TX) */
};
```

`struct pci_device` に DMA state 追加:
```c
    uint64_t desc_addr;     /* descriptor の GPA */
    uint32_t last_dma_len;  /* 最後の DMA 結果 */
    uint8_t *ram;           /* guest RAM ポインタ */
    size_t ram_size;
```

### pci.c — doorbell handler に DMA 実装

```c
case PCI_DEV_REG_DOORBELL: {
    /* guest RAM から descriptor を読む */
    struct dma_desc desc;
    memcpy(&desc, dev->ram + dev->desc_addr, sizeof(desc));

    if (desc.flags == 0) {
        /* TX: device が guest から読む → stdout に出力 */
        write(STDOUT_FILENO, dev->ram + desc.addr, desc.len);
    } else {
        /* RX: device が guest に書く */
        memcpy(dev->ram + desc.addr, "DMA-WRITE-OK\n", msg_len);
    }
    dev->last_dma_len = desc.len;
    break;
}
case PCI_DEV_REG_DESC_LO:
    dev->desc_addr = (dev->desc_addr & 0xFFFFFFFF00000000ULL) | value;
    break;
case PCI_DEV_REG_DESC_HI:
    dev->desc_addr = (dev->desc_addr & 0xFFFFFFFF) | ((uint64_t)value << 32);
    break;
```

### microkvm.c — PCI デバイスに guest RAM アクセスを提供

```c
pci_dev.ram = (uint8_t *)mem;
pci_dev.ram_size = GUEST_MEM_SIZE;
```

## 出力

```
/ # devmem 0x07F00000 32 0x07F00100
/ # devmem 0x07F00004 32 0x00000000
/ # devmem 0x07F00008 32 0x00000005
/ # devmem 0x07F0000C 32 0x00000000
/ # devmem 0x07F00100 32 0x6C6C6568
/ # devmem 0x07F00104 8 0x6F
/ # devmem 0x0800000C 32 0x07F00000
[pci-dev] MMIO write offset=0x0c ← 0x7f00000
/ # devmem 0x08000004 32 0x1
[pci-dev] MMIO write offset=0x04 ← 0x1
[pci-dma] DMA read: 5 bytes from GPA 0x7f00100
hello
/ # devmem 0x08000008
[pci-dev] MMIO read  offset=0x08 → 0x5
0x00000005
```

手順の説明:
1. DMA descriptor を guest RAM (0x07F00000) に配置 (addr=0x07F00100, len=5, flags=0)
2. データバッファ (0x07F00100) に "hello" を書き込み
3. DESC_LO = 0x07F00000（descriptor の場所をデバイスに通知）
4. DOORBELL kick → VMM が descriptor を読み、DMA 実行、"hello" 出力
5. RESULT 読み取り → 5（転送バイト数）

## 重要な知見

DMA は VM exit を O(N/4) から O(1) に削減する — 転送サイズに関係なく。より正確には: descriptor 数が固定なら、ペイロードサイズに関係なく exit 数は一定。descriptor + doorbell パターンは高性能デバイス I/O の普遍的抽象化 — NVMe, virtio, GPU コマンドバッファ, ネットワーク DMA エンジンが全てこのモデルを使う。データ自体は VM exit 境界を越えない。越えるのは 4 byte の doorbell write だけ。

## このステップで学べること

| 概念 | ここでの現れ方 |
|------|---------------|
| DMA | デバイスが guest RAM に直接アクセス（バイトごとの MMIO exit なし）|
| Descriptor | 共有メモリ上の addr + len + flags 構造体（ドライバが配置）|
| Doorbell | 1回の MMIO write で転送全体をトリガー |
| O(1) exits | 転送サイズが exit 数に影響しない |
| DESC_LO/HI 分割 | 32-bit レジスタで 64-bit アドレスを伝える |
| Virtio との等価性 | dma_desc ≈ vring_desc, doorbell ≈ QueueNotify |
| 境界チェック | desc.addr + desc.len を ram_size と照合 |

## 変わったこと

Step 24 からの変更:
- **pci.h**: `PCI_DEV_REG_DESC_LO/HI`, `struct dma_desc`, `struct pci_device` に DMA state
- **pci.c**: doorbell handler が descriptor 読み取り + DMA 実行、DESC_LO/HI handler、RESULT が `last_dma_len` を返す、`#include <unistd.h>`（write() 用）
- **microkvm.c**: `pci_dev.ram` / `pci_dev.ram_size` 設定

## 次のステップ

[Step 26: MSI-X emulation](step26_msix.md) では DMA 完了後の割り込み通知を追加する。RESULT を polling する代わりに、デバイスが MSI-X で guest に通知する — レガシー割り込みコントローラを完全にバイパスする PCI の write-to-LAPIC メカニズム。RX パス (flags=1) も、デバイスが「新データ到着」を通知できるようになることでより意味を持つ。
