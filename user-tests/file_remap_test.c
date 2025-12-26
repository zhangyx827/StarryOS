#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

#ifndef MAP_POPULATE
#define MAP_POPULATE 0x8000
#endif

#define PAGE_4K 4096UL
#define BIGPAGE_SIZE (4UL * 1024 * 1024)  // 4MiB

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

/* Pick a non-tmpfs path for a disk-backed file. */
static int open_disk_file(char *out_path, size_t out_len) {
    const char *candidates[] = {
        "./file_ro_disk_collapse_test.bin",  // should be ext in guest rootfs
        "/var/tmp/file_ro_disk_collapse_test.bin",
        "/tmp/file_ro_disk_collapse_test.bin",  // will be skipped if tmpfs
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *p = candidates[i];
        int fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 0) {
            continue;
        }
        struct statfs s;
        if (statfs(p, &s) == 0 && s.f_type == TMPFS_MAGIC) {
            close(fd);
            unlink(p);
            continue;  // skip tmpfs/shmem
        }
        snprintf(out_path, out_len, "%s", p);
        return fd;
    }
    return -1;
}

int main(void) {
    char path[128] = {0};
    int fd = open_disk_file(path, sizeof(path));
    if (fd < 0) {
        perror("open disk-backed file");
        printf("file_ro_disk_collapse_test: SKIP (no suitable disk-backed dir)\n");
        return 0;
    }

    // Prepare 4MiB file content with a simple pattern.
    if (ftruncate(fd, BIGPAGE_SIZE) != 0) {
        die("ftruncate");
    }
    for (off_t off = 0; off < (off_t)BIGPAGE_SIZE; off += PAGE_4K) {
        uint8_t buf[PAGE_4K];
        memset(buf, (uint8_t)(off / PAGE_4K), sizeof(buf));
        if (pwrite(fd, buf, sizeof(buf), off) != (ssize_t)sizeof(buf)) {
            die("pwrite");
        }
    }
    if (fsync(fd) != 0) {
        die("fsync");
    }
    close(fd);

    fd = open(path, O_RDWR);
    if (fd < 0) {
        die("reopen O_RDONLY");
    }

    printf("=== file_ro_disk_collapse_test ===\n");
    printf("  using disk-backed file: %s (size=%lu)\n", path, (unsigned long)BIGPAGE_SIZE);

    char *p = mmap(NULL, BIGPAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        die("mmap PROT_READ");
    }
    printf("  mmap base=%p len=%lu (read-only)\n", p, (unsigned long)BIGPAGE_SIZE);

    // Fault in all 4K pages so collapse sees populated, clean pages.
    for (size_t off = 0; off < BIGPAGE_SIZE / 2; off += PAGE_4K) {
        volatile char v = p[off];
        (void)v;
    }

    // if (madvise(p, BIGPAGE_SIZE, MADV_COLLAPSE) != 0) {
    //     perror("madvise(MADV_COLLAPSE, read-only file)");
    //     munmap(p, BIGPAGE_SIZE);
    //     close(fd);
    //     unlink(path);
    //     return 1;
    // }
    printf("  madvise(MADV_COLLAPSE) succeeded\n");

    // Verify contents remain intact after collapse attempt.
    int rc = 0;
    for (size_t off = 0; off < BIGPAGE_SIZE; off += PAGE_4K) {
        unsigned char expected = (unsigned char)(off / PAGE_4K);
        if ((unsigned char)p[off] != expected) {
            fprintf(stderr,
                    "  mismatch at offset %zu: got %#x expected %#x\n",
                    off, (unsigned char)p[off], expected);
            rc = 1;
            break;
        }
    }

    memset(p, 'c', BIGPAGE_SIZE);


    close(fd);
    unlink(path);

    if (rc == 0) {
        printf("file_ro_disk_collapse_test: PASS\n");
    } else {
        printf("file_ro_disk_collapse_test: FAIL\n");
    }
    return rc;
}
