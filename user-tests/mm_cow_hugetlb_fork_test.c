#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// 2MiB huge page size.
#define HUGEPAGE_SIZE (2UL * 1024 * 1024)
#define PAGE_4K       4096UL

// Fallback MAP_HUGETLB definition for toolchains that don't provide it.
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void fill_pattern(char *p, size_t len, unsigned seed) {
    for (size_t i = 0; i < len; i += PAGE_4K) {
        p[i] = (char)(((i / PAGE_4K) ^ seed) & 0xFF);
    }
}

static int check_pattern(char *p, size_t len, unsigned seed, const char *tag) {
    for (size_t i = 0; i < len; i += PAGE_4K) {
        char expected = (char)(((i / PAGE_4K) ^ seed) & 0xFF);
        if (p[i] != expected) {
            fprintf(stderr,
                    "[%s] mismatch at offset %#zx: got %#x, expected %#x\n",
                    tag, i, (unsigned char)p[i], (unsigned char)expected);
            return -1;
        }
    }
    return 0;
}

int main(void) {
    printf("=== mm_cow_hugetlb_fork_test ===\n");
    printf("This test verifies COW isolation for MAP_HUGETLB mappings\n");
    printf("across fork(): child writes must not corrupt parent.\n\n");

    size_t len = HUGEPAGE_SIZE;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                   -1, 0);
    if (p == MAP_FAILED) {
        printf("[WARN] mmap(MAP_HUGETLB) failed: %s\n", strerror(errno));
        printf("[WARN] MAP_HUGETLB may not be supported; skipping test.\n");
        return 77; // skip
    }

    printf("[INFO] huge mapping at %p, len=%zu\n", p, len);

    unsigned seed_parent = 0x23;
    fill_pattern(p, len, seed_parent);

    // Ensure the hugepage is fully populated.
    for (size_t i = 0; i < len; i += PAGE_4K) {
        volatile char x = p[i];
        (void)x;
    }

    pid_t pid = fork();
    if (pid < 0) {
        die("fork");
    }

    if (pid == 0) {
        // Child: overwrite the entire huge page with a different pattern.
        unsigned seed_child = 0xA5;
        printf("[child] writing new pattern (seed=%#x) into hugepage\n", seed_child);
        fill_pattern(p, len, seed_child);

        // Verify child sees its own pattern.
        if (check_pattern(p, len, seed_child, "child") != 0) {
            fprintf(stderr, "[child] pattern verification failed after write\n");
            _exit(1);
        }

        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        die("waitpid");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[parent] child exited abnormally: status=%d\n", status);
        munmap(p, len);
        return 1;
    }

    // Parent: verify its mapping still holds the original pattern.
    printf("[parent] verifying original pattern after child COW writes...\n");
    if (check_pattern(p, len, seed_parent, "parent") != 0) {
        fprintf(stderr,
                "[mm_cow_hugetlb_fork_test] FAIL: parent mapping corrupted by child\n");
        munmap(p, len);
        return 1;
    }

    if (munmap(p, len) != 0) {
        die("munmap");
    }

    printf("[mm_cow_hugetlb_fork_test] PASS\n");
    return 0;
}

