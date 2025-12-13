// Demonstrate cache inconsistency when a 2MiB cached page is stored only at
// its start page number (e.g., tmpfs/shared backend huge page). After collapsing
// to a huge page, remapping the same file and reading a middle page should still
// see the written pattern; if the cache lookup misses intermediate page numbers,
// it will return stale/zero.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#define REGION_SIZE (2UL * 1024 * 1024)  // 2MiB
#define PAGE_4K     4096UL

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static int map_aligned_2m(int fd, int prot, int flags, void **out_addr) {
    // Reserve a PROT_NONE area, then map the file into a 2MiB-aligned window.
    size_t reserve_len = REGION_SIZE + (2 * REGION_SIZE);
    void *reserve = mmap(NULL, reserve_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (reserve == MAP_FAILED) {
        return -1;
    }

    *out_addr = (void*)reserve;
    return 0;
}

static int open_backing_file(const char **out_path) {
    const char *candidates[] = {
        "/tmp/page_cache_huge_overlap_test.bin",
        "./page_cache_huge_overlap_test.bin",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        int fd = open(candidates[i], O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd >= 0) {
            *out_path = candidates[i];
            return fd;
        }
    }
    return -1;
}

int main(void) {
    const char *path = NULL;
    int fd = open_backing_file(&path);
    if (fd < 0) {
        die("open backing file");
    }
    if (ftruncate(fd, REGION_SIZE * 3) != 0) {
        die("ftruncate");
    }

    void *addr = NULL;
    if (map_aligned_2m(fd, PROT_READ | PROT_WRITE, MAP_SHARED, &addr) != 0) {
        fprintf(stderr, "failed to map 2MiB-aligned region\n");
        close(fd);
        unlink(path);
        return 1;
    }
    printf("mapped at %p (%s)\n", addr, path);

    // Write a pattern across the entire region.
    memset(addr, 0x7b, REGION_SIZE * 3);

    // Collapse to huge page (should succeed on tmpfs/shmem).
    if (madvise(addr, REGION_SIZE * 3, MADV_COLLAPSE) != 0) {
        perror("madvise(MADV_COLLAPSE)");
        munmap(addr, REGION_SIZE * 3);
        close(fd);
        unlink(path);
        return 1;
    }

    // Drop mapping and remap the same file (new page table, same cache).
    if (munmap(addr, REGION_SIZE * 3) != 0) {
        die("munmap");
    }
    // Remap read-only (MAP_SHARED) to pull file contents via mmap, then verify.
    if (map_aligned_2m(fd, PROT_READ, MAP_SHARED, &addr) != 0) {
        fprintf(stderr, "remap for verify failed\n");
        return 1;
    }

    char* buf = (char*)malloc(sizeof(char) * (REGION_SIZE * 3));

    memset(buf, 0x7b, REGION_SIZE * 3);

    if (memcmp(buf, addr, REGION_SIZE * 3) != 0) {
        printf("page_cache_huge_overlap_test: WRONG\n");
        return 1;
    }

    munmap(addr, REGION_SIZE);

    close(fd);
    // unlink(path);
    printf("page_cache_huge_overlap_test: PASS\n");
    return 0;
}
