#pragma once

#include <sys/epoll.h>

extern int epollfd;

typedef struct epollcb {
  void (*cb)(int fd, int events, void *opaque);
  int fd;
  void *opaque;
} epollcb_t;
