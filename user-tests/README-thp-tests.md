# THP / backend stress tests

This directory contains targeted user-space tests for StarryOS THP and related VM backends.

## Build

Example (RISC-V, static):

```sh
riscv64-linux-gnu-gcc -O2 -static user-tests/filebackend_split_remap_stress_test.c -o user-tests/filebackend_split_remap_stress_test
```

## Run

Run inside the guest / StarryOS userspace:

```sh
./filebackend_split_remap_stress_test
./filebackend_split_remap_stress_test 10000
./sharedbackend_split_unmap_stress_test 10000
./filebackend_cross_boundary_split_test
./cowbackend_cross_boundary_unmap_test
./sharedbackend_cross_boundary_unmap_test
./khugepaged_perf_bench
```

## What it targets

- `filebackend_split_remap_stress_test.c`: File-backed tmpfs mapping; repeatedly `MADV_COLLAPSE` then `munmap` a 4KiB subpage (forces PMD split) and `mmap(MAP_FIXED)` the subpage back; verifies per-page byte patterns to catch cache/page-indexing bugs.
- Note: the test recreates a fresh 2MiB-aligned single-VMA mapping each iteration so the split path is exercised every time (it does not rely on the VM area manager to merge split areas back).
- `sharedbackend_split_unmap_stress_test.c`: MAP_SHARED|MAP_ANONYMOUS mapping; collapses a 2MiB-aligned window and repeatedly `munmap` a 4KiB subpage to stress the SharedBackend PMD-split path; checks neighbors and periodically asserts SIGSEGV on the hole.
- `filebackend_cross_boundary_split_test.c`: FileBackend; collapses 4MiB (two windows), unmaps a 16KiB range straddling the 2MiB boundary, remaps it back from file offsets, and checks pattern integrity.
- `cowbackend_cross_boundary_unmap_test.c`: CowBackend; collapses 4MiB then unmaps a 16KiB cross-boundary range; neighbors must stay intact; remapped anonymous hole must be zero-filled (detects stale reuse).
- `sharedbackend_cross_boundary_unmap_test.c`: SharedBackend; collapses 4MiB then unmaps a 16KiB cross-boundary range; neighbors must stay intact and the hole must fault.
- `khugepaged_perf_bench.c`: micro-benchmark; compares 4KiB vs THP mappings under the *same* pseudo-random per-page access pattern, while trying to reduce CPU cache effects by thrashing cache+TLB between measurements and alternating order.
  - Args: `./khugepaged_perf_bench <MiB> <iters> <wait_ms> <require_collapse> <fallback_madv_collapse> <repeats> <trash_mib>`
  - Output prints `base_ns/access` vs `thp_ns/access` and a median speedup; a real khugepaged benefit should show `thp_ns/access < base_ns/access` and `pages_collapsed` delta > 0.
- `sqlite_thp_pagecache_bench.c`: SQLite-based benchmark that forces a large contiguous anonymous region by providing SQLite's pager page cache buffer via `SQLITE_CONFIG_PAGECACHE` (2MiB-aligned + `MADV_HUGEPAGE`), making it easier for khugepaged/THP to take effect on a real program.
  - Args: `./sqlite_thp_pagecache_bench <use_huge> <cache_mib> <rows> <ops> <blob_bytes> <wait_ms> <force_collapse>`
  - Build:
    - Inside the guest (recommended): `cc -O2 sqlite_thp_pagecache_bench.c -lsqlite3 -o sqlite_thp_pagecache_bench`
    - Note: this source intentionally does not include `sqlite3.h`.
  - Recommended A/B:
    - Baseline: `echo never > /sys/kernel/mm/transparent_hugepage/enabled` then run with `use_huge=0`
    - THP: `echo madvise > /sys/kernel/mm/transparent_hugepage/enabled` (and make khugepaged aggressive), then run with `use_huge=1`
