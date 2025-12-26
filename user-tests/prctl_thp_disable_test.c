#define _GNU_SOURCE
#include <errno.h>
#include <linux/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <linux/prctl.h>


static void expect_fail(int ret, int err, const char *what) {
    if (ret != -1 || errno != err) {
        fprintf(stderr, "%s: expected ret=-1 errno=%d, got ret=%d errno=%d\n",
                what, err, ret, errno);
        exit(1);
    }
}

static void expect_ok(int ret, const char *what) {
    if (ret != 0) {
        fprintf(stderr, "%s: expected 0, got %d (errno=%d)\n", what, ret, errno);
        exit(1);
    }
}

static void expect_full_diable(int ret, const char *what) {
    if (ret != 1) {
        fprintf(stderr, "%s: expected 0, got %d (errno=%d)\n", what, ret, errno);
        exit(1);
    }
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== prctl_thp_disable_test ===\n");

    int ret;

    // Invalid extra args on SET
    errno = 0;
    ret = prctl(PR_SET_THP_DISABLE, 1, 0, 1, 0);
    expect_fail(ret, EINVAL, "SET extra arg4");

    // Invalid arg3 on SET (not 0 or PR_THP_DISABLE_EXCEPT_ADVISED)
    errno = 0;
    ret = prctl(PR_SET_THP_DISABLE, 1, 0x1234, 0, 0);
    expect_fail(ret, EINVAL, "SET invalid arg3");

    // Invalid extra args on GET
    errno = 0;
    ret = prctl(PR_GET_THP_DISABLE, 1, 0, 0, 0);
    expect_fail(ret, EINVAL, "GET extra arg2");

    // Valid: set full disable
    errno = 0;
    ret = prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
    expect_ok(ret, "SET disable");

    errno = 0;
    ret = prctl(PR_GET_THP_DISABLE, 0, 0, 0, 0);
    expect_full_diable(ret, "GET disable");
    if (ret != 1) {
        fprintf(stderr, "GET after disable: expected 1, got %d\n", ret);
        exit(1);
    }
    printf("  mode=1 OK\n");

    // Valid: set EXCEPT_ADVISED (mode 3)
    // errno = 0;
    // ret = prctl(PR_SET_THP_DISABLE, 1, 2, 0, 0);
    // expect_ok(ret, "SET except-advised");

    errno = 0;

    // Valid: re-enable (mode 0)
    ret = prctl(PR_SET_THP_DISABLE, 0, 0, 0, 0);
    expect_ok(ret, "SET enable");

    errno = 0;
    ret = prctl(PR_GET_THP_DISABLE, 0, 0, 0, 0);
    expect_ok(ret, "GET enable");
    if (ret != 0) {
        fprintf(stderr, "GET after enable: expected 0, got %d\n", ret);
        exit(1);
    }
    printf("  mode=0 OK\n");

    printf("prctl_thp_disable_test: PASS\n");
    return 0;
}
