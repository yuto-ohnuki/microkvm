# Step 10: ★ Boot minimal Linux

## Goal

Boot a real Linux kernel on microkvm. This is the **milestone** that brings together everything from Steps 1–9: mode transitions, memory management, device emulation, interrupt delivery, and paravirtualization.

After this step, microkvm runs an unmodified Linux kernel.

## Background

### What Linux needs to boot

A Linux kernel (bzImage) requires a VMM to provide:

1. **Memory** - enough RAM with a valid e820 map describing it
2. **Boot Protocol** - boot_params (zero page) with command line, memory map, and loader metadata
3. **CPU state** - 32-bit protected mode with flat segments (the kernel handles its own transition to long mode)
4. **Serial console** - 8250 UART at I/O port 0x3F8 for output
5. **Interrupt delivery** - provided by KVM's in-kernel irqchip (PIC, IOAPIC, and LAPIC)
6. **CPUID** - KVM-filtered CPU feature information (the kernel checks for long mode support during early boot)
7. **Time source** - kvmclock or calibrated TSC for timekeeping

### Linux Boot Protocol

The Linux boot protocol requires:
- CPU in 32-bit protected mode, paging disabled
- Flat segments (base=0, limit=4GB) for CS/DS/ES/FS/GS/SS
- `%esi` pointing to `boot_params` (the "zero page")
- Kernel loaded at physical address 0x100000 (1MB)

The VMM sets `RIP = 0x100000`, which is the entry point of the compressed kernel. From there, the kernel decompresses itself, transitions to long mode, and eventually reaches `start_kernel`.

### bzImage structure

```
┌──────────────────────┬─────────────────────────────────┐
│  Setup (real-mode)   │  Protected-mode kernel          │
│  ~16KB               │  (compressed, loaded at 1MB)    │
└──────────────────────┴─────────────────────────────────┘
         ↑                          ↑
    setup header            compressed kernel entry
    (protocol version,      (decompresses itself,
     loadflags, etc.)        enters long mode,
                             reaches start_kernel)
```

The VMM parses the setup header to find the kernel offset, then copies the protected-mode portion to 0x100000.

### 8250 UART emulation

Linux's serial console driver (`8250`) communicates via PIO at 0x3F8–0x3FF.
The VMM must emulate enough of the UART to support output:

| Register | Port | Read | Write |
|----------|------|------|-------|
| THR/RBR | 0x3F8 | Receive buffer | Transmit data |
| IER | 0x3F9 | — | Interrupt enable |
| IIR | 0x3FA | Interrupt ID | — |
| LCR | 0x3FB | — | Line control (DLAB) |
| LSR | 0x3FD | Line status | — |

The DLAB (Divisor Latch Access Bit) in LCR changes the meaning of ports 0x3F8 and 0x3F9 - when DLAB=1, they become baud rate divisor registers. Without DLAB handling, the driver's baud rate initialization would be misinterpreted as character output.

### In-kernel PIC and PIT

Rather than emulating the interrupt controller in userspace, we use KVM's built-in emulation:

```c
ioctl(vmfd, KVM_CREATE_IRQCHIP, 0);     /* PIC (8259) + IOAPIC + LAPIC */
ioctl(vmfd, KVM_CREATE_PIT2, &pit);     /* PIT (8254) timer */
```

This gives the kernel working timer interrupts without any userspace involvement - KVM handles the entire interrupt delivery path internally.

### kvmclock and CPUID

Linux detects KVM via CPUID and uses **kvmclock** for timekeeping.
Linux discovers kvmclock through KVM CPUID leaves (vendor string "KVMKVMKVM" and feature flags). This avoids expensive TSC calibration loops. We pass through the host's CPUID (with TSC-deadline mode hidden to avoid LAPIC timer issues) and set a fixed TSC frequency:

```c
ioctl(vcpufd, KVM_SET_TSC_KHZ, 1000000UL);  /* 1 GHz */
```

## Execution flow

```
VMM (microkvm.c)                         Linux kernel
────────────────                         ────────────
1. Parse bzImage setup header
2. Copy kernel to GPA 0x100000
3. Set up boot_params at 0x7000:
     - e820 map (0–640KB, 1MB–128MB)
     - command line pointer
     - initramfs address/size
4. Load initramfs at 0x4000000
5. KVM_CREATE_IRQCHIP + KVM_CREATE_PIT2
6. KVM_SET_CPUID2 (host CPUID passthrough)
7. KVM_SET_TSC_KHZ (1 GHz)
8. Set vCPU: 32-bit protected mode
     RIP=0x100000, ESI=0x7000
9. KVM_RUN
                                         startup_32:
                                           verify_cpu (CPUID check)
                                           build page tables
                                           enable long mode
                                           decompress kernel
                                         start_kernel:
                                           detect KVM (CPUID)
                                           init kvmclock
                                           init serial console (8250)
                                           mount initramfs
                                           exec /init
                                              ↓
                                         "=== Hello from microkvm guest! ==="
```

