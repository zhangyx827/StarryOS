#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#define PAGE_4K 4096UL
#define THP_2M (2UL * 1024 * 1024)

static void die(const char *msg) {
    perror(msg);
    exit(1);
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

    // Keep [aligned, aligned+2MiB); unmap prefix and suffix.
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

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== thp_collapse_oom_stress_test ===\n");

    uint64_t max_thps = 0;
    int hold_seconds = 60;
    if (argc >= 2) {
        max_thps = strtoull(argv[1], NULL, 0);
    }
    if (argc >= 3) {
        hold_seconds = atoi(argv[2]);
        if (hold_seconds < 0) hold_seconds = 0;
    }

    // Track mappings so they stay alive (and can be unmapped if desired).
    size_t cap = 128;
    void **kept = calloc(cap, sizeof(void *));
    if (!kept) {
        die("calloc");
    }

    uint64_t ok = 0;
    for (;;) {
        if (max_thps != 0 && ok >= max_thps) {
            printf("[done] reached max_thps=%" PRIu64 "\n", max_thps);
            break;
        }

        void *p = mmap_aligned_2m();
        if (p == MAP_FAILED) {
            printf("[fail] mmap: errno=%d (%s)\n", errno, strerror(errno));
            break;
        }

        touch_all_4k((char *)p, THP_2M);

        if (madvise(p, THP_2M, MADV_COLLAPSE) != 0) {
            int e = errno;
            printf("[fail] madvise(MADV_COLLAPSE) after %" PRIu64 " THPs: errno=%d (%s)\n",
                   ok, e, strerror(e));
            munmap(p, THP_2M);
            break;
        }

        if (ok >= cap) {
            cap *= 2;
            void **new_kept = realloc(kept, cap * sizeof(void *));
            if (!new_kept) {
                printf("[fail] realloc after %" PRIu64 " THPs\n", ok);
                munmap(p, THP_2M);
                break;
            }
            kept = new_kept;
        }
        kept[ok] = p;
        ok++;

        if ((ok % 16) == 0) {
            uint64_t mib = (ok * THP_2M) / (1024 * 1024);
            printf("[ok] collapsed=%" PRIu64 " (held ~%" PRIu64 " MiB)\n", ok, mib);
        }
    }

    uint64_t mib = (ok * THP_2M) / (1024 * 1024);
    printf("[summary] held THPs=%" PRIu64 " (~%" PRIu64 " MiB)\n", ok, mib);
    char *p = mmap(NULL, THP_2M * 3, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                
    if (p == MAP_FAILED) {
        die("mmap");
    }

    if (hold_seconds > 0) {
        printf("[hold] sleeping %d seconds with memory pressure...\n", hold_seconds);
        sleep((unsigned)hold_seconds);
    }

    // Cleanup.
    for (uint64_t i = 0; i < ok; i++) {
        if (kept[i]) {
            munmap(kept[i], THP_2M);
        }
    }
    free(kept);

    printf("thp_collapse_oom_stress_test: DONE\n");
    return 0;
}

