#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#define THP_SIZE (8UL * 1024 * 1024)
#define PAGE_4K  4096UL
#define PAGE_SIZE_4K 4096UL

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

static const char *SYS_THP_ENABLED = "/sys/kernel/mm/transparent_hugepage/enabled";
static const char *SYS_SHMEM_ENABLED = "/sys/kernel/mm/transparent_hugepage/shmem_enabled";
static const char *SYS_KHUGEPAGED_SLEEP =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs";
static const char *SYS_KHUGEPAGED_PAGES_TO_SCAN =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan";
static const char *SYS_KHUGEPAGED_FULL_SCANS =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/full_scans";
static const char *SYS_KHUGEPAGED_PAGES_COLLAPSED =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed";

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static int write_text(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    // NOTE: These sysfs-like pseudo files do not necessarily truncate on
    // short writes at offset 0. To avoid stale tail bytes (e.g. "10\\n00")
    // breaking kernel-side `.trim().parse()`, always overwrite with a padded
    // buffer longer than any existing content.
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
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
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

static int open_tmpfs_file(char *out_path, size_t out_len) {
    const char *candidates[] = {
        "./test"
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char *p = candidates[i];
        int fd = open(p, O_CREAT | O_RDWR, 0600);
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
        printf("thp_read_only_mapping_test: SKIP no file");
        return 0;
    }

    printf("=== thp_read_only_mapping_test (khugepaged) ===");
    printf("  tmpfs file: %s", path);

    char old_enabled[32] = {0};
    char old_shmem[32] = {0};
    char old_sleep[32] = {0};
    char old_pages_to_scan[32] = {0};

    if (read_text_trim(SYS_THP_ENABLED, old_enabled, sizeof(old_enabled)) != 0 ||
        read_text_trim(SYS_SHMEM_ENABLED, old_shmem, sizeof(old_shmem)) != 0 ||
        read_text_trim(SYS_KHUGEPAGED_SLEEP, old_sleep, sizeof(old_sleep)) != 0 ||
        read_text_trim(SYS_KHUGEPAGED_PAGES_TO_SCAN, old_pages_to_scan, sizeof(old_pages_to_scan)) != 0) {
        printf("  SKIP: sysfs THP controls not available");
        close(fd);
        unlink(path);
        return 0;
    }

    // Configure to make only MADV_HUGEPAGE VMAs eligible, and speed up khugepaged scanning.
    if (write_text(SYS_THP_ENABLED, "madvise") != 0 ||
        write_text(SYS_SHMEM_ENABLED, "advise") != 0 ||
        write_text(SYS_KHUGEPAGED_SLEEP, "0") != 0 ||
        write_text(SYS_KHUGEPAGED_PAGES_TO_SCAN, "65536") != 0) {
        die("write sysfs");
    }


    char *p = mmap(NULL, THP_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        die("mmap(PROT_READ)");
    }
    printf("  mmap(PROT_READ) base=%p len=%lu", p, (unsigned long)THP_SIZE);

    // Mark this VMA as eligible for THP (khugepaged will do the actual collapse).
    if (madvise(p, THP_SIZE, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE)");
    }

    // Fault in all pages and verify the initial pattern.
    for (size_t off = 0; off < THP_SIZE; off += PAGE_4K) {
        unsigned char expected = 0x3c;
        unsigned char got = (unsigned char)p[off];
        if (got != expected) {
            fprintf(stderr, "  mismatch before collapse at %zu: got %#x expected %#x",
                    off, got, expected);
            munmap(p, THP_SIZE);
            close(fd);
            unlink(path);
            return 1;
        }
    }

    unsigned long collapsed0 = read_ulong(SYS_KHUGEPAGED_PAGES_COLLAPSED);
    unsigned long scans0 = read_ulong(SYS_KHUGEPAGED_FULL_SCANS);

    int collapsed = 0;
    for (int i = 0; i < 3000; i++) { // ~5s max
        usleep(10 * 1000);
        unsigned long now = read_ulong(SYS_KHUGEPAGED_PAGES_COLLAPSED);
        unsigned long scans = read_ulong(SYS_KHUGEPAGED_FULL_SCANS);
        if (scans != scans0 || now != collapsed0) {
            // khugepaged has progressed; require at least one 2MiB collapse (512 x 4KiB pages).
            if (now >= collapsed0 + 512) {
                collapsed = 1;
                break;
            }
        }
    }

    // Verify contents remain intact after khugepaged collapse attempt.
    int rc = 0;
    for (size_t off = 0; off < THP_SIZE; off += PAGE_4K) {
        unsigned char expected = 0x3c;
        if ((unsigned char)p[off] != expected) {
            fprintf(stderr, "  mismatch after collapse at %zu: got %#x expected %#x",
                    off, (unsigned char)p[off], expected);
            rc = 1;
            break;
        }
    }

    if (!collapsed) {
        printf("  WARN: khugepaged did not report a collapse within timeout");
        rc = 1;
    } else {
        printf("  khugepaged collapse observed (pages_collapsed: %lu -> %lu)",
               collapsed0, read_ulong(SYS_KHUGEPAGED_PAGES_COLLAPSED));
    }

    munmap(p, THP_SIZE);
    close(fd);
    unlink(path);

    // Restore sysfs settings.
    {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s", old_enabled);
        write_text(SYS_THP_ENABLED, tmp);
        snprintf(tmp, sizeof(tmp), "%s", old_shmem);
        write_text(SYS_SHMEM_ENABLED, tmp);
        snprintf(tmp, sizeof(tmp), "%s", old_sleep);
        write_text(SYS_KHUGEPAGED_SLEEP, tmp);
        snprintf(tmp, sizeof(tmp), "%s", old_pages_to_scan);
        write_text(SYS_KHUGEPAGED_PAGES_TO_SCAN, tmp);
    }

    if (rc == 0) {
        printf("thp_read_only_mapping_test: PASS");
    } else {
        printf("thp_read_only_mapping_test: FAIL");
    }
    return rc;
}
