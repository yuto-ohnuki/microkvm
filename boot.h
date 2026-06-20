#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>
#include <stddef.h>

/* Guest physical address layout for Linux boot */
#define BOOT_PARAMS_ADDR  0x7000
#define CMDLINE_ADDR      0x20000
#define KERNEL_ADDR       0x100000
#define INITRD_ADDR       0x4000000

int load_bzimage(const char *path, void *mem, const char *cmdline);
int load_initramfs(const char *path, void *mem, uint32_t *out_size);

#endif /* BOOT_H */