#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#define THP_SIZE 1024 * 1024 * 2

int main() {
  int fd = open("/tmp/test", O_CREAT | O_TRUNC | O_WRONLY);
  if (fd < 0) {
    perror("open");
  }

  char* buf = (char*)malloc(sizeof(char) * THP_SIZE);
  memset(buf, 0x3c, THP_SIZE);

  int written = write(fd, buf, THP_SIZE);
  if (written < 0) {
    perror("write");
  }

  close(fd);
  return 0;
}