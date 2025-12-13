// Validate that MADV_COLLAPSE on a MAP_PRIVATE file mapping can fault-in
// missing pages and still preserves file content (COW writes do not corrupt
// the backing file).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#define PATH_PRIMARY  "/tmp/cow_collapse_faultin_test.bin"
#define PATH_FALLBACK "./cow_collapse_faultin_test.bin"
#define REGION_SIZE   (4UL * 1024 * 1024)  // 2 MiB
#define PAGE_4K       4096UL

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static int open_test_file(const char **path_out) {
    int fd = open(PATH_PRIMARY, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
        *path_out = PATH_PRIMARY;
        return fd;
    }
    perror("open /tmp failed, fallback to cwd");
    fd = open(PATH_FALLBACK, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
        *path_out = PATH_FALLBACK;
    }
    return fd;
}

static void fill_file_pattern(int fd, size_t len) {
    uint8_t buf[PAGE_4K];
    for (size_t off = 0; off < len; off += sizeof(buf)) {
        uint8_t pat = (uint8_t)((off / PAGE_4K) & 0xff);
        memset(buf, pat, sizeof(buf));
        if (pwrite(fd, buf, sizeof(buf), (off_t)off) != (ssize_t)sizeof(buf)) {
            die("pwrite pattern");
        }
    }
}

static int verify_file_pattern(const char *path, size_t len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        die("reopen for verify");
    }
    uint8_t buf[PAGE_4K];
    int rc = 0;
    for (size_t off = 0; off < len; off += sizeof(buf)) {
        ssize_t n = pread(fd, buf, sizeof(buf), (off_t)off);
        if (n != (ssize_t)sizeof(buf)) {
            die("pread verify");
        }
        uint8_t expected = (uint8_t)((off / PAGE_4K) & 0xff);
        if (buf[0] != expected || buf[PAGE_4K - 1] != expected) {
            fprintf(stderr,
                    "verify mismatch at file offset %zu: got %#x/%#x expected %#x\n",
                    off, buf[0], buf[PAGE_4K - 1], expected);
            rc = 1;
            break;
        }
    }
    close(fd);
    return rc;
}

int main(void) {
    const char *path = NULL;
    int fd = open_test_file(&path);
    if (fd < 0) {
        die("open test file");
    }

    if (ftruncate(fd, REGION_SIZE) != 0) {
        die("ftruncate");
    }
    fill_file_pattern(fd, REGION_SIZE);

    // MAP_PRIVATE to exercise COW backend.
    char *p = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        die("mmap");
    }
    printf("cow_collapse_faultin_test: mapped %zu bytes at %p from %s\n",
           (size_t)REGION_SIZE, p, path);

    // Touch only the first quarter to leave the rest as PTE_NONE.
    size_t quarter = REGION_SIZE / 4;
    for (size_t off = 0; off < quarter; off += PAGE_4K) {
        volatile char sink = p[off];
        (void)sink;
    }

    // MADV_COLLAPSE should fault-in the untouched pages and succeed.
    if (madvise(p, REGION_SIZE, MADV_COLLAPSE) != 0) {
        perror("madvise(MADV_COLLAPSE, private file)");
        munmap(p, REGION_SIZE);
        close(fd);
        unlink(path);
        return 1;
    }

    // Verify full mapping content matches the original pattern.
    for (size_t off = 0; off < REGION_SIZE; off += PAGE_4K) {
        uint8_t expected = (uint8_t)((off / PAGE_4K) & 0xff);
        if ((uint8_t)p[off] != expected) {
            fprintf(stderr,
                    "mapping mismatch at offset %zu: got %#x expected %#x\n",
                    off, (unsigned char)p[off], expected);
            munmap(p, REGION_SIZE);
            close(fd);
            unlink(path);
            return 1;
        }
    }

    if (munmap(p, REGION_SIZE) != 0) {
        die("munmap");
    }
    close(fd);

    // File content must remain the original pattern (COW writes shouldn't persist).
    int rc = verify_file_pattern(path, REGION_SIZE);
    unlink(path);

    if (rc == 0) {
        printf("cow_collapse_faultin_test: PASS\n");
    } else {
        printf("cow_collapse_faultin_test: FAIL\n");
    }
    return rc;
}
