#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <stdint.h>
#include <unistd.h>

static const char *THP_ENABLED =
    "/sys/kernel/mm/transparent_hugepage/enabled";
static const char *KHUGEPAGED_PAGES_COLLAPSED =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed";

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

// Optional debug syscall to force THP collapse.
// On some toolchains/architectures SYS_sync_file_range2 is not defined;
// in that case we just make this a no-op and rely on khugepaged.
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

static void fill_pattern(char *p, size_t len, unsigned seed) {
    const size_t page = 4096;
    for (size_t i = 0; i < len; i += page) {
        p[i] = (char)(((i / page) ^ seed) & 0xFF);
    }
}

static int verify_pattern(char *p, size_t len, unsigned seed) {
    const size_t page = 4096;
    for (size_t i = 0; i < len; i += page) {
        char expected = (char)(((i / page) ^ seed) & 0xFF);
        if (p[i] != expected) {
            fprintf(stderr,
                    "[verify] mismatch at offset %#zx: got %#x, expected %#x\n",
                    i, (unsigned char)p[i], (unsigned char)expected);
            return -1;
        }
    }
    return 0;
}

int main(void) {
    const size_t len = 4UL * 1024 * 1024;     // 4 MiB, covers two 2M THP windows
    const size_t page = 4096;
    const int iterations = 200;               // 压力循环次数

    printf("[thp_cow_stress_test] set THP mode to 'always'\n");
    set_thp_mode("always");

    for (int it = 0; it < iterations; it++) {
        unsigned seed = (unsigned)it & 0xFF;

        printf("[iter %d] mmap len=%zu\n", it, len);
        char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            die("mmap");
        }

        if (madvise(p, len, MADV_HUGEPAGE) != 0) {
            die("madvise(MADV_HUGEPAGE)");
        }

        // 初始化父进程数据模式，逐页填充。
        fill_pattern(p, len, seed);

        // 再次 touch，确保 PTE 全部建立。
        for (size_t i = 0; i < len; i += page) {
            p[i] ^= 1;
            p[i] ^= 1;
        }

        // 通过 debug syscall + 观察 pages_collapsed 主动/被动触发 THP collapse。
        unsigned long prev = read_pages_collapsed();
        long collapsed = debug_thp_collapse(p, len);
        printf("  [iter %d] debug_thp_collapse -> %ld, pages_collapsed(before) = %lu\n",
               it, collapsed, prev);

        // 给 khugepaged 一点时间，同时保持映射“热”，方便折叠。
        for (int sec = 0; sec < 10; sec++) {
            usleep(100 * 1000); // 100ms
            // touch 若干页，避免被认为是冷页
            size_t off = (size_t)(sec * 16 * page) % len;
            p[off] ^= 1;
            p[off] ^= 1;

            unsigned long now = read_pages_collapsed();
            if (now > prev) {
                printf("    [iter %d] THP collapsed: pages_collapsed %lu -> %lu\n",
                       it, prev, now);
                break;
            }
        }

        pid_t pid = fork();
        if (pid < 0) {
            die("fork");
        }

        if (pid == 0) {
            // 子进程：对映射做各种写入/munmap，触发 COW + THP split。
            printf("[child %d] writing & unmapping to stress COW/THP...\n", it);

            // 1) 在第一个 2M 窗口里逐页写入，触发 2M THP 上的 COW。
            size_t thp_len = 2UL * 1024 * 1024;
            if (thp_len > len) thp_len = len;
            for (size_t i = 0; i < thp_len; i += page) {
                p[i] = (char)0xA5;
            }

            // 2) 在第二个 2M 窗口的中间做部分 munmap，触发 split_thp + unmap。
            if (len >= 2UL * 1024 * 1024 + 512UL * 1024 + 64UL * 1024) {
                char *hole = p + 2UL * 1024 * 1024 + 512UL * 1024;
                size_t hole_len = 64UL * 1024;

                // 对齐到 4K，避免 EINVAL。
                if (((uintptr_t)hole % page) == 0) {
                    if (munmap(hole, hole_len) != 0) {
                        perror("[child] munmap(hole)");
                    }
                }
            }

            _exit(0);
        }

        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            die("waitpid");
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr,
                    "[parent] child exited abnormally on iter %d: status=%d\n",
                    it, status);
            munmap(p, len);
            return 1;
        }

        // 父进程：验证整个区域的数据都仍然是自己的模式，
        // 检查：1）COW 是否隔离；2）子进程 munmap 是否错误影响父进程。
        printf("[parent] verifying mapping content on iter %d...\n", it);
        if (verify_pattern(p, len, seed) != 0) {
            fprintf(stderr,
                    "[thp_cow_stress_test] FAIL at iter %d: COW data corruption\n",
                    it);
            munmap(p, len);
            return 1;
        }

        if (munmap(p, len) != 0) {
            die("munmap");
        }
    }

    printf("[thp_cow_stress_test] PASS: %d iterations completed without COW/THP corruption\n",
           iterations);
    return 0;
}
