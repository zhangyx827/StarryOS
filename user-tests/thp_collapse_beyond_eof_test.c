// Verify that attempting to collapse a range extending beyond EOF fails and
// does not corrupt data. Linux returns SIGBUS on fault and EINVAL on collapse
// requests; we accept EINVAL/ENOTSUP/ENOMEM here.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define HUGE_SIZE (2UL * 1024 * 1024)

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void verify_pattern(uint8_t *addr, size_t len, uint8_t pat, const char *tag) {
    for (size_t i = 0; i < len; i++) {
        if (addr[i] != pat) {
            fprintf(stderr, "%s: byte %zu got %#x expected %#x\n", tag, i, addr[i], pat);
            exit(1);
        }
    }
}

int main(void) {
    const char *path = "/tmp/thp_collapse_beyond_eof_test.bin";
    // File size 1.5MiB < 2MiB collapse window.
    size_t file_size = (HUGE_SIZE + HUGE_SIZE / 2);
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) die("open");
    if (ftruncate(fd, file_size) != 0) die("ftruncate");

    void *addr = mmap(NULL, HUGE_SIZE * 3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) die("mmap");
    memset(addr, 0x5a, HUGE_SIZE * 1.5); // fill full 2MiB mapping

    // Attempt to collapse full 2MiB while file is only 1.5MiB.
    if (madvise(addr, HUGE_SIZE * 3, MADV_COLLAPSE) == 0) {
        fprintf(stderr, "BUG: madvise(MADV_COLLAPSE) succeeded beyond EOF\n");
        munmap(addr, HUGE_SIZE * 2);
        close(fd);
        unlink(path);
        return 1;
    } else {
        int e = errno;
        if (e != EINVAL && e != ENOTSUP && e != ENOMEM) {
            fprintf(stderr, "Unexpected errno=%d\n", e);
            munmap(addr, HUGE_SIZE * 2);
            close(fd);
            unlink(path);
            return 1;
        }
    }

    // Verify original pattern intact within file size.
    verify_pattern((uint8_t *)addr, file_size, 0x5a, "content");

    munmap(addr, HUGE_SIZE * 2);
    close(fd);
    unlink(path);
    printf("thp_collapse_beyond_eof_test: PASS\n");
    return 0;
}
