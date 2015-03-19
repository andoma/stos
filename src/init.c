#include <string.h>
#include <linux/loop.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/utsname.h>

#include <stdio.h>



static int
parse_partition_table(void)
{
  char buf[4096];
  int have_added = 0;

  while(1) {
    printf("Scanning for partitions...\n");
    FILE *fp = fopen("/proc/partitions", "r");
    if(fp == NULL) {
      printf("failed to open /proc/partitions -- %s\n", strerror(errno));
      exit(1);
    }

    char *line = NULL;
    ssize_t read;
    size_t len = 0;
    int added_this_round = 0;

    while((read = getline(&line, &len, fp)) != -1) {
      int major, minor, blocks;
      char name[64];
      char path[96];
      if((sscanf(line, "%d %d %d %[^\n]",
		 &major, &minor, &blocks, name)) != 4)
	continue;

      snprintf(path, sizeof(path), "/dev/%s", name);

      if(!mknod(path, S_IFBLK | 0666, makedev(major, minor))) {
	have_added = 1;
	added_this_round = 1;
	printf("Created %s (%d,%d)\n", path, major, minor);
      }
    }
    free(line);
    fclose(fp);

    if(have_added && !added_this_round)
      break;
    usleep(100000);
  }
}

#define FALLBACK_ROOTFS "/boot/rootfs.sqfs"
#define FALLBACK_MODULESFS "/boot/modules.sqfs"

static void
mount_rootfs(void)
{
  // --- rootfs

  printf("Mounting root FS\n");

  int rootfs = open(FALLBACK_ROOTFS, O_RDONLY);
  if(rootfs < 0) {
    printf("Unable to open %s -- %s\n", FALLBACK_ROOTFS,
	   strerror(errno));
    exit(1);
  }

  int loop0 = open("/dev/loop0", O_RDONLY);
  if(loop0 < 0) {
    printf("Unable to open /dev/loop0 -- %s\n",
	   strerror(errno));
    exit(1);
  }

  if(ioctl(loop0, LOOP_SET_FD, rootfs)) {
    printf("Unable to ioctl(loop0, rootfs) -- %s\n", strerror(errno));
    exit(1);
  }

  if(mount("/dev/loop0", "/root", "squashfs", MS_RDONLY, "")) {
    printf("/dev/loop0 failed to mount %s\n", strerror(errno));
    exit(1);
  }

  close(rootfs);
  close(loop0);
}


static void
mount_modulefs(struct utsname *uts)
{
  char tmp[256];

  // --- modules
  snprintf(tmp, sizeof(tmp), "/boot/modules_%s.sqfs", uts->machine);

  int modulesfs = open(tmp, O_RDONLY);
  if(modulesfs < 0) {
    snprintf(tmp, sizeof(tmp), "/boot/modules.sqfs");
    modulesfs = open(tmp, O_RDONLY);
    if(modulesfs < 0) {
      printf("Unable to open %s -- %s\n", tmp, strerror(errno));
      exit(1);
    }
  }

  printf("Mounting modules from %s\n", tmp);

  int loop1 = open("/dev/loop1", O_RDONLY);
  if(loop1 < 0) {
    printf("Unable to open /dev/loop1 -- %s\n",
	   strerror(errno));
    exit(1);
  }

  if(ioctl(loop1, LOOP_SET_FD, modulesfs)) {
    printf("Unable to ioctl(loop1, moduleesfs) -- %s\n", strerror(errno));
    exit(1);
  }

  if(mount("/dev/loop1", "/root/lib/modules", "squashfs", MS_RDONLY, "")) {
    printf("/dev/loop1 failed to mount %s\n", strerror(errno));
    exit(1);
  }
  close(modulesfs);
  close(loop1);
}


int
main(int argc, char **argv)
{
  struct utsname uts;

  mkdir("/dev", 0700);
  mkdir("/proc", 0700);
  mkdir("/boot", 0700);
  mkdir("/root", 0700);
  mkdir("/persistent", 0700);

  if(mount("tmpfs", "/dev", "tmpfs", 0, "")) {
    printf("proc failed to mount %s\n", strerror(errno));
    exit(1);
  }

  mknod("/dev/console", S_IFCHR | 0644, makedev(5, 1));
  mknod("/dev/loop0",   S_IFBLK | 0644, makedev(7, 0));
  mknod("/dev/loop1",   S_IFBLK | 0644, makedev(7, 1));

  dup2(0, open("/dev/console", O_RDWR));
  dup2(1, open("/dev/console", O_RDWR));
  dup2(2, open("/dev/console", O_RDWR));
  printf("sqfs init starting\n");

  if(uname(&uts)) {
    printf("uname() failed -- %s\n", strerror(errno));
    exit(1);
  }

  if(mount("proc", "/proc", "proc", 0, "")) {
    printf("proc failed to mount %s\n", strerror(errno));
    exit(1);
  }

  parse_partition_table();

  extern void fsck(const char *path);
  fsck("/dev/mmcblk0p1");

  if(mount("/dev/mmcblk0p1", "/boot", "vfat", MS_RDONLY, "")) {
    printf("/dev/mmcblk0p1 failed to mount %s\n", strerror(errno));
    exit(1);
  }

  mount_rootfs();
  mount_modulefs(&uts);


  printf("Ok, about to transfer control to rootfs\n");

  umount2("/boot", MNT_DETACH);
  umount2("/proc", MNT_DETACH);
  umount2("/dev",  MNT_DETACH);

  if(mount("devtmpfs", "/root/dev", "devtmpfs", 0, NULL)) {
    printf("Error mounting devtmpfs\n");
    exit(1);
  }

  chdir("/root");
  if(mount(".", "/", NULL, MS_MOVE, NULL)) {
    printf("Error moving root\n");
    exit(1);
  }

  chroot(".");

  // Setup default env vars for creating filesystems

  if(getenv("STOS_mkfs_ext4_args") == NULL)
    setenv("STOS_mkfs_ext4_args",
           "-E nodiscard,stride=2,stripe-width=1024 -b 4096 -O ^has_journal", 1);

  // Spawn init

  argv[0] = "/sbin/init";
  execv(argv[0], argv);
  printf("Unable to execv(\"%s\") -- %s\n", argv[0], strerror(errno));
}
