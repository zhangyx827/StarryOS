
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_4K 4096UL
#define THP_2M (2UL * 1024 * 1024)

static const char *SYS_THP_ENABLED = "/sys/kernel/mm/transparent_hugepage/enabled";
static const char *SYS_PAGES_TO_SCAN =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan";
static const char *SYS_SCAN_SLEEP_MS =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs";
static const char *SYS_PAGES_COLLAPSED =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed";

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static int write_text_padded(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    // Overwrite with a padded buffer to avoid stale tail bytes.
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

static uint64_t read_u64_trim(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        die("fopen");
    }
    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        die("fgets");
    }
    fclose(f);
    return strtoull(buf, NULL, 10);
}

static void *mmap_aligned_2m(void) {
    // Map 4MiB and carve out a 2MiB-aligned window.
    size_t len = THP_2M * 2;
    char *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return MAP_FAILED;
    }

    uintptr_t b = (uintptr_t)base;
    uintptr_t aligned = (b + THP_2M - 1) & ~(THP_2M - 1);

    size_t prefix = (size_t)(aligned - b);
    size_t suffix = (size_t)((b + len) - (aligned + THP_2M));
    if (prefix) {
        munmap(base, prefix);
    }
    if (suffix) {
        munmap((void *)(aligned + THP_2M), suffix);
    }
    return (void *)aligned;
}

static void touch_all_4k(char *p, size_t len) {
    for (size_t off = 0; off < len; off += PAGE_4K) {
        p[off] = (char)((off / PAGE_4K) & 0x7f);
    }
}

static int wait_for_khuge_collapse(uint64_t before_pages, int timeout_ms) {
    // Each 2MiB THP accounts as 512 x 4KiB pages in pages_collapsed.
    uint64_t target = before_pages + (THP_2M / PAGE_4K);
    int waited = 0;
    while (waited < timeout_ms) {
        uint64_t now = read_u64_trim(SYS_PAGES_COLLAPSED);
        if (now >= target) {
            return 0;
        }
        usleep(10 * 1000);
        waited += 10;
    }
    return -1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== thp_khugepaged_oom_stress_test ===\n");

    uint64_t max_regions = 0; // 0 means "until collapse stalls"
    int collapse_timeout_ms = 5000;
    if (argc >= 2) {
        max_regions = strtoull(argv[1], NULL, 0);
    }
    if (argc >= 3) {
        collapse_timeout_ms = atoi(argv[2]);
        if (collapse_timeout_ms < 100) collapse_timeout_ms = 100;
    }

    // Configure khugepaged to run aggressively.
    if (write_text_padded(SYS_THP_ENABLED, "always") != 0 ||
        write_text_padded(SYS_SCAN_SLEEP_MS, "0") != 0 ||
        write_text_padded(SYS_PAGES_TO_SCAN, "65536") != 0) {
        printf("SKIP: cannot configure THP sysfs knobs\n");
        return 77;
    }

    size_t cap = 128;
    void **kept = calloc(cap, sizeof(void *));
    if (!kept) {
        die("calloc");
    }

    uint64_t ok = 0;
    for (;;) {
        if (max_regions != 0 && ok >= max_regions) {
            break;
        }

        void *p = mmap_aligned_2m();
        if (p == MAP_FAILED) {
            printf("[fail] mmap: errno=%d (%s)\n", errno, strerror(errno));
            break;
        }
        touch_all_4k((char *)p, THP_2M);

        uint64_t before = read_u64_trim(SYS_PAGES_COLLAPSED);
        if (wait_for_khuge_collapse(before, collapse_timeout_ms) != 0) {
            uint64_t now = read_u64_trim(SYS_PAGES_COLLAPSED);
            printf("[stall] no collapse within %dms at region=%" PRIu64
                   " (pages_collapsed=%" PRIu64 " -> %" PRIu64 ")\n",
                   collapse_timeout_ms, ok, before, now);
            munmap(p, THP_2M);
            break;
        }

        if (ok >= cap) {
            cap *= 2;
            void **new_kept = realloc(kept, cap * sizeof(void *));
            if (!new_kept) {
                printf("[fail] realloc after %" PRIu64 " regions\n", ok);
                munmap(p, THP_2M);
                break;
            }
            kept = new_kept;
        }
        kept[ok++] = p;

        if ((ok % 16) == 0) {
            uint64_t mib = (ok * THP_2M) / (1024 * 1024);
            printf("[ok] regions=%" PRIu64 " (held ~%" PRIu64 " MiB)\n", ok, mib);
        }
    }

    uint64_t mib = (ok * THP_2M) / (1024 * 1024);
    printf("[summary] collapsed regions=%" PRIu64 " (~%" PRIu64 " MiB held)\n", ok, mib);
    printf("TIP: reduce QEMU memory (e.g., MEM=256M) to see stall earlier.\n");

    // Keep pressure briefly so you can inspect /sys counters interactively.
    sleep(10);

    for (uint64_t i = 0; i < ok; i++) {
        if (kept[i]) {
            munmap(kept[i], THP_2M);
        }
    }
    free(kept);
    return 0;
}

