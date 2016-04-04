#include <pthread.h>
#include <stdarg.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>

#include "util.h"
#include "logging.h"
#include "main.h"

#include <netinet/in.h>
#include <arpa/inet.h>


static pthread_t movian_shell;
static int factory_reset;

/**
 *
 */
static void
status(const char *fmt, ...)
{
  static int trace_fd = -1;
  static struct sockaddr_in log_server;

  char msg[1000];
  va_list ap;

  if(trace_fd == -1) {

    log_server.sin_family = AF_INET;
    log_server.sin_port = htons(4004);
    log_server.sin_addr.s_addr = inet_addr("127.0.0.1");
    trace_fd = socket(PF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if(trace_fd == -1)
      return;
  }

  if(trace_fd == -1)
    return;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  sendto(trace_fd, msg, strlen(msg), 0,
	 (struct sockaddr*)&log_server, sizeof(log_server));
}



#define STOS_BASE "/stos"

#define PERSISTENTPATH STOS_BASE"/persistent"
#define CACHEPATH      STOS_BASE"/cache"
#define MNTPATH        STOS_BASE"/mnt"
#define MOVIANMOUNTPATH MNTPATH"/showtime"

#define DEFAULT_PERSISTENT_PART "/dev/mmcblk0p2"
#define DEFAULT_CACHE_PART      "/dev/mmcblk0p3"
#define DEFAULT_FLASH_DEV       "/dev/mmcblk0"
#define PERSISTENT_SIZE (256 * 1024 * 1024)

#define MOVIAN_PKG_PATH PERSISTENTPATH"/packages/showtime.sqfs"
#define MOVIAN_DEFAULT_PATH "/boot/showtime.sqfs"

#define STARTSCRIPT PERSISTENTPATH"/scripts/boot.sh"

const char *persistent_part = DEFAULT_PERSISTENT_PART;
const char *cache_part = DEFAULT_CACHE_PART;
const char *flash_dev = DEFAULT_FLASH_DEV;

// We want to align to 16384 sector boundary for better
// erase performance on SD cards
#define SD_ALIGN(x) (((x) + 16383) & ~16383)



#define RUN_BUNDLE_MOUNT_PROBLEMS    -1
#define RUN_BUNDLE_COMMAND_NOT_FOUND -2
#define RUN_BUNDLE_COMMAND_CRASH     -3




static void *
start_sshd(void *aux)
{
  struct stat st;

  mkdir(PERSISTENTPATH"/etc", 0700);
  mkdir(PERSISTENTPATH"/etc/dropbear", 0700);

#define SSHD_HOSTKEY PERSISTENTPATH"/etc/dropbear/dropbear_rsa_host_key"

  if(stat(SSHD_HOSTKEY, &st) || st.st_size < 10) {
    unlink(SSHD_HOSTKEY);
    runcmd("/usr/bin/dropbearkey -t rsa -f "SSHD_HOSTKEY);
  }

  task_run("/usr/sbin/dropbear -r "SSHD_HOSTKEY" -F", TASK_DAEMON);
  return NULL;
}



/**
 *
 */
static int
format_partition(int partid, int with_journal)
{
  char cmdline[512];
  const char *label;
  const char *part;

  const char *fsopts = "-E nodiscard,stride=2,stripe-width=1024 -b 4096";
  const char *opts = "";

  if(!with_journal)
    opts = "-O ^has_journal";

  switch(partid) {
  case 2:
    label = "persistent";
    part = persistent_part;
    break;
  case 3:
    label = "cache";
    part = cache_part;
    break;
  default:
    trace(LOG_ERR, "Don't know how to format partition %d", partid);
    return -1;
  }

  status("Formatting %s partition", label);

  trace(LOG_NOTICE, "Formatting partition %d [%s] device: %s",
	partid, label, part);

  snprintf(cmdline, sizeof(cmdline),
           "/usr/sbin/mkfs.ext4 -F -L %s %s %s %s",
           label, fsopts, opts, part);
  return runcmd_ec(cmdline);
}




/**
 *
 */
static int
start_movian_from_bundle(const char *bundle)
{
  loopmount_t lm;

  if(loopmount(bundle, MOVIANMOUNTPATH, "squashfs", 0, 1, &lm))
    return RUN_BUNDLE_MOUNT_PROBLEMS;

  int ret = runcmd(MOVIANMOUNTPATH"/bin/showtime"
                   " --syslog "
                   " -d "
                   " --with-poweroff "
                  " --cache "
                   CACHEPATH"/showtime "
                   " --persistent "
                   PERSISTENTPATH"/showtime"
                   " --upgrade-path "
                   MOVIAN_PKG_PATH);

  loopunmount(&lm, MOVIANMOUNTPATH);

  if(WIFSIGNALED(ret)) {

    if(WTERMSIG(ret) == SIGINT ||
       WTERMSIG(ret) == SIGTERM ||
       WTERMSIG(ret) == SIGQUIT)
      return 0;

    return RUN_BUNDLE_COMMAND_CRASH;
  }

  if(WEXITSTATUS(ret) == 127) {
    return RUN_BUNDLE_COMMAND_NOT_FOUND;
  }

  return WEXITSTATUS(ret);
}

/**
 *
 */
static void *
start_movian(void *aux)
{
  int shortrun = 0;

  if(!access(STARTSCRIPT, X_OK)) {
    status("Running boot script");
    system(STARTSCRIPT);
  }

  status("Starting Movian");

  while(1) {
    int exitcode;
    int from_downloaded = 0;
    mkdir(PERSISTENTPATH"/packages", 0777);

    time_t starttime = monotime();

    if(!access(MOVIAN_PKG_PATH, R_OK)) {
      exitcode = start_movian_from_bundle(MOVIAN_PKG_PATH);
      from_downloaded = 1;
    } else {
      exitcode = start_movian_from_bundle(MOVIAN_DEFAULT_PATH);
    }

    if(!respawn)
      return NULL;

    time_t stoptime = monotime();

    if(stoptime - starttime < 5) {
      shortrun++;
    } else {
      shortrun = 0;
    }


    switch(exitcode) {
    case 8:
    case RUN_BUNDLE_COMMAND_CRASH:
      if(shortrun < 5)
        break;
      // FALLTHRU
    case RUN_BUNDLE_COMMAND_NOT_FOUND:
    case RUN_BUNDLE_MOUNT_PROBLEMS:
      if(from_downloaded) {
        if(!unlink(MOVIAN_PKG_PATH))
          continue;
      }

      kill(1, SIGTERM);
      return NULL;


    case 13:  // Restart
      continue;

    case 14:  // Exit to shell
      return NULL;

    case 16:  // Factory reset
      factory_reset = 1;
      kill(1, SIGTERM);
      return NULL;

    case 15:  // System restart
    case 11:
      kill(1, SIGTERM);
      return NULL;

    default:
      break;
    }


    if(shortrun == 3) {
      trace(LOG_ERR, "Movian keeps respawning quickly, clearing cache");
      unmount(CACHEPATH);
      format_partition(3, 0);

      mount_or_panic(cache_part, CACHEPATH,
                     "ext4",  MS_NOATIME | MS_NOSUID | MS_NODEV, "");
      continue;
    }


    if(shortrun == 6) {
      trace(LOG_ERR,
            "Movian keeps respawning quickly, clearing persistent partition");
      unmount(PERSISTENTPATH);
      format_partition(2, 1);
      mount_or_panic(persistent_part, PERSISTENTPATH,
                     "ext4",  MS_NOATIME | MS_NOSUID | MS_NODEV, "");
      continue;
    }

    if(shortrun)
      sleep(1);
  }
}




/**
 *
 */
static int
create_partition(int start, int end, const char *type, const char *fstype)
{
  char cmdline[512];

  trace(LOG_NOTICE, "Creating %s partition [%s] start:%d end:%d (%d KiB) on %s",
	type, fstype, start, end, (end - start) / 2, flash_dev);

  snprintf(cmdline, sizeof(cmdline),
	   "/usr/sbin/parted -m %s unit s mkpart %s %s %d %d",
	   flash_dev, type, fstype, start, end);

  return runcmd_ec(cmdline);
}


/**
 *
 */
static void
check_partition(int partnum, int with_journal)
{
  char cmdline[512];

  trace(LOG_NOTICE, "Checking partition %d", partnum);
  snprintf(cmdline, sizeof(cmdline),
           "/usr/sbin/fsck.ext4 -y /dev/mmcblk0p%d", partnum);

  runcmd_ec(cmdline);

  snprintf(cmdline, sizeof(cmdline),
           "/usr/sbin/tune2fs -O %s /dev/mmcblk0p%d",
           with_journal ? "has_journal" : "^has_journal",
           partnum);

  runcmd_ec(cmdline);
}




/**
 *
 */
static int
setup_partitions(void)
{
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "parted -m %s unit s print free", flash_dev);

  FILE *fp = popen(cmd, "r");
  char *line = NULL;
  size_t len = 0;
  ssize_t read;

  int partfound[5] = {};

  int free_start = 0;
  int free_end = 0;
  int free_size = 0;

  while((read = getline(&line, &len, fp)) != -1) {
    int part, start, end, size;
    char type[512];
    if((sscanf(line, "%d:%ds:%ds:%ds:%[^;];",
	       &part, &start, &end, &size, type)) != 5)
      continue;

    if(strcmp(type, "free") && part < 5)
      partfound[part] = 1;

    if(!strcmp(type, "free")) {
      if(size > free_size) {
	free_start = start;
	free_end   = end;
	free_size  = size;
      }
    }
  }

  free(line);
  fclose(fp);


  int i;
  for(i = 1; i <= 4; i++)
    trace(LOG_INFO,
	  "Partition %d %s", i, partfound[i] ? "available" : "not found");

  trace(LOG_INFO,
	"Biggest free space available: %d sectors at %d - %d",
	free_size, free_start, free_end);

  if(!partfound[2]) {
    // Create persistent partition

    trace(LOG_INFO, "Need to create partition for persistent data");

    int start = SD_ALIGN(free_start);
    int end   = start + (PERSISTENT_SIZE / 512) - 1;
    if(create_partition(start, end, "primary", "ext4")) {
      trace(LOG_ERR, "Failed to create partition for persistent data");
      return -1;
    }
    format_partition(2, 1);
    free_start = end + 1;
  } else {
    check_partition(2, 1);
  }

  if(!partfound[3]) {
    // Create persistent partition

    trace(LOG_INFO, "Need to create partition for cached data");

    int start = SD_ALIGN(free_start);
    int end   = free_end;
    if(create_partition(start, end, "primary", "ext4")) {
      trace(LOG_ERR, "Failed to create partition for cached data");
      return -1;
    }
    format_partition(3, 0);
  } else {
    check_partition(3, 0);
  }
  return 0;
}



