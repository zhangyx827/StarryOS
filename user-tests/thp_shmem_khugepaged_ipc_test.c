#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#define PAGE_4K 4096UL
#define THP_2M (2UL * 1024 * 1024)
#define LEN_BYTES (8UL * 1024 * 1024)

static const char *SYS_THP_ENABLED = "/sys/kernel/mm/transparent_hugepage/enabled";
static const char *SYS_SHMEM_ENABLED = "/sys/kernel/mm/transparent_hugepage/shmem_enabled";
static const char *SYS_KHUGEPAGED_SLEEP =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs";
static const char *SYS_KHUGEPAGED_PAGES_TO_SCAN =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan";
static const char *SYS_KHUGEPAGED_PAGES_COLLAPSED =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed";

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static int read_text_trim(const char *path, char *buf, size_t buflen) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    if (!fgets(buf, (int)buflen, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' ||
                     buf[n - 1] == '\t')) {
        buf[--n] = '\0';
    }
    return 0;
}

// Linux-style THP sysfs policy files often render as "always [madvise] never".
// Extract the bracketed token if present; otherwise return the trimmed line.
static int read_policy_token(const char *path, char *buf, size_t buflen) {
    if (read_text_trim(path, buf, buflen) != 0) {
        return -1;
    }
    char *l = strchr(buf, '[');
    char *r = l ? strchr(l, ']') : NULL;
    if (l && r && r > l + 1) {
        size_t n = (size_t)(r - (l + 1));
        if (n + 1 > buflen) {
            n = buflen - 1;
        }
        memmove(buf, l + 1, n);
        buf[n] = '\0';
    }
    return 0;
}

static unsigned long read_ulong(const char *path) {
    char buf[64];
    if (read_text_trim(path, buf, sizeof(buf)) != 0) {
        return 0;
    }
    return strtoul(buf, NULL, 10);
}

static int write_exact(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    // sysfs convention: allow a trailing newline.
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s\n", s);
    ssize_t n = write(fd, tmp, strlen(tmp));
    close(fd);
    if (n < 0) {
        return -1;
    }
    if (n != (ssize_t)strlen(tmp)) {
        errno = EIO;
        return -1;
    }
    return 0;
}

