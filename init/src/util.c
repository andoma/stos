#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/epoll.h>
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

#include "queue.h"

#include "logging.h"

#include "util.h"

time_t
monotime(void)
{
  struct timespec tp;
  clock_gettime(CLOCK_MONOTONIC, &tp);
  return tp.tv_sec;
}


int64_t
hirestime(void)
{
  struct timespec tp;
  clock_gettime(CLOCK_MONOTONIC, &tp);
  return tp.tv_sec * 1000000 + tp.tv_nsec / 1000;
}


mode_t
getmode(const char *path)
{
  struct stat st;
  if(lstat(path, &st))
    return 0;

  return st.st_mode;
}



void
mount_or_panic(const char *source, const char *target,
               const char *filesystemtype, unsigned long mountflags,
               const void *data)
{
  mkdir(target, 0755);
  if(mount(source, target, filesystemtype, mountflags, data)) {
    printf("Error mounting %s (type: %s) on %s (opts=%s) flags:%lx -- %s\n",
           source, filesystemtype, target, (const char *)data ?: "", mountflags,
           strerror(errno));
    //    exit(1);
  }
}



/**
 *
 */
void
unmount(const char *path)
{
  while(!umount2(path, MNT_DETACH)) {
    trace(LOG_DEBUG, "Unmounted %s", path);
  }
}


void
remount(const char *path, int flags)
{
  if(mount(NULL, path, NULL, MS_REMOUNT | flags, "") == -1) {
    printf("remount %s 0x%x failed -- %s\n", path, flags, strerror(errno));
  }
}



/**
 *
 */
int
loopmount(const char *image, const char *mountpoint,
	  const char *fstype, int mountflags, int ro,
	  loopmount_t *lm)
{
  struct stat st;
  struct loop_info64 li;
  int mode = ro ? O_RDONLY : O_RDWR;

  if(ro)
    mountflags |= MS_RDONLY;

  trace(LOG_INFO, "loopmount: opening image %s as %s",
	 image, ro ? "read-only" : "read-write");

  int fd = open(image, mode | O_CLOEXEC);
  if(fd < 0) {
    trace(LOG_ERR, "loopmount: failed to open image %s -- %s",
	   image, strerror(errno));
    return -1;
  }
  int i;
  for(i = 0; i < 256; i++) {
    snprintf(lm->devpath, sizeof(lm->devpath), "/dev/loop%d", i);

    if(stat(lm->devpath, &st)) {
      // Nothing there, try to create node
      if(mknod(lm->devpath, S_IFBLK|0644, makedev(7, i))) {
	trace(LOG_ERR, "loopmount: failed to create %s -- %s",
	       lm->devpath, strerror(errno));
	continue;
      }
    } else {
      // Something there
      if(!S_ISBLK(st.st_mode)) {
	trace(LOG_ERR, "loopmount: %s is not a block device",
	       lm->devpath);
	continue; // Not a block device, scary
      }
    }

    lm->loopfd = open(lm->devpath, mode | O_CLOEXEC);
    if(lm->loopfd == -1) {
      trace(LOG_ERR, "loopmount: Unable to open %s -- %s",
	     lm->devpath, strerror(errno));
      continue;
    }

    int rc = ioctl(lm->loopfd, LOOP_GET_STATUS64, &li);
    if(rc && errno == ENXIO) {
      // Device is free, use it 
      memset(&li, 0, sizeof(li));
      snprintf((char *)li.lo_file_name, sizeof(li.lo_file_name), "%s", image);
      if(ioctl(lm->loopfd, LOOP_SET_FD, fd)) {
	trace(LOG_ERR, "loopmount: Failed to SET_FD on %s -- %s",
	       lm->devpath, strerror(errno));
	close(lm->loopfd);
	continue;
      }
      if(ioctl(lm->loopfd, LOOP_SET_STATUS64, &li)) {
	trace(LOG_ERR, "loopmount: Failed to SET_STATUS64 on %s -- %s",
	       lm->devpath, strerror(errno));
	ioctl(lm->loopfd, LOOP_CLR_FD, 0);
	close(lm->loopfd);
	continue;
      }

      close(fd);

      unmount(mountpoint);
      mkdir(mountpoint, 0755);
      if(mount(lm->devpath, mountpoint, fstype, mountflags, "")) {
	trace(LOG_ERR, "loopmount: Unable to mount loop device %s on %s -- %s",
	       lm->devpath, mountpoint, strerror(errno));
	ioctl(lm->loopfd, LOOP_CLR_FD, 0);
	close(lm->loopfd);
	return -1;
      }
      return 0;
    }
    close(lm->loopfd);
  }
  return -1;
}


void
loopunmount(loopmount_t *lm, const char *mountpath)
{
  trace(LOG_INFO, "Unmounting %s", mountpath);
  unmount(mountpath);
  ioctl(lm->loopfd, LOOP_CLR_FD, 0);
  close(lm->loopfd);
  unlink(lm->devpath);
}


void
mount_sqfs_or_panic(const char *source, const char *target)
{
  loopmount_t lm;
  if(loopmount(source, target, "squashfs", 0, 1, &lm)) {
    trace(LOG_ALERT, "Failed to mount sqfs %s on %s",
          source, target);
    exit(1);
  }
}


void
run_detached_thread(void *(*fn)(void *), void *aux)
{
  pthread_t id;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&id, &attr, fn, aux);
  pthread_attr_destroy(&attr);
}





/**
 *
 */
void
writefile(const char *path, const void *data, int len)
{
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if(fd == -1) {
    printf("Unable to open %s -- %s\n", path, strerror(errno));
    exit(1);
  }
  if(write(fd, data, len) != len) {
    printf("Failed to write to %s -- %s\n", path, strerror(errno));
    exit(1);
  }
  close(fd);
}


void
process_status_to_string(char *dst, size_t dstlen, int status)
{
  if(WIFEXITED(status)) {
    snprintf(dst, dstlen, "exit with code %d", WEXITSTATUS(status));
  } else if(WIFSIGNALED(status)) {
    snprintf(dst, dstlen, "exit with signal %d%s", WTERMSIG(status),
             WCOREDUMP(status) ? ", core dumped" : "");
  } else {
    snprintf(dst, dstlen, "exit with unknown reason status=0x%x", status);
  }
}
