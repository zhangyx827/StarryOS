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

static void do_one_iteration(size_t len, size_t it) {
    const size_t thp_size = 2UL * 1024 * 1024;   // 2 MiB

    printf("[iter %zu] mmap len=%zu\n", it, len);
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap");
    }

    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE)");
    }

    // Populate 4K PTEs.
    touch_range(p, len);

    // Try to wait a bit for khugepaged to possibly collapse something.
    unsigned long prev = read_pages_collapsed();
    for (int sec = 0; sec < 10; sec++) {
        sleep(1);
        unsigned long now = read_pages_collapsed();
        if (now > prev) {
            printf("  pages_collapsed: %lu -> %lu\n", prev, now);
            prev = now;
            break;
        }
    }

    // For each 2M window inside [p, p+len), do a partial munmap in the middle,
    // then touch before/after to ensure the kernel correctly split the THP.
    uintptr_t base = (uintptr_t)p;
    uintptr_t end  = base + len;
    for (uintptr_t win = base; win + thp_size <= end; win += thp_size) {
        // Align window start up to 2M for THP expectations.
        uintptr_t thp_start = (win + thp_size - 1) & ~(thp_size - 1);
        if (thp_start + thp_size > end) {
            continue;
        }

        char *thp_region = (char *)thp_start;
        char *before = thp_region;
        char *after  = thp_region + thp_size - 4096;

        // Hole: 64K in the middle of the THP window.
        size_t hole_len = 64UL * 1024;
        char *hole = thp_region + 512UL * 1024;
        if ((uintptr_t)hole % 4096 != 0 ||
            (uintptr_t)hole + hole_len > end) {
            continue;
        }

        printf("  [iter %zu] partial munmap at %p len=%zu in window %p\n",
               it, hole, hole_len, thp_region);

        if (munmap(hole, hole_len) != 0) {
            // EINVAL 等错误直接跳过这个 window。
            perror("    munmap(hole)");
            continue;
        }

        // Touch before hole: 应该仍然可访问。
        *before ^= 1;

        // Touch after hole: 如果 THP 没有正确 split，而是整个 2M
        // 被错误 unmap，这里应该会导致崩溃/异常。
        *after ^= 1;
    }

    if (munmap(p, len) != 0) {
        die("munmap(total)");
    }
}

int main(void) {
    const size_t len = 8UL * 1024 * 1024; // 8 MiB，总共 4 个 THP 窗口

    printf("[thp_split_stress_test] set THP mode to 'always'\n");
    set_thp_mode("always");

    // 多次重复，模拟压力场景。
    for (size_t i = 0; i < 100; i++) {
        do_one_iteration(len, i);
    }

    printf("[thp_split_stress_test] DONE\n");
    return 0;
}

