#include "util.h"
#include "logging.h"
#include "main.h"

void
step_mount_root(void)
{
  mount_or_panic("bootfs", "/boot", "9p", 0, "trans=virtio");
  mount_sqfs_or_panic("/boot/rootfs.sqfs", "/roroot");
  mkdir("/overlay", 0700);
  mkdir("/workdir", 0700);
  mount_or_panic("overlay", "/root", "overlay", 0,
                 "lowerdir=/roroot,upperdir=/overlay,workdir=/workdir");
}





/**
 *
 */
static void *
bootuserland(void *aux)
{
  trace(LOG_INFO, "Booting userland");
  mkdir("/tmp/stos", 0755);
  mkdir("/tmp/stos/mnt", 0755);

  mkdir("/var/run/dbus", 0755);
  mkdir("/var/lock/subsys", 0755);
  mkdir("/tmp/dbus", 0755);

  writefile("/proc/sys/kernel/hotplug", "\x00\0x00\0x00\0x00", 4);
  task_run("/sbin/udevd", TASK_MODE_DAEMON);
  runcmd("/sbin/udevadm trigger --type=subsystems --action=add");
  runcmd("/sbin/udevadm trigger --type=devices --action=add");
  runcmd("/sbin/udevadm settle --timeout=30");

  runcmd("/usr/bin/dbus-uuidgen --ensure");
  task_run("/usr/bin/dbus-daemon --system --nofork", TASK_MODE_DAEMON);

  task_run("/usr/sbin/connmand -n", TASK_MODE_DAEMON);
  return NULL;
}


void
step_start_userland(void)
{
  run_detached_thread(bootuserland, NULL);
}
