/*
 * Copyright 2013-2014 Ian Pilcher <arequipeno@gmail.com>
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

static char *fcd_smart_cmd[] = {
	[0] = "/usr/sbin/smartctl",
	[1] = "smartctl",
	[2] = "--device=sat",
	[3] = "--nocheck=standby",
	[4] = "--quietmode=silent",
	[5] = "--health",
	[6] = NULL,		/* disk goes here */
	[7] = NULL
};

static bool fcd_smart_disabled[FCD_MAX_DISK_COUNT];

static const cip_opt_info fcd_smart_opts[] = {
	{
		.name			= "smart_monitor_ignore",
		.type			= CIP_OPT_TYPE_BOOL,
		.post_parse_fn		= fcd_conf_disk_bool_cb,
		.post_parse_data	= fcd_smart_disabled,
	},
	{	.name			= NULL		}
};

static int fcd_smart_status(int disk, const int *pipe_fds,
			    struct fcd_monitor *mon)
{
	struct timespec timeout;
	int status;

	timeout.tv_sec = 2;
	timeout.tv_nsec = 0;

	fcd_smart_cmd[6] = fcd_conf_disk_names[disk];

	status = fcd_lib_cmd_status(fcd_smart_cmd, &timeout, pipe_fds);

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
	int disk_alerts[FCD_MAX_DISK_COUNT], pipe_fds[2];
	int warn, status, i;
	char buf[21];

	if (pipe2(pipe_fds, O_CLOEXEC) == -1) {
		FCD_PERROR("pipe2");
		fcd_lib_disable_monitor(mon);
	}

	do {
		memset(buf, ' ', sizeof buf);
		memset(disk_alerts, 0, sizeof disk_alerts);
		warn = 0;

		for (i = 0; i < (int)fcd_conf_disk_count; ++i)
		{
			if (fcd_smart_disabled[i])
				continue;	/* inner loop */

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

		fcd_lib_set_mon_status(mon, buf, warn, 0, disk_alerts);

continue_outer_loop:
		i = fcd_lib_monitor_sleep(1800);
		if (i == -1)
			fcd_lib_disable_cmd_mon(mon, pipe_fds, NULL);

	} while (i == 0);

	fcd_proc_close_pipe(pipe_fds);
	pthread_exit(NULL);
}

struct fcd_monitor fcd_smart_monitor = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,
	.name			= "SMART status",
	.monitor_fn		= fcd_smart_fn,
	.buf			= "....."
				  "S.M.A.R.T. STATUS   "
				  "                    ",
	.enabled		= true,
	.enabled_opt_name	= "enable_smart_monitor",
	.raiddisk_opts		= fcd_smart_opts,
};
