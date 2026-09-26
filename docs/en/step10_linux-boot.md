# Step 10: ★ Minimal Linux Boot

## Goal

Replace the handwritten guest with a Linux kernel and boot with 1 vCPU and 128MB of memory. This step covers displaying boot logs and a shell prompt through the UART. Host terminal input is connected in Step 11.

From this step onward, microkvm runs an unmodified Linux kernel.

## Background

### What Linux needs to boot

To boot the Linux kernel (bzImage), this step provides:

1. **Memory**—enough RAM and a valid e820 map describing it
2. **Boot protocol**—boot_params (the zero page) containing the command line, memory map, and loader metadata
3. **CPU state**—32-bit protected mode with flat segments (the kernel transitions to long mode itself)
4. **Serial console**—output through an 8250 UART at I/O port 0x3F8
5. **Interrupt delivery**—provided by KVM's in-kernel irqchip (PIC, IOAPIC, LAPIC)
6. **CPUID**—KVM-filtered CPU feature information (the kernel checks long-mode support during early boot)
7. **Clock source**—kvmclock or a calibrated TSC for timekeeping

### Linux boot protocol

The main initial conditions for the 32-bit entry used here are:

- CPU in 32-bit protected mode, paging disabled
- Flat segments in CS/DS/ES/FS/GS/SS (base=0, limit=0xFFFFFFFF)
- `%esi` points to `boot_params` (the "zero page")
- Kernel loaded at physical address 0x100000 (1MB)

The VMM sets `RIP = 0x100000`, the compressed kernel's entry point. From there, the 64-bit kernel transitions to long mode, decompresses itself, and eventually reaches `start_kernel`.

### bzImage structure

```
┌──────────────────────┬─────────────────────────────────┐
│ Setup (real mode)    │ Protected-mode kernel           │
│ ~16KB                │ (compressed, loaded at 1MB)     │
└──────────────────────┴─────────────────────────────────┘
         ↑                          ↑
    Setup header            Compressed kernel entry
    (protocol version,      (enter long mode,
     loadflags, etc.)        decompress itself,
                             reach start_kernel)
```

The VMM parses the setup header to locate the kernel offset and copies the protected-mode portion to 0x100000.

### 8250 UART emulation

Linux's serial console driver (`8250`) communicates through PIO at 0x3F8–0x3FF.
The VMM must emulate enough of the UART to support output:

| Register | Port | Read | Write |
|---------|--------|------|-------|
| THR/RBR | 0x3F8 | Receive buffer | Transmit data |
| IER | 0x3F9 | Enable state | Interrupt enable |
| IIR | 0x3FA | Interrupt ID | — |
| LCR | 0x3FB | Line control state | Line control (DLAB) |
| MCR | 0x3FC | Modem control state | Modem control |
| LSR | 0x3FD | Line status | — |

DLAB (Divisor Latch Access Bit) in LCR changes the meaning of ports 0x3F8 and 0x3F9: with DLAB=1, they become baud-rate divisor registers. Without DLAB handling, the driver's baud-rate initialization would be mistaken for character output.

### In-kernel PIC and PIT

Instead of emulating interrupt controllers in userspace, use KVM's built-in emulation:

```c
ioctl(vmfd, KVM_CREATE_IRQCHIP, 0);     /* PIC (8259) + IOAPIC + LAPIC */
ioctl(vmfd, KVM_CREATE_PIT2, &pit);     /* PIT (8254) timer */
```

This provides working timer interrupts without userspace involvement: KVM handles the entire interrupt-delivery path internally.

### kvmclock and CPUID

Obtain the CPU features KVM can expose to the guest with `KVM_GET_SUPPORTED_CPUID`, clear the TSC-deadline bit, and apply them with `KVM_SET_CPUID2`. Host CPUID is not passed through unchanged.

Linux detects kvmclock through KVM's CPUID leaves. It obtains time information through shared memory registered via MSRs handled by KVM. The custom MSR handler from Step 8 is not used.