/**
 *
 */
static void *
bootuserland(void *aux)
{

  sethostname("stos", 4);
  runcmd("/sbin/ifconfig lo 127.0.0.1");
  runcmd("/sbin/ifconfig lo up");

  task_run("/usr/sbin/stos-splash -s Booting... -f /usr/share/fonts/Audiowide-Regular.ttf", TASK_F_BACKGROUND);

  trace(LOG_INFO, "Booting userland");

  mkdir("/tmp/stos", 0755);
  mkdir("/tmp/stos/mnt", 0755);

  status("Checking SD card");

  setup_partitions();

  mount_or_panic(persistent_part, PERSISTENTPATH,
                 "ext4",  MS_NOATIME | MS_NOSUID | MS_NODEV, "");

  mount_or_panic(cache_part, CACHEPATH,
                 "ext4",  MS_NOATIME | MS_NOSUID | MS_NODEV, "");

  mkdir("/var/run/dbus", 0755);
  mkdir("/var/lock/subsys", 0755);
  mkdir("/tmp/dbus", 0755);

  status("Starting system services");

  writefile("/proc/sys/kernel/hotplug", "\x00\0x00\0x00\0x00", 4);
  task_run("/sbin/udevd", TASK_DAEMON);
  runcmd("/sbin/udevadm trigger --type=subsystems --action=add");
  runcmd("/sbin/udevadm trigger --type=devices --action=add");
  runcmd("/sbin/udevadm settle --timeout=30");

  runcmd("/usr/bin/dbus-uuidgen --ensure");
  task_run("/usr/bin/dbus-daemon --system --nofork", TASK_DAEMON);

  mkdir("/stos/persistent/connman", 0755);
  task_run("/usr/sbin/connmand -n", TASK_DAEMON);

  task_run("/usr/sbin/avahi-daemon -s", TASK_DAEMON);

  run_detached_thread(start_sshd, NULL);

  pthread_create(&movian_shell, NULL, start_movian, NULL);
  return NULL;
}





