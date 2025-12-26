// cowbackend_cross_boundary_unmap_test.c
//
// Functional test for CowBackend PMD split across a 2MiB boundary.
//
// Goal:
//   - Create a private anonymous mapping covering 4MiB (two 2MiB windows).
//   - Collapse both windows via madvise(MADV_COLLAPSE).
//   - munmap() a 16KiB range straddling the 2MiB boundary to force PMD split.
//   - Verify neighbor pages outside the hole keep their pattern.
//   - Remap the hole back as anonymous MAP_FIXED and verify it is zero-filled,
//     catching stale data reuse bugs.
//
// Build for RISC-V:
//   riscv64-linux-gnu-gcc -O2 -static cowbackend_cross_boundary_unmap_test.c -o cowbackend_cross_boundary_unmap_test
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
#define LEN_4M (4UL * 1024 * 1024)

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

static void install_segv_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, NULL) != 0) die("sigaction");
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

static void expect_segv_on_read(uint8_t *addr, const char *tag) {
    got_segv = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        volatile uint8_t v = *addr;
        (void)v;
        fprintf(stderr, "FAIL: %s: read unexpectedly succeeded\n", tag);
        exit(1);
    }
    if (!got_segv) {
        fprintf(stderr, "FAIL: %s: expected SIGSEGV but handler not triggered\n", tag);
        exit(1);
    }
}

static void expect_zero_page(uint8_t *addr, const char *tag) {
    for (size_t i = 0; i < PAGE_4K; i += 256) {
        if (addr[i] != 0) {
            fprintf(stderr, "FAIL: %s: expected zero-filled page, got %#x at +%zu\n",
                    tag, addr[i], i);
            exit(1);
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    install_segv_handler();
    printf("=== cowbackend_cross_boundary_unmap_test ===\n");

    uint8_t *p = mmap(NULL, LEN_4M, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap");

    fill_pattern(p, LEN_4M);

    if (madvise(p, LEN_4M, MADV_COLLAPSE) != 0) {
        if (errno == ENOMEM) {
            printf("cowbackend_cross_boundary_unmap_test: SKIP (ENOMEM on collapse)\n");
            munmap(p, LEN_4M);
            return 77;
        }
        die("madvise(MADV_COLLAPSE)");
    }

    const size_t hole_len = 4 * PAGE_4K;
    const size_t hole_off = THP_SIZE - 2 * PAGE_4K;
    uint8_t *hole = p + hole_off;

    // Neighbors intact before unmap.
    check_page_header(p, (hole_off / PAGE_4K) - 1, "before neighbor-1");
    check_page_header(p, (hole_off / PAGE_4K) + 4, "before neighbor+4");

    if (munmap(hole, hole_len) != 0) die("munmap hole");

    // Touch inside hole should fault.
    expect_segv_on_read(hole, "hole read");

    // Neighbors intact after unmap.
    check_page_header(p, (hole_off / PAGE_4K) - 1, "after neighbor-1");
    check_page_header(p, (hole_off / PAGE_4K) + 4, "after neighbor+4");

    // Remap hole back as anonymous and expect zeros.
    void *r = mmap(hole, hole_len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (r == MAP_FAILED || r != hole) die("mmap remap hole");
    expect_zero_page(hole, "remapped hole page0");

    // Cleanup: unmap fully mapped segments to avoid ENOMEM from partially-unmapped ranges.
    if (munmap(p, hole_off) != 0) die("munmap prefix");
    if (munmap(hole, hole_len) != 0) die("munmap hole2");
    if (munmap(p + hole_off + hole_len, LEN_4M - (hole_off + hole_len)) != 0) die("munmap suffix");

    printf("cowbackend_cross_boundary_unmap_test: PASS\n");
    return 0;
}

