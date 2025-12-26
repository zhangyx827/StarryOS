// sqlite_thp_pagecache_bench.c
//
// A "real program" THP benchmark using SQLite's pager page cache.
//
// Why this exists:
// - SQLite's default allocations can be fragmented and not 2MiB-aligned, so
//   khugepaged may have little to collapse.
// - SQLite provides SQLITE_CONFIG_PAGECACHE to let the application provide a
//   big, contiguous buffer for the page cache. We allocate that buffer 2MiB
//   aligned and optionally advise/collapse huge pages, so THP can take effect.
//
// Usage (inside StarryOS):
//   # Baseline (4K):
//   echo never > /sys/kernel/mm/transparent_hugepage/enabled
//   ./sqlite_thp_pagecache_bench 0 256 1000000 2000000 256 30000 0
//
//   # THP (khugepaged / madvise):
//   echo madvise > /sys/kernel/mm/transparent_hugepage/enabled
//   echo always  > /sys/kernel/mm/transparent_hugepage/khugepaged/defrag
//   echo 0       > /sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs
//   echo 65536   > /sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan
//   ./sqlite_thp_pagecache_bench 1 256 1000000 2000000 256 30000 0
//
// Args:
//   <use_huge> <cache_mib> <rows> <ops> <blob_bytes> <wait_ms> <force_collapse> [run_ms] [report_ms]
//   - use_huge: 0=unaligned+MADV_NOHUGEPAGE, 1=2MiB-aligned+MADV_HUGEPAGE
//   - wait_ms:  best-effort deadline (ms) to observe a khugepaged collapse while the read phase runs
//   - force_collapse: 1 => madvise(MADV_COLLAPSE) on the pagecache buffer (best-effort)
//   - run_ms:   run the point-lookup phase for at least this duration (ms); 0 uses <ops>
//   - report_ms:progress print interval during the point-lookup phase (ms)
//
// Output:
//   - build_ms: time to populate the DB
//   - read_ns/op: time per point-lookup
//   - pages_collapsed delta (if sysfs is available)

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

// Minimal SQLite API definitions to avoid requiring sqlite3.h at build time.
// This benchmark links against libsqlite3 at build time (e.g. `-lsqlite3`).
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

#ifndef SQLITE_OK
#define SQLITE_OK 0
#endif
#ifndef SQLITE_ROW
#define SQLITE_ROW 100
#endif
#ifndef SQLITE_DONE
#define SQLITE_DONE 101
#endif
// Stable config opcode.
#ifndef SQLITE_CONFIG_PAGECACHE
#define SQLITE_CONFIG_PAGECACHE 7
#endif

extern int sqlite3_shutdown(void);
extern int sqlite3_config(int, ...);
extern int sqlite3_open(const char *filename, sqlite3 **ppDb);
extern const char *sqlite3_errmsg(sqlite3 *);
extern int sqlite3_exec(sqlite3 *,
                        const char *sql,
                        int (*callback)(void *, int, char **, char **),
                        void *,
                        char **errmsg);
extern int sqlite3_prepare_v2(sqlite3 *,
                              const char *zSql,
                              int nByte,
                              sqlite3_stmt **ppStmt,
                              const char **pzTail);
extern int sqlite3_bind_int(sqlite3_stmt *, int, int);
extern int sqlite3_step(sqlite3_stmt *);
extern int sqlite3_reset(sqlite3_stmt *);
extern int sqlite3_clear_bindings(sqlite3_stmt *);
extern int sqlite3_finalize(sqlite3_stmt *);
extern const void *sqlite3_column_blob(sqlite3_stmt *, int iCol);
extern int sqlite3_column_bytes(sqlite3_stmt *, int iCol);
extern int sqlite3_close(sqlite3 *);

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

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void die_sqlite(int rc, sqlite3 *db, const char *what) {
    fprintf(stderr,
            "sqlite error (%s): rc=%d msg=%s\n",
            what,
            rc,
            db ? sqlite3_errmsg(db) : "-");
    exit(1);
}

