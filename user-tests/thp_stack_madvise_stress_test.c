// thp_stack_madvise_stress_test.c
//
// Stress test for Linux-like madvise semantics on stack VMAs:
//   madvise(stack_range, ..., MADV_HUGEPAGE) must fail with EINVAL.
//
// This is important because THP should never be applied to the process stack by default.
//
// Build for RISC-V:
//   riscv64-linux-gnu-gcc -O2 -static thp_stack_madvise_stress_test.c -o thp_stack_madvise_stress_test
//
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static size_t page_size(void) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) die("sysconf(_SC_PAGESIZE)");
    return (size_t)ps;
}

static void *page_align_down(void *p, size_t ps) {
    return (void *)((uintptr_t)p & ~(uintptr_t)(ps - 1));
}

static void expect_stack_einval_once(void) {
    size_t ps = page_size();
    volatile char local[8192];
    void *aligned = page_align_down((void *)local, ps);

    errno = 0;
    int ret = madvise(aligned, ps, MADV_HUGEPAGE);
    if (ret == 0) {
        fprintf(stderr, "FAIL: madvise(MADV_HUGEPAGE) on stack unexpectedly succeeded (addr=%p)\n", aligned);
        exit(1);
    }
    if (errno != EINVAL) {
        fprintf(stderr, "FAIL: madvise(MADV_HUGEPAGE) on stack errno=%d, expected EINVAL(%d)\n", errno, EINVAL);
        exit(1);
    }
}

static void sanity_anon_should_work(void) {
    size_t len = 4UL * 1024 * 1024;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        // If anonymous mmap is unavailable in this env, treat as skip.
        printf("SKIP: anonymous mmap failed: %s\n", strerror(errno));
        exit(77);
    }
    p[0] = 1;
    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        fprintf(stderr, "FAIL: madvise(MADV_HUGEPAGE) on anon returned errno=%d\n", errno);
        munmap(p, len);
        exit(1);
    }
    munmap(p, len);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== thp_stack_madvise_stress_test ===\n");

    int iters = 200000;
    if (argc >= 2) {
        iters = atoi(argv[1]);
        if (iters <= 0) iters = 200000;
    }

    sanity_anon_should_work();

    for (int i = 0; i < iters; i++) {
        expect_stack_einval_once();
        if ((i % 50000) == 0 && i != 0) {
            printf("  progress: %d iterations OK\n", i);
        }
    }

    printf("thp_stack_madvise_stress_test: PASS (iters=%d)\n", iters);
    return 0;
}

