#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

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

    if (fseek(f, 0, SEEK_SET) != 0) die("fseek write");
    if (fprintf(f, "%s\n", mode) < 0) die("fprintf");
    if (fflush(f) != 0) die("fflush");
    fclose(f);
}

static unsigned long read_pages_collapsed(void) {
    FILE *f = fopen(KHUGEPAGED_PAGES_COLLAPSED, "r");
    if (!f) die("fopen pages_collapsed");

    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), f)) {
        die("fgets pages_collapsed");
    }
    fclose(f);
    return strtoul(buf, NULL, 10);
}

int main(void) {
    const size_t len = 4UL * 1024 * 1024; // 4 MiB
    const size_t page = 4096;

    printf("[thp_fork_cow_split_test] set THP mode to 'always'\n");
    set_thp_mode("always");

    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap");
    }
    printf("[thp_fork_cow_split_test] mapping at %p len=%zu\n", p, len);

    // 初始化父进程的内容：每页一个简单模式。
    for (size_t i = 0; i < len; i += page) {
        p[i] = (char)(i / page);
    }

    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE)");
    }

    // 再次 touch，保证 PTE 全部建立。
    for (size_t i = 0; i < len; i += page) {
        p[i] ^= 1;
        p[i] ^= 1;
    }

    // 等 khugepaged 尝试 collapse（如果没 collapse，本测试也应通过）。
    unsigned long prev = read_pages_collapsed();
    for (int sec = 0; sec < 10; sec++) {
        sleep(1);
        unsigned long now = read_pages_collapsed();
        if (now > prev) {
            printf("  pages_collapsed: %lu -> %lu\n", prev, now);
            prev = now;
            break;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        die("fork");
    }

    if (pid == 0) {
        // 子进程：在前 2MiB 范围内写每个 4K 页，触发 COW。
        printf("[child] writing first 2MiB to trigger COW...\n");
        size_t thp_len = 2UL * 1024 * 1024;
        if (thp_len > len) thp_len = len;

        for (size_t i = 0; i < thp_len; i += page) {
            p[i] = (char)0xAA;
        }
        _exit(0);
    }

    // 父进程：等待子进程结束，然后验证自己的映射没被改坏。
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        die("waitpid");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[parent] child exited abnormally: status=%d\n", status);
        exit(1);
    }

    printf("[parent] verifying COW isolation...\n");
    for (size_t i = 0; i < len; i += page) {
        char expected = (char)((i / page) ^ 1 ^ 1); // 原始写入 + 两次 XOR
        if (p[i] != expected) {
            fprintf(stderr,
                    "[parent] COW violation at offset %#zx: got %#x, expected %#x\n",
                    i, (unsigned char)p[i], (unsigned char)expected);
            exit(1);
        }
    }

    printf("[thp_fork_cow_split_test] PASS: child writes did not corrupt parent pages\n");

    if (munmap(p, len) != 0) {
        die("munmap");
    }

    return 0;
}

