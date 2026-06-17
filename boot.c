#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "boot.h"

/* Load a flat binary into guest memory at GPA 0x0 */
int load_guest(const char *path, void *mem) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return -1;
    }

    struct stat st;
    fstat(fd, &st);
    read(fd, mem, st.st_size);
    close(fd);
    printf("Loaded guest: %ld bytes\n", st.st_size);
    return 0;
}