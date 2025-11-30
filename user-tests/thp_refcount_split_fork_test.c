#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#define THP_SIZE (2UL * 1024 * 1024)
#define PAGE_4K  (4096UL)

static const char *THP_ENABLED = "/sys/kernel/mm/transparent_hugepage/enabled";
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

// Wait for khugepaged to collapse the 2M window that contains thp_base.
// 如果超过一定时间还没 collapse，测试仍然继续，但会打印告警。
static void wait_for_collapse(char *thp_base) {
    unsigned long prev = read_pages_collapsed();
    printf("[thp_refcount_split_fork_test] waiting for collapse (pages_collapsed=%lu)...\n", prev);
    for (int iter = 0; iter < 30; iter++) {
        sleep(1);
        ((volatile char *)thp_base)[0] ^= 1;
        unsigned long now = read_pages_collapsed();
        if (now > prev) {
            printf("  collapse detected (pages_collapsed=%lu)\n", now);
            return;
        }
    }
    printf("  WARNING: no collapse after 30s, continuing anyway\n");
}

int main(void) {
    printf("[thp_refcount_split_fork_test] verifying split/unmap keeps other processes alive\n");

    set_thp_mode("always");

    // 分配 4MB，足够容纳一个 2M 对齐窗口。
    size_t len = 4UL * 1024 * 1024;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap");

    if (madvise(p, len, MADV_HUGEPAGE) != 0) die("madvise");

    // 找到 2M 对齐的窗口。
    uintptr_t base = (uintptr_t)p;
    uintptr_t end  = base + len;
    uintptr_t win  = (base + THP_SIZE - 1) & ~(THP_SIZE - 1);
    if (win + THP_SIZE > end) {
        printf("Cannot find 2M-aligned window inside mapping\n");
        munmap(p, len);
        return 1;
    }

    char *thp_base = (char *)win;
    printf("  mapping base=%p, THP window=%p - %p\n",
           p, thp_base, thp_base + THP_SIZE);

    // 初始化整个 2M 区域，每页一个简单模式。
    for (size_t i = 0; i < THP_SIZE; i += PAGE_4K) {
        thp_base[i] = (char)(i / PAGE_4K);
    }

    // 等待 khugepaged 尝试折叠成 THP。
    wait_for_collapse(thp_base);

    pid_t pid = fork();
    if (pid < 0) {
        die("fork");
    }

    if (pid == 0) {
        // 子进程：在 THP 中间做一次部分 munmap，触发 split_thp + 4K unmap。
        char *hole = thp_base + 10 * PAGE_4K;
        size_t hole_len = 5 * PAGE_4K;  // 5 页

        printf("[child] munmap(%p, %zu) inside THP window\n", hole, hole_len);
        if (munmap(hole, hole_len) != 0) {
            perror("[child] munmap");
            _exit(1);
        }

        // 再做一次小的写入，验证子进程自身仍然可访问其余部分。
        thp_base[0] ^= 1;
        thp_base[THP_SIZE - PAGE_4K] ^= 1;

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

    // 父进程：验证自己这份映射完全没被破坏：
    // 1) 不应该出现洞（即任何 4K 页仍然可读）；
    // 2) 每页内容仍然是最初写入的模式。
    printf("[parent] verifying THP contents after child munmap...\n");
    for (size_t i = 0; i < THP_SIZE; i += PAGE_4K) {
        volatile char val = thp_base[i];
        (void)val;  // 避免被优化掉

        char expected = (char)(i / PAGE_4K);
        if (thp_base[i] != expected) {
            fprintf(stderr,
                    "[parent] data corruption at offset %#zx: got %#x, expected %#x\n",
                    i, (unsigned char)thp_base[i], (unsigned char)expected);
            munmap(p, len);
            return 1;
        }
    }

    printf("[thp_refcount_split_fork_test] PASS: child split+munmap did not affect parent mapping\n");

    if (munmap(p, len) != 0) {
        die("munmap");
    }

    return 0;
}

