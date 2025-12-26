// khugepaged_perf_bench.c
//
// A/B benchmark to demonstrate potential application speedup from THP after
// khugepaged collapses 4KiB pages into 2MiB mappings.
//
// This benchmark tries to reduce CPU cache effects in the A/B comparison by:
//   - Mapping two regions of the same size:
//       * base: 4KiB pages (unaligned + MADV_NOHUGEPAGE)
//       * thp:  2MiB aligned + MADV_HUGEPAGE (candidate for collapse)
//   - Using the *same* pseudo-random page access pattern for both regions
//   - Thrashing data cache + TLB between measurements with a large "trash" buffer
//   - Alternating measurement order across repeats (base->thp, then thp->base)
//
// This cannot perfectly eliminate all cache influences, but it significantly
// reduces the common "second run is faster because everything is warm" bias.
//
// Suggested usage (from StarryOS shell):
//   # THP via khugepaged:
//   echo madvise > /sys/kernel/mm/transparent_hugepage/enabled
//   echo always  > /sys/kernel/mm/transparent_hugepage/khugepaged/defrag
//   echo 0       > /sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs
//   echo 65536   > /sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan
//   ./khugepaged_perf_bench 256 20000000 30000 1 0 5 256
//
// Compare the ns/access numbers and also check:
//   cat /sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif
#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE 15
#endif
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#define PAGE_4K 4096UL
#define THP_2M  (2UL * 1024 * 1024)

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static long read_long_file(const char *path, long def) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return def;
    }
    long v = def;
    if (fscanf(f, "%ld", &v) != 1) {
        v = def;
    }
    fclose(f);
    return v;
}

static void touch_each_page(volatile uint8_t *p, size_t len) {
    for (size_t off = 0; off < len; off += PAGE_4K) {
        p[off] = (uint8_t)(off / PAGE_4K);
    }
}

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void shuffle_u32(uint32_t *a, size_t n, uint64_t *rng_state) {
    for (size_t i = n - 1; i > 0; i--) {
        uint64_t r = xorshift64(rng_state);
        size_t j = (size_t)(r % (i + 1));
        uint32_t tmp = a[i];
        a[i] = a[j];
        a[j] = tmp;
    }
}

static void thrash_cache(volatile uint8_t *trash, size_t len) {
    volatile uint64_t sum = 0;
    for (size_t off = 0; off + 64 <= len; off += 64) {
        sum += trash[off];
    }
    if (sum == 0xdeadbeefULL) {
        fprintf(stderr, "thrash_cache: unexpected sum\n");
    }
}

static double bench_page_pattern(uint8_t *p,
                                 size_t len,
                                 uint64_t iters,
                                 const uint32_t *page_perm,
                                 size_t pages,
                                 uint64_t *sum_out) {
    volatile uint64_t sum = 0;
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < iters; i++) {
        size_t page_idx = (size_t)page_perm[i % pages];
        size_t off = page_idx * PAGE_4K;
        if (off + sizeof(uint64_t) > len) {
            break;
        }
        sum += *(volatile uint64_t *)(p + off);
    }
    uint64_t t1 = now_ns();

    if (sum_out) {
        *sum_out = (uint64_t)sum;
    }
    return (double)(t1 - t0) / (double)iters;
}

