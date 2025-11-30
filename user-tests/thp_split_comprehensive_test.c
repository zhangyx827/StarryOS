#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#define THP_SIZE (2UL * 1024 * 1024)
#define PAGE_4K (4096)

static const char *THP_ENABLED = "/sys/kernel/mm/transparent_hugepage/enabled";
static const char *KHUGEPAGED_PAGES_COLLAPSED =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed";

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void set_thp_mode(const char *mode) {
    FILE *f = fopen(THP_ENABLED, "r+");
    if (!f) die("fopen THP_ENABLED");
    if (fseek(f, 0, SEEK_SET) != 0) die("fseek");
    if (fprintf(f, "%s\n", mode) < 0) die("fprintf");
    if (fflush(f) != 0) die("fflush");
    fclose(f);
}

static unsigned long read_pages_collapsed(void) {
    FILE *f = fopen(KHUGEPAGED_PAGES_COLLAPSED, "r");
    if (!f) die("fopen pages_collapsed");
    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), f)) die("fgets");
    fclose(f);
    return strtoul(buf, NULL, 10);
}

static void wait_for_collapse(char *thp_base) {
    unsigned long prev = read_pages_collapsed();
    printf("  Waiting for collapse (pages_collapsed=%lu)...\n", prev);
    for (int iter = 0; iter < 30; iter++) {
        sleep(1);
        ((volatile char *)thp_base)[0] ^= 1;
        unsigned long now = read_pages_collapsed();
        if (now > prev) {
            printf("  Collapse detected (pages_collapsed=%lu)\n", now);
            return;
        }
    }
    printf("  WARNING: No collapse after 30s\n");
}

