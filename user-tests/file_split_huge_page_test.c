#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define HUGE_SIZE (2UL * 1024 * 1024)
#define NUM_SMALL_PAGES 512  // 2M / 4K = 512

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

// Fill buffer with pattern based on page number
static void fill_page_pattern(uint8_t *buf, size_t page_num, size_t len) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)((page_num + i) & 0xFF);
    }
}

// Verify buffer contains expected pattern
static int verify_page_pattern(uint8_t *buf, size_t page_num, size_t len, const char *tag) {
    for (size_t i = 0; i < len; i++) {
        uint8_t expected = (uint8_t)((page_num + i) & 0xFF);
        if (buf[i] != expected) {
            fprintf(stderr, "FAIL %s: page %zu offset %zu: got %#x expected %#x\n",
                    tag, page_num, i, buf[i], expected);
            return -1;
        }
    }
    return 0;
}

// Test 1: Munmap middle portion to trigger split
static int test_munmap_middle(void) {
    printf("\n[TEST 1] Munmap middle portion of huge page\n");

    const char *path = "/tmp/file_split_test1.bin";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) die("open");

    // Create file with 4MB
    if (ftruncate(fd, 4 * HUGE_SIZE) != 0) die("ftruncate");

    // Map the file
    void *addr = mmap(NULL, 4 * HUGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) die("mmap");

    uint8_t *base = (uint8_t *)addr;

    // Write unique pattern to each 4K page in first 2M
    printf("  Writing patterns to 512 pages (2M)...\n");
    for (size_t i = 0; i < NUM_SMALL_PAGES; i++) {
        fill_page_pattern(base + i * PAGE_SIZE, i, PAGE_SIZE);
    }

    // Force collapse to huge page
    printf("  Attempting MADV_COLLAPSE on first 2M...\n");
    if (madvise(base, HUGE_SIZE * 2, MADV_COLLAPSE) != 0) {
        int e = errno;
        if (e == EINVAL || e == ENOTSUP) {
            printf("  MADV_COLLAPSE not supported, skipping test\n");
            munmap(addr, 4 * HUGE_SIZE);
            close(fd);
            unlink(path);
            return 0;
        }
        die("madvise MADV_COLLAPSE");
    }
    printf("  Collapse succeeded (huge page at pn=0)\n");

    // Munmap middle portion (pages 100-199) to trigger split
    size_t unmap_start = 100;
    size_t unmap_count = 100;
    printf("  Unmapping pages %zu-%zu (middle of huge page)...\n",
           unmap_start, unmap_start + unmap_count - 1);

    void *unmap_addr = base + unmap_start * PAGE_SIZE;
    size_t unmap_len = unmap_count * PAGE_SIZE;

    if (munmap(unmap_addr, unmap_len) != 0) {
        perror("munmap");
        munmap(addr, 4 * HUGE_SIZE);
        close(fd);
        unlink(path);
        return -1;
    }
    printf("  Munmap succeeded - should have triggered split_huge_page\n");

    // Verify remaining pages still have correct data
    printf("  Verifying data integrity of remaining pages...\n");
    int failed = 0;

    // Check pages before the hole
    for (size_t i = 0; i < unmap_start; i++) {
        if (verify_page_pattern(base + i * PAGE_SIZE, i, PAGE_SIZE, "before-hole") != 0) {
            failed = 1;
            break;
        }
    }

    // Check pages after the hole
    if (!failed) {
        for (size_t i = unmap_start + unmap_count; i < NUM_SMALL_PAGES; i++) {
            if (verify_page_pattern(base + i * PAGE_SIZE, i, PAGE_SIZE, "after-hole") != 0) {
                failed = 1;
                break;
            }
        }
    }

    munmap(addr, 4 * HUGE_SIZE);
    close(fd);
    unlink(path);

    if (failed) {
        printf("  TEST 1: FAILED - Data corrupted after split\n");
        return -1;
    }

    printf("  TEST 1: PASSED\n");
    return 0;
}

