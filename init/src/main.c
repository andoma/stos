#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/reboot.h>

#include <linux/loop.h>

#include <syslog.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <signal.h>

#include <pthread.h>

#include "util.h"
#include "logging.h"
#include "io.h"
#include "main.h"

int epollfd;

#define TASK_STATUS_INACTIVE 0
#define TASK_STATUS_RUNNING  1
#define TASK_STATUS_EXITED   2

int reboot_action; // REBOOT_ACTION_ -defines (0 is restart)
int respawn = 1;

static int shutdown_state;
static int64_t shutdown_timeout;

LIST_HEAD(task_list, task);

pthread_mutex_t task_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t task_cond = PTHREAD_COND_INITIALIZER;

static struct task_list running_tasks;






/**
 *
 */
static void
openconsole(const char *path)
{
  int console = open(path, O_RDWR | O_CLOEXEC);
  if(console == -1) {
    printf("Unable to open %s for console -- %s", path, strerror(errno));
    exit(127);
  }

  dup2(console, 0);
  dup2(console, 1);
  dup2(console, 2);
  if(console > 2)
    close(console);
}



static void
sigchld_handler(int sig)
{
}

static const struct sigaction sigchld_sa = {
  .sa_handler = sigchld_handler,
  .sa_flags = SA_NOCLDSTOP
};

static void
sigusr1_handler(int sig)
{
  if(!shutdown_state) {
    reboot_action = REBOOT_ACTION_HALT;
    shutdown_state = 1;
  }
  respawn = 0;
}

static const struct sigaction sigusr1_sa = {
  .sa_handler = sigusr1_handler,
};

static void
sigusr2_handler(int sig)
{
  respawn = 0;
}

static const struct sigaction sigusr2_sa = {
  .sa_handler = sigusr2_handler,
};

static void
sigterm_handler(int sig)
{
  if(!shutdown_state)
    shutdown_state = 1;
  respawn = 0;
}

static const struct sigaction sigterm_sa = {
  .sa_handler = sigterm_handler,
};



static int
str_tokenize(char *buf, char **vec, int vecsize, int delimiter)
{
  int n = 0;

  while(1) {
    while((*buf > 0 && *buf < 33) || *buf == delimiter)
      buf++;
    if(*buf == 0)
      break;
    vec[n++] = buf;
    if(n == vecsize)
      break;
    while(*buf > 32 && *buf != delimiter)
      buf++;
    if(*buf == 0)
      break;
    *buf = 0;
    buf++;
  }
  return n;
}


/**
 *
 */
static void
task_start(task_t *t)
{
  trace(LOG_DEBUG, "launching: %s", t->t_cmd);

  pid_t pid = fork();
  if(pid == -1) {
    printf("fork() -- %s", strerror(errno));
    exit(1);
  }

  if(pid == 0) {
    sigset_t sigmask;
    sigfillset(&sigmask);
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);

    if(t->t_flags & TASK_F_BACKGROUND) {
      openconsole("/dev/null");
      setsid();
    } else if(t->t_flags & TASK_F_TTY) {
      ioctl(0, TIOCNOTTY, 0);
      setsid();
      openconsole("/dev/console");
      ioctl(0, TIOCSCTTY, 1);
      tcsetpgrp(0, getpgrp());
    }

    char *cmdline = strdup(t->t_cmd);
    char *argv[64];
    int n = str_tokenize(cmdline, argv, 64 - 1, ' ');
    argv[n] = NULL;
    execv(argv[0], (char *const *)argv);
    exit(127);
  }
  t->t_exitstatus = 0;
  t->t_status = TASK_STATUS_RUNNING;
  t->t_pid = pid;
}

/**
 *
 */
task_t *
task_run(const char *cmd, int flags)
{
  task_t *t = calloc(1, sizeof(task_t));
  t->t_flags = flags;
  t->t_cmd = strdup(cmd);

  pthread_mutex_lock(&task_mutex);
  LIST_INSERT_HEAD(&running_tasks, t, t_run_link);
  task_start(t);
  pthread_mutex_unlock(&task_mutex);
  return t;
}


/**
 *
 */
static int
task_wait(task_t *t)
{
  pthread_mutex_lock(&task_mutex);
  while(t->t_status == TASK_STATUS_RUNNING)
    pthread_cond_wait(&task_cond, &task_mutex);

  int r = t->t_exitstatus;
  free(t->t_cmd);
  free(t);
  pthread_mutex_unlock(&task_mutex);
  return r;
}


/**
 *
 */
int
runcmd(const char *cmd)
{
  return task_wait(task_run(cmd, 0));
}



/**
 *
 */
int
runcmd_ec(const char *cmd)
{
  int r = task_wait(task_run(cmd, 0));
  if(WIFEXITED(r))
    return WEXITSTATUS(r);
  return 128;
}



/**
 *
 */
static void
kill_tasks(int sig)
{
  task_t *t;
  pthread_mutex_lock(&task_mutex);
  LIST_FOREACH(t, &running_tasks, t_run_link) {
    if(t->t_flags & TASK_F_TTY)
      kill(t->t_pid, SIGKILL);
    else
      kill(t->t_pid, sig);
  }
  pthread_mutex_unlock(&task_mutex);
}


static int
tasks_running(void)
{
  int r = 0;
  const task_t *t;
  pthread_mutex_lock(&task_mutex);
  LIST_FOREACH(t, &running_tasks, t_run_link) {
    r++;
  }
  pthread_mutex_unlock(&task_mutex);
  return r;
}


