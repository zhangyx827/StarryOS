// filebackend_split_remap_stress_test.c
//
// Stress FileBackend THP collapse + split + remap.
//
// Targeted backend behaviors:
//   - FileBackend::collapse_page() replaces 512x4KiB with one 2MiB mapping.
//   - FileBackend::unmap() must correctly split a 2MiB PMD mapping when a
//     sub-4KiB range is unmapped, and keep cache/page indexing consistent.
//
// Test strategy:
//   1) Create a tmpfs-backed file (in-memory cache).
//   2) For each iteration:
//      - Create a fresh 2MiB-aligned mapping (single VMA) of the file.
//      - Fill each 4KiB page with a distinct byte pattern.
//      - madvise(MADV_COLLAPSE) to get a 2MiB PMD mapping.
//      - munmap() one 4KiB subpage (forces PMD split in backend unmap path).
//      - mmap(MAP_FIXED) that 4KiB page back from the same file offset.
//      - Verify the remapped page and a few neighbors.
//
// Rationale:
//   After the first partial unmap, the address space areas may remain split and
//   won't necessarily be merged automatically. By recreating a single-VMA
//   mapping every iteration, we ensure each munmap() continues to exercise the
//   "split PMD huge mapping" path.
//
// Note:
//   StarryOS munmap() currently returns ENOMEM if the range is not fully mapped.
//   Avoid "munmap(reserve_len)" style cleanup that includes already-unmapped
//   holes; explicitly unmap only the ranges that are still mapped.
//
// Build for RISC-V:
//   riscv64-linux-gnu-gcc -O2 -static filebackend_split_remap_stress_test.c -o filebackend_split_remap_stress_test
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
#include <sys/types.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define PAGE_4K 4096UL
#define THP_SIZE (2UL * 1024 * 1024)
#define FILE_LEN THP_SIZE

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static uintptr_t align_up(uintptr_t x, uintptr_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

static int open_tmpfs_file(char *out_path, size_t out_len) {
    const char *candidates[] = {
        "/dev/shm/filebackend_split_remap_stress_test.bin",
        "/tmp/filebackend_split_remap_stress_test.bin",
        "./filebackend_split_remap_stress_test.bin",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *p = candidates[i];
        int fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 0) {
            continue;
        }
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

static void fill_pattern(uint8_t *p, size_t len) {
    for (size_t off = 0; off < len; off += PAGE_4K) {
        uint8_t v = (uint8_t)((off / PAGE_4K) & 0xFF);
        memset(p + off, v, PAGE_4K);
    }
}

static void check_page_byte(uint8_t *base, size_t page_idx, const char *tag) {
    uint8_t expected = (uint8_t)(page_idx & 0xFF);
    uint8_t got = base[page_idx * PAGE_4K];
    if (got != expected) {
        fprintf(stderr,
                "FAIL: %s: page=%zu got=%#x expected=%#x\n",
                tag, page_idx, got, expected);
        exit(1);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== filebackend_split_remap_stress_test ===\n");

    int iters = 2000;
    if (argc >= 2) {
        iters = atoi(argv[1]);
        if (iters <= 0) iters = 2000;
    }

    char path[128] = {0};
    int fd = open_tmpfs_file(path, sizeof(path));
    if (fd < 0) {
        perror("open tmpfs file");
        printf("filebackend_split_remap_stress_test: SKIP (no tmpfs path)\n");
        return 77;
    }
    if (ftruncate(fd, FILE_LEN) != 0) {
        die("ftruncate");
    }

    const size_t thp_pages = THP_SIZE / PAGE_4K;
    for (int i = 0; i < iters; i++) {
        // Reserve an address range and pick a 2MiB-aligned base for the mapping.
        // We re-create the mapping each iteration to avoid relying on VMA merge.
        size_t reserve_len = THP_SIZE * 3;
        void *reserve = mmap(NULL, reserve_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reserve == MAP_FAILED) {
            die("mmap reserve");
        }
        uintptr_t base_u = align_up((uintptr_t)reserve, THP_SIZE);
        uint8_t *base = (uint8_t *)base_u;
        if ((void *)(base + THP_SIZE) > (uint8_t *)reserve + reserve_len) {
            printf("filebackend_split_remap_stress_test: SKIP (cannot find aligned window)\n");
            munmap(reserve, reserve_len);
            close(fd);
            unlink(path);
            return 77;
        }

        // Map file at aligned address as a single VMA.
        void *map = mmap((void *)base, THP_SIZE, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_FIXED, fd, 0);
        if (map == MAP_FAILED || map != (void *)base) {
            die("mmap file MAP_FIXED");
        }

        // Ensure all 4KiB pages exist and have deterministic contents.
        fill_pattern(base, THP_SIZE);

        // Collapse into a 2MiB PMD mapping, then we'll force a split via munmap(4K).
        if (madvise(base, THP_SIZE, MADV_COLLAPSE) != 0) {
            // On very small-memory configs, collapse may fail; treat as skip to avoid flakes.
            if (errno == ENOMEM) {
                printf("SKIP: MADV_COLLAPSE returned ENOMEM\n");
                munmap(base, THP_SIZE);
                munmap(reserve, reserve_len);
                close(fd);
                unlink(path);
                return 77;
            }
            fprintf(stderr, "FAIL: MADV_COLLAPSE failed: errno=%d\n", errno);
            exit(1);
        }

        // Pick a pseudo-random page inside the first 2MiB.
        size_t page_idx = (size_t)((i * 1315423911u) % (unsigned)thp_pages);
        size_t off = page_idx * PAGE_4K;
        uint8_t *hole = base + off;

        // Sanity check before unmap.
        check_page_byte(base, page_idx, "before-unmap");

        if (munmap(hole, PAGE_4K) != 0) {
            die("munmap hole");
        }

        // Remap the 4KiB page back from the same file offset at the same VA.
        void *r = mmap(hole, PAGE_4K, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED, fd, (off_t)off);
        if (r == MAP_FAILED || r != (void *)hole) {
            die("mmap remap hole");
        }

        check_page_byte(base, page_idx, "after-remap");

        // Spot-check a few neighbor pages to catch indexing/corruption issues.
        check_page_byte(base, (page_idx + 1) % thp_pages, "neighbor+1");
        check_page_byte(base, (page_idx + 17) % thp_pages, "neighbor+17");

        // Drop the entire 2MiB mapping (and the reservation) so the next
        // iteration starts from a single-VMA mapping again.
        munmap(base, THP_SIZE);
        // Unmap only the still-mapped PROT_NONE parts of the reservation.
        uintptr_t reserve_u = (uintptr_t)reserve;
        uintptr_t base_u = (uintptr_t)base;
        uintptr_t reserve_end = reserve_u + reserve_len;
        if (base_u > reserve_u) {
            munmap((void *)reserve_u, base_u - reserve_u);
        }
        if (base_u + THP_SIZE < reserve_end) {
            munmap((void *)(base_u + THP_SIZE), reserve_end - (base_u + THP_SIZE));
        }

        if ((i % 200) == 0 && i != 0) {
            printf("  progress: %d iterations OK\n", i);
        }
    }

    close(fd);
    unlink(path);

    printf("filebackend_split_remap_stress_test: PASS (iters=%d)\n", iters);
    return 0;
}
