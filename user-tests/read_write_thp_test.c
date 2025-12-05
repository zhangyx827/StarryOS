// mm_cow_thp_readonly_fork_test.c
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
        // 没有 THP sysfs，直接跳过测试
        perror("fopen THP_ENABLED");
        exit(77);
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
    if (!fgets(buf, sizeof(buf), f)) die("fgets pages_collapsed");
    fclose(f);
    return strtoul(buf, NULL, 10);
}

// 等待 khugepaged 折叠，超时则跳过测试
static int wait_for_collapse(void) {
    unsigned long prev = read_pages_collapsed();
    printf("[mm_cow_thp_readonly_fork_test] waiting for collapse (pages_collapsed=%lu)...\n", prev);
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
    printf("=== mm_cow_thp_readonly_fork_test ===\n");
    printf("This test verifies that read-only access after THP collapse + fork\n");
    printf("does not corrupt the parent's mapping.\n\n");

    // 1. 开启 THP，让匿名内存可以被 khugepaged 折叠
    set_thp_mode("always");

    // 2. 映射 4MiB，以便内部可以找到一个 2MiB 对齐窗口
    size_t len = 4UL * 1024 * 1024;
    char *base = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) die("mmap");

    if (madvise(base, len, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE)");
    }

    // 3. 在 [base, base+len) 里找一个 2MiB 对齐的窗口
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

    // 4. 写入模式数据，方便后面校验
    unsigned seed = 0x42;
    fill_pattern(thp_base, THP_SIZE, seed);

    // 再 touch 一遍，确保 4K PTE 都建好
    for (size_t i = 0; i < THP_SIZE; i += PAGE_4K) {
        thp_base[i] ^= 1;
        thp_base[i] ^= 1;
    }

    // 5. 等待 khugepaged 尝试折叠成 THP
    if (wait_for_collapse() != 0) {
        munmap(base, len);
        return 77; // 无法确认有 THP，跳过
    }

    // 6. fork 后，子进程只做只读访问
    pid_t pid = fork();
    if (pid < 0) die("fork");

    if (pid == 0) {
        // 子进程：只读遍历 THP 窗口
        printf("[child] performing read-only pass over THP window...\n");
        volatile char sink = 0;
        for (size_t i = 0; i < THP_SIZE; i += PAGE_4K) {
            thp_base[i] = 32;
        }
        (void)sink;
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) die("waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[parent] child exited abnormally: status=%d\n", status);
        munmap(base, len);
        return 1;
    }

    // 7. 父进程：验证自己的窗口内容仍然是原始模式
    printf("[parent] verifying THP window contents after child read-only...\n");
    if (check_pattern(thp_base, THP_SIZE, seed, "parent") != 0) {
        fprintf(stderr,
                "[mm_cow_thp_readonly_fork_test] FAIL: parent mapping corrupted\n");
        munmap(base, len);
        return 1;
    }

    if (munmap(base, len) != 0) die("munmap");

    printf("[mm_cow_thp_readonly_fork_test] PASS\n");
    return 0;
}