static void *
halt_thread(void *aux)
{
  step_halt();
  sync();

  switch(reboot_action) {
  case REBOOT_ACTION_HALT:
    reboot(RB_HALT_SYSTEM);
    break;
  default:
    break;
  }
  reboot(RB_AUTOBOOT);
  return NULL;
}


/**
 *
 */
int
main(int argc, char **argv)
{
  mkdir("/dev", 0700);
  mknod("/dev/console", S_IFCHR | 0644, makedev(5, 1));
  openconsole("/dev/console");
  printf("init...\n");
  epollfd = epoll_create1(EPOLL_CLOEXEC);

  step_mount_root();

  mount_or_panic("devtmpfs", "/root/dev",     "devtmpfs", 0, NULL);
  mount_or_panic("devpts",   "/root/dev/pts", "devpts",   0, "gid=5,mode=620");
  mount_or_panic("tmpfs",    "/root/dev/shm", "tmpfs",    0, "");
  mount_or_panic("proc",     "/root/proc",    "proc",     0, "");
  mount_or_panic("tmpfs",    "/root/tmp",     "tmpfs",    0, "");

  if(!S_ISLNK(getmode("/root/run")))
    mount_or_panic("tmpfs",    "/root/run",     "tmpfs",    0, "");

  mount_or_panic("sysfs",    "/root/sys",     "sysfs",
                 MS_NODEV | MS_NOEXEC | MS_NOSUID, "");

  int fd = open("/root/tmp", O_PATH | O_CLOEXEC);
  logging_init(fd);

  if(chroot("/root")) {
    printf("chroot failed -- %s\n", strerror(errno));
    exit(1);
  }

  if(chdir("/")) {
    printf("chdir failed -- %s\n", strerror(errno));
    exit(1);
  }


  if(0) {
    setsid();
    ioctl(0, TIOCSCTTY, 1);
    tcsetpgrp(0, getpgrp());
    //  argv[0] = "/sbin/init";
    argv[0] = "/bin/sh";
    execv(argv[0], argv);
  }



  // Block all signals
  sigset_t sigmask;
  sigfillset(&sigmask);
  sigprocmask(SIG_BLOCK, &sigmask, NULL);

  sigaction(SIGCHLD, &sigchld_sa, NULL);
  sigaction(SIGUSR1, &sigusr1_sa, NULL);
  sigaction(SIGUSR2, &sigusr2_sa, NULL);
  sigaction(SIGTERM, &sigterm_sa, NULL);


  step_start_userland();

  task_run("/bin/sh", TASK_F_RESPAWN | TASK_F_TTY);

  // Let some signals thru when in epoll_pwait()
  sigdelset(&sigmask, SIGCHLD);
  sigdelset(&sigmask, SIGUSR1);
  sigdelset(&sigmask, SIGUSR2);
  sigdelset(&sigmask, SIGTERM);

  while(1) {
    int64_t wakeup = INT64_MAX;

    switch(shutdown_state) {
    case 0:
      break;
    case 1:
      trace(LOG_INFO, "Sending all processes the TERM signal");
      kill_tasks(SIGTERM);
      shutdown_state = 2;
      shutdown_timeout = hirestime() + 5000000;
    case 2:
      wakeup = shutdown_timeout;
      if(!tasks_running()) {
        goto dohalt;
      }
      if(hirestime() < shutdown_timeout)
        break;
      shutdown_state = 3;
    case 3:
      trace(LOG_INFO, "Sending all processes the KILL signal");
      kill_tasks(SIGKILL);
      shutdown_state = 4;
      shutdown_timeout = hirestime() + 1000000;
    case 4:
      wakeup = shutdown_timeout;
      if(hirestime() < shutdown_timeout)
        break;
    dohalt:
      run_detached_thread(halt_thread, NULL);
      shutdown_state = 5;
    case 5:
      break;
    }


    int mssleep = -1;
    if(wakeup != INT64_MAX) {
      mssleep = (wakeup - hirestime()) / 1000 + 1;
    }

    struct epoll_event epd[1];
    int n = epoll_pwait(epollfd, epd, 1, mssleep, &sigmask);
    for(int i = 0; i < n; i++) {
      const epollcb_t *ecb = epd[i].data.ptr;
      ecb->cb(ecb->fd, epd[i].events, ecb->opaque);
    }

    while(1) {
      int status;
      pid_t p = waitpid(-1, &status, WNOHANG);
      if(p <= 0)
        break;

      task_t *t;
      pthread_mutex_lock(&task_mutex);
      LIST_FOREACH(t, &running_tasks, t_run_link) {
        if(t->t_pid == p) {
          char reason[128];
          process_status_to_string(reason, sizeof(reason), status);
          trace(LOG_DEBUG, "process '%s' (pid:%d) %s",
                t->t_cmd, t->t_pid, reason);

          if(t->t_flags & TASK_F_RESPAWN && respawn) {
            task_start(t);
          } else {
            LIST_REMOVE(t, t_run_link);
            t->t_pid = 0;
            t->t_status = TASK_STATUS_EXITED;
            t->t_exitstatus = status;
            pthread_cond_broadcast(&task_cond);
          }
          break;
        }
      }
      pthread_mutex_unlock(&task_mutex);
    }
  }
  return 0;
}