static void *alloc_pagecache(size_t bytes, int use_huge, void **raw_out, size_t *raw_len_out) {
    bytes = (bytes + THP_2M - 1) & ~(THP_2M - 1);
    if (bytes < THP_2M) {
        bytes = THP_2M;
    }

    // Use mmap for easy page alignment and deterministic alignment control.
    size_t total = bytes + THP_2M;
    uint8_t *raw = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        perror("mmap(pagecache)");
        exit(1);
    }

    uintptr_t base = (uintptr_t)raw;
    uintptr_t aligned2m = (base + THP_2M - 1) & ~(uintptr_t)(THP_2M - 1);
    uintptr_t aligned4k = (base + PAGE_4K - 1) & ~(uintptr_t)(PAGE_4K - 1);

    // Choose a 4K-aligned address that is *not* 2M-aligned for baseline.
    uintptr_t unaligned = aligned4k;
    if ((unaligned & (THP_2M - 1)) == 0) {
        unaligned += PAGE_4K;
    }
    if ((unaligned & (PAGE_4K - 1)) != 0) {
        fprintf(stderr, "internal: unaligned is not 4K aligned\n");
        exit(1);
    }

    void *p = (void *)(use_huge ? aligned2m : unaligned);
    if (raw_out) {
        *raw_out = raw;
    }
    if (raw_len_out) {
        *raw_len_out = total;
    }

    int adv = use_huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE;
    if (madvise(p, bytes, adv) != 0) {
        fprintf(stderr, "madvise(pagecache, %s) failed: %s\n",
                use_huge ? "MADV_HUGEPAGE" : "MADV_NOHUGEPAGE",
                strerror(errno));
    }

    // Fault-in the buffer so khugepaged has real 4K pages to collapse.
    volatile uint8_t *vp = (volatile uint8_t *)p;
    for (size_t off = 0; off < bytes; off += PAGE_4K) {
        vp[off] = (uint8_t)(off / PAGE_4K);
    }

    return p;
}