## Implementation

### VMM: bzImage loader

```c
/* Parse setup header */
uint8_t setup_sects = hdr[0x1F1];
if (setup_sects == 0) setup_sects = 4;
uint32_t setup_size = (setup_sects + 1) * 512;
uint32_t kernel_size = file_size - setup_size;

/* Copy protected-mode kernel to 1MB */
memcpy((char *)mem + KERNEL_ADDR, bzimage + setup_size, kernel_size);
```

### VMM: boot_params (zero page)

```c
/* e820 memory map */
e820[0]: 0x0 – 0x9FC00        (type=RAM)
e820[1]: 0x100000 – 128MB     (type=RAM)
```

For simplicity, microkvm exposes only two RAM regions. Real systems provide a more detailed e820 map including reserved firmware areas (EBDA, ACPI, etc.).

```c Command line */
strcpy(mem + CMDLINE_ADDR, "console=ttyS0 earlyprintk=serial rdinit=/init");
boot_params->cmd_line_ptr = CMDLINE_ADDR;

/* Loader metadata */
boot_params->type_of_loader = 0xFF;
boot_params->loadflags |= LOADED_HIGH | CAN_USE_HEAP;
```

### VMM: vCPU initialization (Linux Boot Protocol)

```c
sregs.cr0 = 0x11;              /* PE | ET (no paging) */
sregs.cs.db = 1;               /* 32-bit */
sregs.cs.g = 1;                /* 4KB granularity */
sregs.cs.limit = 0xFFFFFFFF;   /* flat 4GB */
/* DS/ES/FS/GS/SS: same flat data segments */

regs.rip = 0x100000;           /* startup_32 */
regs.rsi = 0x7000;             /* boot_params pointer */
```

Compare with Step 9's vCPU 1 (long mode): here we use 32-bit protected mode because the Linux Boot Protocol requires it. The kernel performs its own long mode transition - the same sequence we implemented in Step 4.

### VMM: UART (uart.c)

The UART is a state machine with DLAB-aware register dispatch:

```c
void uart_out(struct uart8250 *u, uint16_t port, uint8_t val, int vmfd) {
    int reg = port - UART_BASE;
    if (u->lcr & UART_LCR_DLAB) {
        if (reg == 0) { u->dll = val; return; }
        if (reg == 1) { u->dlm = val; return; }
    }
    switch (reg) {
    case UART_THR:  /* Transmit */
        putchar(val);
        if (u->ier & UART_IER_THRI) {
            u->pending_thre = 1;
            /* inject IRQ 4 via KVM_IRQ_LINE */
            /* IRQ 4 is the traditional interrupt line for COM1 (0x3F8) */
        }
        break;
    case UART_IER:
        u->ier = val;
        break;
    case UART_LCR:
        u->lcr = val;
        break;
    /* ... */
    }
}
```

## Prerequisites

### Building the kernel (bzImage)

microkvm boots a minimal Linux kernel built with `tinyconfig` plus the options needed for serial console and KVM paravirtualization.

```bash
# Get kernel source
git clone --depth 1 https://github.com/torvalds/linux.git ~/linux-src
cd ~/linux-src

# Start from minimal config and add required options
make tinyconfig

scripts/config --enable CONFIG_64BIT
scripts/config --enable CONFIG_PRINTK
scripts/config --enable CONFIG_TTY
scripts/config --enable CONFIG_SERIAL_8250
scripts/config --enable CONFIG_SERIAL_8250_CONSOLE
scripts/config --enable CONFIG_EARLY_PRINTK
scripts/config --enable CONFIG_KVM_GUEST
scripts/config --enable CONFIG_HYPERVISOR_GUEST
scripts/config --enable CONFIG_PARAVIRT
scripts/config --enable CONFIG_PARAVIRT_CLOCK
scripts/config --enable CONFIG_BLK_DEV_INITRD
scripts/config --enable CONFIG_BINFMT_ELF
scripts/config --enable CONFIG_BINFMT_SCRIPT
scripts/config --enable CONFIG_DEVTMPFS
scripts/config --enable CONFIG_DEVTMPFS_MOUNT
scripts/config --enable CONFIG_PROC_FS
scripts/config --enable CONFIG_SYSFS
scripts/config --disable CONFIG_VT

make olddefconfig
make -j$(nproc) bzImage

cp arch/x86/boot/bzImage ~/microkvm/
```

