// Verify that accessing past the current file length via mmap triggers a fault
// (Linux would raise SIGBUS). We install a handler to catch the signal and
// treat it as success; reaching the write past EOF without a fault is a bug.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

static sigjmp_buf fault_env;
static volatile sig_atomic_t got_fault = 0;

static void on_fault(int sig) {
    (void)sig;
    got_fault = 1;
    siglongjmp(fault_env, 1);
}

static int setup_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGBUS, &sa, NULL) != 0) {
        return -1;
    }
    // Some platforms may signal SIGSEGV instead; catch it too.
    if (sigaction(SIGSEGV, &sa, NULL) != 0) {
        return -1;
    }
    return 0;
}

int main(void) {
    if (setup_handler() != 0) {
        perror("sigaction");
        return 1;
    }

    const char *path = "/tmp/mmap_beyond_eof_test.bin";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // File size: 1 page.
    if (ftruncate(fd, PAGE_SIZE) != 0) {
        perror("ftruncate");
        close(fd);
        unlink(path);
        return 1;
    }

    // Map 2 pages; the second page lies beyond EOF.
    void *addr = mmap(NULL, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        unlink(path);
        return 1;
    }

    // Touch within file (should succeed).
    memset(addr, 0xaa, PAGE_SIZE);

    // Touch beyond EOF; should fault.
    volatile uint8_t *p = (uint8_t *)addr + PAGE_SIZE + 128;
    if (sigsetjmp(fault_env, 1) == 0) {
        *p = 0x5a; // Expect SIGBUS/SIGSEGV here
    }

    munmap(addr, 2 * PAGE_SIZE);
    close(fd);
    unlink(path);

    if (got_fault) {
        printf("mmap_beyond_eof_test: PASS (faulted as expected)\n");
        return 0;
    }

    fprintf(stderr, "BUG: write past EOF via mmap did not fault\n");
    return 1;
}
