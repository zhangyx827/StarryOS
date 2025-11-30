// Simple test for /sys/kernel/mm/transparent_hugepage/enabled
// Build for RISC-V:
//   riscv64-linux-gnu-gcc -O2 -static thp_sysfs_test.c -o thp_sysfs_test

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *THP_ENABLED = "/sys/kernel/mm/transparent_hugepage/enabled";

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

int main(void) {
    const char *modes[] = {"always", "madvise", "never"};
    char buf[64];

    FILE *f = fopen(THP_ENABLED, "r+");
    if (!f) {
        die("fopen");
    }

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        const char *mode = modes[i];

        if (fseek(f, 0, SEEK_SET) != 0) {
            die("fseek write");
        }
        if (fprintf(f, "%s\n", mode) < 0) {
            die("fprintf");
        }
        if (fflush(f) != 0) {
            die("fflush");
        }

        if (fseek(f, 0, SEEK_SET) != 0) {
            die("fseek read");
        }
        memset(buf, 0, sizeof(buf));
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        if (ferror(f)) {
            die("fread");
        }

        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
            buf[--n] = '\0';
        }

        if (strcmp(buf, mode) != 0) {
            fprintf(stderr, "Mismatch: wrote \"%s\", read \"%s\"\n", mode, buf);
            return 1;
        } else {
            printf("OK: mode \"%s\" round-tripped\n", mode);
        }
    }

    fclose(f);
    printf("thp_sysfs_test: PASS\n");
    return 0;
}

