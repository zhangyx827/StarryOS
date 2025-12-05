// Simple coverage test for sys_madvise(MADV_COLLAPSE).
//
// 参考 Linux 6.1 之后 MADV_COLLAPSE 的行为，主要覆盖这些方面：
//   - addr 必须按页对齐，否则 EINVAL（通用 madvise 约束）；
//   - length == 0 被视为 no-op，直接成功；
//   - 对小于 1 个 huge page 的范围是 no-op（Linux 会“clamp”到
//     hugepage 对齐/长度，不会报错）；
//   - 对显式标记了 MADV_NOHUGEPAGE 的区域，MADV_COLLAPSE 应失败；
//   - 对普通匿名映射、对齐且已填充的范围调用 MADV_COLLAPSE，应成功，
//     映射保持可访问（是否真的折叠成 THP 不在本测试里强行检查）。

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
// MADV_COLLAPSE was added in Linux 6.1. If the host libc is older,
// define the value here to match the kernel uapi.
#define MADV_COLLAPSE 25
#endif

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static int test_zero_length(void) {
    const size_t len = 4UL * 1024 * 1024;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap (zero_length)");
    }

    printf("[madvise_collapse_test] test_zero_length: addr=%p len=0\n", p);

    if (madvise(p, 0, MADV_COLLAPSE) != 0) {
        perror("madvise(MADV_COLLAPSE, len=0)");
        munmap(p, len);
        return 1;
    }

    // Mapping should still be accessible.
    p[0] ^= 1;

    if (munmap(p, len) != 0) {
        die("munmap (zero_length)");
    }
    return 0;
}

static int test_misaligned_addr(void) {
    const size_t len = 2UL * 4096; // 2 pages
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap (misaligned)");
    }

    // Choose an address that is not 4K-aligned.
    char *misaligned = p + 1;
    printf("[madvise_collapse_test] test_misaligned_addr: base=%p misaligned=%p len=%zu\n",
           p, misaligned, len - 1);

    errno = 0;
    int ret = madvise(misaligned, len - 1, MADV_COLLAPSE);
    if (ret != -1) {
        fprintf(stderr,
                "madvise(MADV_COLLAPSE, misaligned) returned %d, expected -1\n",
                ret);
        munmap(p, len);
        return 1;
    }
    if (errno != EINVAL) {
        fprintf(stderr,
                "madvise(MADV_COLLAPSE, misaligned) errno=%d, expected EINVAL(%d)\n",
                errno, EINVAL);
        munmap(p, len);
        return 1;
    }

    // Mapping should still be accessible at the original base.
    p[0] ^= 1;

    if (munmap(p, len) != 0) {
        die("munmap (misaligned)");
    }
    return 0;
}

static int test_valid_range(void) {
    const size_t len = 4UL * 1024 * 1024; // 4 MiB
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap (valid_range)");
    }

    printf("[madvise_collapse_test] test_valid_range: base=%p len=%zu\n", p, len);

    // Touch the mapping so that pages are faulted in.
    for (size_t i = 0; i < len; i += 4096) {
        p[i] = (char)(i / 4096);
    }

    if (madvise(p, len, MADV_COLLAPSE) != 0) {
        perror("madvise(MADV_COLLAPSE, valid_range)");
        munmap(p, len);
        return 1;
    }

    // Mapping should remain accessible after MADV_COLLAPSE.
    for (size_t i = 0; i < len; i += 4096) {
        p[i] ^= 1;
    }

    if (munmap(p, len) != 0) {
        die("munmap (valid_range)");
    }
    return 0;
}

// addr/page 对齐，但长度不足 1 个 huge page：Linux 里 MADV_COLLAPSE 会
// 自动“clamp”到 2M 对齐/长度，如果范围内没有任何 2M 窗口，通常视为 no-op 成功。
static int test_small_range_no_huge(void) {
    const size_t len = 1UL * 1024 * 1024; // 1 MiB < 2 MiB
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap (small_range)");
    }

    printf("[madvise_collapse_test] test_small_range_no_huge: base=%p len=%zu\n",
           p, len);

    // 触发缺页，让这一段至少有一些页是“热”的
    for (size_t i = 0; i < len; i += 4096) {
        p[i] = (char)(i / 4096);
    }

    // 期望行为：返回 0（no-op），映射仍可访问。
    if (madvise(p, len, MADV_COLLAPSE) != 0) {
        perror("madvise(MADV_COLLAPSE, small_range)");
        munmap(p, len);
        return 1;
    }

    p[0] ^= 1;

    if (munmap(p, len) != 0) {
        die("munmap (small_range)");
    }
    return 0;
}

// 对显式 NOHUGEPAGE 区域的 MADV_COLLAPSE：Linux 中 VM_NOHUGEPAGE 会禁止
// THP，MADV_COLLAPSE 应失败（通常是 EINVAL/EAGAIN 之类的错误码）。
static int test_nohugepage_disallows_collapse(void) {
    const size_t len = 4UL * 1024 * 1024; // 4 MiB
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        die("mmap (nohuge)");
    }

    printf("[madvise_collapse_test] test_nohugepage_disallows_collapse: base=%p len=%zu\n",
           p, len);

    // 显式禁止 THP
    if (madvise(p, len, MADV_NOHUGEPAGE) != 0) {
        perror("madvise(MADV_NOHUGEPAGE)");
        munmap(p, len);
        return 1;
    }

    // 再尝试 collapse：预期应失败（至少不能返回 0）
    errno = 0;
    int ret = madvise(p, len, MADV_COLLAPSE);
    if (ret == 0) {
        fprintf(stderr,
                "madvise(MADV_COLLAPSE) on NOHUGEPAGE region unexpectedly succeeded\n");
        munmap(p, len);
        return 1;
    }

    // 映射仍应可访问
    p[0] ^= 1;

    if (munmap(p, len) != 0) {
        die("munmap (nohuge)");
    }
    return 0;
}

int main(void) {
    int rc = 0;

    rc |= test_zero_length();
    rc |= test_misaligned_addr();
    rc |= test_valid_range();
    rc |= test_small_range_no_huge();
    rc |= test_nohugepage_disallows_collapse();

    if (rc == 0) {
        printf("[madvise_collapse_test] PASS\n");
    } else {
        printf("[madvise_collapse_test] FAIL (rc=%d)\n", rc);
    }
    return rc;
}
