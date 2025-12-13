// Verify that MAP_SHARED writes are persisted to the underlying file.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

int main(void) {
    const char *path = "/tmp/mmap_shared_writeback_test.bin";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // Prepare file size.
    if (ftruncate(fd, PAGE_SIZE) != 0) {
        perror("ftruncate");
        close(fd);
        unlink(path);
        return 1;
    }

    // MAP_SHARED, write pattern.
    void *addr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        unlink(path);
        return 1;
    }

    memset(addr, 0xab, PAGE_SIZE);
    // Ensure it is pushed; optional msync to avoid lazy write timing differences.
    if (msync(addr, PAGE_SIZE, MS_SYNC) != 0) {
        perror("msync");
        munmap(addr, PAGE_SIZE);
        close(fd);
        unlink(path);
        return 1;
    }
    munmap(addr, PAGE_SIZE);

    // Read back via plain read to verify file content updated.
    lseek(fd, 0, SEEK_SET);
    unsigned char buf[PAGE_SIZE];
    ssize_t n = read(fd, buf, PAGE_SIZE);
    if (n != PAGE_SIZE) {
        perror("read");
        close(fd);
        unlink(path);
        return 1;
    }
    for (int i = 0; i < PAGE_SIZE; i++) {
        if (buf[i] != 0xab) {
            fprintf(stderr, "BUG: offset %d got %#x expected 0xab\n", i, buf[i]);
            close(fd);
            unlink(path);
            return 1;
        }
    }

    close(fd);
    unlink(path);
    printf("mmap_shared_writeback_test: PASS\n");
    return 0;
}