Set the TSC frequency to 1 GHz. If this setting fails, report the error and continue booting.

```c
if (ioctl(vcpus[i].fd, KVM_SET_TSC_KHZ, 1000000UL) < 0) {
    perror("KVM_SET_TSC_KHZ");
}
```

## Execution flow

```text
Guest (Linux)               KVM                         VMM
     |                       |                           |
     |                       |<-- Create IRQCHIP / PIT2 --|
     |                       |<-- Register memory slots -|
     |                       |                           | Place bzImage and initramfs
     |                       |                           | Set up boot_params
     |                       |<-- CPUID, TSC, initial state
     |                       |<-- KVM_RUN ---------------|
     |<-- Run in 32-bit mode |                           |
     | Enter long mode       |                           |
     | Decompress/init kernel|                           |
     |-- PIO to UART ------->|-- KVM_EXIT_IO ------------>|
     |                       |                           | uart_in / uart_out
     |                       |<-- KVM_RUN ---------------|
     |<-- Continue ----------|                           |
     | Unpack initramfs      |                           |
     | /init → start shell   |                           |
     |-- UART output ------->|-- KVM_EXIT_IO ------------>|
     |                       |                           | Print greeting and prompt
```

When UART transmit interrupts are enabled, the VMM raises and lowers IRQ 4 through `KVM_IRQ_LINE`. KVM's irqchip handles delivery to the guest.

## Implementation

### VMM: bzImage loader

```c
/* Parse the setup header */
uint8_t setup_sects = hdr[0x1F1];
if (setup_sects == 0) setup_sects = 4;
uint32_t setup_size = (setup_sects + 1) * 512;
uint32_t kernel_size = st.st_size - setup_size;

/* Copy the protected-mode kernel to 1MB */
memcpy((char *)mem + KERNEL_ADDR, (char *)bzimage + setup_size, kernel_size);
```

### VMM: boot_params (zero page)

`boot.c` places boot_params at GPA `0x7000`. e820 tells Linux which physical memory is available; it is separate from KVM memory slot registration.

| Range (end exclusive) | Type |
|----------------------|------|
| `0x0`–`0x9F000` | RAM |
| `0x9F000`–`0x100000` | Reserved |
| `0x100000`–128MB | RAM |

Pass three entries: two RAM regions and one reserved region. The existing MMIO hole at `0xD0000`–`0xD1000` lies within the reserved region.

```c
/* Command line and its pointer */
strcpy((char *)mem + CMDLINE_ADDR, cmdline);
*(uint32_t *)((char *)mem + BOOT_PARAMS_ADDR + 0x228) = CMDLINE_ADDR;

/* type_of_loader / loadflags */
*((char *)mem + BOOT_PARAMS_ADDR + 0x210) = 0xFF;
*((char *)mem + BOOT_PARAMS_ADDR + 0x211) |= 0x01 | 0x80;
```

The command line is `console=ttyS0 earlyprintk=serial rdinit=/init`. Place initramfs at GPA `0x4000000` (64MB), and write its address and size to `ramdisk_image` / `ramdisk_size` in boot_params.

### VMM: vCPU initialization (Linux boot protocol)

```c
sregs.cr0 = 0x11;              /* PE | ET (no paging) */
sregs.cs.db = 1;               /* 32-bit */
sregs.cs.g = 1;                /* 4KB granularity */
sregs.cs.limit = 0xFFFFFFFF;   /* Flat 4GB */
/* DS/ES/FS/GS/SS: same flat data segment */

regs.rip = KERNEL_ADDR;        /* 0x100000: startup_32 */
regs.rsi = BOOT_PARAMS_ADDR;   /* 0x7000: boot_params */
```

Unlike vCPU 1 in Step 9 (long mode), this entry uses 32-bit protected mode as required by the Linux boot protocol. The VMM directly sets segment state through `KVM_SET_SREGS`; Linux then builds page tables and transitions to long mode.

