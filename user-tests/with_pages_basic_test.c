#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PAGE_4K     4096UL
#define FILE_SIZE   (PAGE_4K * 8)              /* 32 KiB */
#define TEST_LEN    (PAGE_4K * 3 + 200)        /* 覆盖 3 页多一点 */
#define TEST_OFF1   0                          /* 对齐到页首 */
#define TEST_OFF2   (PAGE_4K / 2)              /* 非对齐，从页中间开始 */

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void write_full(int fd, const void *buf, size_t len, off_t off) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = (off >= 0)
            ? pwrite(fd, p, len, off)
            : write(fd, p, len);
        if (n < 0) die("write/pwrite");
        if (n == 0) die("short write");
        if (off >= 0) off += n;
        p   += n;
        len -= n;
    }
}

static void read_full(int fd, void *buf, size_t len, off_t off) {
    char *p = buf;
    while (len > 0) {
        ssize_t n = (off >= 0)
            ? pread(fd, p, len, off)
            : read(fd, p, len);
        if (n < 0) die("read/pread");
        if (n == 0) die("short read");
        if (off >= 0) off += n;
        p   += n;
        len -= n;
    }
}

int main(void) {
    const char *path = "/tmp/with_pages_basic_test.dat";
    int fd;

    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        /* 有些环境可能没有 /tmp，就退回当前目录 */
        path = "with_pages_basic_test.dat";
        fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) {
            die("open");
        }
    }

    if (ftruncate(fd, FILE_SIZE) != 0) {
        die("ftruncate");
    }

    if (TEST_OFF2 + TEST_LEN > FILE_SIZE) {
        fprintf(stderr, "TEST_OFF2 + TEST_LEN 超过 FILE_SIZE，调整宏定义\n");
        return 1;
    }

    printf("=== with_pages_basic_test ===\n");
    printf("  file: %s (size=%lu)\n", path, (unsigned long)FILE_SIZE);

    uint8_t *buf  = malloc(TEST_LEN);
    uint8_t *buf2 = malloc(TEST_LEN);
    if (!buf || !buf2) {
        die("malloc");
    }

    /* ------- case 1: 从 0 开始，跨多页连续写/读 ------- */
    for (size_t i = 0; i < TEST_LEN; i++) {
        buf[i] = (uint8_t)(i & 0xff);
    }

    printf("  [case1] write from offset=%d, len=%lu\n",
            TEST_OFF1, (unsigned long)TEST_LEN);
    write_full(fd, buf, TEST_LEN, TEST_OFF1);

    memset(buf2, 0, TEST_LEN);
    read_full(fd, buf2, TEST_LEN, TEST_OFF1);

    if (memcmp(buf, buf2, TEST_LEN) != 0) {
        fprintf(stderr, "case1: data mismatch\n");
        return 1;
    } else {
        printf("  [case1] OK (跨多页对齐访问)\n");
    }

    /* ------- case 2: 从页中间开始，跨多页非对齐写/读 ------- */
    for (size_t i = 0; i < TEST_LEN; i++) {
        buf[i] = (uint8_t)(0x80 ^ (i & 0x7f));  /* 换一套 pattern */
    }

    printf("  [case2] write from offset=%lu (unaligned), len=%lu\n",
            (unsigned long)TEST_OFF2, (unsigned long)TEST_LEN);
    write_full(fd, buf, TEST_LEN, TEST_OFF2);

    memset(buf2, 0, TEST_LEN);
    read_full(fd, buf2, TEST_LEN, TEST_OFF2);

    if (memcmp(buf, buf2, TEST_LEN) != 0) {
        fprintf(stderr, "case2: data mismatch\n");
        return 1;
    } else {
        printf("  [case2] OK (跨多页非对齐访问)\n");
    }

    free(buf);
    free(buf2);
    close(fd);

    printf("with_pages_basic_test: PASS\n");
    return 0;
}