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

static int test_mprotect_hugetlb_basic(void) {
    const char *tag = "hugetlb_mprotect_basic";
    size_t len = HUGEPAGE_SIZE * 2;
    size_t pages = len / PAGE_4K;

    printf("[%s] mapping %zu bytes (2 huge pages)...\n", tag, len);
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap hugetlb");
        return 1;
    }

    unsigned seed = 0x71;
    fill_pattern(p, len, seed);

    char *prot_addr = p + HUGEPAGE_SIZE;
    printf("[%s] mprotect second hugepage to PROT_READ: addr=%p len=%zu\n",
           tag, prot_addr, (size_t)HUGEPAGE_SIZE);
    if (mprotect(prot_addr, HUGEPAGE_SIZE, PROT_READ) != 0) {
        perror("mprotect hugetlb aligned");
        munmap(p, len);
        return 1;
    }

    // Data across both huge pages should remain intact.
    if (check_range(p, 0, pages, seed, tag) != 0) {
        munmap(p, len);
        return 1;
    }

    if (munmap(p, len) != 0) {
        die("munmap hugetlb");
    }

    printf("[%s] PASS\n", tag);
    return 0;
}

static int test_mprotect_hugetlb_unaligned_fails(void) {
    const char *tag = "hugetlb_mprotect_unaligned";
    size_t len = HUGEPAGE_SIZE;
    size_t pages = len / PAGE_4K;

    printf("[%s] mapping %zu bytes (1 huge page)...\n", tag, len);
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap hugetlb");
        return 1;
    }

    unsigned seed = 0x72;
    fill_pattern(p, len, seed);

    // Try to protect only part of the hugepage using a 4K-aligned range.
    errno = 0;
    char *prot_addr = p + PAGE_4K;
    size_t prot_len = len - PAGE_4K;
    printf("[%s] trying mprotect on non-huge-aligned range: addr=%p len=%zu\n",
           tag, prot_addr, prot_len);
    int ret = mprotect(prot_addr, prot_len, PROT_READ);
    if (ret == 0) {
        fprintf(stderr,
                "[%s] expected failure when mprotect() on non-huge-aligned range\n",
                tag);
        munmap(p, len);
        return 1;
    }
    if (errno != EINVAL) {
        fprintf(stderr,
                "[%s] expected errno=EINVAL for unaligned mprotect, got errno=%d\n",
                tag, errno);
        munmap(p, len);
        return 1;
    }

    // Mapping should still be intact.
    if (check_range(p, 0, pages, seed, tag) != 0) {
        munmap(p, len);
        return 1;
    }

    if (munmap(p, len) != 0) {
        die("munmap hugetlb after unaligned mprotect");
    }

    printf("[%s] PASS\n", tag);
    return 0;
}

int main(void) {
    int failed = 0;

    if (test_mprotect_hugetlb_basic()          != 0) failed = 1;
    if (test_mprotect_hugetlb_unaligned_fails() != 0) failed = 1;

    printf("\n[mm_huge_protect_test] %s\n",
           failed ? "FAILED" : "ALL TESTS PASSED");
    return failed ? 1 : 0;
}

