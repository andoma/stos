#pragma once

#include <syslog.h>

void trace(int level, const char *fmt, ...);

void logging_init(int fd);