int main(void) {
    printf("=== THP Split Bug Detection Test (Single-threaded) ===\n\n");
    set_thp_mode("always");

    int test_failed = 0;

    //
    // TEST 1: munmap 非 2M 对齐地址（检测 split_thp 参数错误）
    //
    printf("[TEST 1] munmap from non-2M-aligned address\n");
    printf("  This tests if split_thp receives correct (aligned) base address\n");

    size_t len = 4 * 1024 * 1024;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap");
    if (madvise(p, len, MADV_HUGEPAGE) != 0) die("madvise");

    uintptr_t base = (uintptr_t)p;
    uintptr_t win = (base + THP_SIZE - 1) & ~(THP_SIZE - 1);
    char *thp_base = (char *)win;

    printf("  Mapping base: %p\n", p);
    printf("  THP base (2M-aligned): %p\n", thp_base);

    // Touch 整个 2M
    for (size_t i = 0; i < THP_SIZE; i += PAGE_4K) {
        thp_base[i] = (char)(i / PAGE_4K);
    }
    wait_for_collapse(thp_base);

    // 从非对齐地址开始 munmap
    char *unaligned = thp_base + PAGE_4K * 10;  // 偏移 10 页
    size_t unmap_len = PAGE_4K * 5;  // unmap 5 页

    printf("  Unmapping: addr=%p (offset=0x%lx from THP base), len=%zu\n",
           unaligned, (uintptr_t)unaligned - (uintptr_t)thp_base, unmap_len);
    printf("  BUG CHECK: cow.rs should call split_thp(%p), NOT split_thp(%p)\n",
           thp_base, unaligned);

    if (munmap(unaligned, unmap_len) != 0) {
        perror("munmap");
        test_failed = 1;
    } else {
        printf("  munmap returned successfully\n");
    }

    // 验证周围页是否仍可访问
    printf("  Verifying pages around the hole...\n");
    volatile char val;

    val = thp_base[0];
    printf("    Page 0 (before hole): readable = %d\n", val);

    val = thp_base[PAGE_4K * 9];
    printf("    Page 9 (before hole): readable = %d\n", val);

    // 访问 hole 应该失败（已 unmap）
    printf("    Pages 10-14 (hole): should be unmapped\n");

    val = thp_base[PAGE_4K * 15];
    printf("    Page 15 (after hole): readable = %d\n", val);

    val = thp_base[PAGE_4K * 511];
    printf("    Page 511 (last page): readable = %d\n", val);

    munmap(p, len);
    printf("  TEST 1: %s\n\n", test_failed ? "FAILED" : "PASSED");

    //
    // TEST 2: munmap 跨越 2M 边界（检测边界处理）
    //
    printf("[TEST 2] munmap spanning two 2M THPs\n");

    len = 8 * 1024 * 1024;  // 8MB，足够容纳 2 个完整的 2M THP
    p = mmap(NULL, len, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap");
    if (madvise(p, len, MADV_HUGEPAGE) != 0) die("madvise");

    base = (uintptr_t)p;
    win = (base + THP_SIZE - 1) & ~(THP_SIZE - 1);
    char *thp1_base = (char *)win;
    char *thp2_base = thp1_base + THP_SIZE;

    printf("  THP1 base: %p - %p\n", thp1_base, thp1_base + THP_SIZE);
    printf("  THP2 base: %p - %p\n", thp2_base, thp2_base + THP_SIZE);

    // Touch 两个 2M 区域
    for (size_t i = 0; i < THP_SIZE * 2; i += PAGE_4K) {
        thp1_base[i] = (char)(i / PAGE_4K);
    }

    printf("  Waiting for both THPs to collapse...\n");
    wait_for_collapse(thp1_base);
    sleep(1);
    ((volatile char *)thp2_base)[0] ^= 1;

    // munmap 跨越边界：THP1 的最后 3 页 + THP2 的前 3 页
    char *cross_addr = thp1_base + THP_SIZE - PAGE_4K * 3;
    size_t cross_len = PAGE_4K * 6;

    printf("  Unmapping across boundary: %p, len=%zu\n", cross_addr, cross_len);
    printf("  This covers:\n");
    printf("    - THP1 pages [509, 510, 511]\n");
    printf("    - THP2 pages [0, 1, 2]\n");
    printf("  Should trigger split on BOTH THP1(%p) and THP2(%p)\n",
           thp1_base, thp2_base);

    if (munmap(cross_addr, cross_len) != 0) {
        perror("munmap cross-boundary");
        test_failed = 1;
    } else {
        printf("  munmap returned successfully\n");
    }

    // 验证
    printf("  Verifying pages...\n");
    val = thp1_base[PAGE_4K * 508];
    printf("    THP1 page 508 (before hole): readable = %d\n", val);

    val = thp2_base[PAGE_4K * 3];
    printf("    THP2 page 3 (after hole): readable = %d\n", val);

    munmap(p, len);
    printf("  TEST 2: %s\n\n", test_failed ? "FAILED" : "PASSED");

    //
    // TEST 3: 连续 munmap 同一 THP 的不同部分（检测 dealloc_frame page_size 问题）
    //
    printf("[TEST 3] Multiple partial munmaps on same THP\n");
    printf("  This tests if page_size is correctly updated after split\n");

    len = 4 * 1024 * 1024;
    p = mmap(NULL, len, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap");
    if (madvise(p, len, MADV_HUGEPAGE) != 0) die("madvise");

    base = (uintptr_t)p;
    win = (base + THP_SIZE - 1) & ~(THP_SIZE - 1);
    thp_base = (char *)win;

    for (size_t i = 0; i < THP_SIZE; i += PAGE_4K) {
        thp_base[i] = (char)(i / PAGE_4K);
    }
    wait_for_collapse(thp_base);

    // 第一次 munmap：中间部分
    printf("  1st munmap: pages 100-105\n");
    if (munmap(thp_base + PAGE_4K * 100, PAGE_4K * 5) != 0) {
        perror("1st munmap");
        test_failed = 1;
    }

    // 第二次 munmap：另一部分（THP 已经 split）
    printf("  2nd munmap: pages 200-205 (THP already split to 4K)\n");
    printf("  BUG CHECK: dealloc_frame should receive Size4K, not Size2M\n");
    if (munmap(thp_base + PAGE_4K * 200, PAGE_4K * 5) != 0) {
        perror("2nd munmap");
        test_failed = 1;
    }

    // 第三次 munmap：再一部分
    printf("  3rd munmap: pages 300-305\n");
    if (munmap(thp_base + PAGE_4K * 300, PAGE_4K * 5) != 0) {
        perror("3rd munmap");
        test_failed = 1;
    }

    printf("  All munmaps completed\n");
    printf("  Verifying remaining pages...\n");

    val = thp_base[0];
    printf("    Page 0: readable = %d\n", val);

    val = thp_base[PAGE_4K * 99];
    printf("    Page 99 (before 1st hole): readable = %d\n", val);

    val = thp_base[PAGE_4K * 106];
    printf("    Page 106 (after 1st hole): readable = %d\n", val);

    munmap(p, len);
    printf("  TEST 3: %s\n\n", test_failed ? "FAILED" : "PASSED");

    //
    // TEST 4: munmap 整个 2M（不应该 split）
    //
    printf("[TEST 4] munmap entire 2M THP (should NOT split)\n");

    len = 4 * 1024 * 1024;
    p = mmap(NULL, len, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap");
    if (madvise(p, len, MADV_HUGEPAGE) != 0) die("madvise");

    base = (uintptr_t)p;
    win = (base + THP_SIZE - 1) & ~(THP_SIZE - 1);
    thp_base = (char *)win;

    for (size_t i = 0; i < THP_SIZE; i += PAGE_4K) {
        thp_base[i] = (char)(i / PAGE_4K);
    }
    wait_for_collapse(thp_base);

    printf("  Unmapping entire 2M: %p - %p\n", thp_base, thp_base + THP_SIZE);
    printf("  This should NOT trigger split (complete unmap)\n");

    if (munmap(thp_base, THP_SIZE) != 0) {
        perror("munmap full THP");
        test_failed = 1;
    } else {
        printf("  munmap successful\n");
    }

    // 剩余部分仍应可访问
    if (thp_base > p) {
        val = p[0];
        printf("  Pages before THP: readable = %d\n", val);
    }
    if (thp_base + THP_SIZE < p + len) {
        val = (thp_base + THP_SIZE)[0];
        printf("  Pages after THP: readable = %d\n", val);
    }

    munmap(p, len);
    printf("  TEST 4: %s\n\n", test_failed ? "FAILED" : "PASSED");

    printf("===========================================\n");
    printf("Overall result: %s\n", test_failed ? "FAILED" : "ALL TESTS PASSED");
    return test_failed ? 1 : 0;
}