// Test 2: Munmap at offset within huge page (pn != base_pn) - the critical test
static int test_munmap_at_offset(void) {
    printf("\n[TEST 2] Munmap at offset within huge page (pn != base_pn)\n");
    printf("  This test targets the bug where split_huge_page uses `pn` instead of `base_pn`\n");

    const char *path = "/tmp/file_split_test2.bin";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) die("open");

    // Create large file
    if (ftruncate(fd, 8 * HUGE_SIZE) != 0) die("ftruncate");

    // Map at 2M offset (so base_pn for our huge page will be 512, not 0)
    off_t map_offset = 2 * HUGE_SIZE;
    void *addr = mmap(NULL, 4 * HUGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map_offset);
    if (addr == MAP_FAILED) die("mmap");

    uint8_t *base = (uint8_t *)addr;

    // Write patterns
    printf("  Writing patterns (file offset = %lu, base_pn = 512)...\n",
           (unsigned long)map_offset);
    for (size_t i = 0; i < NUM_SMALL_PAGES; i++) {
        fill_page_pattern(base + i * PAGE_SIZE, i, PAGE_SIZE);
    }

    // Collapse to huge page
    printf("  Attempting MADV_COLLAPSE...\n");
    if (madvise(base, HUGE_SIZE * 2, MADV_COLLAPSE) != 0) {
        int e = errno;
        if (e == EINVAL || e == ENOTSUP) {
            printf("  MADV_COLLAPSE not supported, skipping test\n");
            munmap(addr, 4 * HUGE_SIZE);
            close(fd);
            unlink(path);
            return 0;
        }
        die("madvise MADV_COLLAPSE");
    }
    printf("  Collapse succeeded (page cache now has huge page at pn=512)\n");

    // Munmap at an OFFSET within the huge page range
    // For example, munmap pages 300-349 (50 pages in the middle)
    size_t unmap_offset_pages = 300;
    size_t unmap_count = 50;
    void *unmap_addr = base + unmap_offset_pages * PAGE_SIZE;

    printf("  Unmapping %zu pages starting at offset %zu from base...\n",
           unmap_count, unmap_offset_pages);
    // printf("  BUG: split_huge_page will receive pn=%zu (base_pn + %zu)\n",
    //        512 + unmap_offset_pages, unmap_offset_pages);
    // printf("  BUG: If function uses `pn` directly instead of `base_pn`, it will access wrong page\n");

    if (munmap(unmap_addr, unmap_count * PAGE_SIZE) != 0) {
        perror("munmap");
        printf("  TEST 2: FAILED - Munmap operation failed (possible bug detected)\n");
        munmap(addr, 4 * HUGE_SIZE);
        close(fd);
        unlink(path);
        return -1;
    }
    printf("  Munmap returned successfully\n");

    // Verify all remaining pages
    printf("  Verifying data integrity for remaining pages...\n");
    int failed = 0;

    // Pages before the hole
    for (size_t i = 0; i < unmap_offset_pages; i++) {
        if (verify_page_pattern(base + i * PAGE_SIZE, i, PAGE_SIZE, "test2-before") != 0) {
            printf("  ERROR: Data corruption detected at page %zu\n", i);
            failed = 1;
            break;
        }
    }

    // Pages after the hole
    if (!failed) {
        for (size_t i = unmap_offset_pages + unmap_count; i < NUM_SMALL_PAGES; i++) {
            if (verify_page_pattern(base + i * PAGE_SIZE, i, PAGE_SIZE, "test2-after") != 0) {
                printf("  ERROR: Data corruption detected at page %zu\n", i);
                failed = 1;
                break;
            }
        }
    }

    munmap(addr, 4 * HUGE_SIZE);
    close(fd);
    unlink(path);

    if (failed) {
        printf("  TEST 2: FAILED - Data corrupted after split\n");
        return -1;
    }

    printf("  TEST 2: PASSED\n");
    return 0;
}

// Test 3: Multiple huge pages, munmap in the second one
static int test_multiple_huge_pages(void) {
    printf("\n[TEST 3] Multiple huge pages - munmap in second one\n");

    const char *path = "/tmp/file_split_test3.bin";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) die("open");

    // Create file with 6MB (3 huge pages)
    if (ftruncate(fd, 6 * HUGE_SIZE) != 0) die("ftruncate");

    void *addr = mmap(NULL, 6 * HUGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) die("mmap");

    uint8_t *base = (uint8_t *)addr;

    // Write patterns and collapse first two huge pages
    printf("  Writing patterns and collapsing 2 huge pages...\n");
    for (size_t i = 0; i < 2 * NUM_SMALL_PAGES; i++) {
        fill_page_pattern(base + i * PAGE_SIZE, i, PAGE_SIZE);
    }

    // Collapse first huge page
    if (madvise(base, HUGE_SIZE * 2, MADV_COLLAPSE) != 0) {
        int e = errno;
        if (e == EINVAL || e == ENOTSUP) {
            printf("  MADV_COLLAPSE not supported, skipping test\n");
            munmap(addr, 6 * HUGE_SIZE);
            close(fd);
            unlink(path);
            return 0;
        }
        die("madvise MADV_COLLAPSE 1");
    }

    // Collapse second huge page
    if (madvise(base + HUGE_SIZE, HUGE_SIZE * 2, MADV_COLLAPSE) != 0) {
        die("madvise MADV_COLLAPSE 2");
    }
    printf("  Both huge pages collapsed\n");

    // Munmap in the SECOND huge page at some offset
    size_t second_hp_start = NUM_SMALL_PAGES;
    size_t munmap_offset = 200;  // Offset within second huge page
    size_t munmap_count = 50;

    void *munmap_addr = base + (second_hp_start + munmap_offset) * PAGE_SIZE;
    printf("  Unmapping %zu pages in second huge page (offset %zu from its base)...\n",
           munmap_count, munmap_offset);

    if (munmap(munmap_addr, munmap_count * PAGE_SIZE) != 0) {
        perror("munmap");
        munmap(addr, 6 * HUGE_SIZE);
        close(fd);
        unlink(path);
        return -1;
    }
    printf("  Munmap succeeded\n");

    // Verify data in both huge page regions (excluding the munmapped area)
    printf("  Verifying all data...\n");
    int failed = 0;

    // First huge page (should be intact)
    for (size_t i = 0; i < NUM_SMALL_PAGES; i++) {
        if (verify_page_pattern(base + i * PAGE_SIZE, i, PAGE_SIZE, "test3-hp1") != 0) {
            failed = 1;
            break;
        }
    }

    // Second huge page before hole
    if (!failed) {
        for (size_t i = second_hp_start; i < second_hp_start + munmap_offset; i++) {
            if (verify_page_pattern(base + i * PAGE_SIZE, i, PAGE_SIZE, "test3-hp2-before") != 0) {
                failed = 1;
                break;
            }
        }
    }

    // Second huge page after hole
    if (!failed) {
        for (size_t i = second_hp_start + munmap_offset + munmap_count;
             i < 2 * NUM_SMALL_PAGES; i++) {
            if (verify_page_pattern(base + i * PAGE_SIZE, i, PAGE_SIZE, "test3-hp2-after") != 0) {
                failed = 1;
                break;
            }
        }
    }

    munmap(addr, 6 * HUGE_SIZE);
    close(fd);
    unlink(path);

    if (failed) {
        printf("  TEST 3: FAILED\n");
        return -1;
    }

    printf("  TEST 3: PASSED\n");
    return 0;
}