**Why these options:**
- `SERIAL_8250` + `SERIAL_8250_CONSOLE`: kernel output goes to our emulated UART
- `KVM_GUEST` + `PARAVIRT_CLOCK`: enables kvmclock for timer calibration (avoids TSC hang)
- `DEVTMPFS_MOUNT`: auto-creates `/dev` entries so init can run without manual mknod
- `VT=n`: prevents kernel from claiming `/dev/console` for a virtual terminal (we use serial)

### Building the initramfs

The kernel needs a root filesystem. We create a minimal initramfs with busybox and a shell script init:

```bash
# Get static busybox binary
cd /tmp
curl -o busybox https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox
chmod +x busybox

# Create directory structure
mkdir -p /tmp/initramfs_root/{bin,dev,proc,sys,lib/modules}
cp /tmp/busybox /tmp/initramfs_root/bin/

# Create symlinks for shell commands
cd /tmp/initramfs_root/bin
for cmd in sh echo cat ls mount mkdir uname head ps \
           dd wc sync free mv cp rm \
           hexdump devmem lspci \
           insmod rmmod lsmod dmesg grep; do
    ln -sf busybox $cmd
done

# Create init script
cat > /tmp/initramfs_root/init << 'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
echo "=== Hello from microkvm guest! ==="
exec /bin/sh
EOF
chmod +x /tmp/initramfs_root/init

# Pack as compressed cpio archive
cd /tmp/initramfs_root
find . | cpio -o -H newc | gzip > ~/microkvm/initramfs.gz
```

The VMM loads `initramfs.gz` into guest memory at GPA 0x4000000 and passes its address and size via `boot_params`. The kernel unpacks it as the root filesystem and executes `/init` as PID 1, which mounts essential filesystems and starts an interactive shell.

## Output

```
$ ./microkvm
bzImage: protocol 2.15, setup 16384 bytes, kernel 943104 bytes
Kernel loaded at 0x100000 (943104 bytes)
initramfs loaded at 0x4000000 (699991 bytes)
Starting guest...
Linux version 7.1.0+ ...
Command line: console=ttyS0 earlyprintk=serial rdinit=/init
...
Hypervisor detected: KVM
kvm-clock: Using msrs 4b564d01 and 4b564d00
...
serial8250: ttyS0 at I/O 0x3f8 (irq = 4, base_baud = 115200) is a 8250
...
Run /init as init process
=== Hello from microkvm guest! ===
/ #
```

## Key insight

Booting Linux is not a new concept - it is the **application** of every concept from Steps 1–9:

| Linux needs... | Provided by... |
|----------------|----------------|
| Protected mode entry | Step 3 (GDT, CR0.PE, flat segments) |
| Long mode transition | Step 4 (kernel does this itself) |
| Serial output | Step 2 (PIO) + UART state machine (Step 6 pattern) |
| Timer interrupts | Step 7 (IRQ injection via in-kernel irqchip) |
| kvmclock | Step 8 (MSR-based paravirt interface) |
| CPUID | Step 8 (hypervisor detection) |
| Memory | Step 4 (memory slots → e820 map) |

The VMM's role is to prepare the environment and emulate just enough hardware for the kernel to initialize. Once Linux is running, it uses the same mechanisms we built by hand in earlier steps - but now through real device drivers and kernel subsystems.

### What we did NOT implement

microkvm relies on KVM's in-kernel emulation for:
- **PIC/IOAPIC/LAPIC** - `KVM_CREATE_IRQCHIP`
- **PIT timer** - `KVM_CREATE_PIT2`
- **kvmclock MSRs** - handled by KVM internally

A production VMM like QEMU implements these in userspace for flexibility, but for booting a minimal kernel, KVM's built-in support is sufficient.

## What this teaches

| Concept | How it appears here |
|---------|-------------------|
| Linux Boot Protocol | boot_params, e820, cmdline, 32-bit entry |
| bzImage parsing | Setup header → kernel offset and size |
| 8250 UART | State machine with DLAB, IER, IIR, THR, LSR |
| In-kernel irqchip | KVM_CREATE_IRQCHIP + KVM_CREATE_PIT2 |
| CPUID passthrough | KVM_GET_SUPPORTED_CPUID → KVM_SET_CPUID2 |
| kvmclock | Paravirt time source via synthetic MSRs |
| initramfs | Loaded into guest memory, address passed via boot_params |

## Next step

[Step 11: Interactive shell](step11_interactive-shell.md) - add serial RX support so the guest can receive input from the host terminal, enabling an interactive busybox shell.

## What changed

Up to Step 9, every guest instruction was written by hand. From Step 10 onward, the guest is a real operating system that independently performs paging, interrupt handling, scheduling, and driver initialization.

The VMM's role has shifted from "execute a few instructions and observe exits" to "prepare an environment and let the kernel take over." This is exactly what QEMU does - and from here, the focus gradually shifts from basic correctness toward completeness and performance.
