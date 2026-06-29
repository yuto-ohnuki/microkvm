#include <stdint.h>
#include <string.h>
#include "boot.h"
#include "platform.h"

/*
 * Setup Intel MP (Multi-Processor) table in guest RAM.
 * Linux needs this to discover the IOAPIC, which is required for
 * MSI-X vector allocation (pci_alloc_irq_vectors).
 * Without MP table, Linux falls back to legacy PIC-only mode and
 * pci_alloc_irq_vectors(PCI_IRQ_MSIX) fails.
 *
 * Placed at GPA 0xF0000 (reserved BIOS area in e820 map).
 */
void setup_mp_table(void *mem)
{
    char *mp = (char *)mem + MP_TABLE_ADDR;

    /* MP Floating Pointer (16 bytes) */
    char *mpfp = mp;
    memset(mpfp, 0, 16);
    memcpy(mpfp, "_MP_", 4);
    *(uint32_t *)(mpfp + 4) = MP_TABLE_ADDR + 0x10;     /* phys addr of config table */
    mpfp[8] = 1;        /* length in 16-byte units */
    mpfp[9] = 4;        /* spec revision 1.4 */

    /* MP Config Table */
    char *mpc = mp + 0x10;
    char *entry = mpc + 44;

    /* Processor entry (20 bytes) */
    memset(entry, 0, 20);
    entry[0] = 0;       /* type: processor */
    entry[1] = 0;       /* LAPIC ID */
    entry[2] = 0x14;    /* LAPIC version */
    entry[3] = 0x03;    /* flags: EN + BSP */
    entry += 20;

    /* Bus entry: ISA (8 bytes) */
    memset(entry, 0, 8);
    entry[0] = 1;       /* type: bus */
    entry[1] = 0;       /* bus ID 0 = ISA */
    memcpy(entry + 2, "ISA   ", 6);
    entry += 8;

    /* Bus entry: PCI (8 bytes) */
    memset(entry, 0, 8);
    entry[0] = 1;       /* type: bus */
    entry[1] = 1;       /* bus ID 1 = PCI */
    memcpy(entry + 2, "PCI   ", 6);
    entry += 8;

    /* IOAPIC entry (8 bytes) */
    memset(entry, 0, 8);
    entry[0] = 2;       /* type: IOAPIC */
    entry[1] = 2;       /* IOAPIC ID */
    entry[2] = 0x14;    /* version */
    entry[3] = 0x01;    /* flags: enabled */
    *(uint32_t *)(entry + 4) = 0xFEC00000;
    entry += 8;

    /* I/O interrupt entries: ISA IRQ 0-15 -> IOAPIC pin 0-15 */
    for (int i = 0; i < 16; i++) {
        memset(entry, 0, 8);
        entry[0] = 3;   /* type: I/O interrupt */
        entry[1] = 0;   /* INT type */
        entry[4] = 0;   /* source bus (ISA) */
        entry[5] = i;   /* source IRQ */
        entry[6] = 2;   /* dest IOAPIC ID */
        entry[7] = i;   /* dest IOAPIC pin */
        entry += 8;
    }

    /* Fill config table header */
    uint16_t table_len = (uint16_t)(entry - mpc);
    memset(mpc, 0, 44);
    memcpy(mpc, "PCMP", 4);
    *(uint16_t *)(mpc + 4) = table_len;
    mpc[6] = 4;                             /* spec revision 1.4 */
    memcpy(mpc + 8, "MICROKVM", 8);         /* OEM ID */
    memcpy(mpc + 16, "MKVMCONFIG  ", 12);   /* product ID */
    *(uint32_t *)(mpc + 36) = 0xFEE00000;   /* LAPIC address */

    /* Config table checksum */
    uint8_t cksum = 0;
    for (int i = 0; i < table_len; i++)
        cksum += (uint8_t)mpc[i];
    mpc[7] = (uint8_t)(0 - cksum);

    /* Floating pointer checksum */
    cksum = 0;
    for (int i = 0; i < 16; i++)
        cksum += (uint8_t)mpfp[i];
    mpfp[10] = (uint8_t)(0 - cksum);
}