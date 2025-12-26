#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main() {
    int fd = open("/tmp/test", O_RDONLY);  // Assume example.txt is 5000 bytes
    if (fd == -1) { perror("open"); return 1; }

    // Map 3 pages , even though file is only 5000 bytes
    void *map = mmap(NULL, 4096 * 3, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); return 1; }

    // Accessing beyond file end  may SIGBUS
    char byte = *((char *)map + 8192);  // This could raise SIGBUS
    printf("Byte: %c\n", byte);  // Unlikely to reach here

    munmap(map, 8192);
    close(fd);
    return 0;
}
