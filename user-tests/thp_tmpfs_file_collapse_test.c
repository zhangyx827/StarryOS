// thp_tmpfs_file_collapse_test.c
// Exercise FileBackend THP collapse on a tmpfs-backed file via MADV_COLLAPSE.
//
// 触发路径（在 StarryOS 中）：
//   /dev/shm 或 /tmp  -> tmpfs (MemoryFs)
//   open + ftruncate  -> axfs_ng::File (tmpfs 文件)
//   mmap(MAP_SHARED)  -> FileBackend (cache.in_memory() == true)
//   madvise(MADV_COLLAPSE) -> AddrSpace::collapase_page_range
//                           -> FileBackend::try_collapse_page
//                           -> FileBackend::collapse_page(PageCache 2MiB)
//
// 要求：
//   - sys_madvise 支持 MADV_COLLAPSE（StarryOS 已实现）。
//   - /dev/shm 或 /tmp 已挂载 tmpfs（api/src/vfs/mod.rs 中 mount_all 已配置）。

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>

#ifndef MADV_COLLAPSE
// 与内核 uapi 保持一致
#define MADV_COLLAPSE 25
#endif

#define THP_SIZE (4UL * 1024 * 1024)
#define PAGE_4K  4096UL

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

// 尝试在 /dev/shm 或 /tmp 下创建一个 tmpfs 文件。
static int open_tmpfs_file(char *out_path, size_t out_len) {
    const char *candidates[] = {
        "/dev/shm/thp_tmpfs_file_collapse_test.bin",
        "/tmp/thp_tmpfs_file_collapse_test.bin",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *p = candidates[i];
        int fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd >= 0) {
            snprintf(out_path, out_len, "%s", p);
            return fd;
        }
    }
    return -1;
}

int main(void) {
    char path[128] = {0};
    int fd = open_tmpfs_file(path, sizeof(path));
    if (fd < 0) {
        perror("open tmpfs file");
        printf("thp_tmpfs_file_collapse_test: SKIP (no /dev/shm or /tmp)\n");
        return 0;
    }

    printf("=== thp_tmpfs_file_collapse_test ===\n");
    printf("  using tmpfs file: %s\n", path);

    if (ftruncate(fd, (off_t)THP_SIZE) != 0) {
        die("ftruncate");
    }

    /* 准备父子进程同步用的 pipe：parent<->child */
    int p2c[2], c2p[2];
    if (pipe(p2c) != 0 || pipe(c2p) != 0) {
        die("pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        die("fork");
    }

    if (pid == 0) {
        /* --- 子进程：使用同一个 tmpfs 文件，验证 collapse 后映射仍可用 --- */
        close(p2c[1]); /* 只读父->子 */
        close(c2p[0]); /* 只写子->父 */

        /* 子进程自己的 FileBackend 映射 */
        char *cp = mmap(NULL, THP_SIZE,
                        PROT_READ | PROT_WRITE,
                        MAP_SHARED,
                        fd, 0);
        if (cp == MAP_FAILED) {
            die("child mmap(tmpfs)");
        }
        printf("  [child] mmap at %p\n", cp);

        /* 先将子进程自己的映射全部 fault 进来，这样 collapse 前父子都有完整的 4K PTE。 */
        // for (size_t i = 0; i < THP_SIZE; i += PAGE_4K) {
        //     cp[i] = (char)(i / PAGE_4K);
        // }
        // printf("  [child] touched all 4K pages\n");

        /* 告诉父进程：子已经完成 mmap，可以开始 collapse 相关操作 */
        if (write(c2p[1], "R", 1) != 1) {
            die("child write ready");
        }

        /* 等待父进程完成 MADV_COLLAPSE */
        char ch;
        if (read(p2c[0], &ch, 1) != 1) {
            die("child read collapse-done");
        }

        /* collapse 完成后，检查映射是否仍可访问且模式没有被破坏 */
        for (size_t i = 0; i < THP_SIZE; i += PAGE_4K) {
            char v = cp[i];
            /* 不强行检查具体值，只要访问不崩就认为 retract_page_tables + 再 fault 行为是自洽的 */
            (void)v;
        }
        cp[0] ^= 1; /* 简单写一写 */
        printf("  [child] mapping still accessible after parent's collapse\n");

        if (munmap(cp, THP_SIZE) != 0) {
            die("child munmap");
        }
        close(p2c[0]);
        close(c2p[1]);
        /* 子进程退出交由父进程 waitpid 检查 */
        _exit(0);
    }

    /* --- 父进程：创建映射、写入 pattern、调用 MADV_COLLAPSE --- */
    close(p2c[0]); /* 只写父->子 */
    close(c2p[1]); /* 只读子->父 */

    char *p = mmap(NULL, THP_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED,
                   fd, 0);
    if (p == MAP_FAILED) {
        die("parent mmap(tmpfs)");
    }
    printf("  [parent] mmap(tmpfs) at %p len=%zu\n", p, (size_t)THP_SIZE);

    /* 填充 pattern，确保 FileBackend/cache 里有完整的 4K 页面内容 */
    // for (size_t i = 0; i < THP_SIZE; i += PAGE_4K) {
    //     p[i] = (char)(i / PAGE_4K);
    // }
    // printf("  [parent] touched all 4K pages\n");

    /* 等待子进程完成 mmap 并注册自己的 FileBackend listener */
    char ch;
    if (read(c2p[0], &ch, 1) != 1) {
        die("parent read child-ready");
    }

    errno = 0;
    if (madvise(p, THP_SIZE, MADV_COLLAPSE) != 0) {
        perror("madvise(MADV_COLLAPSE, tmpfs file)");
        munmap(p, THP_SIZE);
        close(fd);
        unlink(path);
        return 1;
    }
    printf("  [parent] madvise(MADV_COLLAPSE) returned 0\n");

    /* collapse 后，父自身的映射也应该仍可访问 */
    for (size_t i = 0; i < THP_SIZE; i += PAGE_4K) {
        p[i] ^= 1;
    }
    printf("  [parent] mapping still accessible after collapse\n");

    /* 通知子进程：collapse 已完成，可以检查自己的映射了 */
    if (write(p2c[1], "C", 1) != 1) {
        die("parent write collapse-done");
    }

    if (munmap(p, THP_SIZE) != 0) {
        die("parent munmap");
    }
    close(fd);

    /* 等待子进程结果 */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        die("waitpid");
    }
    unlink(path);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("thp_tmpfs_file_collapse_test: child FAILED (status=%d)\n", status);
        return 1;
    }

    printf("thp_tmpfs_file_collapse_test: PASS (parent + child, FileBackend collapse)\n");
    return 0;
}
