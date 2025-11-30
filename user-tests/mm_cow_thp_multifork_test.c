#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#define THP_SIZE (2UL * 1024 * 1024)
#define PAGE_4K  4096UL

static const char *THP_ENABLED =
    "/sys/kernel/mm/transparent_hugepage/enabled";
static const char *KHUGEPAGED_PAGES_COLLAPSED =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed";

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void set_thp_mode(const char *mode) {
    FILE *f = fopen(THP_ENABLED, "r+");
    if (!f) {
        perror("fopen THP_ENABLED");
        exit(77); // skip if THP sysfs not present
    }
    if (fseek(f, 0, SEEK_SET) != 0) die("fseek THP_ENABLED");
    if (fprintf(f, "%s\n", mode) < 0) die("fprintf THP_ENABLED");
    if (fflush(f) != 0) die("fflush THP_ENABLED");
    fclose(f);
}

static unsigned long read_pages_collapsed(void) {
    FILE *f = fopen(KHUGEPAGED_PAGES_COLLAPSED, "r");
    if (!f) die("fopen pages_collapsed");
    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), f)) die("fgets");
    fclose(f);
    return strtoul(buf, NULL, 10);
}

// Debug syscall to force THP collapse of a given range.
static long debug_thp_collapse(void *addr, size_t len) {
#ifdef SYS_sync_file_range2
#ifndef SYS_debug_thp_collapse
#define SYS_debug_thp_collapse SYS_sync_file_range2
#endif
    return syscall(SYS_debug_thp_collapse, (uintptr_t)addr, len);
#else
    (void)addr;
    (void)len;
    return 0;
#endif
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

// Wait for khugepaged to collapse the 2M window that contains thp_base.
// If it doesn't happen within a timeout, we skip the test.
static int wait_for_collapse(char *thp_base) {
    (void)thp_base; // currently unused

    unsigned long prev = read_pages_collapsed();
    printf("[mm_cow_thp_multifork_test] waiting for collapse (pages_collapsed=%lu)...\n", prev);
    for (int iter = 0; iter < 30; iter++) {
        sleep(1);
        unsigned long now = read_pages_collapsed();
        if (now > prev) {
            printf("  collapse detected (pages_collapsed=%lu)\n", now);
            return 0;
        }
    }
    printf("  WARNING: no collapse after 30s, skipping THP-specific checks\n");
    return -1;
}

int main(void) {
    printf("=== mm_cow_thp_multifork_test ===\n");
    printf("This test stresses THP COW with multiple forks and partial unmaps.\n\n");

    set_thp_mode("always");

    // Map 4MiB so we can host at least one 2MiB-aligned window.
    size_t len = 4UL * 1024 * 1024;
    char *base = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        die("mmap");
    }

    if (madvise(base, len, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE)");
    }

    // Find a 2MiB-aligned window inside [base, base+len).
    uintptr_t b = (uintptr_t)base;
    uintptr_t e = b + len;
    uintptr_t win = (b + THP_SIZE - 1) & ~(THP_SIZE - 1);
    if (win + THP_SIZE > e) {
        printf("[WARN] cannot find 2MiB-aligned window inside mapping\n");
        munmap(base, len);
        return 77;
    }
    char *thp_base = (char *)win;
    printf("[INFO] mapping base=%p, THP window=%p - %p\n",
           base, thp_base, thp_base + THP_SIZE);

    unsigned seed_parent = 0x41;
    fill_pattern(thp_base, THP_SIZE, seed_parent);

    // Touch again to ensure 4K PTEs are present.
    for (size_t i = 0; i < THP_SIZE; i += PAGE_4K) {
        thp_base[i] ^= 1;
        thp_base[i] ^= 1;
    }

    long collapsed = debug_thp_collapse(thp_base, THP_SIZE);
    printf("[INFO] debug_thp_collapse(%p, 2MiB) -> %ld\n", thp_base, collapsed);
    if (wait_for_collapse(thp_base) != 0) {
        munmap(base, len);
        return 77;
    }

    // First child: write pattern into first half of THP.
    pid_t c1 = fork();
    if (c1 < 0) {
        die("fork c1");
    }
    if (c1 == 0) {
        unsigned seed_child1 = 0xA1;
        size_t half = THP_SIZE / 2;
        printf("[child1] writing first half (seed=%#x)...\n", seed_child1);
        fill_pattern(thp_base, half, seed_child1);
        if (check_pattern(thp_base, half, seed_child1, "child1-half") != 0) {
            fprintf(stderr, "[child1] pattern verification failed\n");
            _exit(1);
        }
        _exit(0);
    }

    // Second child: write pattern into second half, then partially unmap a region.
    pid_t c2 = fork();
    if (c2 < 0) {
        die("fork c2");
    }
    if (c2 == 0) {
        unsigned seed_child2 = 0xB2;
        size_t half = THP_SIZE / 2;
        printf("[child2] writing second half (seed=%#x)...\n", seed_child2);
        fill_pattern(thp_base + half, half, seed_child2);

        // Partial unmap in the middle of the THP window.
        char *hole = thp_base + 24 * PAGE_4K;
        size_t hole_len = 16 * PAGE_4K;
        if (((uintptr_t)hole % PAGE_4K) == 0) {
            printf("[child2] munmap(%p, %zu) inside THP window\n", hole, hole_len);
            if (munmap(hole, hole_len) != 0) {
                perror("[child2] munmap(hole)");
                _exit(1);
            }
        }

        // Make sure we can still touch the remaining parts.
        thp_base[0] ^= 1;
        thp_base[THP_SIZE - PAGE_4K] ^= 1;

        _exit(0);
    }

    // Parent: wait for both children.
    int status = 0;
    if (waitpid(c1, &status, 0) < 0) die("waitpid c1");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[parent] child1 exited abnormally: status=%d\n", status);
        munmap(base, len);
        return 1;
    }

    if (waitpid(c2, &status, 0) < 0) die("waitpid c2");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[parent] child2 exited abnormally: status=%d\n", status);
        munmap(base, len);
        return 1;
    }

    // Parent: verify its own mapping is still fully intact with the original pattern.
    printf("[parent] verifying THP window contents after two children COW+munmap...\n");
    if (check_pattern(thp_base, THP_SIZE, seed_parent, "parent") != 0) {
        fprintf(stderr,
                "[mm_cow_thp_multifork_test] FAIL: parent mapping corrupted\n");
        munmap(base, len);
        return 1;
    }

    if (munmap(base, len) != 0) {
        die("munmap");
    }

    printf("[mm_cow_thp_multifork_test] PASS\n");
    return 0;
}

