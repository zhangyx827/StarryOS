#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdint.h>
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#define THP_SIZE (2UL * 1024 * 1024)
#define NUM_READER_THREADS 3
#define NUM_MUNMAP_THREADS 1
#define ITERATIONS 1000

static const char *THP_ENABLED = "/sys/kernel/mm/transparent_hugepage/enabled";
static const char *KHUGEPAGED_PAGES_COLLAPSED =
    "/sys/kernel/mm/transparent_hugepage/khugepaged/pages_collapsed";

static atomic_int start_flag = 0;
static atomic_int error_count = 0;
static atomic_int stop_readers = 0;
static char *shared_region = NULL;  // 整个 4M 映射
static char *thp_base = NULL;        // 2M 对齐的 THP 区域

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void set_thp_mode(const char *mode) {
    FILE *f = fopen(THP_ENABLED, "r+");
    if (!f) die("fopen THP_ENABLED");
    if (fseek(f, 0, SEEK_SET) != 0) die("fseek");
    if (fprintf(f, "%s\n", mode) < 0) die("fprintf");
    if (fflush(f) != 0) die("fflush");
    fclose(f);
}

static unsigned long read_pages_collapsed(void) {
    FILE *f = fopen(KHUGEPAGED_PAGES_COLLAPSED, "r");
    if (!f) die("fopen pages_collapsed");
    char buf[64] = {0};
    if (!fgets(buf, sizeof(buf), f)) die("fgets");
    fclose(f);
    return strtoul(buf, NULL, 10);
}

// 读线程：不断访问 THP 区域的不同位置
void *reader_thread(void *arg) {
    int id = *(int *)arg;
    unsigned long read_count = 0;

    while (!atomic_load(&start_flag)) {
        usleep(1000);
    }

    while (!atomic_load(&stop_readers)) {
        // 随机访问 2M 区域的各个页
        for (size_t offset = 0; offset < THP_SIZE; offset += 4096) {
            if (atomic_load(&stop_readers)) break;

            // 访问这个页，检查是否能正常读取
            volatile char val = thp_base[offset];
            (void)val;  // 防止编译器优化
            read_count++;

            // 偶尔写一下
            if (read_count % 100 == 0) {
                thp_base[offset] = (char)(read_count & 0xFF);
            }
        }
    }

    printf("[Reader %d] Completed %lu reads\n", id, read_count);
    return NULL;
}

// munmap 线程：反复 unmap 并重新 map THP 的部分区域（触发 split）
void *munmap_thread(void *arg) {
    int id = *(int *)arg;

    while (!atomic_load(&start_flag)) {
        usleep(1000);
    }

    for (int i = 0; i < ITERATIONS; i++) {
        // 在 THP 中间 unmap 64KB（触发 split）
        void *hole = thp_base + 512 * 1024;  // 0.5M 偏移
        size_t hole_len = 64 * 1024;

        printf("[Munmap %d][%d] Unmapping %p len=%zu (should trigger split)\n",
               id, i, hole, hole_len);

        if (munmap(hole, hole_len) != 0) {
            perror("munmap hole");
            atomic_fetch_add(&error_count, 1);
            continue;
        }

        // 短暂延迟，让读线程有机会访问（测试 TLB 一致性）
        usleep(1000);

        // 重新 map 这个区域
        void *new_map = mmap(hole, hole_len, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (new_map == MAP_FAILED) {
            perror("mmap hole");
            atomic_fetch_add(&error_count, 1);
            continue;
        }
        if (new_map != hole) {
            printf("[Munmap %d] MAP_FIXED failed: got %p, expected %p\n",
                   id, new_map, hole);
            atomic_fetch_add(&error_count, 1);
        }

        // 初始化新映射的数据
        memset(new_map, 0xAB, hole_len);
    }

    atomic_store(&stop_readers, 1);
    return NULL;
}

int main(void) {
    printf("[thp_split_race_test] Testing TLB/race conditions during munmap split\n");

    set_thp_mode("always");

    // 分配 4MB
    size_t len = 4 * 1024 * 1024;
    shared_region = mmap(NULL, len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (shared_region == MAP_FAILED) die("mmap");

    if (madvise(shared_region, len, MADV_HUGEPAGE) != 0) die("madvise");

    // 找到 2M 对齐的窗口
    uintptr_t base = (uintptr_t)shared_region;
    uintptr_t end = base + len;
    uintptr_t win = (base + THP_SIZE - 1) & ~(THP_SIZE - 1);
    if (win + THP_SIZE > end) {
        printf("Cannot find 2M-aligned window\n");
        munmap(shared_region, len);
        return 1;
    }

    thp_base = (char *)win;
    printf("[thp_split_race_test] THP region at %p\n", thp_base);

    // 初始化整个 2M 区域
    for (size_t i = 0; i < THP_SIZE; i += 4096) {
        thp_base[i] = (char)(i / 4096);
    }

    // 等待 collapse
    unsigned long prev = read_pages_collapsed();
    printf("[thp_split_race_test] Waiting for THP collapse...\n");
    for (int iter = 0; iter < 30; iter++) {
        sleep(1);
        ((volatile char *)thp_base)[0] ^= 1;
        unsigned long now = read_pages_collapsed();
        if (now > prev) {
            printf("  THP collapse detected (pages_collapsed: %lu -> %lu)\n", prev, now);
            break;
        }
    }

    // 创建线程
    pthread_t threads[NUM_READER_THREADS + NUM_MUNMAP_THREADS];
    int thread_ids[NUM_READER_THREADS + NUM_MUNMAP_THREADS];

    printf("[thp_split_race_test] Starting %d readers + %d munmap threads...\n",
           NUM_READER_THREADS, NUM_MUNMAP_THREADS);

    // 启动读线程
    for (int i = 0; i < NUM_READER_THREADS; i++) {
        thread_ids[i] = i;
        if (pthread_create(&threads[i], NULL, reader_thread, &thread_ids[i]) != 0) {
            die("pthread_create reader");
        }
    }

    // 启动 munmap 线程
    for (int i = 0; i < NUM_MUNMAP_THREADS; i++) {
        int idx = NUM_READER_THREADS + i;
        thread_ids[idx] = i;
        if (pthread_create(&threads[idx], NULL, munmap_thread, &thread_ids[idx]) != 0) {
            die("pthread_create munmap");
        }
    }

    // 启动所有线程
    atomic_store(&start_flag, 1);

    // 等待所有线程完成
    for (int i = 0; i < NUM_READER_THREADS + NUM_MUNMAP_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    int errors = atomic_load(&error_count);
    if (errors > 0) {
        printf("[thp_split_race_test] FAIL: %d errors detected!\n", errors);
    } else {
        printf("[thp_split_race_test] PASS: No errors detected\n");
    }

    munmap(shared_region, len);
    return errors > 0 ? 1 : 0;
}
