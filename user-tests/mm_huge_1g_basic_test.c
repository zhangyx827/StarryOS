#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// Base page size for pattern/checking.
#define PAGE_4K 4096UL

// 1 GiB huge page size.
#define HUGEPAGE_1G (1UL * 1024 * 1024 * 1024)

// Fallback definitions in case libc headers don't expose them.
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << MAP_HUGE_SHIFT)
#endif

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void write_pattern(char *base, size_t page_idx, unsigned seed) {
    base[page_idx * PAGE_4K] = (char)((page_idx ^ seed) & 0xFF);
}

static int check_pattern(char *base, size_t page_idx, unsigned seed, const char *tag) {
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

// Basic sanity test: map a 1 GiB hugetlb mapping, check alignment and that
// first/last 4K pages are usable, then unmap the whole thing.
static int test_map_1g_hugetlb_basic(void) {
    const char *tag = "hugetlb_1g_basic";

    printf("[%s] mapping %zu bytes (1 GiB hugepage)...\n", tag, (size_t)HUGEPAGE_1G);
    char *p = mmap(NULL, HUGEPAGE_1G, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_1GB,
                   -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap 1G hugetlb");
        return 1;
    }

    // Address should be 1 GiB aligned.
    if (((uintptr_t)p % HUGEPAGE_1G) != 0) {
        fprintf(stderr, "[%s] address %p is not 1GiB-aligned\n", tag, p);
        munmap(p, HUGEPAGE_1G);
        return 1;
    }

    size_t pages = HUGEPAGE_1G / PAGE_4K;
    unsigned seed = 0x61;

    // Only touch a few pages to avoid stressing memory usage.
    size_t first = 0;
    size_t second = 1;
    size_t last = pages - 1;

    write_pattern(p, first, seed);
    write_pattern(p, second, seed);
    write_pattern(p, last, seed);

    if (check_pattern(p, first, seed, tag) != 0 ||
        check_pattern(p, second, seed, tag) != 0 ||
        check_pattern(p, last, seed, tag) != 0) {
        munmap(p, HUGEPAGE_1G);
        return 1;
    }

    if (munmap(p, HUGEPAGE_1G) != 0) {
        die("munmap 1G hugetlb");
    }

    printf("[%s] PASS\n", tag);
    return 0;
}

// Negative test: unaligned / too-small munmap on a 1 GiB hugetlb mapping
// should fail with EINVAL and leave the mapping intact.
static int test_unaligned_4k_unmap_fails_1g(void) {
    const char *tag = "hugetlb_1g_unaligned_unmap";

    printf("[%s] mapping %zu bytes (1 GiB hugepage)...\n", tag, (size_t)HUGEPAGE_1G);
    char *p = mmap(NULL, HUGEPAGE_1G, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_1GB,
                   -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap 1G hugetlb");
        return 1;
    }

    size_t pages = HUGEPAGE_1G / PAGE_4K;
    unsigned seed = 0x62;

    // Put a recognizable pattern in a few pages.
    write_pattern(p, 0, seed);
    write_pattern(p, pages / 2, seed);
    write_pattern(p, pages - 1, seed);

    errno = 0;
    printf("[%s] trying to munmap 4K of a 1G hugetlb mapping: addr=%p len=%zu\n",
           tag, p, (size_t)PAGE_4K);
    int ret = munmap(p, PAGE_4K);
    if (ret == 0) {
        fprintf(stderr,
                "[%s] expected failure when unmapping 4K of MAP_HUGETLB|MAP_HUGE_1GB mapping\n",
                tag);
        munmap(p, HUGEPAGE_1G);
        return 1;
    }
    if (errno != EINVAL) {
        fprintf(stderr,
                "[%s] expected errno=EINVAL when unmapping 4K, got errno=%d\n",
                tag, errno);
        munmap(p, HUGEPAGE_1G);
        return 1;
    }

    // Mapping should still be intact; patterns must still match.
    if (check_pattern(p, 0, seed, tag) != 0 ||
        check_pattern(p, pages / 2, seed, tag) != 0 ||
        check_pattern(p, pages - 1, seed, tag) != 0) {
        munmap(p, HUGEPAGE_1G);
        return 1;
    }

    // Finally unmap the whole 1G region properly.
    if (munmap(p, HUGEPAGE_1G) != 0) {
        die("munmap full 1G hugepage");
    }

    printf("[%s] PASS\n", tag);
    return 0;
}

int main(void) {
    int failed = 0;

    if (test_map_1g_hugetlb_basic()      != 0) failed = 1;
    if (test_unaligned_4k_unmap_fails_1g() != 0) failed = 1;

    printf("\n[mm_huge_1g_basic_test] %s\n",
           failed ? "FAILED" : "ALL TESTS PASSED");
    return failed ? 1 : 0;
}

