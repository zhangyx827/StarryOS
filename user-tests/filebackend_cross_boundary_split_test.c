// filebackend_cross_boundary_split_test.c
//
// Functional test for FileBackend PMD split across a 2MiB boundary.
//
// Goal:
//   - Create a file-backed tmpfs mapping covering 4MiB (two 2MiB windows).
//   - Collapse both windows via madvise(MADV_COLLAPSE).
//   - munmap() a 16KiB range that straddles the 2MiB boundary:
//       [2MiB-8KiB, 2MiB+8KiB)
//     This should force splitting on both sides without corrupting neighbors.
//   - Remap the hole back from the same file offsets at the same VA and verify
//     the original per-page pattern is preserved.
//
// Build for RISC-V:
//   riscv64-linux-gnu-gcc -O2 -static filebackend_cross_boundary_split_test.c -o filebackend_cross_boundary_split_test
//
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/statfs.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

#define PAGE_4K 4096UL
#define THP_SIZE (2UL * 1024 * 1024)
#define LEN_4M (4UL * 1024 * 1024)

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static uintptr_t align_up(uintptr_t x, uintptr_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

static int open_tmpfs_file(char *out_path, size_t out_len) {
    const char *candidates[] = {
        "/dev/shm/filebackend_cross_boundary_split_test.bin",
        "/tmp/filebackend_cross_boundary_split_test.bin",
        "./filebackend_cross_boundary_split_test.bin",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *p = candidates[i];
        int fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 0) continue;
        struct statfs s;
        if (statfs(p, &s) == 0 && s.f_type == TMPFS_MAGIC) {
            snprintf(out_path, out_len, "%s", p);
            return fd;
        }
        close(fd);
        unlink(p);
    }
    return -1;
}

static void fill_pattern(uint8_t *base, size_t len) {
    for (size_t off = 0; off < len; off += PAGE_4K) {
        uint8_t v = (uint8_t)((off / PAGE_4K) & 0xFF);
        memset(base + off, v, PAGE_4K);
    }
}

static void check_page_header(uint8_t *base, size_t page_idx, const char *tag) {
    uint8_t expected = (uint8_t)(page_idx & 0xFF);
    uint8_t got = base[page_idx * PAGE_4K];
    if (got != expected) {
        fprintf(stderr, "FAIL: %s: page=%zu got=%#x expected=%#x\n", tag, page_idx, got, expected);
        exit(1);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== filebackend_cross_boundary_split_test ===\n");

    char path[128] = {0};
    int fd = open_tmpfs_file(path, sizeof(path));
    if (fd < 0) {
        perror("open tmpfs file");
        printf("filebackend_cross_boundary_split_test: SKIP (no tmpfs)\n");
        return 77;
    }
    if (ftruncate(fd, LEN_4M) != 0) die("ftruncate");

    // Reserve enough VA space to find a 2MiB-aligned window for a 4MiB mapping.
    size_t reserve_len = LEN_4M + THP_SIZE;
    void *reserve = mmap(NULL, reserve_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserve == MAP_FAILED) die("mmap reserve");

    uintptr_t base_u = align_up((uintptr_t)reserve, THP_SIZE);
    uint8_t *base = (uint8_t *)base_u;
    if ((void *)(base + LEN_4M) > (uint8_t *)reserve + reserve_len) {
        printf("filebackend_cross_boundary_split_test: SKIP (no aligned window)\n");
        munmap(reserve, reserve_len);
        close(fd);
        unlink(path);
        return 77;
    }

    void *map = mmap(base, LEN_4M, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (map == MAP_FAILED || map != base) die("mmap file");

    fill_pattern(base, LEN_4M);

    if (madvise(base, LEN_4M, MADV_COLLAPSE) != 0) {
        if (errno == ENOMEM) {
            printf("filebackend_cross_boundary_split_test: SKIP (ENOMEM on collapse)\n");
            munmap(base, LEN_4M);
            // Unmap remaining PROT_NONE parts.
            uintptr_t r0 = (uintptr_t)reserve;
            uintptr_t r1 = (uintptr_t)base;
            uintptr_t r2 = (uintptr_t)reserve + reserve_len;
            if (r1 > r0) munmap((void *)r0, r1 - r0);
            if (r1 + LEN_4M < r2) munmap((void *)(r1 + LEN_4M), r2 - (r1 + LEN_4M));
            close(fd);
            unlink(path);
            return 77;
        }
        die("madvise(MADV_COLLAPSE)");
    }

    // Create a 16KiB hole centered at the 2MiB boundary.
    const size_t hole_len = 4 * PAGE_4K;
    const size_t hole_off = THP_SIZE - 2 * PAGE_4K;
    uint8_t *hole = base + hole_off;

    // Neighbor checks before.
    check_page_header(base, (hole_off / PAGE_4K) - 1, "before neighbor-1");
    check_page_header(base, (hole_off / PAGE_4K) + 4, "before neighbor+4");

    if (munmap(hole, hole_len) != 0) die("munmap hole");

    // Remap the exact file offsets back at the same address.
    void *r = mmap(hole, hole_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)hole_off);
    if (r == MAP_FAILED || r != hole) die("mmap remap hole");

    // Validate the hole and some neighbors.
    for (size_t off = hole_off; off < hole_off + hole_len; off += PAGE_4K) {
        check_page_header(base, off / PAGE_4K, "hole remapped");
    }
    check_page_header(base, (hole_off / PAGE_4K) - 1, "after neighbor-1");
    check_page_header(base, (hole_off / PAGE_4K) + 4, "after neighbor+4");

    // Full unmap should now succeed (no holes).
    if (munmap(base, LEN_4M) != 0) die("munmap full mapping");

    // Unmap remaining PROT_NONE parts.
    uintptr_t r0 = (uintptr_t)reserve;
    uintptr_t r1 = (uintptr_t)base;
    uintptr_t r2 = (uintptr_t)reserve + reserve_len;
    if (r1 > r0) munmap((void *)r0, r1 - r0);
    if (r1 + LEN_4M < r2) munmap((void *)(r1 + LEN_4M), r2 - (r1 + LEN_4M));

    close(fd);
    unlink(path);
    printf("filebackend_cross_boundary_split_test: PASS\n");
    return 0;
}

