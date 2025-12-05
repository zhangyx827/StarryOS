#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE 15
#endif

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

int main(void) {
    const size_t page_size = 4096;
    const size_t thp_size  = 2UL * 1024 * 1024;  // 2 MiB
    const size_t len       = 4UL * 1024 * 1024;  // 4 MiB

    printf("[madvise_split_test] mmap anonymous region len=%zu\n", len);
    char *base = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        die("mmap");
    }
    printf("  mapping at %p\n", base);

    // 先全部 touch 一下，确保都被映射。
    for (size_t i = 0; i < len; i += page_size) {
        base[i] = (char)(i / page_size);
    }

    // 对齐出一个 2MiB 的窗口，保证完全落在 mapping 中间。
    uintptr_t start = (uintptr_t)base;
    uintptr_t end   = start + len;

    uintptr_t mid_aligned = (start + thp_size - 1) & ~(thp_size - 1);  // 向上 2M 对齐
    if (mid_aligned + thp_size > end) {
        printf("[madvise_split_test] cannot find 2M-aligned window inside mapping, abort\n");
        munmap(base, len);
        return 0;
    }

    char  *mid      = (char *)mid_aligned;
    size_t mid_len  = thp_size;
    char  *head     = base;
    size_t head_len = (size_t)(mid - head);
    char  *tail     = mid + mid_len;
    size_t tail_len = (size_t)((base + len) - tail);

    printf("[madvise_split_test] head=[%p, %p), mid=[%p, %p), tail=[%p, %p)\n",
            head, head + head_len, mid, mid + mid_len, tail, tail + tail_len);

    if (head_len == 0 || tail_len == 0) {
        printf("[madvise_split_test] head/tail too small, abort\n");
        munmap(base, len);
        return 0;
    }

    // 1) 先把整段设成 NOHUGEPAGE，清理状态。
    if (madvise(base, len, MADV_NOHUGEPAGE) != 0) {
        die("madvise(MADV_NOHUGEPAGE, whole)");
    }
    printf("[madvise_split_test] madvise(NO_HUGEPAGE) on whole mapping OK\n");

    // 2) 只对中间那段设置 HUGEPAGE —— 这是触发 split 的关键：
    //    原来一个 VMA，现在中间一段策略不同，如果要精确实现 per‑VMA 策略，就必须拆成三段。
    if (madvise(mid, mid_len, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE, mid)");
    }
    printf("[madvise_split_test] madvise(HUGEPAGE) on middle 2M OK\n");

    // 3) 再分别对 head/tail 做 NOHUGEPAGE，这三个区间互相交错，
    //    如果内核内部是按 VMA 分段维护策略，就会在第一次 mid 上做一次 split，
    //    之后 head/tail 上的 madvise 会各自命中不同的 area。
    if (madvise(head, head_len, MADV_NOHUGEPAGE) != 0) {
        die("madvise(MADV_NOHUGEPAGE, head)");
    }
    if (madvise(tail, tail_len, MADV_NOHUGEPAGE) != 0) {
        die("madvise(MADV_NOHUGEPAGE, tail)");
    }
    printf("[madvise_split_test] madvise(NO_HUGEPAGE) on head/tail OK\n");

    // 简单 touch 一下三段，验证映射都还存在。
    head[0] ^= 1;
    mid[0]  ^= 1;
    tail[0] ^= 1;

    if (munmap(base, len) != 0) {
        die("munmap");
    }

    printf("[madvise_split_test] PASS\n");
    return 0;
}