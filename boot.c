#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "boot.h"
#include "microkvm.h"
#include "platform.h"

/* Load bzImage and set up boot_params per Linux Boot Protocol */
int load_bzimage(const char *path, void *mem, const char *cmdline)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return -1;
    }

    struct stat st;
    fstat(fd, &st);

    void *bzimage = malloc(st.st_size);
    if (!bzimage) {
        perror("malloc");
        close(fd);
        return -1;
    }
    read(fd, bzimage, st.st_size);
    close(fd);

    /* Parse setup header */
    uint8_t *hdr = bzimage;
    uint8_t setup_sects = hdr[0x1F1];
    if (setup_sects == 0)
        setup_sects = 4;
    uint32_t setup_size = (setup_sects + 1) * 512;
    uint32_t kernel_size = st.st_size - setup_size;

    if (memcmp(hdr + 0x202, "HdrS", 4) != 0) {
        fprintf(stderr, "Not a valid bzImage (no HdrS magic)\n");
        free(bzimage);
        return -1;
    }

    uint16_t protocol = *(uint16_t *)(hdr + 0x206);
    printf("bzImage: protocol %d.%d, setup %d bytes, kernel %d bytes\n",
        protocol >> 8, protocol & 0xff, setup_size, kernel_size);

    /* Setup boot_params (zero page) */
    memset((char *)mem + BOOT_PARAMS_ADDR, 0, 4096);
    memcpy((char *)mem + BOOT_PARAMS_ADDR + 0x1F1, hdr + 0x1F1, setup_size - 0x1F1);

    /* Command line */
    strcpy((char *)mem + CMDLINE_ADDR, cmdline);
    *(uint32_t *)((char *)mem + BOOT_PARAMS_ADDR + 0x228) = CMDLINE_ADDR;

    /* type_of_loader (required, non-zero) */
    *((char *)mem + BOOT_PARAMS_ADDR + 0x210) = 0xFF;

    /* loadflags: LOADED_HIGH | CAN_USE_HEAP */
    *((char *)mem + BOOT_PARAMS_ADDR + 0x211) |= 0x01 | 0x80;

    /* Copy protected-mode kernel to 1MB */
    memcpy((char *)mem + KERNEL_ADDR, (char *)bzimage + setup_size, kernel_size);
    printf("Kernel loaded at 0x%x (%d bytes)\n", KERNEL_ADDR, kernel_size);

    /* e820 memory map (offset 0x2D0, each entry 20 bytes) */
    char *e820 = (char *)mem + BOOT_PARAMS_ADDR + 0x2D0;
    /* Entry 0: 0 - 0x9F000 = usable RAM */
    *(uint64_t *)(e820 + 0)  = 0;
    *(uint64_t *)(e820 + 8)  = 0x9F000;
    *(uint32_t *)(e820 + 16) = 1;   /* E820_RAM */
    /* Entry 1: 0x9F000 - 0x100000 = reserved (MP table + BIOS area) */
    *(uint64_t *)(e820 + 20) = 0x9F000;
    *(uint64_t *)(e820 + 28) = 0x100000 - 0x9F000;
    *(uint32_t *)(e820 + 36) = 2;   /* E820_RESERVED */
    /* Entry 2: 0x100000 - end = usable RAM */
    *(uint64_t *)(e820 + 40) = 0x100000;
    *(uint64_t *)(e820 + 48) = GUEST_MEM_SIZE - 0x100000;
    *(uint32_t *)(e820 + 56) = 1;   /* E820_RAM */
    *((char *)mem + BOOT_PARAMS_ADDR + 0x1E8) = 3;  /* 3 entries */

    free(bzimage);
    return 0;
}

/* Load initramfs into guest memory and update boot_params */
int load_initramfs(const char *path, void *mem, uint32_t *out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    struct stat st;
    fstat(fd, &st);
    read(fd, (char *)mem + INITRD_ADDR, st.st_size);
    close(fd);

    *(uint32_t *)((char *)mem + BOOT_PARAMS_ADDR + 0x218) = INITRD_ADDR;
    *(uint32_t *)((char *)mem + BOOT_PARAMS_ADDR + 0x21C) = st.st_size;

    *out_size = st.st_size;
    printf("initramfs loaded at 0x%x (%ld bytes)\n", INITRD_ADDR, st.st_size);

    /* Setup MP table for IOAPIC discovery */
    setup_mp_table(mem);
    return 0;
}
