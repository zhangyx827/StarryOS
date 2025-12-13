// Exercise overlapping collapse attempts with varying mmap offsets to create
// random-looking huge page placement. We create multiple mappings at different
// offsets, collapse some to huge pages, then attempt to collapse overlapping
// windows. We verify data integrity; collapse may fail with EINVAL/ENOTSUP.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define HUGE_SIZE (2UL * 1024 * 1024)

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void fill_pattern(void *addr, size_t len, uint8_t pat) {
    memset(addr, pat, len);
}

static void verify_pattern(uint8_t *addr, size_t len, uint8_t pat, const char *tag) {
    for (size_t i = 0; i < len; i++) {
        if (addr[i] != pat) {
            fprintf(stderr, "%s: byte %zu got %#x expected %#x\n", tag, i, addr[i], pat);
            exit(1);
        }
    }
}

static void try_collapse(void *addr, size_t len, const char *tag) {
    if (madvise(addr, len, MADV_COLLAPSE) != 0) {
        int e = errno;
        if (e != EINVAL && e != ENOTSUP && e != ENOMEM) {
            fprintf(stderr, "%s: madvise failed errno=%d\n", tag, e);
            exit(1);
        }
        printf("%s: madvise errno=%d (acceptable)\n", tag, e);
    } else {
        printf("%s: madvise success\n", tag);
    }
}

static void *map_at(int fd, off_t offset, size_t len, int prot, int flags) {
    // Ensure offset and length are 4KiB aligned.
    if ((offset % PAGE_SIZE) != 0 || (len % PAGE_SIZE) != 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    void *addr = mmap(NULL, len, prot, flags, fd, offset);
    return addr;
}

int main(void) {
    const char *path = "/tmp/thp_overlap_huge_cache_test.bin";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) die("open");

    // Prepare 6 MiB file.
    size_t file_size = 3 * HUGE_SIZE;
    if (ftruncate(fd, file_size) != 0) die("ftruncate");

    // Map three regions with different offsets to create varied placement.
    void *map0 = map_at(fd, 0, HUGE_SIZE * 2, PROT_READ | PROT_WRITE, MAP_SHARED);
    if (map0 == MAP_FAILED) die("mmap0");
    // Use 512KiB offset to create overlap and intentionally test misalignment.
    void *map1 = map_at(fd, HUGE_SIZE / 2, HUGE_SIZE * 2, PROT_READ | PROT_WRITE, MAP_SHARED);
    if (map1 == MAP_FAILED) die("mmap1");
    void *map2 = map_at(fd, HUGE_SIZE, HUGE_SIZE * 2, PROT_READ | PROT_WRITE, MAP_SHARED);
    if (map2 == MAP_FAILED) die("mmap2");

    // Fill patterns in each mapped window.
    fill_pattern(map0, HUGE_SIZE, 0x11);
    fill_pattern(map1, HUGE_SIZE, 0x22);
    fill_pattern(map2, HUGE_SIZE, 0x33);

    // Collapse first region aligned at 0.
    try_collapse(map0, HUGE_SIZE * 2, "collapse map0");
    // Collapse second region at offset 1MiB (misaligned w.r.t 2MiB).
    try_collapse(map1, HUGE_SIZE * 2, "collapse map1");
    // Collapse third region aligned at 2MiB.
    try_collapse(map2, HUGE_SIZE * 2, "collapse map2");

    // Fork to simulate reuse of cache in another process.
    pid_t pid = fork();
    if (pid < 0) die("fork");

    if (pid == 0) {
        // Child attempts overlapping collapse over [1MiB, 3MiB) which spans map1 and map2.
        // offset=1MiB is 4KiB-aligned but not 2MiB-aligned; collapse may fail gracefully.
        void *overlap1 = (uint8_t *)map1; // starts at 1MiB
        try_collapse(overlap1, HUGE_SIZE, "child overlap1");
        // Another overlap covering tail of map0 and head of map1: [0.5MiB, 2.5MiB)
        void *overlap2 = (uint8_t *)map0 + HUGE_SIZE / 2;
        try_collapse(overlap2, HUGE_SIZE, "child overlap2");

        // Verify patterns remain intact in their respective regions.
        verify_pattern(map0, HUGE_SIZE / 2, 0x11, "map0 head");
        verify_pattern((uint8_t *)map0 + HUGE_SIZE / 2, HUGE_SIZE / 2, 0x22, "map0 tail overlaps map1");
        verify_pattern(map2, HUGE_SIZE, 0x33, "map2");

        munmap(map0, HUGE_SIZE);
        munmap(map1, HUGE_SIZE);
        munmap(map2, HUGE_SIZE);
        close(fd);
        printf("thp_overlap_huge_cache_test: PASS\n");
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    int ret = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child failed status=%d\n", status);
        ret = 1;
    }

    munmap(map0, HUGE_SIZE);
    munmap(map1, HUGE_SIZE);
    munmap(map2, HUGE_SIZE);
    close(fd);
    unlink(path);
    return ret;
}
