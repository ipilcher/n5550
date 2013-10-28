/*
 * Copyright 2013 Ian Pilcher <arequipeno@gmail.com>
 *
 * This program is free software.  You can redistribute it or modify it under
 * the terms of version 2 of the GNU General Public License (GPL), as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY -- without even the implied warranty of MERCHANTIBILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the text of the GPL for more details.
 *
 * Version 2 of the GNU General Public License is available at:
 *
 *   http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 */

#include "freecusd.h"

#include <string.h>
#include <fcntl.h>
#include <time.h>

/* Defining this as an array rather than a pointer makes it modifiable */
static char fcd_smart_cmd_dev[] = "/dev/sdX";

static char *fcd_smart_cmd[] = {
	"/usr/sbin/smartctl",
	"smartctl",
	"--device=sat",
	"--nocheck=standby",
	"--quietmode=silent",
	"--health",
	fcd_smart_cmd_dev,
	NULL
};

static int fcd_smart_status(int disk, const int *pipe_fds,
			    struct fcd_monitor *mon)
{
	struct timespec timeout;
	int status;

	timeout.tv_sec = 2;
	timeout.tv_nsec = 0;

	fcd_smart_cmd_dev[7] = 'b' + disk;

	status = fcd_cmd_status(fcd_smart_cmd, &timeout, pipe_fds);

	switch (status) {
		case -3:	return -3;
		case -2:	FCD_WARN("smartctl command timed out\n");
		case -1:	fcd_lib_disable_cmd_mon(mon, pipe_fds, NULL);
	}

	return status;
}

__attribute__((noreturn))
static void *fcd_smart_fn(void *arg)
{
	struct fcd_monitor *mon = arg;
	int disk_presence[5], disk_alerts[5], pipe_fds[2];
	int warn, status, i, disks_changed;
	time_t now, last;
	char buf[21];

	if (pipe2(pipe_fds, O_CLOEXEC) == -1) {
		FCD_PERROR("pipe2");
		fcd_lib_disable_monitor(mon);
	}

	memset(disk_presence, 0, sizeof disk_presence);
	last = 0;

	do {
		memset(buf, ' ', sizeof buf);
		now = time(NULL);

		disks_changed = fcd_update_disk_presence(disk_presence);
		if (disks_changed == -1)
			fcd_lib_disable_cmd_mon(mon, pipe_fds, NULL);

		if (disks_changed == 0 && now - last < 1800)
			continue;

		last = now;

		memset(disk_alerts, 0, sizeof disk_alerts);
		warn = 0;

		for (i = 0; i < 5; ++i)
		{
			if (disk_presence[i] == 0) {
				memset(buf + i * 3, '-', 2);
				continue;
			}

			status = fcd_smart_status(i, pipe_fds, mon);
			if (status == -3)
				goto continue_outer_loop;

			if (status == 0) {
				memcpy(buf + i * 3, "OK", 2);
			}
			else if (status == 2) {
				memset(buf + i * 3, '?', 2);
			}
			else {
				memset(buf + i * 3, '*', 2);
				disk_alerts[i] = 1;
				warn = 1;
			}

		}

		fcd_copy_buf_and_alerts(mon, buf, warn, 0, disk_alerts);

continue_outer_loop:

		i = fcd_lib_monitor_sleep(30);
		if (i == -1)
			fcd_lib_disable_cmd_mon(mon, pipe_fds, NULL);

	} while (i == 0);

	fcd_proc_close_pipe(pipe_fds);
	pthread_exit(NULL);
}

struct fcd_monitor fcd_smart_monitor = {
	.mutex		= PTHREAD_MUTEX_INITIALIZER,
	.name		= "SMART status",
	.monitor_fn	= fcd_smart_fn,
	.buf		= "....."
			  "S.M.A.R.T. STATUS   "
			  "                    ",
};
