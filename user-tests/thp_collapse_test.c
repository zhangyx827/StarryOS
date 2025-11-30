#define _GNU_SOURCE
#include <errno.h>
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

int main(void) {
    // 1. 开启 THP，使用 "always" 模式，方便匿名映射被考虑折叠
    printf("Setting THP mode to 'always'...\n");
    set_thp_mode("always");

    // 2. 分配一块较大的匿名内存，至少几 MB
    size_t len = 64UL * 1024 * 1024; // 64 MiB
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap");
    }
    printf("Anonymous mapping at %p, length %zu bytes\n", p, len);

    // 3. 对这块内存 madvise(MADV_HUGEPAGE)，给 VMA 标 THP hint
    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE)");
    }
    printf("madvise(MADV_HUGEPAGE) done\n");

    // 4. 逐页访问，制造 4K 页表项（触发缺页）
    printf("Touching pages to populate 4K PTEs...\n");
    for (size_t i = 0; i < len; i += 4096) {
        ((volatile char *)p)[i] = (char)(i / 4096);
    }
    printf("Initial touch done\n");

    // 5. 等待 khugepaged 工作，并观察 pages_collapsed 变化
    printf("Waiting for khugepaged to collapse pages...\n");
    unsigned long prev = read_pages_collapsed();
    printf("Initial pages_collapsed = %lu\n", prev);

    for (int iter = 0; iter < 60; iter++) { // 最多等 ~60 秒
        sleep(1);

        // 轻微访问，保持内存“热”
        for (size_t i = 0; i < len; i += 2 * 1024 * 1024) {
            ((volatile char *)p)[i] ^= 1;
        }

        unsigned long now = read_pages_collapsed();
        printf("[iter %d] pages_collapsed = %lu\n", iter, now);
        if (now > prev) {
            printf("Detected collapse: pages_collapsed increased from %lu to %lu\n",
                    prev, now);
            break;
        }
    }

    if (munmap(p, len) != 0) {
        die("munmap");
    }
    printf("thp_collapse_test: DONE\n");
    return 0;
}