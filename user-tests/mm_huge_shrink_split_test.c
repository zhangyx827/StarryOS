#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// Base page size we assume for pattern/checking.
#define PAGE_4K 4096UL

// Huge page size used for MAP_HUGETLB mappings (typically 2 MiB).
// If the kernel uses a different size, adjust this constant accordingly.
#define HUGEPAGE_SIZE (2UL * 1024 * 1024)

// Fallback definition in case the libc headers don't expose MAP_HUGETLB.
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif

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

// Check all 4K pages in [start_page, start_page + nr_pages).
static int check_range(char *base,
                       size_t start_page,
                       size_t nr_pages,
                       unsigned seed,
                       const char *tag) {
    for (size_t i = 0; i < nr_pages; i++) {
        if (check_page(base, start_page + i, seed, tag) != 0) {
            return -1;
        }
    }
    return 0;
}

// Map 3 * HUGEPAGE_SIZE with MAP_HUGETLB and test unmapping the tail huge page
// (analogous to shrinking VMA on the right).
static int test_shrink_right_huge(void) {
    const char *tag = "hugetlb_shrink_right";
    size_t len = HUGEPAGE_SIZE * 3;
    size_t pages_per_huge = HUGEPAGE_SIZE / PAGE_4K;

    printf("[%s] mapping %zu bytes (3 huge pages)...\n", tag, len);
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) die("mmap");

    unsigned seed = 0x51;
    fill_pattern(p, len, seed);

    // Unmap the last huge page.
    char *unmap_addr = p + 2 * HUGEPAGE_SIZE;
    printf("[%s] munmap tail hugepage: addr=%p len=%zu\n",
           tag, unmap_addr, (size_t)HUGEPAGE_SIZE);
    if (munmap(unmap_addr, HUGEPAGE_SIZE) != 0) {
        die("munmap tail hugepage");
    }

    // Huge page 0 and 1 should still be present and unchanged.
    if (check_range(p, 0, 2 * pages_per_huge, seed, tag) != 0) {
        // best-effort cleanup
        munmap(p, 2 * HUGEPAGE_SIZE);
        return 1;
    }

    // Clean up remaining mapping.
    if (munmap(p, 2 * HUGEPAGE_SIZE) != 0) {
        die("munmap remaining (shrink_right)");
    }

    printf("[%s] PASS\n", tag);
    return 0;
}

// Map 3 * HUGEPAGE_SIZE with MAP_HUGETLB and test unmapping the head huge page
// (analogous to shrinking VMA on the left).
static int test_shrink_left_huge(void) {
    const char *tag = "hugetlb_shrink_left";
    size_t len = HUGEPAGE_SIZE * 3;
    size_t pages_per_huge = HUGEPAGE_SIZE / PAGE_4K;

    printf("[%s] mapping %zu bytes (3 huge pages)...\n", tag, len);
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) die("mmap");

    unsigned seed = 0x52;
    fill_pattern(p, len, seed);

    printf("[%s] munmap head hugepage: addr=%p len=%zu\n",
           tag, p, (size_t)HUGEPAGE_SIZE);
    if (munmap(p, HUGEPAGE_SIZE) != 0) {
        die("munmap head hugepage");
    }

    // Huge page 1 and 2 (original indexes) should still be present and unchanged.
    if (check_range(p, pages_per_huge, 2 * pages_per_huge, seed, tag) != 0) {
        // best-effort cleanup
        munmap(p + HUGEPAGE_SIZE, 2 * HUGEPAGE_SIZE);
        return 1;
    }

    if (munmap(p + HUGEPAGE_SIZE, 2 * HUGEPAGE_SIZE) != 0) {
        die("munmap remaining (shrink_left)");
    }

    printf("[%s] PASS\n", tag);
    return 0;
}

// Map 3 * HUGEPAGE_SIZE with MAP_HUGETLB and unmap the middle huge page to
// force the VMA to split into two disjoint pieces (left + right).
static int test_split_middle_huge(void) {
    const char *tag = "hugetlb_split_middle";
    size_t len = HUGEPAGE_SIZE * 3;
    size_t pages_per_huge = HUGEPAGE_SIZE / PAGE_4K;

    printf("[%s] mapping %zu bytes (3 huge pages)...\n", tag, len);
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) die("mmap");

    unsigned seed = 0x53;
    fill_pattern(p, len, seed);

    char *unmap_addr = p + HUGEPAGE_SIZE;
    printf("[%s] munmap middle hugepage: addr=%p len=%zu\n",
           tag, unmap_addr, (size_t)HUGEPAGE_SIZE);
    if (munmap(unmap_addr, HUGEPAGE_SIZE) != 0) {
        die("munmap middle hugepage");
    }

    // Huge page 0 and 2 should still be present and unchanged.
    if (check_range(p, 0, pages_per_huge, seed, tag) != 0 ||
        check_range(p, 2 * pages_per_huge, pages_per_huge, seed, tag) != 0) {
        // best-effort cleanup of remaining mapped parts.
        munmap(p, HUGEPAGE_SIZE);
        munmap(p + 2 * HUGEPAGE_SIZE, HUGEPAGE_SIZE);
        return 1;
    }

    // Clean up both sides.
    if (munmap(p, HUGEPAGE_SIZE) != 0) {
        die("munmap left hugepage (split_middle)");
    }
    if (munmap(p + 2 * HUGEPAGE_SIZE, HUGEPAGE_SIZE) != 0) {
        die("munmap right hugepage (split_middle)");
    }

    printf("[%s] PASS\n", tag);
    return 0;
}

// Optional: verify that trying to unmap only 4K of a hugetlb mapping fails.
static int test_unaligned_4k_unmap_fails(void) {
    const char *tag = "hugetlb_unaligned_4k_unmap";
    size_t len = HUGEPAGE_SIZE;

    printf("[%s] mapping %zu bytes (1 huge page)...\n", tag, len);
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) die("mmap");

    errno = 0;
    printf("[%s] trying to munmap 4K of a hugepage: addr=%p len=%zu\n",
           tag, p, (size_t)PAGE_4K);
    int ret = munmap(p, PAGE_4K);
    if (ret == 0) {
        fprintf(stderr, "[%s] expected failure when unmapping 4K of MAP_HUGETLB mapping\n", tag);
        // clean up whole mapping and signal failure
        munmap(p, len);
        return 1;
    }

    if (errno != EINVAL) {
        fprintf(stderr,
                "[%s] expected errno=EINVAL when unmapping 4K of hugepage, got errno=%d\n",
                tag, errno);
        munmap(p, len);
        return 1;
    }

    // The mapping should still be there; now unmap the whole huge page properly.
    if (munmap(p, len) != 0) {
        die("munmap full hugepage after failed 4K unmap");
    }

    printf("[%s] PASS\n", tag);
    return 0;
}

int main(void) {
    int failed = 0;

    if (test_shrink_right_huge()      != 0) failed = 1;
    if (test_shrink_left_huge()       != 0) failed = 1;
    if (test_split_middle_huge()      != 0) failed = 1;
    if (test_unaligned_4k_unmap_fails() != 0) failed = 1;

    printf("\n[mm_huge_shrink_split_test] %s\n",
           failed ? "FAILED" : "ALL TESTS PASSED");
    return failed ? 1 : 0;
}