// Test 5: Sequential munmaps on same huge page
static int test_sequential_munmaps(void) {
    printf("\n[TEST 5] Sequential munmaps on same huge page\n");

    const char *path = "/tmp/file_split_test5.bin";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) die("open");

    if (ftruncate(fd, 4 * HUGE_SIZE) != 0) die("ftruncate");

    void *addr = mmap(NULL, 4 * HUGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) die("mmap");

    uint8_t *base = (uint8_t *)addr;

    // Write patterns
    for (size_t i = 0; i < NUM_SMALL_PAGES; i++) {
        fill_page_pattern(base + i * PAGE_SIZE, i, PAGE_SIZE);
    }

    // Collapse
    if (madvise(base, HUGE_SIZE * 2, MADV_COLLAPSE) != 0) {
        int e = errno;
        if (e == EINVAL || e == ENOTSUP) {
            printf("  MADV_COLLAPSE not supported, skipping test\n");
            munmap(addr, 4 * HUGE_SIZE);
            close(fd);
            unlink(path);
            return 0;
        }
        die("madvise MADV_COLLAPSE");
    }
    printf("  Huge page created\n");

    // First munmap (will split)
    printf("  First munmap: pages 100-119\n");
    if (munmap(base + 100 * PAGE_SIZE, 20 * PAGE_SIZE) != 0) {
        perror("first munmap");
        munmap(addr, 4 * HUGE_SIZE);
        close(fd);
        unlink(path);
        return -1;
    }

    // Second munmap (huge page already split)
    printf("  Second munmap: pages 200-219\n");
    if (munmap(base + 200 * PAGE_SIZE, 20 * PAGE_SIZE) != 0) {
        perror("second munmap");
        munmap(addr, 4 * HUGE_SIZE);
        close(fd);
        unlink(path);
        return -1;
    }

    // Third munmap
    printf("  Third munmap: pages 400-419\n");
    if (munmap(base + 400 * PAGE_SIZE, 20 * PAGE_SIZE) != 0) {
        perror("third munmap");
        munmap(addr, 4 * HUGE_SIZE);
        close(fd);
        unlink(path);
        return -1;
    }

    printf("  All munmaps completed\n");

    munmap(addr, 4 * HUGE_SIZE);
    close(fd);
    unlink(path);

    printf("  TEST 5: PASSED\n");
    return 0;
}

int main(void) {
    printf("=== File Backend split_huge_page Test Suite ===\n");
    printf("Testing: arceos/modules/axfs-ng/src/highlevel/file.rs:774-794\n");
    printf("Method: Using munmap on partial regions to trigger split_huge_page\n");

    int failed = 0;

    if (test_munmap_middle() != 0) {
        failed = 1;
    }

    if (test_munmap_at_offset() != 0) {
        failed = 1;
    }

    if (test_multiple_huge_pages() != 0) {
        failed = 1;
    }

    if (test_sequential_munmaps() != 0) {
        failed = 1;
    }

    printf("\n===========================================\n");
    if (failed) {
        printf("RESULT: FAILED - Bug detected in split_huge_page implementation\n");
        return 1;
    } else {
        printf("RESULT: ALL TESTS PASSED\n");
        return 0;
    }
}
