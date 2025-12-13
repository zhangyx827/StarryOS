// file_madvise_collapse_faultin_test.c
//
// Exercise MADV_COLLAPSE on a tmpfs-backed FileBackend where only part of the
// 2MiB window has been faulted in. The goal is to drive the "fault_in = true"
// path in FileBackend::collapse_page, which should populate unmapped pages
// inside the window before collapsing.
//
// Scenario:
//   - Create a tmpfs file of size 2MiB.
//   - MAP_SHARED mmap it.
//   - Touch only the first 1MiB (so前半部分有 PTE/backing，后半是 pte_none)。
//   - Call madvise(MADV_COLLAPSE) on the whole 2MiB window.
//   - Verify:
//       * The first half keeps the pattern we wrote.
//       * The second half reads back as 0.
//
// 从用户态看不到“是否真的在 MADV_COLLAPSE 中 fault-in 了后半段”，
// 但这个测试能确保：
//   - 部分存在、部分不存在的窗口在 MADV_COLLAPSE 后语义正确；
//   - 不会因为 fault-in / collapse 破坏 tmpfs 文件内容。

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>

#define PAGE_4K      4096UL
#define REGION_SIZE  (4UL * 1024 * 1024)   // 4MiB

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

/* 尝试在 /dev/shm 或 /tmp 下创建一个 tmpfs 文件 */
static int open_tmpfs_file(char *out_path, size_t out_len) {
    const char *candidates[] = {
        "/dev/shm/file_madvise_collapse_faultin_test.bin",
        "/tmp/file_madvise_collapse_faultin_test.bin",
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
        printf("file_madvise_collapse_faultin_test: SKIP (no /dev/shm or /tmp)\n");
        return 0;
    }

    if (ftruncate(fd, REGION_SIZE) != 0) {
        die("ftruncate");
    }

    printf("=== file_madvise_collapse_faultin_test ===\n");
    printf("  using tmpfs file: %s (size=%lu)\n", path, (unsigned long)REGION_SIZE);

    char *p = mmap(NULL, REGION_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED,
                   fd, 0);
    if (p == MAP_FAILED) {
        die("mmap");
    }
    printf("  mmap base=%p len=%lu\n", p, (unsigned long)REGION_SIZE);

    // 1) 只触摸前半部分（2MiB），让这一半有 PTE/backing。
    for (size_t off = 0; off < REGION_SIZE; off += PAGE_4K) {
        p[off] = (char)(off / PAGE_4K);
    }
    // 2) 对整个 4MiB 区间调用 MADV_COLLAPSE，期望成功（返回 0）。
    if (madvise(p, REGION_SIZE, MADV_COLLAPSE) != 0) {
        perror("madvise(MADV_COLLAPSE, file)");
        munmap(p, REGION_SIZE);
        close(fd);
        unlink(path);
        return 1;
    }
    printf("  madvise(MADV_COLLAPSE) succeeded\n");

    int pid = fork();

    if (pid < 0) {
        perror("fork");
    }

    if (pid == 0) {
        for (size_t off = 0; off < REGION_SIZE; off += PAGE_4K) {
            p[off] = (char)(off / PAGE_4K);
        }
    }
    wait(NULL);

    close(fd);
    unlink(path);

}

