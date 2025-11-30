#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

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

int main(void) {
    const size_t len = 2UL * 1024 * 1024; // 2 MiB
    const size_t page = 4096;

    printf("[thp_debug_collapse_cow_test] mmap 2MiB anon region\n");
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap");
    }

    // Initialize content per page.
    for (size_t i = 0; i < len; i += page) {
        p[i] = (char)(i / page);
    }

    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE)");
    }

    // Touch again to ensure PTEs are present.
    for (size_t i = 0; i < len; i += page) {
        p[i] ^= 1;
        p[i] ^= 1;
    }

    printf("[thp_debug_collapse_cow_test] forcing THP collapse via debug syscall...\n");
    long collapsed = debug_thp_collapse(p, len);
    printf("  debug_thp_collapse -> %ld\n", collapsed);

    pid_t pid = fork();
    if (pid < 0) {
        die("fork");
    }

    if (pid == 0) {
        // Child: write first 16 pages to trigger COW.
        printf("[child] writing first 16 pages to trigger COW...\n");
        for (size_t i = 0; i < page * 16 && i < len; i += page) {
            p[i] = (char)0x7A;
        }
        _exit(0);
    }

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
        char expected = (char)(i / page);
        if (p[i] != expected) {
            fprintf(stderr,
                    "[parent] COW violation at offset %#zx: got %#x, expected %#x\n",
                    i, (unsigned char)p[i], (unsigned char)expected);
            exit(1);
        }
    }

    printf("[thp_debug_collapse_cow_test] PASS\n");

    if (munmap(p, len) != 0) {
        die("munmap");
    }

    return 0;
}
