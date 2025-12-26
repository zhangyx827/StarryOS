#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#define THP_SIZE 1024 * 1024 * 2
#define FILE_SIZE 1024 * 1024 * 4

int main() {
  int fd = open("./test", O_CREAT | O_TRUNC | O_WRONLY);
  if (fd < 0) {
    perror("open");
  }

  char* buf = (char*)malloc(sizeof(char) * FILE_SIZE);
  memset(buf, 0x3c, FILE_SIZE);

  int written = write(fd, buf, FILE_SIZE);
  if (written < 0) {
    perror("write");
  }

  close(fd);
  return 0;
}