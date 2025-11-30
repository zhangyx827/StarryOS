// Simple test for madvise(MADV_HUGEPAGE / MADV_NOHUGEPAGE) on anonymous memory
// Build for RISC-V:
//   riscv64-linux-gnu-gcc -O2 -static thp_madvise_test.c -o thp_madvise_test

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif
#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE 15
#endif

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

int main(void) {
    size_t len = 4 * 1024 * 1024; // 4 MiB
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap");
    }

    printf("Anonymous mapping at %p, length %zu\n", p, len);

    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE)");
    } else {
        printf("madvise(MADV_HUGEPAGE) succeeded\n");
    }

    for (size_t i = 0; i < len; i += 4096) {
        ((volatile char *)p)[i] = 1;
    }
    printf("Touched mapping to trigger page faults\n");

    if (madvise(p, len, MADV_NOHUGEPAGE) != 0) {
        die("madvise(MADV_NOHUGEPAGE)");
    } else {
        printf("madvise(MADV_NOHUGEPAGE) succeeded\n");
    }

    if (munmap(p, len) != 0) {
        die("munmap");
    }
    printf("thp_madvise_test: PASS\n");
    return 0;
}

