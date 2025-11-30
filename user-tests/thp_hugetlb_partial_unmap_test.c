#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#ifndef MAP_HUGETLB
// Standard Linux value; our kernel maps this to MmapFlags::HUGE.
#define MAP_HUGETLB 0x40000
#endif

#define THP_SIZE (2UL * 1024 * 1024)
#define PAGE_4K  (4096UL)

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

int main(void) {
    printf("=== thp_hugetlb_partial_unmap_test ===\n");
    printf("This test verifies that a mapping created with MAP_HUGETLB\n");
    printf("does NOT allow partial unmap of a single huge page.\n\n");

    // Step 1: create a 2MiB mapping using MAP_HUGETLB.
    size_t len = THP_SIZE;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) {
        printf("[WARN] mmap(MAP_HUGETLB) failed: %s\n", strerror(errno));
        printf("[WARN] MAP_HUGETLB may not be supported in this configuration.\n");
        printf("[WARN] Skipping test.\n");
        return 77;  // skip
    }
    printf("[INFO] huge mapping at %p, len=%zu (2MiB)\n", p, len);

    // Touch the huge page so that it is definitely mapped.
    for (size_t i = 0; i < len; i += PAGE_4K) {
        p[i] = (char)(i / PAGE_4K);
    }

    // Step 2: attempt to unmap only part of the huge page.
    // For example, unmap 4KiB in the middle of the 2MiB range.
    char *partial = p + PAGE_4K * 10;
    size_t partial_len = PAGE_4K * 5;

    printf("[INFO] attempting partial munmap(%p, %zu) inside huge page\n",
           partial, partial_len);

    int rc = munmap(partial, partial_len);
    if (rc == 0) {
        printf("[FAIL] partial munmap on MAP_HUGETLB mapping unexpectedly succeeded\n");
        // Clean up the rest to avoid leaking mapping.
        if (munmap(p, len) != 0) {
            die("munmap(full) after unexpected success");
        }
        return 1;
    } else {
        printf("[INFO] partial munmap failed as expected: rc=%d errno=%d (%s)\n",
               rc, errno, strerror(errno));
    }

    // Step 3: full unmap of the huge page should still succeed.
    printf("[INFO] unmapping full huge page %p len=%zu\n", p, len);
    if (munmap(p, len) != 0) {
        perror("munmap(full huge)");
        printf("[FAIL] full munmap on MAP_HUGETLB mapping failed unexpectedly\n");
        return 1;
    }

    printf("[PASS] thp_hugetlb_partial_unmap_test: partial munmap rejected, full munmap OK\n");
    return 0;
}

