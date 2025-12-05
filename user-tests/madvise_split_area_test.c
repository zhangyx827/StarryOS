// madvise_split_area_test.c
//
// Exercise sys_madvise(MADV_HUGEPAGE / MADV_NOHUGEPAGE) on a subrange
// that lies strictly inside a single mapping, so the kernel must split
// the underlying VMA/MemoryArea into [head][middle][tail].
//
// This is a black‑box test: we can't see VMAs directly, but we can at
// least ensure that:
//   - madvise on a middle subrange succeeds (no EINVAL/ENOMEM);
//   - subsequent madvise on the head/tail parts also succeeds;
//   - the mapping remains accessible in all three segments.

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
    const size_t len       = 4UL * 1024 * 1024; // 4 MiB

    printf("[madvise_split_area_test] mmap anonymous region len=%zu\n", len);
    char *base = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        die("mmap");
    }
    printf("  mapping at %p\n", base);

    // Touch all pages once to ensure they are mapped.
    for (size_t i = 0; i < len; i += page_size) {
        base[i] = (char)(i / page_size);
    }

    // Choose a middle subrange that starts/ends strictly inside the mapping,
    // so that [head][middle][tail] are all non‑empty.
    //   head:   [base, mid_start)
    //   middle: [mid_start, mid_start + mid_len)
    //   tail:   [mid_start + mid_len, base + len)
    const size_t mid_len = 2UL * 1024 * 1024; // 2 MiB
    char *mid_start = base + page_size;       // 4K offset into the mapping
    char *mid_end   = mid_start + mid_len;

    if (mid_end > base + len) {
        fprintf(stderr, "[madvise_split_area_test] mid range out of bounds\n");
        munmap(base, len);
        return 1;
    }

    char  *head     = base;
    size_t head_len = (size_t)(mid_start - head);
    char  *tail     = mid_end;
    size_t tail_len = (size_t)((base + len) - tail);

    if (head_len == 0 || tail_len == 0) {
        fprintf(stderr, "[madvise_split_area_test] head or tail is empty, abort\n");
        munmap(base, len);
        return 1;
    }

    printf("[madvise_split_area_test] head=[%p, %p), middle=[%p, %p), tail=[%p, %p)\n",
           head, head + head_len, mid_start, mid_end, tail, tail + tail_len);

    // 1) 标记整段为 NOHUGEPAGE 以清理状态（可选）。
    if (madvise(base, len, MADV_NOHUGEPAGE) != 0) {
        die("madvise(MADV_NOHUGEPAGE, whole)");
    }

    // 2) 只对中间子区间设置 HUGEPAGE。
    //    这一步在内核里需要把原来的 VMA 拆成 head/middle/tail 三段，
    //    否则无法对 middle 单独设置 VM_HUGEPAGE。
    if (madvise(mid_start, mid_len, MADV_HUGEPAGE) != 0) {
        die("madvise(MADV_HUGEPAGE, middle)");
    }
    printf("[madvise_split_area_test] madvise(HUGEPAGE, middle) OK\n");

    // 3) 再对 head / tail 分别调用 MADV_NOHUGEPAGE。
    //    如果 split 行为正确，这里会命中已经拆分好的 head/tail VMA，
    //    不应该报错。
    if (madvise(head, head_len, MADV_NOHUGEPAGE) != 0) {
        die("madvise(MADV_NOHUGEPAGE, head)");
    }
    if (madvise(tail, tail_len, MADV_NOHUGEPAGE) != 0) {
        die("madvise(MADV_NOHUGEPAGE, tail)");
    }
    printf("[madvise_split_area_test] madvise(NO_HUGEPAGE, head/tail) OK\n");

    // 4) 确认 head/middle/tail 都仍然可访问。
    head[0]     ^= 1;
    mid_start[0]^= 1;
    tail[0]     ^= 1;

    if (munmap(base, len) != 0) {
        die("munmap");
    }

    printf("[madvise_split_area_test] PASS\n");
    return 0;
}

