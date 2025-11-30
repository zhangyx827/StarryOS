#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_4K 4096UL

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void fill_pattern(char *p, size_t len, unsigned seed) {
    for (size_t off = 0; off < len; off += PAGE_4K) {
        p[off] = (char)(((off / PAGE_4K) ^ seed) & 0xFF);
    }
}

static int check_page(char *base, size_t page_idx, unsigned seed, const char *tag) {
    char expected = (char)((page_idx ^ seed) & 0xFF);
    char got = base[page_idx * PAGE_4K];
    if (got != expected) {
        fprintf(stderr,
                "[%s] page %zu corrupted: got %#x, expected %#x\n",
                tag, page_idx, (unsigned char)got, (unsigned char)expected);
        return -1;
    }
    return 0;
}

static int test_shrink_right(void) {
    printf("[shrink_right] mapping 3 pages...\n");
    size_t len = PAGE_4K * 3;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap");

    unsigned seed = 0x11;
    fill_pattern(p, len, seed);

    printf("[shrink_right] munmap tail: addr=%p len=%zu (last page)\n",
           p + PAGE_4K * 2, (size_t)PAGE_4K);
    if (munmap(p + PAGE_4K * 2, PAGE_4K) != 0) {
        die("munmap tail");
    }

    // 页 0,1 仍然应该可读且内容不变。
    if (check_page(p, 0, seed, "shrink_right") != 0 ||
        check_page(p, 1, seed, "shrink_right") != 0) {
        munmap(p, PAGE_4K * 2);
        return 1;
    }

    // 清理剩余区域。
    if (munmap(p, PAGE_4K * 2) != 0) {
        die("munmap remaining");
    }

    printf("[shrink_right] PASS\n");
    return 0;
}

static int test_shrink_left(void) {
    printf("[shrink_left] mapping 3 pages...\n");
    size_t len = PAGE_4K * 3;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap");

    unsigned seed = 0x22;
    fill_pattern(p, len, seed);

    printf("[shrink_left] munmap head: addr=%p len=%zu (first page)\n",
           p, (size_t)PAGE_4K);
    if (munmap(p, PAGE_4K) != 0) {
        die("munmap head");
    }

    // 预期：page1, page2 仍然是原来的内容。
    if (check_page(p, 1, seed, "shrink_left") != 0 ||
        check_page(p, 2, seed, "shrink_left") != 0) {
        if (check_page(p, 0, seed, "shrink_left(recheck)") != 0 ||
            check_page(p, 1, seed, "shrink_left(recheck)") != 0) {
            // 如果内容明显错了，就直接判失败。
            munmap(p + PAGE_4K, PAGE_4K * 2);
            return 1;
        }
    }

    if (munmap(p + PAGE_4K, PAGE_4K * 2) != 0) {
        die("munmap remaining");
    }

    printf("[shrink_left] PASS\n");
    return 0;
}

static int test_split_middle(void) {
    printf("[split_middle] mapping 3 pages...\n");
    size_t len = PAGE_4K * 3;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap");

    unsigned seed = 0x33;
    fill_pattern(p, len, seed);

    // 在中间 unmap 一页，引发 MemorySet::unmap 使用 split + shrink_right。
    printf("[split_middle] munmap middle: addr=%p len=%zu (page 1)\n",
           p + PAGE_4K, (size_t)PAGE_4K);
    if (munmap(p + PAGE_4K, PAGE_4K) != 0) {
        die("munmap middle");
    }

    // 预期：page0 和 page2 仍然存在且内容不变。
    if (check_page(p, 0, seed, "split_middle") != 0 ||
        check_page(p, 2, seed, "split_middle") != 0) {
        munmap(p, PAGE_4K);            // 尝试清理剩余
        munmap(p + 2 * PAGE_4K, PAGE_4K);
        return 1;
    }

    // 清理左右两端。
    if (munmap(p, PAGE_4K) != 0) {
        die("munmap left part");
    }
    if (munmap(p + 2 * PAGE_4K, PAGE_4K) != 0) {
        die("munmap right part");
    }

    printf("[split_middle] PASS\n");
    return 0;
}

int main(void) {
    int failed = 0;

    if (test_shrink_right() != 0) failed = 1;
    if (test_shrink_left()  != 0) failed = 1;
    if (test_split_middle() != 0) failed = 1;

    printf("\n[mm_shrink_split_test] %s\n", failed ? "FAILED" : "ALL TESTS PASSED");
    return failed ? 1 : 0;
}

