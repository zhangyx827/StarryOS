#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void on_fault(int sig, siginfo_t *info, void *ucontext) {
    (void)ucontext;
    printf("caught signal %s at address %p (si_code=%d)\n", strsignal(sig), info->si_addr,
           info->si_code);
    _exit(0);
}

int main(void) {
    const size_t size = 4096;
    const size_t total = size * 2;  // allocate an extra page we can unmap as a guard

    // Make stdout unbuffered so we see output even if we exit from a signal handler.
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction sa = {
        .sa_sigaction = on_fault,
        .sa_flags = SA_SIGINFO,
    };

    if (sigaction(SIGSEGV, &sa, NULL) != 0 || sigaction(SIGBUS, &sa, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    void *addr = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n",
                (errno == EINVAL)
                    ? "MAP_SHARED with fd=-1 requires MAP_ANONYMOUS (EINVAL)"
                    : strerror(errno));
        return 1;
    }

    // Carve out a guard page so that an overrun reliably faults.
    if (munmap((char *)addr + size, size) != 0) {
        perror("munmap");
        return 1;
    }

    printf("mapped %zu-byte region at %p (MAP_SHARED | MAP_ANONYMOUS, fd=-1); unmapped the next page as guard\n",
           size, addr);

    char *overrun = (char *)addr + size + 16;  // deliberately past the mapping
    printf("writing past the mapping to %p ...\n", overrun);
    *overrun = 0xAA;  // expected to fault here

    printf("write unexpectedly succeeded\n");
    munmap(addr, size);
    return 0;
}
