// sharedbackend_split_unmap_stress_test.c
//
// Stress SharedBackend THP collapse + split on partial munmap.
//
// SharedBackend is used by MAP_SHARED|MAP_ANONYMOUS mappings (internal shmem/tmpfs).
// This test ensures that:
//   - madvise(MADV_COLLAPSE) can create a 2MiB PMD mapping on a 2MiB-aligned range.
//   - munmap() of a 4KiB subrange inside that PMD triggers a PMD split path and
//     does not corrupt data outside the hole.
//
// Build for RISC-V:
//   riscv64-linux-gnu-gcc -O2 -static sharedbackend_split_unmap_stress_test.c -o sharedbackend_split_unmap_stress_test
//
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#define PAGE_4K 4096UL
#define THP_SIZE (2UL * 1024 * 1024)

static sigjmp_buf jmpbuf;
static volatile sig_atomic_t got_segv = 0;

static void on_segv(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    (void)info;
    (void)ucontext;
    got_segv = 1;
    siglongjmp(jmpbuf, 1);
}

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static uintptr_t align_up(uintptr_t x, uintptr_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

static void install_segv_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        die("sigaction");
    }
}

static void fill_pattern(uint8_t *p, size_t len) {
    for (size_t off = 0; off < len; off += PAGE_4K) {
        uint8_t v = (uint8_t)((off / PAGE_4K) & 0xFF);
        memset(p + off, v, PAGE_4K);
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

static void unmap_fully_mapped_segments(uint8_t *base, size_t len, size_t hole_off) {
    // Unmap [base, base+hole_off) and [base+hole_off+4K, base+len) if non-empty.
    if (hole_off > 0) {
        if (munmap(base, hole_off) != 0) die("munmap prefix");
    }
    size_t suffix_off = hole_off + PAGE_4K;
    if (suffix_off < len) {
        if (munmap(base + suffix_off, len - suffix_off) != 0) die("munmap suffix");
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    install_segv_handler();
    printf("=== sharedbackend_split_unmap_stress_test ===\n");

    int iters = 2000;
    if (argc >= 2) {
        iters = atoi(argv[1]);
        if (iters <= 0) iters = 2000;
    }

    // Create a larger mapping and carve out a 2MiB-aligned window inside.
    const size_t map_len = THP_SIZE * 2;
    const size_t thp_pages = THP_SIZE / PAGE_4K;

    for (int i = 0; i < iters; i++) {
        uint8_t *p = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            printf("SKIP: mmap failed: %s\n", strerror(errno));
            return 77;
        }

        uintptr_t base_u = align_up((uintptr_t)p, THP_SIZE);
        uint8_t *base = (uint8_t *)base_u;
        if (base + THP_SIZE > p + map_len) {
            // Unlikely, but avoid depending on MAP_FIXED.
            munmap(p, map_len);
            printf("SKIP: cannot find 2MiB-aligned window\n");
            return 77;
        }

        fill_pattern(base, THP_SIZE);

        if (madvise(base, THP_SIZE, MADV_COLLAPSE) != 0) {
            if (errno == ENOMEM) {
                printf("SKIP: MADV_COLLAPSE returned ENOMEM\n");
                munmap(p, map_len);
                return 77;
            }
            fprintf(stderr, "FAIL: MADV_COLLAPSE failed: errno=%d\n", errno);
            munmap(p, map_len);
            return 1;
        }

        // Pick a pseudo-random 4KiB page inside the 2MiB window and unmap it.
        size_t page_idx = (size_t)((i * 2654435761u) % (unsigned)thp_pages);
        size_t hole_off = page_idx * PAGE_4K;

        check_page_header(base, page_idx, "before-unmap");

        if (munmap(base + hole_off, PAGE_4K) != 0) {
            die("munmap hole");
        }

        // Verify data outside the hole remains intact.
        check_page_header(base, (page_idx + 1) % thp_pages, "neighbor+1");
        check_page_header(base, (page_idx + 17) % thp_pages, "neighbor+17");

        // Optionally verify the hole is unmapped (SIGSEGV on read).
        if ((i % 50) == 0) {
            got_segv = 0;
            if (sigsetjmp(jmpbuf, 1) == 0) {
                volatile uint8_t v = base[hole_off];
                (void)v;
                fprintf(stderr, "FAIL: read inside hole unexpectedly succeeded (iter=%d)\n", i);
                unmap_fully_mapped_segments(base, THP_SIZE, hole_off);
                munmap(p, map_len - THP_SIZE); // best-effort cleanup for the rest
                return 1;
            }
            if (!got_segv) {
                fprintf(stderr, "FAIL: expected SIGSEGV for hole, but handler not triggered\n");
                unmap_fully_mapped_segments(base, THP_SIZE, hole_off);
                munmap(p, map_len - THP_SIZE);
                return 1;
            }
        }

        // Cleanup: avoid munmap() over partially unmapped ranges (StarryOS returns ENOMEM).
        // Unmap the 2MiB window except the hole (as two segments), plus the other half.
        unmap_fully_mapped_segments(base, THP_SIZE, hole_off);
        // Unmap the prefix region before the 2MiB window, if any, and the suffix after it.
        if (base > p) {
            munmap(p, (size_t)(base - p));
        }
        if (base + THP_SIZE < p + map_len) {
            munmap(base + THP_SIZE, (size_t)((p + map_len) - (base + THP_SIZE)));
        }

        if ((i % 200) == 0 && i != 0) {
            printf("  progress: %d iterations OK\n", i);
        }
    }

    printf("sharedbackend_split_unmap_stress_test: PASS (iters=%d)\n", iters);
    return 0;
}

