// Stress test for MADV_COLLAPSE running concurrently with heavy page faults.
//
// Motivation:
//   - In a Linux-like implementation, MADV_COLLAPSE may need to "fault in"
//     non-resident pages while holding mm/aspace locks.
//   - If the kernel ever calls into the fault-in path in a bad lock context,
//     it can deadlock with normal user page faults or copy-from-user paths.
//
// This test exercises:
//   - A large anonymous mapping that is repeatedly collapsed with
//     madvise(MADV_COLLAPSE).
//   - Multiple worker threads that constantly touch that mapping to trigger
//     demand faults, and also exercise copy-from-user (vm_read_slice style)
//     via write(2).
//
// Expected behaviour on StarryOS today:
//   - MADV_COLLAPSE may succeed or fail depending on kernel support/heuristics,
//     but the test must always complete and the mapping must remain usable.
//   - Any deadlock in the interaction between MADV_COLLAPSE and page faults
//     will manifest as the test hanging.

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
// Linux value; StarryOS wires this through to sys_madvise(MADV_COLLAPSE).
#define MADV_COLLAPSE 25
#endif

// Use a 4 MiB region so we have at least two THP-sized windows.
#define REGION_LEN   (4UL * 1024 * 1024)
#define PAGE_4K      4096UL
#define NUM_WORKERS  4
#define COLLAPSE_ITERS  2000

static char *g_region;
static atomic_int g_start_flag = 0;
static atomic_int g_stop_flag = 0;
static atomic_int g_error_flag = 0;

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

// Worker thread: constantly fault-in / touch the shared mapping and exercise
// copy-from-user by writing the first 4 KiB to stdout.
static void *faulting_worker(void *arg) {
    (void)arg;

    // Wait until main thread signals start.
    while (!atomic_load(&g_start_flag)) {
        usleep(1000);
    }

    unsigned long iter = 0;
    while (!atomic_load(&g_stop_flag)) {
        // Touch each 4 KiB page to trigger demand paging on first pass.
        for (size_t off = 0; off < REGION_LEN; off += PAGE_4K) {
            g_region[off] ^= (char)(iter & 1);
        }

        // Exercise the vm_read_slice/access_user_memory path in the kernel by
        // passing our mapping as a write() buffer. We don't care about the
        // actual output; this is just to drive fault-in from inside syscalls.
        ssize_t n = write(STDOUT_FILENO, g_region, PAGE_4K);
        if (n < 0) {
            perror("[thp_collapse_faultin_race_test] worker write");
            atomic_store(&g_error_flag, 1);
            break;
        }

        iter++;
        sched_yield();
    }

    return NULL;
}

int main(void) {
    g_region = mmap(NULL, REGION_LEN,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS,
                    -1, 0);
    if (g_region == MAP_FAILED) {
        die("mmap");
    }

    printf("[thp_collapse_faultin_race_test] region=%p len=%zu\n",
           g_region, (size_t)REGION_LEN);

    // Spawn worker threads that will generate page faults and copy-from-user
    // activity on the same address space.
    pthread_t threads[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (pthread_create(&threads[i], NULL, faulting_worker, NULL) != 0) {
            die("pthread_create");
        }
    }

    atomic_store(&g_start_flag, 1);

    // In the main thread, repeatedly invoke MADV_COLLAPSE on the region.
    // We don't assert on errno here: some configurations may legally reject
    // collapse (e.g. small ranges or NOHUGEPAGE-like policies). What we want
    // to detect is a hang or kernel crash.
    for (int i = 0; i < COLLAPSE_ITERS; i++) {
        int ret = madvise(g_region, REGION_LEN, MADV_COLLAPSE);
        if (ret != 0) {
            int err = errno;
            fprintf(stderr,
                    "[thp_collapse_faultin_race_test] madvise(MADV_COLLAPSE) "
                    "failed at iter=%d, errno=%d\n",
                    i, err);
            // Do not treat this as fatal for now; just stop collapsing and
            // let workers drain. The key property is that the call returns.
            break;
        }
    }

    atomic_store(&g_stop_flag, 1);
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(threads[i], NULL);
    }

    // Final sanity: mapping should still be usable.
    g_region[0] ^= 1;

    if (munmap(g_region, REGION_LEN) != 0) {
        die("munmap");
    }

    if (atomic_load(&g_error_flag)) {
        printf("[thp_collapse_faultin_race_test] FAIL (worker error)\n");
        return 1;
    }

    printf("[thp_collapse_faultin_race_test] PASS (no deadlock observed)\n");
    return 0;
}