int main(int argc, char **argv) {
    int use_huge = 1;
    size_t cache_mib = 256;
    int rows = 1000000;
    int ops = 2000000;
    int blob_bytes = 256;
    uint64_t wait_ms = 0;
    int force_collapse = 0;
    uint64_t run_ms = 0;
    uint64_t report_ms = 1000;

    if (argc >= 2) use_huge = (int)strtol(argv[1], NULL, 0) != 0;
    if (argc >= 3) cache_mib = (size_t)strtoul(argv[2], NULL, 0);
    if (argc >= 4) rows = (int)strtol(argv[3], NULL, 0);
    if (argc >= 5) ops = (int)strtol(argv[4], NULL, 0);
    if (argc >= 6) blob_bytes = (int)strtol(argv[5], NULL, 0);
    if (argc >= 7) wait_ms = (uint64_t)strtoull(argv[6], NULL, 0);
    if (argc >= 8) force_collapse = (int)strtol(argv[7], NULL, 0) != 0;
    if (argc >= 9) run_ms = (uint64_t)strtoull(argv[8], NULL, 0);
    if (argc >= 10) report_ms = (uint64_t)strtoull(argv[9], NULL, 0);

    if (cache_mib < 16) cache_mib = 16;
    if (rows < 1000) rows = 1000;
    if (ops < 1000) ops = 1000;
    if (blob_bytes < 16) blob_bytes = 16;
    if (report_ms == 0) report_ms = 1000;

    size_t cache_bytes = cache_mib * 1024 * 1024;
    int n_pages = (int)(cache_bytes / PAGE_4K);
    if (n_pages < 128) n_pages = 128;

    const char *pages_collapsed_path =
        "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed";
    long collapsed_before = read_long_file(pages_collapsed_path, -1);

    void *raw = NULL;
    size_t raw_len = 0;
    void *pagecache = alloc_pagecache(cache_bytes, use_huge, &raw, &raw_len);

    if (force_collapse) {
        if (madvise(pagecache, cache_bytes, MADV_COLLAPSE) != 0) {
            fprintf(stderr, "madvise(MADV_COLLAPSE) failed: %s\n", strerror(errno));
        }
    }

    // Configure SQLite global page cache. Must happen before any initialization.
    sqlite3_shutdown();
    int rc = sqlite3_config(SQLITE_CONFIG_PAGECACHE, pagecache, (int)PAGE_4K, n_pages);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite3_config(SQLITE_CONFIG_PAGECACHE) failed: rc=%d\n", rc);
        return 1;
    }

    sqlite3 *db = NULL;
    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) die_sqlite(rc, db, "open");

    // Reduce overhead from journaling; in-memory DB shouldn't need it anyway.
    rc = sqlite3_exec(
        db,
        "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; PRAGMA temp_store=MEMORY;",
        NULL,
        NULL,
        NULL);
    if (rc != SQLITE_OK) die_sqlite(rc, db, "pragma");

    rc = sqlite3_exec(db, "CREATE TABLE kv(k INTEGER PRIMARY KEY, v BLOB);", NULL, NULL, NULL);
    if (rc != SQLITE_OK) die_sqlite(rc, db, "create");

    rc = sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) die_sqlite(rc, db, "begin");

    sqlite3_stmt *ins = NULL;
    char ins_sql[128];
    snprintf(ins_sql, sizeof(ins_sql), "INSERT INTO kv(k,v) VALUES(?, randomblob(%d));", blob_bytes);
    rc = sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL);
    if (rc != SQLITE_OK) die_sqlite(rc, db, "prepare insert");

    uint64_t t_build0 = now_ns();
    for (int i = 1; i <= rows; i++) {
        sqlite3_bind_int(ins, 1, i);
        rc = sqlite3_step(ins);
        if (rc != SQLITE_DONE) die_sqlite(rc, db, "insert step");
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
    }
    sqlite3_finalize(ins);

    rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK) die_sqlite(rc, db, "commit");
    uint64_t t_build1 = now_ns();

    sqlite3_stmt *sel = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT v FROM kv WHERE k=?;", -1, &sel, NULL);
    if (rc != SQLITE_OK) die_sqlite(rc, db, "prepare select");

    volatile uint64_t sum = 0;
    uint64_t rng = 0x123456789abcdef0ull;
    long collapsed_read0 = read_long_file(pages_collapsed_path, -1);
    uint64_t t0 = now_ns();
    uint64_t next_report = t0 + report_ms * 1000000ull;
    uint64_t deadline = (run_ms > 0) ? (t0 + run_ms * 1000000ull) : 0;
    uint64_t observe_deadline = (wait_ms > 0) ? (t0 + wait_ms * 1000000ull) : 0;
    int warned_no_collapse = 0;

    uint64_t done = 0;
    for (;;) {
        if (run_ms == 0) {
            if (done >= (uint64_t)ops) {
                break;
            }
        } else {
            if (now_ns() >= deadline && done >= 1000) {
                break;
            }
        }

        int key = (int)(xorshift64(&rng) % (uint64_t)rows) + 1;
        sqlite3_bind_int(sel, 1, key);
        rc = sqlite3_step(sel);
        if (rc != SQLITE_ROW) die_sqlite(rc, db, "select step");
        const void *blob = sqlite3_column_blob(sel, 0);
        int n = sqlite3_column_bytes(sel, 0);
        if (blob && n > 0) {
            sum += (uint64_t)n;
        }
        sqlite3_reset(sel);
        sqlite3_clear_bindings(sel);

        done++;

        uint64_t now = now_ns();
        if (now >= next_report) {
            double ns_per = (double)(now - t0) / (double)done;
            long cur = read_long_file(pages_collapsed_path, -1);
            long delta = (collapsed_read0 >= 0 && cur >= 0) ? (cur - collapsed_read0) : -1;
            printf("  progress ops=%" PRIu64 " read_ns/op=%.2f pages_collapsed_delta=%ld\n",
                   done,
                   ns_per,
                   delta);
            next_report = now + report_ms * 1000000ull;

            if (!warned_no_collapse && observe_deadline > 0 && now > observe_deadline) {
                if (delta <= 0) {
                    printf("  warning: no khugepaged collapse observed within %" PRIu64 " ms\n",
                           wait_ms);
                }
                warned_no_collapse = 1;
            }

            // Yield a bit to reduce noise and allow background work to run.
            usleep(0);
        }
    }
    uint64_t t1 = now_ns();
    sqlite3_finalize(sel);

    long collapsed_after = read_long_file(pages_collapsed_path, -1);

    double build_ms = (double)(t_build1 - t_build0) / 1000000.0;
    double read_ns_per = (double)(t1 - t0) / (double)done;

    printf("sqlite_thp_pagecache_bench: use_huge=%d cache_mib=%zu rows=%d ops=%d blob=%d\n",
           use_huge, cache_mib, rows, ops, blob_bytes);
    printf("  pagecache=%p (raw=%p) n_pages=%d\n", pagecache, raw, n_pages);
    printf("  build_ms=%.2f\n", build_ms);
    printf("  read_ns/op=%.2f ops_done=%" PRIu64 " sum=%" PRIu64 "\n",
           read_ns_per,
           done,
           (uint64_t)sum);
    if (collapsed_before >= 0 && collapsed_after >= 0) {
        printf("  pages_collapsed delta=%ld (before=%ld after=%ld)\n",
               collapsed_after - collapsed_before, collapsed_before, collapsed_after);
    } else {
        printf("  pages_collapsed=N/A\n");
    }

    sqlite3_close(db);
    munmap(raw, raw_len);
    return 0;
}
