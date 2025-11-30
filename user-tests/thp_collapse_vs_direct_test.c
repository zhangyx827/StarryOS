#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#ifndef MAP_HUGETLB
// Linux/glibc standard value; our kernel maps MAP_HUGETLB -> MmapFlags::HUGE.
#define MAP_HUGETLB 0x40000
#endif

#define THP_SIZE (2UL * 1024 * 1024)
#define PAGE_4K  (4096UL)

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
    if (!f) die("fopen THP_ENABLED");
    if (fseek(f, 0, SEEK_SET) != 0) die("fseek");
    if (fprintf(f, "%s\n", mode) < 0) die("fprintf");
    if (fflush(f) != 0) die("fflush");
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

// Optional debug syscall to force collapse; see thp_debug_collapse_cow_test.c
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

static int verify_pattern(char *p, size_t len, unsigned seed, const char *who) {
    for (size_t i = 0; i < len; i += PAGE_4K) {
        char expected = (char)(((i / PAGE_4K) ^ seed) & 0xFF);
        if (p[i] != expected) {
            fprintf(stderr,
                    "[%s] pattern mismatch at offset %#zx: got %#x, expected %#x\n",
                    who, i, (unsigned char)p[i], (unsigned char)expected);
            return -1;
        }
    }
    return 0;
}

// Common scenario: fork + child COW writes + parent verifies its view is intact.
static int do_fork_cow(char *base, size_t len, unsigned seed, const char *tag) {
    printf("[%s] fork + COW test...\n", tag);

    fill_pattern(base, len, seed);

    pid_t pid = fork();
    if (pid < 0) die("fork");

    if (pid == 0) {
        // Child: write the first 32 pages to trigger COW (if sharing).
        size_t max = PAGE_4K * 32;
        if (max > len) max = len;
        printf("[child %s] writing first %zu bytes...\n", tag, max);
        for (size_t i = 0; i < max; i += PAGE_4K) {
            base[i] = (char)0xA5;
        }
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) die("waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[%s] child exited abnormally: status=%d\n", tag, status);
        return -1;
    }

    // Parent: verify自己的 view 仍然是原始 pattern。
    if (verify_pattern(base, len, seed, tag) != 0) {
        fprintf(stderr, "[%s] COW isolation failed\n", tag);
        return -1;
    }

    printf("[%s] COW isolation OK\n", tag);
    return 0;
}

// Partial munmap hole inside one THP: basic split+refcount check.
static int do_partial_munmap(char *base, const char *tag) {
    printf("[%s] partial munmap test...\n", tag);

    // Hole: pages [10, 14]
    char *hole = base + PAGE_4K * 10;
    size_t hole_len = PAGE_4K * 5;

    printf("[%s] munmap hole %p len=%zu\n", tag, hole, hole_len);
    if (munmap(hole, hole_len) != 0) {
        perror("munmap hole");
        return -1;
    }

    // Check before-hole and after-hole still readable.
    volatile char val;
    val = base[0];
    printf("[%s] page 0 before hole: %d\n", tag, val);

    val = base[PAGE_4K * 9];
    printf("[%s] page 9 before hole: %d\n", tag, val);

    printf("[%s] pages 10-14 should be unmapped (expect fault if accessed)\n", tag);

    val = base[PAGE_4K * 15];
    printf("[%s] page 15 after hole: %d\n", tag, val);

    val = base[PAGE_4K * 511];
    printf("[%s] page 511 (last): %d\n", tag, val);

    return 0;
}

// Case 1: mapping created as 2MiB hugepage from the beginning (MAP_HUGETLB).
static int case_direct_huge(void) {
    printf("=== [Case 1] Direct 2MiB huge mapping via MAP_HUGETLB ===\n");

    char *p = mmap(NULL, THP_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap MAP_HUGETLB");
        printf("  NOTE: MAP_HUGETLB may not be supported; treating as failure.\n");
        return -1;
    }
    printf("[Case 1] mapping at %p len=%zu (2MiB huge)\n", p, (size_t)THP_SIZE);

    int ret = 0;

    // 1) fork + COW 测试
    if (do_fork_cow(p, THP_SIZE, 0x11, "case1-direct") != 0) {
        ret = -1;
        goto out;
    }

    // 2) 在这个 THP 内部做一次部分 munmap，测试 split + refcount。
    if (do_partial_munmap(p, "case1-direct") != 0) {
        ret = -1;
        goto out;
    }

out:
    if (munmap(p, THP_SIZE) != 0) {
        perror("munmap case1");
        ret = -1;
    }
    return ret;
}

// Wait for khugepaged or debug syscall to collapse this 2MiB range.
static void wait_for_collapse(char *base) {
    unsigned long prev = read_pages_collapsed();
    printf("  [collapse] pages_collapsed initial = %lu\n", prev);

    // Try debug collapse first (if wired up).
    long dbg = debug_thp_collapse(base, THP_SIZE);
    printf("  debug_thp_collapse(%p, 2M) -> %ld\n", base, dbg);

    for (int iter = 0; iter < 30; iter++) {
        sleep(1);
        ((volatile char *)base)[0] ^= 1;
        unsigned long now = read_pages_collapsed();
        if (now > prev) {
            printf("  collapse detected: %lu -> %lu\n", prev, now);
            return;
        }
    }
    printf("  WARNING: THP collapse not observed after 30s\n");
}

// Case 2: start from 4K anon mapping + MADV_HUGEPAGE, then let collapse/THP形成。
static int case_collapse_huge(void) {
    printf("=== [Case 2] 2MiB mapping obtained via collapse (THP) ===\n");

    char *p = mmap(NULL, THP_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap anon");
    }
    printf("[Case 2] mapping at %p len=%zu\n", p, (size_t)THP_SIZE);

    if (madvise(p, THP_SIZE, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE)");
    }

    // 先按 4K touch 一遍，建立 PTE。
    fill_pattern(p, THP_SIZE, 0x22);

    // 等待 khugepaged 把这一段折叠成 THP（或者 debug syscall 强行触发）。
    wait_for_collapse(p);

    int ret = 0;

    // 1) fork + COW 测试
    if (do_fork_cow(p, THP_SIZE, 0x22, "case2-collapse") != 0) {
        ret = -1;
        goto out;
    }

    // 2) 部分 munmap 测试 split + refcount
    if (do_partial_munmap(p, "case2-collapse") != 0) {
        ret = -1;
        goto out;
    }

out:
    if (munmap(p, THP_SIZE) != 0) {
        perror("munmap case2");
        ret = -1;
    }
    return ret;
}

int main(void) {
    printf("=== thp_collapse_vs_direct_test: compare direct vs collapsed 2MiB THPs ===\n");
    set_thp_mode("always");

    int failed = 0;

    if (case_direct_huge() != 0) {
        printf("[Case 1] FAILED\n");
        failed = 1;
    } else {
        printf("[Case 1] PASSED\n");
    }

    if (case_collapse_huge() != 0) {
        printf("[Case 2] FAILED\n");
        failed = 1;
    } else {
        printf("[Case 2] PASSED\n");
    }

    printf("Overall result: %s\n", failed ? "FAILED" : "ALL CASES PASSED");
    return failed ? 1 : 0;
}

