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

int main(void) {
    printf("[thp_unaligned_munmap_test] Testing munmap with non-2M-aligned address\n");

    set_thp_mode("always");

    // 分配 4MB
    size_t len = 4 * 1024 * 1024;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap");

    if (madvise(p, len, MADV_HUGEPAGE) != 0) die("madvise");

    // 找到 2M 对齐的窗口
    uintptr_t base = (uintptr_t)p;
    uintptr_t end = base + len;
    uintptr_t win = (base + THP_SIZE - 1) & ~(THP_SIZE - 1);
    if (win + THP_SIZE > end) {
        printf("Cannot find 2M-aligned window\n");
        munmap(p, len);
        return 1;
    }

    char *thp_base = (char *)win;
    printf("[thp_unaligned_munmap_test] THP base (2M-aligned): %p\n", thp_base);

    // Touch 整个 2M 区域
    for (size_t i = 0; i < THP_SIZE; i += 4096) {
        thp_base[i] = (char)(i / 4096);
    }

    // 等待 collapse
    unsigned long prev = read_pages_collapsed();
    printf("[thp_unaligned_munmap_test] Waiting for collapse...\n");
    for (int iter = 0; iter < 30; iter++) {
        sleep(1);
        ((volatile char *)thp_base)[0] ^= 1;
        unsigned long now = read_pages_collapsed();
        if (now > prev) {
            printf("  Collapse detected!\n");
            break;
        }
    }

    // 测试 1: unmap 从非对齐地址开始（触发 split_thp 参数错误）
    printf("\n[TEST 1] Unmapping from NON-aligned address within THP\n");
    char *unaligned_addr = thp_base + 4096;  // 偏移 1 页（非 2M 对齐）
    size_t unmap_len = 8192;  // 2 页

    printf("  Calling munmap(%p, %zu)\n", unaligned_addr, unmap_len);
    printf("  Note: THP base is %p, unmap starts at offset 0x1000\n", thp_base);
    printf("  If split_thp receives %p (non-aligned), it's a bug!\n", unaligned_addr);

    if (munmap(unaligned_addr, unmap_len) != 0) {
        perror("munmap unaligned");
        munmap(p, len);
        return 1;
    }
    printf("  munmap succeeded\n");

    // 验证周围的页仍然可访问
    printf("  Verifying surrounding pages are still accessible...\n");
    volatile char val;
    val = thp_base[0];  // 第一页
    printf("    thp_base[0] = %d (OK)\n", val);

    val = thp_base[16384];  // unmap 后的第一页
    printf("    thp_base[16384] = %d (OK)\n", val);

    // 测试 2: 重新 map，然后 unmap 跨越 2M 边界
    printf("\n[TEST 2] Unmapping across 2M boundary\n");

    // 重新 map 完整的 4M 区域
    munmap(p, len);
    p = mmap(NULL, len, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap");
    if (madvise(p, len, MADV_HUGEPAGE) != 0) die("madvise");

    // 找到 2M 对齐位置
    base = (uintptr_t)p;
    end = base + len;
    win = (base + THP_SIZE - 1) & ~(THP_SIZE - 1);
    thp_base = (char *)win;

    // Touch 两个 2M 区域
    for (size_t i = 0; i < THP_SIZE * 2; i += 4096) {
        if (thp_base + i < p + len) {
            thp_base[i] = (char)(i / 4096);
        }
    }

    // 等待 collapse
    prev = read_pages_collapsed();
    printf("  Waiting for collapse of two THPs...\n");
    for (int iter = 0; iter < 30; iter++) {
        sleep(1);
        ((volatile char *)thp_base)[0] ^= 1;
        ((volatile char *)(thp_base + THP_SIZE))[0] ^= 1;
        unsigned long now = read_pages_collapsed();
        if (now > prev + 1) {  // 等待至少 2 个 collapse
            printf("    Collapse detected!\n");
            break;
        }
    }

    // unmap 跨越 2M 边界（应该触发两次 split）
    char *cross_addr = thp_base + THP_SIZE - 8192;  // 最后 2 页
    size_t cross_len = 16384;  // 跨越边界的 4 页

    printf("  Unmapping across boundary: %p len=%zu\n", cross_addr, cross_len);
    printf("  This spans from THP1[last 2 pages] to THP2[first 2 pages]\n");

    if (munmap(cross_addr, cross_len) != 0) {
        perror("munmap cross-boundary");
        munmap(p, len);
        return 1;
    }
    printf("  munmap succeeded\n");

    // 验证两侧的页
    printf("  Verifying pages before and after the hole...\n");
    val = thp_base[THP_SIZE - 16384];  // THP1 倒数第 4 页
    printf("    Before hole: %d (OK)\n", val);

    val = thp_base[THP_SIZE + 16384];  // THP2 第 5 页
    printf("    After hole: %d (OK)\n", val);

    printf("\n[thp_unaligned_munmap_test] PASS: All tests completed\n");

    munmap(p, len);
    return 0;
}
