#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
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
    const size_t thp_size = 2UL * 1024 * 1024;  // 2 MiB
    const size_t len = 4UL * 1024 * 1024;       // 4 MiB, enough for one THP window

    printf("[thp_munmap_split_test] Setting THP mode to 'always'...\n");
    set_thp_mode("always");

    printf("[thp_munmap_split_test] mmap anonymous region len=%zu\n", len);
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap");
    }
    printf("  mapping at %p\n", p);

    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE)");
    }

    // Touch all 4K pages to populate PTEs.
    printf("[thp_munmap_split_test] touching 4K pages...\n");
    for (size_t i = 0; i < len; i += 4096) {
        ((volatile char *)p)[i] = (char)(i / 4096);
    }

    // Try to get a 2M-aligned window fully inside the mapping.
    uintptr_t base = (uintptr_t)p;
    uintptr_t end = base + len;
    uintptr_t win = (base + thp_size - 1) & ~(thp_size - 1);  // align up to 2M
    if (win + thp_size > end) {
        printf("[thp_munmap_split_test] cannot find 2M-aligned window, skipping THP wait\n");
        win = base;  // still test partial munmap on normal pages
    }

    void *thp_region = (void *)win;
    printf("[thp_munmap_split_test] candidate THP window at %p (size 2M)\n", thp_region);

    // Observe pages_collapsed to see if khugepaged likely collapsed something.
    unsigned long prev = read_pages_collapsed();
    printf("[thp_munmap_split_test] pages_collapsed(before) = %lu\n", prev);

    for (int iter = 0; iter < 30; iter++) {
        sleep(1);
        // keep region hot
        ((volatile char *)thp_region)[0] ^= 1;

        unsigned long now = read_pages_collapsed();
        printf("  [iter %d] pages_collapsed = %lu\n", iter, now);
        if (now > prev) {
            printf("[thp_munmap_split_test] detected THP collapse (pages_collapsed increased)\n");
            break;
        }
    }

    // Now perform a partial munmap inside the (possibly) huge-mapped 2M range.
    const size_t hole_len = 64UL * 1024;  // 64 KiB
    void *hole = (void *)(win + 512UL * 1024);  // 0.5 MiB offset into the 2M window

    // Ensure hole is 4K-aligned and fully inside mapping.
    if (((uintptr_t)hole % 4096) != 0 ||
        (uintptr_t)hole < base ||
        (uintptr_t)hole + hole_len > end) {
        printf("[thp_munmap_split_test] hole region misaligned / out of range, aborting\n");
        goto out;
    }

    printf("[thp_munmap_split_test] munmap partial hole at %p, len=%zu\n", hole, hole_len);
    if (munmap(hole, hole_len) != 0) {
        die("munmap(hole)");
    }

    // Access before the hole: should still be mapped.
    printf("[thp_munmap_split_test] touching bytes before hole...\n");
    ((volatile char *)thp_region)[0] ^= 1;

    // Access after the hole: if kernel mistakenly unmapped whole 2M,
    // this should segfault / panic the test.
    printf("[thp_munmap_split_test] touching bytes after hole...\n");
    size_t offset_after = (size_t)(thp_size - 4096);  // last 4K page in the window
    ((volatile char *)thp_region)[offset_after] ^= 1;

    printf("[thp_munmap_split_test] PASS: partial munmap did not break surrounding mappings\n");

out:
    if (munmap(p, len) != 0) {
        die("munmap(total)");
    }
    return 0;
}

