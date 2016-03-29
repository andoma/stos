#pragma once

#include <time.h>
#include <sys/stat.h>

#define ARRAYSIZE(x) (sizeof(x) / sizeof(x[0]))





time_t monotime(void);

int64_t hirestime(void);

mode_t getmode(const char *path);

void unmount(const char *path);

void remount(const char *path, int flags);

void mount_or_panic(const char *source, const char *target,
                    const char *filesystemtype, unsigned long mountflags,
                    const void *data);

typedef struct loopmount {
  char devpath[128];
  int loopfd;
} loopmount_t;

int loopmount(const char *image, const char *mountpoint,
              const char *fstype, int mountflags, int ro,
              loopmount_t *lm);

void loopunmount(loopmount_t *lm, const char *mountpath);

void mount_sqfs_or_panic(const char *source, const char *target);

void run_detached_thread(void *(*fn)(void *), void *aux);

void writefile(const char *path, const void *data, int len);

void process_status_to_string(char *dst, size_t dstlen, int status);
