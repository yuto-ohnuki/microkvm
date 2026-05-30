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

    /* Enable paging (CR0 bit 31) → activates long mode */
    mov %cr0, %eax
    or $0x80000000, %eax
    mov %eax, %cr0

    /* Far jump to 64-bit code segment */
    ljmpl *far_ptr

long_mode:
    .byte 0xB0, 'L'      /* mov al, 'L' */
    .byte 0xE6, 0x10     /* out 0x10, al */
    .byte 0xB0, '\n'     /* mov al, '\n' */
    .byte 0xE6, 0x10     /* out 0x10, al */
    .byte 0xF4           /* hlt */

/* ============ Data ============ */

.align 4
far_ptr:
    .long long_mode      /* offset */
    .word 0x18           /* selector: GDT[3] = 64-bit code */

.align 8
gdt:
    .quad 0                          /* GDT[0]: null */
    .quad 0x00CF9A000000FFFF         /* GDT[1]: code (32-bit, exec/read) */
    .quad 0x00CF92000000FFFF         /* GDT[2]: data (32-bit, read/write) */
    .quad 0x00209A0000000000         /* GDT[3] (selector 0x18): 64-bit code */
    .quad 0x0000920000000000         /* GDT[4] (selector 0x20): 64-bit data */

gdt_desc:
    .word gdt_desc - gdt - 1         /* limit: size of GDT - 1 */
    .long gdt                        /* base: address of GDT */