static int
parse_partition_table(void)
{
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
  return 0;
}



void
step_mount_root(void)
{
  mount_or_panic("proc",     "/proc",    "proc",     0, "");
  parse_partition_table();
  unmount("/proc");

  extern void fsck(const char *path);
  fsck("/dev/mmcblk0p1");

  mount_or_panic("/dev/mmcblk0p1", "/boot", "vfat", MS_RDONLY, "");

  mount_sqfs_or_panic("/boot/rootfs.sqfs", "/root");
  mount_sqfs_or_panic("/boot/firmware.sqfs", "/root/lib/firmware");

  struct utsname uts;
  if(uname(&uts)) {
    printf("uname() failed -- %s\n", strerror(errno));
    exit(1);
  }

  char modulesname[128];
  snprintf(modulesname, sizeof(modulesname), "/boot/modules_%s.sqfs",
           uts.machine);

  if(!access(modulesname, R_OK)) {
    mount_sqfs_or_panic(modulesname, "/root/lib/modules");
  } else {
    mount_sqfs_or_panic("/boot/modules.sqfs", "/root/lib/modules");
  }

  mount_or_panic("/boot", "/root/boot", NULL, MS_MOVE, "");
}


void
step_start_userland(void)
{
  run_detached_thread(bootuserland, NULL);
}


void
step_halt(void)
{
  pthread_join(movian_shell, NULL);
  unmount(CACHEPATH);
  unmount(PERSISTENTPATH);
  remount("/boot", MS_RDONLY);
  if(factory_reset) {
    format_partition(2, 1);
    format_partition(3, 0);
  }
}
