#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

// 大页面通常是 2MB
#define HPAGE_SIZE (2 * 1024 * 1024)

/**
 * 辅助函数：读取 /proc/[pid]/smaps
 * 对于文件后端的大页，我们关注 "ShmemPmdMapped" 或 "FilePmdMapped" 字段。
 * 这表示该区域有多少内存是被 PMD (大页表项) 映射的。
 */
void check_thp_status(const char *prefix, void *addr) {
    char cmd[512];
    char buffer[1024];
    FILE *fp;

    // 搜索包含我们地址的 smaps 范围，并查找 ShmemPmdMapped
    // ShmemPmdMapped 是 tmpfs/shmem 上大页映射的标准指示器
    sprintf(cmd, "grep -A 20 \"%lx-\" /proc/%d/smaps | grep -E \"ShmemPmdMapped|FilePmdMapped\"", 
            (unsigned long)addr, getpid());

    printf("[%s PID %d] Checking Huge Page status:\n", prefix, getpid());
    fp = popen(cmd, "r");
    if (fp) {
        int found = 0;
        while (fgets(buffer, sizeof(buffer), fp) != NULL) {
            printf("    %s --> %s", prefix, buffer);
            found = 1;
        }
        if (!found) {
            printf("    %s --> (No PmdMapped entry found - likely 4kB pages)\n", prefix);
        }
        pclose(fp);
    }
}

int main() {
    int pipefd[2];
    int pipefd2[2];
    int fd;
    void *parent_map_addr;
    void *child_map_addr;
    
    // 1. 创建管道用于同步
    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }
    
    if (pipe(pipefd2) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }
    // 2. 创建一个 memfd (内存文件)，存在于 RAM (tmpfs) 中
    // 这模拟了一个文件，但允许我们使用 tmpfs 的大页特性
    fd = open("/tmp/test", O_RDWR);
    if (fd == -1) {
        perror("memfd_create");
        exit(EXIT_FAILURE);
    }

    // 设置文件大小为 2MB
    if (ftruncate(fd, HPAGE_SIZE * 2) == -1) {
        perror("ftruncate");
        exit(EXIT_FAILURE);
    }

    // 3. 父进程映射文件 (MAP_SHARED)
    // 必须对齐地址，虽然 mmap 通常会自动处理，但这是为了保险
    parent_map_addr = mmap(NULL, HPAGE_SIZE * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (parent_map_addr == MAP_FAILED) {
        perror("mmap parent");
        exit(EXIT_FAILURE);
    }

    memset(parent_map_addr, 0xAA, HPAGE_SIZE * 2); // 填满 4MB

    // 4. 建议内核使用大页面
    // 注意：这取决于系统设置 (/sys/kernel/mm/transparent_hugepage/shmem_enabled)
    if (madvise(parent_map_addr, HPAGE_SIZE * 2, MADV_COLLAPSE) == -1) {
        perror("madvise");
        // 继续尝试，虽然这里报错可能意味着内核不支持
    }

    // 5. 关键：写入数据以触发 Page Fault 和物理内存分配
    printf("[Parent] Writing to file-backed memory to trigger THP allocation...\n");


    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        exit(EXIT_FAILURE);
    }

    if (pid == 0) {
        /* ---------------- 子进程 ---------------- */
        close(pipefd[1]); // 关闭写端
        close(pipefd2[0]);
        // 6. 子进程：使用相同的文件描述符创建自己的 Create New Mapping
        // 注意：这里不是用 inherited COW mapping，而是显式 mmap 同一个 fd
        child_map_addr = mmap(NULL, HPAGE_SIZE * 2, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
        if (child_map_addr == MAP_FAILED) {
            perror("mmap child");
            exit(EXIT_FAILURE);
        }

        printf("[Child ] Mapped same file at %p. Waiting for Parent's destructive action...\n", child_map_addr);
        write(pipefd2[1], "X", 1);

        

        // 此时子进程应该也看到大页，因为底层的 Page Cache 是大页

        // 等待父进程破坏页面
        char buf;
        read(pipefd[0], &buf, 1); 
        
        printf("\n[Child ] Received signal. Checking memory status again...\n");
        char* p = (char*)child_map_addr;
        for (int i = 0; i < HPAGE_SIZE * 2; i += 4096) {
          char c = p[i];
        }


        // 验证数据：中间被挖洞的地方应该是 0，其他地方是 0xAA
        p = (unsigned char *)child_map_addr;
        printf("[Child ] Data at offset 0: 0x%x (Expected 0xaa)\n", p[0]);
        // 这里的 1024 * 1024 是 1MB 处
        printf("[Child ] Data at offset 1MB (Punch Hole): 0x%x (Expected 0x00)\n", p[1024 * 1024]);
        close(pipefd[0]);
        exit(EXIT_SUCCESS);

    } else {
        /* ---------------- 父进程 ---------------- */
        close(pipefd[0]); // 关闭读端
        close(pipefd2[1]); //

        char buf;
        read(pipefd2[0], &buf, 1);
        printf("\n[Parent] Now child has mapped the file\n");
        
        printf("\n[Parent] Now Punching a Hole in the file (simulating partial unmap)...\n");
        printf("[Parent] Removing 4KB at offset 1MB forces the kernel to SPLIT the huge page shared by everyone.\n");

        if (munmap(parent_map_addr + 4096 * 200, 4096 * 3) == -1) {
            perror("munmap");
            exit(EXIT_FAILURE);
        }

        // 通知子进程
        write(pipefd[1], "X", 1);
        close(pipefd[1]);

        wait(NULL); 
        
        // 清理
        munmap(parent_map_addr, HPAGE_SIZE * 2);
        close(fd);
    }

    return 0;
}
