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

    hlt

/* ============ Data ============ */

.align 8
gdt:
    .quad 0                          /* GDT[0]: null */
    .quad 0x00CF9A000000FFFF         /* GDT[1] (0x08): code (32-bit, exec/read) */
    .quad 0x00CF92000000FFFF         /* GDT[2] (0x10): data (32-bit, read/write) */

gdt_desc:
    .word gdt_desc - gdt - 1         /* limit: size of GDT - 1 */
    .long gdt                        /* base: address of GDT */
