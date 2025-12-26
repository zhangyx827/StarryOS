#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *THP_ENABLED = "/sys/kernel/mm/transparent_hugepage/enabled";
static const char *THP_SHMEM_ENABLED = "/sys/kernel/mm/transparent_hugepage/shmem_enabled";

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void write_expect_einval(const char *path, const char *payload, const char *tag) {
    FILE *f = fopen(path, "r+");
    if (!f) {
        perror("fopen");
        fprintf(stderr, "skip: cannot open %s\n", path);
        exit(77);
    }

    errno = 0;
    if (fseek(f, 0, SEEK_SET) != 0) die("fseek");
    int ret = fprintf(f, "%s", payload);
    if (ret >= 0 && fflush(f) == 0) {
        fprintf(stderr, "%s: expected EINVAL for '%s', but write succeeded\n", tag, payload);
        fclose(f);
        exit(1);
    }

    if (errno != EINVAL) {
        fprintf(stderr, "%s: expected errno=EINVAL(%d), got %d\n", tag, EINVAL, errno);
        fclose(f);
        exit(1);
    }

    fclose(f);
}

int main(void) {
    printf("=== thp_sysfs_invalid_input_test ===\n");

    write_expect_einval(THP_ENABLED, "\n", "enabled-empty");
    write_expect_einval(THP_ENABLED, "bad\n", "enabled-bad");

    write_expect_einval(THP_SHMEM_ENABLED, "\n", "shmem-empty");
    write_expect_einval(THP_SHMEM_ENABLED, "bad\n", "shmem-bad");

    printf("thp_sysfs_invalid_input_test: PASS\n");
    return 0;
}

