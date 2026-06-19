.code16
.global _start
_start:
    /* Real mode: output 'R' via PIO  */
    mov $'R', %al
    out %al, $0x10
    mov $'\n', %al
    out %al, $0x10

    /* Load GDT */
    lgdt gdt_desc

    /* Enable protected mode: set CR0.PE = 1 */
    mov %cr0, %eax
    or $1, %eax
    mov %eax, %cr0

    /* Far jump to reload CS with 32-bit code segment */
    ljmp $0x08, $protected_mode   /* selector for GDT[1] */

.code32
protected_mode:
    /* Set data segments to GDT[2] */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %ss

    /* Protected mode: output 'P' via PIO */
    mov $'P', %al
    out %al, $0x10
    mov $'\n', %al
    out %al, $0x10

    /* CR3 = PML4 */
    mov $0x70000, %eax
    mov %eax, %cr3

    /* CR4.PAE = 1 */
    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4

    /* Enable paging (CR0 bit 31), which activates long mode */
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0

    /* Far jump to 64-bit code segment */
    ljmpl *far_ptr

long_mode:
    /* Set up stack (required for interrupt handler to push RIP/CS/RFLAGS) */
    .byte 0x48, 0xC7, 0xC4, 0x00, 0x00, 0x06, 0x00  /* mov rsp, 0x60000 */

    /* PIO: confirm long mode entry */
    .byte 0xB0, 'L'      /* mov al, 'L' */
    .byte 0xE6, 0x10     /* out 0x10, al */
    .byte 0xB0, '\n'     /* mov al, '\n' */
    .byte 0xE6, 0x10     /* out 0x10, al */

    /* MMIO write: store to 0xD0000 → EPT violation → KVM_EXIT_MMIO */
    .byte 0x48, 0xC7, 0xC3, 0x00, 0x00, 0x0D, 0x00  /* mov rbx, 0xD0000 */
    .byte 0xB0, 'M'                                 /* mov al, 'M' */
    .byte 0x88, 0x03                                /* mov [rbx], al */
    .byte 0xB0, '\n'                                /* mov al, '\n' */
    .byte 0x88, 0x03                                /* mov [rbx], al */

    /* MMIO read: read counter from device at 0xD0000 */
    .byte 0x8A, 0x03                                /* mov al, [rbx] */
    .byte 0x04, 0x30                                /* add al, '0' */
    .byte 0xE6, 0x10                                /* out 0x10, al */
    .byte 0xB0, '\n'                                /* mov al, '\n' */
    .byte 0xE6, 0x10                                /* out 0x10, al */

    /* Set up IDT and wait for interrupt */
    .byte 0x48, 0xC7, 0xC1                          /* mov rcx, imm32 */
    .long idt_desc                                  /* absolute address of idt_desc */
    .byte 0x0F, 0x01, 0x19                          /* lidt [rcx] */

    .byte 0xFB                                      /* sti */
    .byte 0xF4                                      /* hlt - wait for interrupt */

    /* After iretq returns here */
    .byte 0xF4                                      /* hlt */

/* ============ Data ============ */

.align 4
far_ptr:
    .long long_mode      /* offset */
    .word 0x18           /* selector: GDT[3] = 64-bit code */

/* === GDT === */

.align 8
gdt:
    .quad 0                          /* GDT[0]: null */
    .quad 0x00CF9A000000FFFF         /* GDT[1] (0x08): code (32-bit, exec/read) */
    .quad 0x00CF92000000FFFF         /* GDT[2] (0x10): data (32-bit, read/write) */
    .quad 0x00209A0000000000         /* GDT[3] (0x18): 64-bit code */
    .quad 0x0000920000000000         /* GDT[4] (0x20): 64-bit data */

gdt_desc:
    .word gdt_desc - gdt - 1         /* limit: size of GDT - 1 */
    .long gdt                        /* base: address of GDT */

/* === Interrupt handler (vector 32) === */
.align 16
irq_handler:
    .byte 0xB0, 'I'                  /* mov al, 'I' */
    .byte 0xE6, 0x10                 /* out 0x10, al */
    .byte 0xB0, '\n'                 /* mov al, '\n' */
    .byte 0xE6, 0x10                 /* out 0x10, al */
    .byte 0x48, 0xCF                 /* iretq */

/* === IDT === */
.align 16
idt:
    .fill 64, 8, 0                   /* vectors 0-31: null */

    /* Vector 32: interrupt gate → irq_handler */
    .word irq_handler                /* offset_low (address fits in 16 bits) */
    .word 0x18                       /* selector: 64-bit code */
    .byte 0x00                       /* IST = 0 */
    .byte 0x8E                       /* present, DPL=0, interrupt gate */
    .word 0x0000                     /* offset_mid = 0 (handler < 64KB) */
    .long 0x00000000                 /* offset_high = 0 */
    .long 0x00000000                 /* reserved */

idt_desc:
    .word idt_desc - idt - 1         /* limit */
    .long idt                        /* base_low (fits in 32 bits) */
    .long 0                          /* base_high = 0 */