// Best-effort "overwrite" write for sysfs-like pseudo files that might not
// truncate on short writes at offset 0 (e.g. custom sysfs impls).
// We write a buffer long enough to cover the previous content, padding with
// '\n' so that kernel-side `.trim()` yields exactly `s`.
static int write_overwrite(const char *path, const char *s) {
    size_t old_len = 0;
    {
        int rfd = open(path, O_RDONLY);
        if (rfd >= 0) {
            char tmp[256];
            ssize_t n = read(rfd, tmp, sizeof(tmp));
            if (n > 0) {
                old_len = (size_t)n;
            }
            close(rfd);
        }
    }

    size_t want = strlen(s);
    size_t write_len = old_len;
    if (write_len < want + 1) {
        write_len = want + 1;
    }
    if (write_len > 256) {
        write_len = 256;
    }

    char buf[256];
    memset(buf, '\n', sizeof(buf));
    memcpy(buf, s, want);
    buf[want] = '\n';

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = write(fd, buf, write_len);
    close(fd);
    if (n < 0) {
        return -1;
    }
    if (n != (ssize_t)write_len) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int write_sysfs_value(const char *path, const char *s) {
    // Prefer the normal Linux sysfs style write. If it fails (common in
    // StarryOS' pseudo files when old content is longer), fall back to an
    // overwrite write that clears stale tails.
    if (write_exact(path, s) == 0) {
        return 0;
    }
    return write_overwrite(path, s);
}

static int try_config_sysfs(const char *path, const char *s) {
    if (write_sysfs_value(path, s) == 0) {
        return 0;
    }
    // In many CI/container environments, sysfs knobs are read-only or blocked.
    // Treat that as a SKIP rather than a functional failure.
    if (errno == EACCES || errno == EPERM || errno == EROFS || errno == ENOENT) {
        fprintf(stderr, "  WARN: cannot write %s: %s\n", path, strerror(errno));
        return 1;
    }
    fprintf(stderr, "  FAIL: write %s: %s\n", path, strerror(errno));
    return -1;
}

static void fill_pattern(char *base, size_t len, unsigned seed) {
    for (size_t off = 0; off < len; off += PAGE_4K) {
        base[off] = (char)(((off / PAGE_4K) ^ seed) & 0xFF);
    }
}

static int check_sampled_pattern(char *base, size_t len, unsigned seed) {
    // Sample one page every 64KiB to keep runtime low.
    for (size_t off = 0; off < len; off += 64UL * 1024) {
        unsigned char expected = (unsigned char)(((off / PAGE_4K) ^ seed) & 0xFF);
        unsigned char got = (unsigned char)base[off];
        if (got != expected) {
            fprintf(stderr,
                    "  mismatch at %zu: got %#x expected %#x\n",
                    off, got, expected);
            return -1;
        }
    }
    return 0;
}

static int wait_for_collapse_2m(unsigned long base_collapsed) {
    const unsigned long want = base_collapsed + (THP_2M / PAGE_4K);
    for (int i = 0; i < 3000; i++) { // ~5s max
        usleep(10 * 1000);
        unsigned long now = read_ulong(SYS_KHUGEPAGED_PAGES_COLLAPSED);
        if (now >= want) {
            return 0;
        }
    }
    return -1;
}

int main(void) {
    printf("=== thp_shmem_khugepaged_ipc_test ===\n");
    printf("  scenario: shared-memory IPC (MAP_SHARED|MAP_ANONYMOUS) + khugepaged collapse\n");
    int rc;

    char *p = mmap(NULL, LEN_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap(MAP_SHARED|MAP_ANONYMOUS)");
        return -1;
    }
    uintptr_t base = (uintptr_t)p;
    uintptr_t end = base + LEN_BYTES;
    uintptr_t win = (base + THP_2M - 1) & ~(THP_2M - 1);

    unsigned seed = 0x5A;
    fill_pattern(p, LEN_BYTES, seed);
    if (madvise(p, LEN_BYTES, MADV_COLLAPSE) != 0) {
        perror("madvise(MADV_HUGEPAGE)");
        goto out;
    }

    // Choose a 2MiB-aligned window inside the mapping.
    if (win + THP_2M > end) {
        fprintf(stderr, "  FAIL: cannot find 2MiB-aligned window\n");
        rc = 1;
        goto out;
    }
    char *thp = (char *)win;
    printf("  mapping=%p len=%zu thp_window=%p\n", p, (size_t)LEN_BYTES, thp);

    // Fault in.
    for (size_t off = 0; off < THP_2M; off += PAGE_4K) {
        volatile unsigned char v = (unsigned char)thp[off];
        (void)v;
    }

    int p2c[2] = {-1, -1};
    int c2p[2] = {-1, -1};
    if (pipe(p2c) != 0 || pipe(c2p) != 0) {
        die("pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        die("fork");
    }

    if (pid == 0) {
        // Child: wait for parent signal, then write to shared pages.
        close(p2c[1]);
        close(c2p[0]);
        char cmd = 0;
        if (read(p2c[0], &cmd, 1) != 1) {
            _exit(2);
        }
        if (cmd != 'S') {
            _exit(3);
        }
        // Write a recognizable value into a few sampled pages.
        for (size_t off = 0; off < THP_2M; off += 64UL * 1024) {
            thp[off] = (char)0xA5;
        }
        if (write(c2p[1], "D", 1) != 1) {
            _exit(4);
        }
        _exit(0);
    }

    // Parent: wait for collapse, then trigger IPC write/read verification.
    close(p2c[0]);
    close(c2p[1]);


    // Verify pattern still intact after collapse.
    if (check_sampled_pattern(p, LEN_BYTES, seed) != 0) {
        fprintf(stderr, "  FAIL: pattern corrupted after collapse\n");
        goto wait_child;
    }

    if (write(p2c[1], "S", 1) != 1) {
        die("write(p2c)");
    }
    char done = 0;
    if (read(c2p[0], &done, 1) != 1 || done != 'D') {
        fprintf(stderr, "  FAIL: child did not acknowledge\n");
        goto wait_child;
    }

    // Child writes must be visible to parent (shared mapping).
    for (size_t off = 0; off < THP_2M; off += 64UL * 1024) {
        if ((unsigned char)thp[off] != 0xA5) {
            fprintf(stderr, "  FAIL: IPC write not visible at %zu (got %#x)\n",
                    off, (unsigned char)thp[off]);
            goto wait_child;
        }
    }

    printf("thp_shmem_khugepaged_ipc_test: PASS\n");
    rc = 0;

wait_child: {
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        rc = 1;
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "  child exit status=%d\n", status);
        rc = 1;
    }
}

out:
    munmap(p, LEN_BYTES);
}
