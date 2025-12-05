// file_mmap_backend_test.c
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define PAGE_4K   4096UL
#define FILE_SIZE (2UL * 1024 * 1024)  // 2MiB

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

int main(void) {
    const char *path = "/tmp/file_backend_test.dat";
    int fd;

    /* 1. 打开一个普通文件（优先 /tmp，不行就当前目录） */
    fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open /tmp failed, fallback to ./file_backend_test.dat");
        path = "file_backend_test.dat";
        fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) {
            die("open");
        }
    }

    if (ftruncate(fd, FILE_SIZE) != 0) {
        die("ftruncate");
    }

    /* 2. 对这个文件做 MAP_SHARED mmap —— 这一步会创建 FileBackend 映射 */
    void *addr = mmap(NULL, FILE_SIZE,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED,   // 关键：MAP_SHARED + fd > 0 → FileBackend
                      fd, 0);
    if (addr == MAP_FAILED) {
        die("mmap");
    }

    printf("mmap(FileBackend) => addr=%p, len=%zu, path=%s\n",
            addr, (size_t)FILE_SIZE, path);

    /* 3. 触发 page fault，从而走到 FileBackend::populate() */
    char *p = (char *)addr;
    unsigned seed = 0x42;
    for (size_t i = 0; i < FILE_SIZE; i += PAGE_4K) {
        p[i] = (char)((i / PAGE_4K) ^ seed);
    }

    /* 4. 刷回文件，验证 FileBackend 写回是否生效 */
    if (msync(addr, FILE_SIZE, MS_SYNC) != 0) {
        die("msync");
    }

    if (munmap(addr, FILE_SIZE) != 0) {
        die("munmap");
    }
    close(fd);

    /* 5. 重新打开文件校验内容 */
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        die("open for verify");
    }

    uint8_t buf[PAGE_4K];
    for (size_t page = 0; page < FILE_SIZE / PAGE_4K; ++page) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n != (ssize_t)sizeof(buf)) {
            die("read");
        }
        uint8_t expected = (uint8_t)(page ^ seed);
        if (buf[0] != expected) {
            fprintf(stderr,
                    "verify failed at page %zu: got %#x expected %#x\n",
                    page, buf[0], expected);
            return 1;
        }
    }

    printf("FileBackend mmap/read/write/flush OK\n");
    close(fd);
    return 0;
}