### VMM: UART (uart.c)

Excerpt of the transmit and interrupt-enable handling in `uart_out()`:

```c
case UART_THR:
    if (u->lcr & UART_LCR_DLAB) {
        u->dll = val;
    } else {
        write(STDOUT_FILENO, &val, 1);
        u->lsr |= UART_LSR_THRE | UART_LSR_TEMT;
        uart_maybe_raise_thre(u, vmfd);
    }
    break;
case UART_IER:
    if (u->lcr & UART_LCR_DLAB) {
        u->dlm = val;
    } else {
        u->ier = val;
        if (u->lsr & UART_LSR_THRE)
            uart_maybe_raise_thre(u, vmfd);
    }
    break;
```

With DLAB=0, transmit data is written to host standard output. If IER is enabled while the transmit buffer is empty, `uart_maybe_raise_thre()` also marks an unreported transmit interrupt pending. Reading IIR from Linux clears that pending transmit interrupt.

`uart_rx()` exists in this tag, but there is no code yet to read host standard input and call it.

## Prerequisites

### Build the kernel (bzImage)

Run the following on an x86_64 Linux host. Separately from microkvm's `make`, build a Linux kernel from `tinyconfig` with the options needed for a serial console and KVM paravirtualization:

```bash
# Obtain kernel sources
$ git clone --depth 1 https://github.com/torvalds/linux.git ~/linux-src
$ cd ~/linux-src

# Add required options to the minimal configuration
$ make tinyconfig

$ scripts/config --enable CONFIG_64BIT
$ scripts/config --enable CONFIG_PRINTK
$ scripts/config --enable CONFIG_TTY
$ scripts/config --enable CONFIG_SERIAL_8250
$ scripts/config --enable CONFIG_SERIAL_8250_CONSOLE
$ scripts/config --enable CONFIG_EARLY_PRINTK
$ scripts/config --enable CONFIG_KVM_GUEST
$ scripts/config --enable CONFIG_HYPERVISOR_GUEST
$ scripts/config --enable CONFIG_PARAVIRT
$ scripts/config --enable CONFIG_PARAVIRT_CLOCK
$ scripts/config --enable CONFIG_BLK_DEV_INITRD
$ scripts/config --enable CONFIG_RD_GZIP
$ scripts/config --enable CONFIG_BINFMT_ELF
$ scripts/config --enable CONFIG_BINFMT_SCRIPT
$ scripts/config --enable CONFIG_DEVTMPFS
$ scripts/config --enable CONFIG_PROC_FS
$ scripts/config --enable CONFIG_SYSFS
$ scripts/config --disable CONFIG_VT

$ make olddefconfig
$ make -j$(nproc) bzImage

$ cp arch/x86/boot/bzImage ~/microkvm/
```

**Why each option is needed:**

- `SERIAL_8250` + `SERIAL_8250_CONSOLE`: send kernel output to the emulated UART
- `KVM_GUEST` + `PARAVIRT_CLOCK`: enable kvmclock (avoid a TSC calibration hang)
- `BLK_DEV_INITRD` + `RD_GZIP`: load an external gzip-compressed initramfs
- `DEVTMPFS`: provide device nodes in `/dev`; `/init` explicitly mounts it in initramfs
- `VT=n`: omit unused virtual terminals; select the serial console with `console=ttyS0`

### Build initramfs

The kernel needs a root filesystem. Create a minimal initramfs with busybox and a shell-script init:

