#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

#define PAGE_4K 4096UL
#define REGION_SIZE (4UL * 1024 * 1024)  // 2MiB window

static sigjmp_buf jmpbuf;
static volatile sig_atomic_t got_segv = 0;

static void on_segv(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    (void)info;
    (void)ucontext;
    got_segv = 1;
    siglongjmp(jmpbuf, 1);
}

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

/* Pick a tmpfs path so shared backend uses in-memory cache. */
static int open_tmpfs_file(char *out_path, size_t out_len) {
    const char *candidates[] = {
        "/tmp/shared_unmap_split_test.bin",
        "/dev/shm/shared_unmap_split_test.bin",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *p = candidates[i];
        int fd = open(p, O_CREAT | O_RDWR | O_TRUNC, 0600);
        if (fd < 0) {
            continue;
        }
        struct statfs s;
        if (statfs(p, &s) == 0 && s.f_type == TMPFS_MAGIC) {
            snprintf(out_path, out_len, "%s", p);
            return fd;
        }
        close(fd);
        unlink(p);
    }
    return -1;
}

static void install_segv_handler(void) {
    struct sigaction sa = {
        .sa_sigaction = on_segv,
        .sa_flags = SA_SIGINFO,
    };
    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        die("sigaction");
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    install_segv_handler();

    char path[128] = {0};
    char *p = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap");
    }
    printf("  mmap base=%p len=%lu\n", p, (unsigned long)REGION_SIZE);

    // Populate all 4K pages with a pattern so collapse can reuse them.
    for (size_t off = 0; off < REGION_SIZE; off += PAGE_4K) {
        p[off] = (char)(off / PAGE_4K);
    }

    if (madvise(p, REGION_SIZE, MADV_COLLAPSE) != 0) {
        perror("madvise(MADV_COLLAPSE)");
        munmap(p, REGION_SIZE);
        return 1;
    }
    printf("  madvise(MADV_COLLAPSE) succeeded\n");

    // Unmap a single 4K subrange in the middle; this should split any PMD mapping.
    size_t hole_off = REGION_SIZE / 2;
    if (munmap(p + hole_off, PAGE_4K) != 0) {
        die("munmap hole");
    }
    printf("  unmapped 4K hole at offset %zu\n", hole_off);

    // Access before and after the hole should still work and keep the pattern.
    for (size_t off = 0; off < REGION_SIZE; off += PAGE_4K) {
        if (off == hole_off) {
            continue;
        }
        unsigned char expected = (unsigned char)(off / PAGE_4K);
        if ((unsigned char)p[off] != expected) {
            fprintf(stderr,
                    "  mismatch outside hole at offset %zu: got %#x expected %#x\n",
                    off, (unsigned char)p[off], expected);
            munmap(p, REGION_SIZE - PAGE_4K); // best effort cleanup
            return 1;
        }
    }
    printf("  pattern intact outside the hole\n");

    // Access inside the unmapped hole should fault.
    got_segv = 0;
    if (sigsetjmp(jmpbuf, 1) == 0) {
        volatile char v = p[hole_off];
        (void)v;
        fprintf(stderr, "  read inside hole unexpectedly succeeded\n");
        munmap(p, REGION_SIZE - PAGE_4K);
        return 1;
    } else if (!got_segv) {
        fprintf(stderr, "  expected SIGSEGV when touching hole, but handler not triggered\n");
        munmap(p, REGION_SIZE - PAGE_4K);
        return 1;
    } else {
        printf("  touching hole raised SIGSEGV as expected\n");
    }

    // Remap the hole back and verify content can be read again (zeroed).
    void *remap = mmap(p + hole_off, PAGE_4K, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS, -1, hole_off);
    if (remap == MAP_FAILED) {
        die("remap hole");
    }
    if (remap != p + hole_off) {
        fprintf(stderr, "  MAP_FIXED remap returned unexpected addr %p\n", remap);
        munmap(p, REGION_SIZE - PAGE_4K);
        return 1;
    }
    printf("  remapped hole successfully\n");

    int rc = 0;
    for (size_t off = 0; off < REGION_SIZE; off += PAGE_4K) {
        unsigned char expected = (unsigned char)(off / PAGE_4K);
        if ((unsigned char)p[off] != expected) {
            fprintf(stderr,
                    "  mismatch after remap at offset %zu: got %#x expected %#x\n",
                    off, (unsigned char)p[off], expected);
            rc = 1;
            break;
        }
    }

    munmap(p, REGION_SIZE);

    if (rc == 0) {
        printf("shared_unmap_split_test: PASS\n");
    } else {
        printf("shared_unmap_split_test: FAIL\n");
    }
    return rc;
}
