// file_set_len_test.c
//
// Exercise CachedFile::set_len() semantics via ftruncate():
//  - old_len < new_len (extension): bytes [old_len, new_len) should read as 0,
//    while [0, old_len) keep their original pattern.
//  - new_len < old_len (truncation): bytes [0, new_len) keep their original
//    contents; reads from offset >= new_len return 0 bytes.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PAGE_4K       4096UL
#define INITIAL_LEN   (PAGE_4K * 3 + 1000)   // 3 full pages + 1000 bytes
#define EXT_LEN       (INITIAL_LEN + 3000)   // extend within the same 4K page
#define TRUNC_LEN     (PAGE_4K * 2 + 500)    // truncate to somewhere in page 2

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void write_full(int fd, const void *buf, size_t len, off_t off) {
    const uint8_t *p = (const uint8_t *)buf;
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
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = pread(fd, p, len, off);
        if (n < 0) die("pread");
        if (n == 0) die("short read");
        off += n;
        p   += n;
        len -= n;
    }
}

static uint8_t pattern_at(size_t i) {
    return (uint8_t)(i & 0xff);
}

int main(void) {
    const char *path = "/tmp/file_set_len_test.dat";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        // Fallback to current directory if /tmp is not available.
        path = "file_set_len_test.dat";
        fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) {
            die("open");
        }
    }

    printf("=== file_set_len_test ===\n");
    printf("  file: %s\n", path);

    uint8_t *buf = malloc(EXT_LEN);
    uint8_t *chk = malloc(EXT_LEN);
    if (!buf || !chk) {
        die("malloc");
    }

    // 1) 初始写入 INITIAL_LEN 字节 pattern。
    for (size_t i = 0; i < INITIAL_LEN; i++) {
        buf[i] = pattern_at(i);
    }

    printf("  [step1] write INITIAL_LEN=%lu\n", (unsigned long)INITIAL_LEN);
    write_full(fd, buf, INITIAL_LEN, 0);

    // 2) ftruncate 扩展到 EXT_LEN，验证 [old_len, new_len) 为 0。
    printf("  [step2] ftruncate to EXT_LEN=%lu\n", (unsigned long)EXT_LEN);
    if (ftruncate(fd, EXT_LEN) != 0) {
        die("ftruncate(extend)");
    }

    memset(chk, 0, EXT_LEN);
    read_full(fd, chk, EXT_LEN, 0);

    for (size_t i = 0; i < EXT_LEN; i++) {
        uint8_t expected;
        if (i < INITIAL_LEN) {
            expected = pattern_at(i);
        } else {
            expected = 0;  // 新增区间应读出 0
        }
        if (chk[i] != expected) {
            fprintf(stderr,
                    "  [step2] mismatch at offset %zu: got %#x expected %#x\n",
                    i, chk[i], expected);
            return 1;
        }
    }
    printf("  [step2] extend semantics OK (old data preserved, new bytes are 0)\n");

    // 3) 再次 ftruncate 收缩到 TRUNC_LEN，验证 [0, TRUNC_LEN) 保持原值。
    printf("  [step3] ftruncate to TRUNC_LEN=%lu\n", (unsigned long)TRUNC_LEN);
    if (ftruncate(fd, TRUNC_LEN) != 0) {
        die("ftruncate(truncate)");
    }

    memset(chk, 0, TRUNC_LEN);
    read_full(fd, chk, TRUNC_LEN, 0);

    for (size_t i = 0; i < TRUNC_LEN; i++) {
        uint8_t expected = pattern_at(i);
        if (chk[i] != expected) {
            fprintf(stderr,
                    "  [step3] mismatch at offset %zu after truncate: got %#x expected %#x\n",
                    i, chk[i], expected);
            return 1;
        }
    }
    printf("  [step3] truncate semantics OK (prefix data preserved)\n");

    // 4) 额外检查：从 TRUNC_LEN 开始的 pread 应该返回 0 字节。
    uint8_t extra[16];
    ssize_t n = pread(fd, extra, sizeof(extra), TRUNC_LEN);
    if (n < 0) {
        die("pread(after truncate)");
    }
    if (n != 0) {
        fprintf(stderr,
                "  [step4] expected pread at offset TRUNC_LEN to return 0, got %zd\n",
                n);
        return 1;
    }
    printf("  [step4] pread at EOF returns 0 bytes as expected\n");

    free(buf);
    free(chk);
    close(fd);
    unlink(path);

    printf("file_set_len_test: PASS\n");
    return 0;
}

