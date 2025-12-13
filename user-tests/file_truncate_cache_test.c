// Verify cached file truncate/extend correctness.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PATH_PRIMARY   "/tmp/axfs_file_cache_test.bin"
#define PATH_FALLBACK  "./axfs_file_cache_test.bin"
#define PAGE_SIZE_4K   4096UL
#define FILE_LEN       (3UL * 1024 * 1024)  // Cross the 2MiB boundary.

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void fill_pattern(uint8_t *p, size_t len) {
    size_t pages = len / PAGE_SIZE_4K;
    for (size_t i = 0; i < pages; ++i) {
        memset(p + i * PAGE_SIZE_4K, (uint8_t)(i & 0xff), PAGE_SIZE_4K);
    }
}

static void expect_zero(int fd, off_t off, size_t len, const char *what) {
    uint8_t buf[PAGE_SIZE_4K];
    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }
    ssize_t n = pread(fd, buf, len, off);
    if (n < 0) {
        die(what);
    }
    if ((size_t)n != len) {
        fprintf(stderr, "%s: short read (%zd)\n", what, n);
        exit(1);
    }
    for (size_t i = 0; i < (size_t)n; ++i) {
        if (buf[i] != 0) {
            fprintf(stderr, "%s: expected zero, got %#x at off %lld\n",
                    what, buf[i], (long long)(off + (off_t)i));
            exit(1);
        }
    }
}

int main(void) {
    const char *path = PATH_PRIMARY;
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open /tmp failed, fallback to cwd");
        path = PATH_FALLBACK;
        fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) {
            die("open");
        }
    }

    if (ftruncate(fd, FILE_LEN) != 0) {
        die("ftruncate (grow)");
    }

    /* Touch the whole range via MAP_SHARED so FileBackend cached path is used. */
    void *addr = mmap(NULL, FILE_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        die("mmap");
    }
    fill_pattern((uint8_t *)addr, FILE_LEN);
    if (msync(addr, FILE_LEN, MS_SYNC) != 0) {
        die("msync");
    }
    if (munmap(addr, FILE_LEN) != 0) {
        die("munmap");
    }

    /* Verify persisted pattern via plain read(). */
    uint8_t buf[PAGE_SIZE_4K];
    size_t pages = FILE_LEN / PAGE_SIZE_4K;
    for (size_t i = 0; i < pages; ++i) {
        ssize_t n = pread(fd, buf, sizeof(buf), (off_t)(i * PAGE_SIZE_4K));
        if (n != (ssize_t)sizeof(buf)) {
            die("pread verify");
        }
        if (buf[0] != (uint8_t)(i & 0xff) || buf[PAGE_SIZE_4K - 1] != (uint8_t)(i & 0xff)) {
            fprintf(stderr, "verify mismatch at page %zu: got %#x/%#x\n",
                    i, buf[0], buf[PAGE_SIZE_4K - 1]);
            return 1;
        }
    }

    /* Shrink the file; cache beyond new EOF must not leak. */
    off_t shrink_len = FILE_LEN / 2;  // 1.5MiB
    if (ftruncate(fd, shrink_len) != 0) {
        die("ftruncate (shrink)");
    }
    ssize_t at_eof = pread(fd, buf, 1, FILE_LEN - PAGE_SIZE_4K);
    if (at_eof < 0) {
        die("pread after shrink");
    }
    if (at_eof != 0) {
        fprintf(stderr, "expected EOF after shrink, got %zd bytes\n", at_eof);
        return 1;
    }

    /* Re-extend: truncated tail should reappear as zeroes, not stale data. */
    off_t new_len = FILE_LEN + PAGE_SIZE_4K;
    if (ftruncate(fd, new_len) != 0) {
        die("ftruncate (re-extend)");
    }
    expect_zero(fd, shrink_len, PAGE_SIZE_4K, "re-extended area");

    /* Write a new pattern into the re-extended tail and verify. */
    memset(buf, 0x5a, sizeof(buf));
    if (pwrite(fd, buf, sizeof(buf), shrink_len) != (ssize_t)sizeof(buf)) {
        die("pwrite tail");
    }
    memset(buf, 0, sizeof(buf));
    if (pread(fd, buf, sizeof(buf), shrink_len) != (ssize_t)sizeof(buf)) {
        die("pread tail verify");
    }
    for (size_t i = 0; i < sizeof(buf); ++i) {
        if (buf[i] != 0x5a) {
            fprintf(stderr, "tail verify failed at %zu: %#x\n", i, buf[i]);
            return 1;
        }
    }

    close(fd);
    unlink(path);
    printf("file truncate/extend cache test passed (%s)\n", path);
    return 0;
}
