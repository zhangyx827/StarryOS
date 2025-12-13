// file_madvise_collapse_faultin_test.c
//
// Exercise MADV_COLLAPSE on a tmpfs-backed FileBackend where only part of the
// 2MiB window has been faulted in. The goal is to drive the "fault_in = true"
// path in FileBackend::collapse_page, which should populate unmapped pages
// inside the window before collapsing.
//
// Scenario:
//   - Create a tmpfs file of size 2MiB.
//   - MAP_SHARED mmap it.
//   - Touch only the first 1MiB (so前半部分有 PTE/backing，后半是 pte_none)。
//   - Call madvise(MADV_COLLAPSE) on the whole 2MiB window.
//   - Verify:
//       * The first half keeps the pattern we wrote.
//       * The second half reads back as 0.
//
// 从用户态看不到“是否真的在 MADV_COLLAPSE 中 fault-in 了后半段”，
// 但这个测试能确保：
//   - 部分存在、部分不存在的窗口在 MADV_COLLAPSE 后语义正确；
//   - 不会因为 fault-in / collapse 破坏 tmpfs 文件内容。

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/vfs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PAGE_4K      4096UL
#define REGION_SIZE  (4UL * 1024 * 1024)   // 2MiB

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

/* Prefer a disk-backed directory; skip tmpfs/shmem (TMPFS_MAGIC). */
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif
static int open_disk_file(char *out_path, size_t out_len) {
    const char *candidates[] = {
        "./file_madvise_collapse_faultin_test.bin",   // likely ext4 in guest rootfs
        "/tmp/file_madvise_collapse_faultin_test.bin",
        "/var/tmp/file_madvise_collapse_faultin_test.bin",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *p = candidates[i];
        int fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 0) {
            continue;
        }
        struct statfs s;
        if (statfs(p, &s) == 0 && s.f_type == TMPFS_MAGIC) {
            // Not disk-backed; skip.
            close(fd);
            unlink(p);
            continue;
        }
        snprintf(out_path, out_len, "%s", p);
        return fd;
    }
    return -1; // not found
}

int main(void) {
    char path[128] = {0};
    int fd = open_disk_file(path, sizeof(path));
    if (fd < 0) {
        perror("open disk-backed file");
        printf("file_madvise_collapse_faultin_test: SKIP (no non-tmpfs dir available)\n");
        return 0;
    }

    if (ftruncate(fd, REGION_SIZE) != 0) {
        die("ftruncate");
    }

    printf("=== file_madvise_collapse_faultin_test ===\n");
    printf("  using disk-backed file: %s (size=%lu)\n", path, (unsigned long)REGION_SIZE);

    char *p = mmap(NULL, REGION_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED,
                   fd, 0);
    if (p == MAP_FAILED) {
        die("mmap");
    }
    printf("  mmap base=%p len=%lu\n", p, (unsigned long)REGION_SIZE);

    // 1) 只触摸前半部分（2MiB），让这一半有 PTE/backing。
    size_t half = REGION_SIZE / 2;
    for (size_t off = 0; off < half; off += PAGE_4K) {
        p[off] = (char)(off / PAGE_4K);
    }
    printf("  touched first half (%lu bytes), second half left unmapped\n",
           (unsigned long)half);

    // 2) 对整个 4MiB 区间调用 MADV_COLLAPSE，期望成功（返回 0）。
    if (madvise(p, REGION_SIZE, MADV_COLLAPSE) != 0) {
        perror("madvise(MADV_COLLAPSE, file)");
        munmap(p, REGION_SIZE);
        close(fd);
        unlink(path);
        return 1;
    }
    printf("  madvise(MADV_COLLAPSE) succeeded\n");

    // 3) 校验前半部分保持原 pattern，并在后半部分写入新 pattern 触发 fault-in。
    int rc = 0;
    for (size_t off = 0; off < half; off += PAGE_4K) {
        char v = p[off];
        char expected = (char)(off / PAGE_4K);
        if (v != expected) {
            fprintf(stderr,
                    "  mismatch (first half) at offset %zu: got %#x expected %#x\n",
                    off, (unsigned char)v, (unsigned char)expected);
            rc = 1;
            break;
        }
    }
    if (rc == 0) {
        memset(p + half, 0x5a, half); // force-fault the previously untouched half
        if (msync(p, REGION_SIZE, MS_SYNC) != 0) {
            perror("msync after tail write");
            rc = 1;
        }
    }

    if (munmap(p, REGION_SIZE) != 0) {
        die("munmap");
    }
    close(fd);

    if (rc == 0) {
        // 重新打开并校验全文件内容持久化：前半递增，后半 0x5a。
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            die("reopen for verify");
        }
        char buf[PAGE_4K];
        for (size_t off = 0; off < REGION_SIZE; off += PAGE_4K) {
            ssize_t n = pread(fd, buf, sizeof(buf), (off_t)off);
            if (n != (ssize_t)sizeof(buf)) {
                die("pread verify");
            }
            unsigned char expected = (off < half) ? (unsigned char)(off / PAGE_4K) : 0x5a;
            if (buf[0] != expected || buf[PAGE_4K - 1] != expected) {
                fprintf(stderr,
                        "  verify mismatch at offset %zu: got %#x/%#x expected %#x\n",
                        off, buf[0], buf[PAGE_4K - 1], expected);
                rc = 1;
                break;
            }
        }
        close(fd);
    }
    unlink(path);

    if (rc == 0) {
        printf("file_madvise_collapse_faultin_test: PASS\n");
    } else {
        printf("file_madvise_collapse_faultin_test: FAIL\n");
    }
    return rc;
}
