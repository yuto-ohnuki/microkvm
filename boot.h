#ifndef BOOT_H
#define BOOT_H

#include <stdint.h>
#include <stddef.h>

int load_guest(const char *path, void *mem);

#endif /* BOOT_H */