```bash
# Obtain a static busybox binary
$ cd /tmp
$ curl -fL -o busybox https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox
$ chmod +x busybox

# Create the directory structure
$ mkdir -p /tmp/initramfs_root/{bin,dev,proc,sys,lib/modules}
$ cp /tmp/busybox /tmp/initramfs_root/bin/

# Initial console for /init's standard input/output
$ if [ ! -e /tmp/initramfs_root/dev/console ]; then
    sudo mknod -m 600 /tmp/initramfs_root/dev/console c 5 1
fi

# Create symlinks for shell commands
$ cd /tmp/initramfs_root/bin
$ for cmd in sh echo cat ls mount mkdir uname head ps \
           dd wc sync free mv cp rm touch \
           hexdump devmem lspci \
           insmod rmmod lsmod dmesg grep; do
    ln -sf busybox $cmd
done

# Create the init script
$ cat > /tmp/initramfs_root/init << 'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
echo "=== Hello from microkvm guest! ==="
exec /bin/sh
EOF
$ chmod +x /tmp/initramfs_root/init

# Pack as a compressed cpio archive
$ cd /tmp/initramfs_root
$ find . | cpio -o -H newc | gzip > ~/microkvm/initramfs.gz
```

The VMM loads `initramfs.gz` at GPA 0x4000000 and passes its address and size through `boot_params`. The kernel unpacks it as the root filesystem and executes `/init` as PID 1. `/init` mounts proc, sysfs, and devtmpfs, then starts a shell. `/dev/console` in the archive provides standard input/output before `/init` mounts devtmpfs.

## Output

Place `bzImage` and `initramfs.gz` in the microkvm directory and run it. `make` builds only the VMM.

```bash
$ cd ~/microkvm
$ make clean
$ make
$ ./microkvm
```

```
$ ./microkvm
bzImage: protocol 2.15, setup 16384 bytes, kernel 939008 bytes
Kernel loaded at 0x100000 (939008 bytes)
initramfs loaded at 0x4000000 (699794 bytes)
Starting guest...
Linux version 7.3.0-rc4+ ...
Command line: console=ttyS0 earlyprintk=serial rdinit=/init
...
Hypervisor detected: KVM
kvm-clock: Using msrs 4b564d01 and 4b564d00
...
clocksource: Switched to clocksource kvm-clock
...
clocksource: Switched to clocksource tsc
...
serial8250: ttyS0 I/O:0x3f8 (irq = 4, base_baud = 115200) is a 8250
...
Run /init as init process
=== Hello from microkvm guest! ===
/bin/sh: can't access tty; job control turned off
/ #
```

`Hypervisor detected: KVM` and `kvm-clock` show KVM detection and clock-source initialization; the greeting and `/ #` show that `/init` and the shell have started. Input at the prompt is not available in this step. Press `Ctrl-C` in the host terminal to exit.

## Key insight

The VMM prepares the memory layout, initial CPU state, and boot information, leaving subsequent OS initialization to Linux. The VMM handles UART PIO, while KVM handles PIC/IOAPIC/LAPIC, PIT, and kvmclock.

## What this step teaches

| Concept | How it appears here |
|------|--------------|
| Linux boot protocol | boot_params, e820, cmdline, 32-bit entry |
| bzImage parsing | Setup header → kernel offset and size |
| 8250 UART | State machine using DLAB, IER, IIR, THR, and LSR |
| In-kernel irqchip | KVM_CREATE_IRQCHIP + KVM_CREATE_PIT2 |
| CPUID setup | KVM_GET_SUPPORTED_CPUID → remove TSC-deadline → KVM_SET_CPUID2 |
| kvmclock | Paravirtualized clock source with shared memory registered through MSRs |
| initramfs | Load into guest memory; pass the address through boot_params |

## What changed

- Replace `guest.bin` with `bzImage` + `initramfs.gz`; add loaders and boot_params setup.
- Change from 2 vCPUs to 1 and from 1MB of guest memory to 128MB.
- Replace custom PIO output and MMIO counter handling with an 8250 UART.
- Add CPUID setup, an in-kernel irqchip / PIT, and TSC frequency setup.

## Next step

[Step 11: Interactive Shell](step11_interactive-shell.md)—Add serial RX support so the guest can accept input from the host terminal, enabling an interactive busybox shell.