static uint8_t *mmap_2m_aligned(size_t len) {
    size_t total = len + THP_2M;
    uint8_t *p = mmap(NULL, total, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return MAP_FAILED;
    }
    uintptr_t base = (uintptr_t)p;
    uintptr_t aligned = (base + THP_2M - 1) & ~(uintptr_t)(THP_2M - 1);
    munmap(p, total);

    p = mmap((void *)aligned, len, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        // Fallback: let the kernel choose the address.
        p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    return p;
}

static uint8_t *mmap_4k_unaligned(size_t len, uint8_t **base_map_out, size_t *base_map_len_out) {
    size_t total = len + PAGE_4K;
    uint8_t *base_map = mmap(NULL, total, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base_map == MAP_FAILED) {
        return MAP_FAILED;
    }
    if (base_map_out) {
        *base_map_out = base_map;
    }
    if (base_map_len_out) {
        *base_map_len_out = total;
    }
    return base_map + PAGE_4K;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static double median(double *vals, size_t n) {
    qsort(vals, n, sizeof(double), cmp_double);
    if (n == 0) {
        return 0.0;
    }
    if (n & 1) {
        return vals[n / 2];
    }
    return 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
}

int main(int argc, char **argv) {
    size_t mb = 64;
    uint64_t iters = 20000000ull;
    uint64_t wait_ms = 30000ull;
    int require_collapse = 0;
    int fallback_to_madv_collapse = 0;
    size_t repeats = 5;
    size_t trash_mib = 0;
    if (argc >= 2) {
        mb = (size_t)strtoul(argv[1], NULL, 0);
    }
    if (argc >= 3) {
        iters = (uint64_t)strtoull(argv[2], NULL, 0);
    }
    if (argc >= 4) {
        wait_ms = (uint64_t)strtoull(argv[3], NULL, 0);
    }
    if (argc >= 5) {
        require_collapse = (int)strtol(argv[4], NULL, 0) != 0;
    }
    if (argc >= 6) {
        fallback_to_madv_collapse = (int)strtol(argv[5], NULL, 0) != 0;
    }
    if (argc >= 7) {
        repeats = (size_t)strtoul(argv[6], NULL, 0);
        if (repeats == 0) {
            repeats = 1;
        }
    }
    if (argc >= 8) {
        trash_mib = (size_t)strtoul(argv[7], NULL, 0);
    }

    size_t len = mb * 1024 * 1024;
    len = (len + THP_2M - 1) & ~(THP_2M - 1);
    if (len < THP_2M) {
        len = THP_2M;
    }

    const char *pages_collapsed_path =
        "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed";
    long collapsed_before = read_long_file(pages_collapsed_path, -1);

    uint8_t *base_map = NULL;
    size_t base_map_len = 0;
    uint8_t *p_base = mmap_4k_unaligned(len, &base_map, &base_map_len);
    if (p_base == MAP_FAILED) {
        perror("mmap(base)");
        return 1;
    }

    uint8_t *p_thp = mmap_2m_aligned(len);
    if (p_thp == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    if (madvise(p_base, len, MADV_NOHUGEPAGE) != 0) {
        fprintf(stderr, "madvise(base, MADV_NOHUGEPAGE) failed: %s\n", strerror(errno));
    }
    if (madvise(p_thp, len, MADV_HUGEPAGE) != 0) {
        fprintf(stderr, "madvise(thp, MADV_HUGEPAGE) failed: %s\n", strerror(errno));
    }

    size_t pages = len / PAGE_4K;
    if (pages < 2) {
        fprintf(stderr, "region too small\n");
        munmap(base_map, base_map_len);
        munmap(p_thp, len);
        return 1;
    }
    uint32_t *page_perm = (uint32_t *)malloc(pages * sizeof(uint32_t));
    if (!page_perm) {
        fprintf(stderr, "malloc(page_perm) failed\n");
        munmap(base_map, base_map_len);
        munmap(p_thp, len);
        return 1;
    }
    for (size_t i = 0; i < pages; i++) {
        page_perm[i] = (uint32_t)i;
    }
    uint64_t rng = 0x123456789abcdef0ull;
    shuffle_u32(page_perm, pages, &rng);

    if (trash_mib == 0) {
        trash_mib = mb;
    }
    size_t trash_len = trash_mib * 1024 * 1024;
    if (trash_len < 64 * 1024 * 1024) {
        trash_len = 64 * 1024 * 1024;
    }
    volatile uint8_t *trash = mmap(NULL, trash_len, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trash == MAP_FAILED) {
        fprintf(stderr, "mmap(trash) failed: %s (continuing without thrash)\n", strerror(errno));
        trash = NULL;
        trash_len = 0;
    } else {
        touch_each_page(trash, trash_len);
    }

    printf("khugepaged_perf_bench: len=%zu MiB pages=%zu iters=%" PRIu64 " repeats=%zu wait_ms=%" PRIu64 " trash=%zu MiB\n",
           len / 1024 / 1024, pages, iters, repeats, wait_ms, trash_len / 1024 / 1024);
    printf("  base=%p (4K-unaligned), thp=%p (2M-aligned)\n", p_base, p_thp);
    if (collapsed_before >= 0) {
        printf("  pages_collapsed(before)=%ld\n", collapsed_before);
    } else {
        printf("  pages_collapsed(before)=N/A\n");
    }

    // Fault-in: ensure there are real 4K pages for khugepaged to collapse.
    touch_each_page((volatile uint8_t *)p_base, len);
    touch_each_page((volatile uint8_t *)p_thp, len);

    // Best-effort: wait for khugepaged to make progress.
    // (This is global state, but useful as a hint.)
    uint64_t wait_start = now_ns();
    int saw_progress = 0;
    for (;;) {
        long cur = read_long_file(pages_collapsed_path, -1);
        if (collapsed_before >= 0 && cur >= 0 && cur > collapsed_before) {
            printf("  pages_collapsed(after_faultin)=%ld (delta=%ld)\n",
                   cur, cur - collapsed_before);
            saw_progress = 1;
            break;
        }
        if ((now_ns() - wait_start) / 1000000ull > wait_ms) {
            if (cur >= 0) {
                printf("  pages_collapsed(after_wait)=%ld (delta=%ld)\n",
                       cur, (collapsed_before >= 0) ? (cur - collapsed_before) : 0);
            }
            break;
        }
        usleep(20000); // 20ms
    }

    if (!saw_progress && fallback_to_madv_collapse) {
        printf("  no khugepaged progress observed, trying madvise(MADV_COLLAPSE)...\n");
        if (madvise(p_thp, len, MADV_COLLAPSE) != 0) {
            fprintf(stderr, "  madvise(MADV_COLLAPSE) failed: %s\n", strerror(errno));
        } else {
            printf("  madvise(MADV_COLLAPSE) ok\n");
        }
    }

    if (!saw_progress && require_collapse && !fallback_to_madv_collapse) {
        fprintf(stderr,
                "  ERROR: no khugepaged collapse observed within %" PRIu64 " ms\n",
                wait_ms);
        free(page_perm);
        if (trash) {
            munmap((void *)trash, trash_len);
        }
        munmap(base_map, base_map_len);
        munmap(p_thp, len);
        return 2;
    }

    double *base_ns = (double *)calloc(repeats, sizeof(double));
    double *thp_ns = (double *)calloc(repeats, sizeof(double));
    if (!base_ns || !thp_ns) {
        fprintf(stderr, "calloc failed\n");
        free(page_perm);
        free(base_ns);
        free(thp_ns);
        if (trash) {
            munmap((void *)trash, trash_len);
        }
        munmap(base_map, base_map_len);
        munmap(p_thp, len);
        return 1;
    }

    for (size_t r = 0; r < repeats; r++) {
        uint64_t sum0 = 0, sum1 = 0;
        if ((r & 1) == 0) {
            if (trash) thrash_cache(trash, trash_len);
            base_ns[r] = bench_page_pattern(p_base, len, iters, page_perm, pages, &sum0);
            if (trash) thrash_cache(trash, trash_len);
            thp_ns[r] = bench_page_pattern(p_thp, len, iters, page_perm, pages, &sum1);
        } else {
            if (trash) thrash_cache(trash, trash_len);
            thp_ns[r] = bench_page_pattern(p_thp, len, iters, page_perm, pages, &sum1);
            if (trash) thrash_cache(trash, trash_len);
            base_ns[r] = bench_page_pattern(p_base, len, iters, page_perm, pages, &sum0);
        }
        printf("  repeat=%zu base_ns/access=%.2f thp_ns/access=%.2f speedup=%.3fx sums=%" PRIu64 "/%" PRIu64 "\n",
               r,
               base_ns[r],
               thp_ns[r],
               (thp_ns[r] > 0.0) ? (base_ns[r] / thp_ns[r]) : 0.0,
               sum0,
               sum1);
    }

    double base_med = median(base_ns, repeats);
    double thp_med = median(thp_ns, repeats);
    printf("  median base_ns/access=%.2f thp_ns/access=%.2f speedup=%.3fx\n",
           base_med,
           thp_med,
           (thp_med > 0.0) ? (base_med / thp_med) : 0.0);

    free(page_perm);
    free(base_ns);
    free(thp_ns);
    if (trash) {
        munmap((void *)trash, trash_len);
    }
    munmap(base_map, base_map_len);
    munmap(p_thp, len);
    return 0;
}
