// thp_madvise_semantics_test.c
// More complete tests for madvise(MADV_HUGEPAGE / MADV_NOHUGEPAGE)
// according to the Linux man-pages semantics.
//
// 覆盖点：
//   1. 匿名映射上的 MADV_HUGEPAGE / MADV_NOHUGEPAGE 基本行为（不报错，可访问）。
//   2. shmem/tmpfs 映射上的 MADV_HUGEPAGE 不报错（如果 /dev/shm 不存在则跳过）。
//   3. file-backed 映射上的 MADV_HUGEPAGE 不报错（只读、可执行映射）。
//   4. 栈 VMA 上 MADV_HUGEPAGE 应失败（EINVAL），因为 VMA 是 stack。
//   5. NOHUGEPAGE 优先级：在同一段上 HUGEPAGE -> NOHUGEPAGE 之后，
//      再次 HUGEPAGE 不应崩溃，且映射仍可访问。
//
// 注：很多语义在 Linux 里是 "hint/eligibility"，madvise 本身并不一定
//     返回错误，这里主要检查不该出错的地方不报 EINVAL/EOPNOTSUPP。

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
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

static size_t page_size(void) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) {
        fprintf(stderr, "sysconf(_SC_PAGESIZE) failed\n");
        exit(1);
    }
    return (size_t)ps;
}

// 1. 匿名映射上的基本 HUGEPAGE/NOHUGEPAGE 测试
static int test_anon_basic(void) {
    size_t ps = page_size();
    size_t len = 4UL * 1024 * 1024; // 4 MiB

    printf("[thp_madvise_semantics] test_anon_basic: len=%zu\n", len);
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap(anon)");
    }

    // touch 一下，防止完全未访问的 corner case
    for (size_t i = 0; i < len; i += ps) {
        p[i] = (char)(i / ps);
    }

    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        perror("madvise(MADV_HUGEPAGE, anon)");
        munmap(p, len);
        return 1;
    }

    if (madvise(p, len, MADV_NOHUGEPAGE) != 0) {
        perror("madvise(MADV_NOHUGEPAGE, anon)");
        munmap(p, len);
        return 1;
    }

    // 再 HUGEPAGE 一次，不应该崩溃或 EINVAL
    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        perror("madvise(MADV_HUGEPAGE after NOHUGEPAGE, anon)");
        munmap(p, len);
        return 1;
    }

    // 映射仍可访问
    p[0] ^= 1;

    if (munmap(p, len) != 0) {
        die("munmap(anon)");
    }
    return 0;
}

// 2. shmem/tmpfs 映射上的 MADV_HUGEPAGE 不报错
static int test_shmem_basic(void) {
    const char *path = "/dev/shm/thp_madvise_semantics_test";
    size_t ps = page_size();
    size_t len = 4UL * 1024 * 1024;

    printf("[thp_madvise_semantics] test_shmem_basic: %s len=%zu\n", path, len);

    int fd = shm_open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("shm_open");
        // 如果 /dev/shm 不存在或 shm_open 不可用，视为跳过
        return 0;
    }
    if (ftruncate(fd, (off_t)len) != 0) {
        perror("ftruncate(shmem)");
        close(fd);
        shm_unlink(path);
        return 1;
    }

    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        die("mmap(shmem)");
    }

    for (size_t i = 0; i < len; i += ps) {
        p[i] = (char)(i / ps);
    }

    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        perror("madvise(MADV_HUGEPAGE, shmem)");
        munmap(p, len);
        close(fd);
        shm_unlink(path);
        return 1;
    }

    // 映射仍可访问
    p[0] ^= 1;

    if (munmap(p, len) != 0) {
        die("munmap(shmem)");
    }
    close(fd);
    shm_unlink(path);
    return 0;
}

// 3. file-backed 映射上的 MADV_HUGEPAGE 不报错（只读、可执行）
static int test_file_exec_basic(void) {
    const char *path = "/tmp/thp_madvise_semantics_test.bin";
    size_t ps = page_size();
    size_t len = 4UL * 1024 * 1024;

    printf("[thp_madvise_semantics] test_file_exec_basic: %s len=%zu\n", path, len);

    // 创建并填充文件
    int fdw = open(path, O_CREAT | O_TRUNC | O_RDWR, 0700);
    if (fdw < 0) {
        perror("open(tmpfile, O_RDWR)");
        // /tmp 不可写则跳过
        return 0;
    }
    if (ftruncate(fdw, (off_t)len) != 0) {
        perror("ftruncate(tmpfile)");
        close(fdw);
        unlink(path);
        return 1;
    }

    char buf[4096];
    memset(buf, 0xAA, sizeof(buf));
    if (write(fdw, buf, sizeof(buf)) < 0) {
        perror("write(tmpfile)");
        close(fdw);
        unlink(path);
        return 1;
    }
    close(fdw);

    int fdr = open(path, O_RDONLY);
    if (fdr < 0) {
        perror("open(tmpfile, O_RDONLY)");
        unlink(path);
        return 1;
    }

    char *p = mmap(NULL, len, PROT_READ | PROT_EXEC,
                   MAP_PRIVATE, fdr, 0);
    if (p == MAP_FAILED) {
        perror("mmap(file, PROT_EXEC)");
        close(fdr);
        unlink(path);
        return 1;
    }

    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        perror("madvise(MADV_HUGEPAGE, file-exec)");
        munmap(p, len);
        close(fdr);
        unlink(path);
        return 1;
    }

    // 尝试读一下，确保映射可用
    volatile char sink = 0;
    for (size_t i = 0; i < len; i += ps * 512) {
        sink ^= p[i];
    }
    (void)sink;

    if (munmap(p, len) != 0) {
        die("munmap(file)");
    }
    close(fdr);
    unlink(path);
    return 0;
}

// 4. 栈 VMA 上 MADV_HUGEPAGE 应失败（VMA 是 stack）
static int test_stack_hugepage_einval(void) {
    size_t ps = page_size();
    char local[8192];
    uintptr_t addr = (uintptr_t)local;
    addr &= ~(ps - 1);
    void *aligned = (void *)addr;

    printf("[thp_madvise_semantics] test_stack_hugepage_einval: aligned stack addr=%p\n",
           aligned);

    errno = 0;
    int ret = madvise(aligned, ps, MADV_HUGEPAGE);
    if (ret == 0) {
        fprintf(stderr,
                "madvise(MADV_HUGEPAGE, stack) unexpectedly succeeded\n");
        return 1;
    }
    if (errno != EINVAL) {
        fprintf(stderr,
                "madvise(MADV_HUGEPAGE, stack) errno=%d, expected EINVAL(%d)\n",
                errno, EINVAL);
        return 1;
    }
    return 0;
}

int main(void) {
    int rc = 0;

    rc |= test_anon_basic();
    rc |= test_shmem_basic();
    rc |= test_file_exec_basic();
    rc |= test_stack_hugepage_einval();

    if (rc == 0) {
        printf("[thp_madvise_semantics] PASS\n");
    } else {
        printf("[thp_madvise_semantics] FAIL (rc=%d)\n", rc);
    }
    return rc;
}

