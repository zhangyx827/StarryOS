#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#define PAGE_4K 4096UL
#define THP_2M (2UL * 1024 * 1024)

static const char *SYS_THP_ENABLED = "/sys/kernel/mm/transparent_hugepage/enabled";
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
    while (n > 0 &&
           (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' ||
            buf[n - 1] == '\t')) {
        buf[--n] = '\0';
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

static int write_text_padded(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    // NOTE: These sysfs-like pseudo files do not necessarily truncate on short
    // writes at offset 0. Always overwrite with a padded buffer to avoid stale
    // tail bytes (e.g. writing "madvise" after "always" becoming "madviseays").
    char buf[128];
    memset(buf, ' ', sizeof(buf));
    size_t len = strlen(s);
    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }
    memcpy(buf, s, len);
    ssize_t n = write(fd, buf, sizeof(buf));
    close(fd);
    return (n == (ssize_t)sizeof(buf)) ? 0 : -1;
}

static void touch_pattern(char *p, size_t len) {
    for (size_t off = 0; off < len; off += PAGE_4K) {
        p[off] = (char)((off / PAGE_4K) & 0x7F);
    }
}

static int check_pattern(char *p, size_t len) {
    for (size_t off = 0; off < len; off += PAGE_4K) {
        unsigned char expected = (unsigned char)((off / PAGE_4K) & 0x7F);
        unsigned char got = (unsigned char)p[off];
        if (got != expected) {
            fprintf(stderr,
                    "[thp_mprotect_split_test] pattern mismatch at %zu: got %#x expected %#x\n",
                    off, got, expected);
            return -1;
        }
    }
    return 0;
}

static sigjmp_buf g_jmp;

static void fault_handler(int signo, siginfo_t *info, void *ctx) {
    (void)signo;
    (void)info;
    (void)ctx;
    siglongjmp(g_jmp, 1);
}

static void install_fault_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        die("sigaction(SIGSEGV)");
    }
    if (sigaction(SIGBUS, &sa, NULL) != 0) {
        die("sigaction(SIGBUS)");
    }
}

static int expect_write_fault(volatile char *addr) {
    if (sigsetjmp(g_jmp, 1) == 0) {
        *addr ^= 1;
        return 0; // no fault
    }
    return 1; // faulted
}

int main(void) {
    printf("=== thp_mprotect_split_test (khugepaged) ===\n");

    int rc = 1;
    char *p = MAP_FAILED;

    char old_enabled[32] = {0};
    char old_sleep[32] = {0};
    char old_pages_to_scan[32] = {0};

    if (read_text_trim(SYS_THP_ENABLED, old_enabled, sizeof(old_enabled)) != 0 ||
        read_text_trim(SYS_KHUGEPAGED_SLEEP, old_sleep, sizeof(old_sleep)) != 0 ||
        read_text_trim(SYS_KHUGEPAGED_PAGES_TO_SCAN, old_pages_to_scan,
                       sizeof(old_pages_to_scan)) != 0) {
        printf("thp_mprotect_split_test: SKIP (sysfs THP controls not available)\n");
        return 0;
    }

    if (write_text_padded(SYS_THP_ENABLED, "madvise") != 0 ||
        write_text_padded(SYS_KHUGEPAGED_SLEEP, "0") != 0 ||
        write_text_padded(SYS_KHUGEPAGED_PAGES_TO_SCAN, "65536") != 0) {
        fprintf(stderr, "[thp_mprotect_split_test] FAIL: cannot configure sysfs knobs\n");
        goto restore;
    }

    install_fault_handlers();

    // Allocate 4MiB so we can always find a 2MiB-aligned window inside.
    const size_t len = 4UL * 1024 * 1024;
    p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        goto restore;
    }

    // Mark this VMA as eligible for THP collapse (global policy is "madvise").
    if (madvise(p, len, MADV_HUGEPAGE) != 0) {
        perror("madvise(MADV_HUGEPAGE)");
        goto out;
    }

    uintptr_t base = (uintptr_t)p;
    uintptr_t end = base + len;
    uintptr_t win = (base + THP_2M - 1) & ~(THP_2M - 1);
    if (win + THP_2M > end) {
        fprintf(stderr, "[thp_mprotect_split_test] FAIL: cannot find 2MiB-aligned window\n");
        goto out;
    }
    char *thp_region = (char *)win;
    printf("  mapping=%p len=%zu, thp_region=%p\n", p, len, thp_region);

    touch_pattern(thp_region, THP_2M);
    if (check_pattern(thp_region, THP_2M) != 0) {
        goto out;
    }

    // Wait for khugepaged to collapse this 2MiB window.
    unsigned long collapsed0 = read_ulong(SYS_KHUGEPAGED_PAGES_COLLAPSED);
    int collapsed = 0;
    for (int i = 0; i < 3000; i++) { // ~5s max
        usleep(10 * 1000);
        ((volatile char *)thp_region)[0] ^= 1;
        unsigned long now = read_ulong(SYS_KHUGEPAGED_PAGES_COLLAPSED);
        if (now >= collapsed0 + (THP_2M / PAGE_4K)) {
            collapsed = 1;
            break;
        }
    }
    if (!collapsed) {
        fprintf(stderr,
                "[thp_mprotect_split_test] FAIL: no THP collapse observed (pages_collapsed %lu -> %lu)\n",
                collapsed0, read_ulong(SYS_KHUGEPAGED_PAGES_COLLAPSED));
        goto out;
    }

    // mprotect a subrange: should split the THP so only the subrange becomes RO.
    char *before = thp_region;
    char *after = thp_region + THP_2M - PAGE_4K;
    size_t hole_len = 64UL * 1024;
    char *hole = thp_region + 512UL * 1024;

    if (((uintptr_t)hole % PAGE_4K) != 0 ||
        (uintptr_t)hole + hole_len > (uintptr_t)thp_region + THP_2M) {
        fprintf(stderr, "[thp_mprotect_split_test] FAIL: hole misaligned/out-of-range\n");
        goto out;
    }

    if (mprotect(hole, hole_len, PROT_READ) != 0) {
        perror("mprotect(hole, PROT_READ)");
        goto out;
    }

    // Writes outside the protected hole must succeed.
    *before ^= 1;
    *after ^= 1;

    // Writes inside the protected hole must fault.
    if (!expect_write_fault((volatile char *)hole)) {
        fprintf(stderr,
                "[thp_mprotect_split_test] FAIL: write in PROT_READ hole did not fault\n");
        goto out;
    }

    printf("thp_mprotect_split_test: PASS\n");
    rc = 0;

out:
    if (p != MAP_FAILED) {
        munmap(p, len);
    }
restore:
    // Best-effort restore.
    write_text_padded(SYS_THP_ENABLED, old_enabled);
    write_text_padded(SYS_KHUGEPAGED_SLEEP, old_sleep);
    write_text_padded(SYS_KHUGEPAGED_PAGES_TO_SCAN, old_pages_to_scan);
    return rc;
}
