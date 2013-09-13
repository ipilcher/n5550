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

#include <string.h>
#include <time.h>

#include "freecusd.h"

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

__attribute__((noreturn))
static void *fcd_smart_fn(void *arg)
{
	struct fcd_monitor *mon = arg;
	int disk_presence[5] = { 0 };
	int status, i, disks_changed;
	time_t now, last = 0;
	char buf[21];
	pid_t child;
	FILE *fp;

	do {
		memset(buf, ' ', sizeof buf);
		now = time(NULL);

		disks_changed = fcd_update_disk_presence(disk_presence);
		if (disks_changed == -1)
			fcd_disable_monitor(mon);

		if (disks_changed == 0 && now - last < 1800)
			continue;
		last = now;

		for (i = 0; i < 5; ++i)
		{
			if (disk_presence[i] == 0) {
				memset(buf + i * 3, '-', 2);
			}
			else {
				fcd_smart_cmd_dev[7] = 'b' + i;

				fp = fcd_cmd_spawn(&child, fcd_smart_cmd);
				if (fp == NULL)
					fcd_disable_monitor(mon);

				status = fcd_cmd_cleanup(fp, child);
				if (status == -1)
					fcd_disable_monitor(mon);

				if (status == 0)
					memcpy(buf + i * 3, "OK", 2);
				else if (status == 2)
					memset(buf + i * 3, '?', 2);
				else
					memset(buf + i * 3, '*', 2);
			}
		}

		fcd_copy_buf(buf, mon);

	} while (fcd_sleep_and_check_exit(30) == 0);

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
