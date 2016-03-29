#include <sys/socket.h>
#include <sys/un.h>

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <syslog.h>

#include "logging.h"
#include "io.h"

#define LOG_ROTATE_SIZE 500000

static int logdir;
static int syslogfd = -1;
static int logsize;


pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char months[12][4] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static char *facilities[] = {
  "kernel",
  "user",
  "mail",
  "daemon",
  "security",
  "syslog",
  "lps",
  "news",
  "uucp",
  "clock",
  "security",
  "ftp",
  "ntp",
  "audit",
  "alert",
  "clock",
  "local0",
  "local1",
  "local2",
  "local3",
  "local4",
  "local5",
  "local6",
  "local7",
};



static void
writelog(const char *system, int pri, const char *msg)
{
  char tmp[2048];
  time_t now;
  time(&now);
  struct tm tm;
  localtime_r(&now, &tm);

  if(system == NULL) {
    const int fac = pri >> 3;
    if(fac > 23) {
      system = "unknown";
    } else {
      system = facilities[fac];
    }
  }

  snprintf(tmp, sizeof(tmp), "%s %2d %02d:%02d:%02d %s: %s\n",
           months[tm.tm_mon], tm.tm_mday,
           tm.tm_hour, tm.tm_min, tm.tm_sec,
           system, msg);

  pthread_mutex_lock(&log_mutex);


  if(logsize >= LOG_ROTATE_SIZE) {
    close(syslogfd);
    renameat(logdir, "syslog", logdir, "syslog.0");
    syslogfd = -1;
  }

  if(syslogfd == -1) {
    syslogfd = openat(logdir, "syslog", O_TRUNC | O_CREAT | O_WRONLY, 0644);
    if(syslogfd != -1) {
      logsize = 0;
    }
  }

  if(syslogfd != -1) {
    int r = write(syslogfd, tmp, strlen(tmp));
    if(r < 0)
      printf("syslog error -- %s", strerror(errno));
    else
      logsize += r;
  }

  pthread_mutex_unlock(&log_mutex);
}



void
trace(int level, const char *fmt, ...)
{
  char tmp[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);

  //  printf("init: %s\n", tmp);

  writelog("init", level, tmp);
}



/***************************************************************************
 * klog input
 ***************************************************************************/
static void
klog_input(int fd, int events, void *opaque)
{
  static char buf[1024];
  static int len;
  int r = read(fd, buf + len, sizeof(buf) - len);

  if(r < 0) {
    printf("klog read error -- %s\n", strerror(errno));
    exit(1);
  }
  len += r;
 again:
  for(int i = 0; i < len; i++) {
    if(buf[i] == '\n') {
      buf[i] = 0;

      int pri = LOG_DEBUG;
      char *m = buf;

      pri = LOG_INFO;
      if(*m == '<') {
        m++;
        if(*m)
          pri = strtoul(m, &m, 10);
        if(*m == '>')
          m++;
      }

      writelog(NULL, pri, m);

      i++;
      memmove(buf, buf + i, len - i);
      len -= i;
      goto again;
    }
  }
}

static void
klog_open(const char *path)
{
  static epollcb_t klog_epollcb = { klog_input };

  if((klog_epollcb.fd = open(path, O_RDONLY | O_CLOEXEC)) == -1) {
    fprintf(stderr, "Failed to open klog %s -- %s", path, strerror(errno));
    exit(1);
  }
  epoll_ctl(epollfd, EPOLL_CTL_ADD, klog_epollcb.fd,
            &(struct epoll_event) { EPOLLIN, { &klog_epollcb}});
}


/***************************************************************************
 * syslog input
 ***************************************************************************/
static void
devlog_input(int fd, int events, void *opaque)
{
  char buf[4096];
  int r = read(fd, buf, sizeof(buf) - 1);

  if(r < 0) {
    printf("devlog read error -- %s\n", strerror(errno));
    exit(1);
  }
  buf[r] = 0;
  while(r > 0 && buf[r - 1] < 32)
    buf[--r] = 0;

  int pri = LOG_DEBUG;
  char *m = buf;

  pri = LOG_INFO;
  if(*m == '<') {
    m++;
    if(*m)
      pri = strtoul(m, &m, 10);
    if(*m == '>')
      m++;
  }

  if(strlen(m) > 16 && m[3] == ' ' && m[6] == ' ' && m[9] == ':') {
    m += 16;
  }

  writelog(NULL, pri, m);
}

static void
devlog_open(const char *path)
{
  unlink(path);

  static epollcb_t devlog_epollcb = { devlog_input };

  if((devlog_epollcb.fd = socket(AF_LOCAL, SOCK_DGRAM|SOCK_CLOEXEC, 0)) == -1) {
    fprintf(stderr, "Failed to open socket %s -- %s", path, strerror(errno));
    exit(1);
  }
  struct sockaddr_un sun = {.sun_family = AF_LOCAL};
  strcpy(sun.sun_path, path);
  if(bind(devlog_epollcb.fd, (struct sockaddr *)&sun, sizeof(sun)) == -1) {
    fprintf(stderr, "Failed to bind socket %s -- %s", path, strerror(errno));
    exit(1);
  }
  epoll_ctl(epollfd, EPOLL_CTL_ADD, devlog_epollcb.fd,
            &(struct epoll_event) { EPOLLIN, { &devlog_epollcb}});
}


void
logging_init(int fd)
{
  logdir = fd;
  klog_open("/root/proc/kmsg");
  devlog_open("/root/dev/log");
}
