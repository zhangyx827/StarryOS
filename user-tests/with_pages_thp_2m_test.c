#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PAGE_4K     4096UL
#define THP_SIZE    (4UL * 1024 * 1024)    /* 和 thp_tmpfs_file_collapse_test 一样，用 4MiB */
#define FILE_SIZE   THP_SIZE

/* 覆盖整个文件，这样无论 collapse 的 2MiB 窗口落在哪，都在校验范围内。 */
#define TEST_OFF    0
#define TEST_LEN    FILE_SIZE

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

/* 尝试在 /dev/shm 或 /tmp 下创建一个 tmpfs 文件 */
static int open_tmpfs_file(char *out_path, size_t out_len) {
    const char *candidates[] = {
        "/dev/shm/with_pages_thp_tmpfs_2m_test.bin",
        "/tmp/with_pages_thp_tmpfs_2m_test.bin",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *p = candidates[i];
        int fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd >= 0) {
            snprintf(out_path, out_len, "%s", p);
            return fd;
        }
    }
    return -1;
}

static void write_full(int fd, const void *buf, size_t len, off_t off) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = pwrite(fd, p, len, off);
        if (n < 0) die("pwrite");
        if (n == 0) die("short write");
        off += n;
        p   += n;
        len -= n;
    }
}

static void read_full(int fd, void *buf, size_t len, off_t off) {
    char *p = buf;
    while (len > 0) {
        ssize_t n = pread(fd, p, len, off);
        if (n < 0) die("pread");
        if (n == 0) die("short read");
        off += n;
        p   += n;
        len -= n;
    }
}

int main(void) {
    char path[128] = {0};
    int fd = open_tmpfs_file(path, sizeof(path));
    if (fd < 0) {
        perror("open tmpfs file");
        printf("with_pages_thp_tmpfs_2m_test: SKIP (no /dev/shm or /tmp)\n");
        return 0;
    }

    if (ftruncate(fd, FILE_SIZE) != 0) {
        die("ftruncate");
    }

    printf("=== with_pages_thp_tmpfs_2m_test ===\n");
    printf("  using tmpfs file: %s (size=%lu)\n", path, (unsigned long)FILE_SIZE);

    uint8_t *buf  = malloc(TEST_LEN);
    uint8_t *buf2 = malloc(TEST_LEN);
    if (!buf || !buf2) {
        die("malloc");
    }

    /* ---------- 阶段 1：THP 之前，用 read/write 跑一遍 with_pages ---------- */

    for (size_t i = 0; i < TEST_LEN; i++) {
        /* 简单 pattern：按“第几个 4K 页”编码 */
        buf[i] = (uint8_t)((i / PAGE_4K) & 0xff);
    }

    printf("  [pre-collapse] write pattern via pwrite (offset=%d, len=%lu)\n",
            TEST_OFF, (unsigned long)TEST_LEN);
    write_full(fd, buf, TEST_LEN, TEST_OFF);

    memset(buf2, 0, TEST_LEN);
    read_full(fd, buf2, TEST_LEN, TEST_OFF);

    if (memcmp(buf, buf2, TEST_LEN) != 0) {
        fprintf(stderr, "  [pre-collapse] data mismatch before THP collapse\n");
        return 1;
    }
    printf("  [pre-collapse] with_pages path works (CachedFile read/write OK)\n");

    /* ---------- 阶段 2：mmap + MADV_HUGEPAGE + MADV_COLLAPSE，触发 2MiB collapse ---------- */

    void *map = mmap(NULL, FILE_SIZE,
                      PROT_READ,          /* 只读就足够触发 fault，不改内容 */
                      MAP_SHARED,
                      fd, 0);
    if (map == MAP_FAILED) {
        die("mmap");
    }

    printf("  [collapse] mmap at %p, len=%lu\n", map, (unsigned long)FILE_SIZE);

    /* 给 VMA 标记 MADV_HUGEPAGE（如果内核需显式 hint） */
    if (madvise(map, FILE_SIZE, MADV_HUGEPAGE) != 0) {
        perror("madvise(MADV_HUGEPAGE) (non-fatal)");
    }

    /* 读每个 4K 的一个字节，保证 4K PTE 全部 fault 进来 */
    for (size_t i = 0; i < FILE_SIZE; i += PAGE_4K) {
        volatile uint8_t v = ((uint8_t *)map)[i];
        (void)v;
    }
    printf("  [collapse] all 4K pages faulted in via read\n");

    if (madvise(map, FILE_SIZE, MADV_COLLAPSE) != 0) {
        perror("madvise(MADV_COLLAPSE)");
        fprintf(stderr, "  [collapse] MADV_COLLAPSE failed\n");
        return 1;
    }
    printf("  [collapse] madvise(MADV_COLLAPSE) succeeded\n");

    if (munmap(map, FILE_SIZE) != 0) {
        die("munmap");
    }

    /* ---------- 阶段 3：THP 之后，再通过 read/pread 跑一遍 with_pages ---------- */

    memset(buf2, 0, TEST_LEN);
    read_full(fd, buf2, TEST_LEN, TEST_OFF);

    if (memcmp(buf, buf2, TEST_LEN) != 0) {
        fprintf(stderr, "  [post-collapse] data mismatch after THP collapse\n");
        return 1;
    }
    printf("  [post-collapse] with_pages still returns consistent content\n");

    free(buf);
    free(buf2);
    close(fd);
    unlink(path);

    printf("with_pages_thp_tmpfs_2m_test: PASS\n");
    return 0;
}
