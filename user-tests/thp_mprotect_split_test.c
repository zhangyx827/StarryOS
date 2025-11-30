#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

static const char *THP_ENABLED =
    "/sys/kernel/mm/transparent_hugepage/enabled";
static const char *KHUGEPAGED_PAGES_COLLAPSED =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed";

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void set_thp_mode(const char *mode) {
    FILE *f = fopen(THP_ENABLED, "r+");
    if (!f) die("fopen THP_ENABLED");

    if (fseek(f, 0, SEEK_SET) != 0) die("fseek write");
    if (fprintf(f, "%s\n", mode) < 0) die("fprintf");
    if (fflush(f) != 0) die("fflush");
    fclose(f);
}

static unsigned long read_pages_collapsed(void) {
    FILE *f = fopen(KHUGEPAGED_PAGES_COLLAPSED, "r");
    if (!f) die("fopen pages_collapsed");

    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), f)) {
        die("fgets pages_collapsed");
    }
    fclose(f);
    return strtoul(buf, NULL, 10);
}

static void touch_range(char *p, size_t len) {
    for (size_t i = 0; i < len; i += 4096) {
        p[i] = (char)(i / 4096);
    }
}

int main(void) {
    const size_t thp_size = 2UL * 1024 * 1024;  // 2 MiB

    printf("[thp_mprotect_split_test] set THP mode to 'always'\n");
    set_thp_mode("always");

    // 分配一个 2M 对齐的 4M 区域（确保至少有一个完整的 2M 对齐窗口）
    const size_t len = 4UL * 1024 * 1024;  // 4 MiB
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap");
    }
    printf("[thp_mprotect_split_test] mapping at %p len=%zu\n", p, len);

    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE)");
    }

    // 找到 2M 对齐的窗口
    uintptr_t base = (uintptr_t)p;
    uintptr_t end  = base + len;
    uintptr_t win  = (base + thp_size - 1) & ~(thp_size - 1);
    if (win + thp_size > end) {
        printf("[thp_mprotect_split_test] cannot find 2M-aligned window, exiting\n");
        munmap(p, len);
        return 1;
    }

    char *thp_region = (char *)win;
    printf("[thp_mprotect_split_test] 2M-aligned region at %p\n", thp_region);

    // 只 touch 这个 2M 窗口
    touch_range(thp_region, thp_size);

    // 等待 pages_collapsed 增加，确保 collapse 已发生
    unsigned long prev = read_pages_collapsed();
    printf("[thp_mprotect_split_test] pages_collapsed(before) = %lu\n", prev);

    for (int iter = 0; iter < 30; iter++) {
        sleep(1);
        // keep region hot
        ((volatile char *)thp_region)[0] ^= 1;

        unsigned long now = read_pages_collapsed();
        printf("  [iter %d] pages_collapsed = %lu\n", iter, now);
        if (now > prev) {
            printf("[thp_mprotect_split_test] detected THP collapse (pages_collapsed increased)\n");
            break;
        }
    }

    // 现在进行 mprotect 测试（在 collapse 后）
    char *before = thp_region;
    char *after  = thp_region + thp_size - 4096;

    // 在中间 64K 上 mprotect，使其只读
    size_t hole_len = 64UL * 1024;
    char *hole = thp_region + 512UL * 1024;
    if ((uintptr_t)hole % 4096 != 0 ||
        (uintptr_t)hole + hole_len > (uintptr_t)thp_region + thp_size) {
        printf("[thp_mprotect_split_test] hole misaligned / out-of-range, aborting\n");
        goto out;
    }

    printf("[thp_mprotect_split_test] mprotect hole %p len=%zu to PROT_READ\n",
           hole, hole_len);
    if (mprotect(hole, hole_len, PROT_READ) != 0) {
        perror("mprotect(hole, PROT_READ)");
        goto out;
    }

    // 写 before/after：如果实现错误地把整个 2M 设成只读，下面会崩溃
    printf("[thp_mprotect_split_test] writing before/after hole...\n");
    *before ^= 1;
    *after  ^= 1;

    printf("[thp_mprotect_split_test] PASS: partial mprotect did not break surrounding pages\n");

out:
    if (munmap(p, len) != 0) {
        die("munmap");
    }

    return 0;
}

