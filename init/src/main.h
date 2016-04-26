#pragma once

#include "queue.h"

extern int respawn;

#define REBOOT_ACTION_RESTART 0 // Default, just restart the system
#define REBOOT_ACTION_HALT    1

extern int reboot_action;


void step_mount_root(void);

void step_start_userland(void);

void step_halt(void);

int runcmd(const char *cmd);

int runcmd_ec(const char *cmd);

#define TASK_F_RESPAWN    0x1
#define TASK_F_BACKGROUND 0x2
#define TASK_F_TTY        0x4

#define TASK_DAEMON (TASK_F_RESPAWN | TASK_F_BACKGROUND)

typedef struct task {
  int t_flags;
  char *t_cmd;

  LIST_ENTRY(task) t_run_link;
  pid_t t_pid;
  int t_status;
  int t_exitstatus;
  int t_mode;
} task_t;


task_t *task_run(const char *cmd, int flags);

