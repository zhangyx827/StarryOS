#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_4K 4096UL
#define HUGEPAGE_SIZE (2UL * 1024 * 1024)

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void fill_pattern(char *p, size_t len) {
    for (size_t off = 0; off < len; off += PAGE_4K) {
        p[off] = (char)((off / PAGE_4K) & 0x7F);
    }
}

static int check_pattern(char *p, size_t len) {
    for (size_t off = 0; off < len; off += PAGE_4K) {
        unsigned char expected = (unsigned char)((off / PAGE_4K) & 0x7F);
        unsigned char got = (unsigned char)p[off];
        if (got != expected) {
            fprintf(stderr,
                    "[hugetlb_partial_mprotect_test] pattern mismatch at %zu: got %#x expected %#x\n",
                    off, got, expected);
            return -1;
        }
    }
    return 0;
}

static sigjmp_buf g_jmp;

static void fault_handler(int signo, siginfo_t *info, void *ctx) {
    (void)signo;
    (void)info;
    (void)ctx;
    siglongjmp(g_jmp, 1);
}

static void install_fault_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        die("sigaction(SIGSEGV)");
    }
    if (sigaction(SIGBUS, &sa, NULL) != 0) {
        die("sigaction(SIGBUS)");
    }
}

static int expect_write_fault(volatile char *addr) {
    if (sigsetjmp(g_jmp, 1) == 0) {
        *addr ^= 1;
        return 0; // no fault
    }
    return 1; // faulted
}

int main(void) {
    printf("=== hugetlb_partial_mprotect_test ===\n");

    install_fault_handlers();

    char *p = mmap(NULL, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        printf("hugetlb_partial_mprotect_test: SKIP (MAP_HUGETLB mmap failed: %s)\n",
               strerror(errno));
        return 77;
    }

    fill_pattern(p, HUGEPAGE_SIZE);
    if (check_pattern(p, HUGEPAGE_SIZE) != 0) {
        munmap(p, HUGEPAGE_SIZE);
        return 1;
    }

    // Try mprotect on a 4KiB-aligned subrange strictly inside a hugetlb hugepage.
    // Linux semantics: should fail with EINVAL (hugetlb mappings require huge-aligned ranges).
    errno = 0;
    char *hole = p + 512UL * 1024;
    size_t hole_len = 64UL * 1024;
    int ret = mprotect(hole, hole_len, PROT_READ);
    if (ret == 0) {
        fprintf(stderr,
            "[hugetlb_partial_mprotect_test] FAIL: partial mprotect unexpectedly succeeded\n");
            munmap(p, HUGEPAGE_SIZE);
            return 1;
        }
    if (errno != EINVAL) {
        fprintf(stderr,
            "[hugetlb_partial_mprotect_test] FAIL: expected errno=EINVAL, got errno=%d\n",
            errno);
            munmap(p, HUGEPAGE_SIZE);
            return 1;
    }
            
    // Mapping should remain writable after the failed mprotect.
    p[0] ^= 1;
    p[HUGEPAGE_SIZE - PAGE_4K] ^= 1;

    // Huge-aligned mprotect over the full hugepage should succeed.
    if (mprotect(p, HUGEPAGE_SIZE, PROT_READ) != 0) {
        perror("mprotect(full hugepage, PROT_READ)");
        munmap(p, HUGEPAGE_SIZE);
        return 1;
    }
    if (!expect_write_fault((volatile char *)p)) {
        fprintf(stderr,
                "[hugetlb_partial_mprotect_test] FAIL: write after PROT_READ did not fault\n");
        munmap(p, HUGEPAGE_SIZE);
        return 1;
    }

    // Restore to RW and verify writes succeed again.
    if (mprotect(p, HUGEPAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
        perror("mprotect(full hugepage, PROT_READ|PROT_WRITE)");
        munmap(p, HUGEPAGE_SIZE);
        return 1;
    }
    p[0] ^= 1;

    munmap(p, HUGEPAGE_SIZE);
    printf("hugetlb_partial_mprotect_test: PASS\n");
    return 0;
}

