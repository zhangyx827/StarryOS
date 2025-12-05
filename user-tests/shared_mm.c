// shm_mmap_shared.c（放在 rootfs 或通过现有 user-tests 改一份）
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <unistd.h>

int main() {
    size_t len = 2UL * 1024 * 1024;

    // 方案 A：MAP_SHARED | MAP_ANONYMOUS
    void *p = mmap(NULL, len, PROT_READ|PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
                    
    munmap(p, 4096);

    // 方案 B：SysV shm
    int id = shmget(IPC_PRIVATE, len, IPC_CREAT | 0600);
    p = shmat(id, NULL, 0);

    printf("pid=%d, p=%p\n", getpid(), p);
    for (size_t i = 0; i < len; i += 4096) {
        ((char*)p)[i] = 1;
    }
    return 0;
}