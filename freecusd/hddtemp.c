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

#include <stdio.h>
#include <limits.h>
#include <string.h>

#include "freecusd.h"

static char *fcd_hddtemp_cmd[] = {
	"/usr/sbin/hddtemp",
	"hddtemp",
	"/dev/sdb",
	"/dev/sdc",
	"/dev/sdd",
	"/dev/sde",
	"/dev/sdf",
	NULL
};

__attribute__((noreturn))
static void *fcd_hddtemp_fn(void *arg)
{
	struct fcd_monitor *mon = arg;
	int disk_presence[5] = { 0 };
	int ret, temp, i, temps[5];
	char *b, c, buf[21];
	pid_t child;
	FILE *fp;

	do {
		temps[0] = temps[1] = temps[2] = temps[3] = temps[4] = INT_MIN;
		memset(buf, ' ', sizeof buf);

		if (fcd_update_disk_presence(disk_presence) == -1)
			fcd_disable_monitor(mon);

		fp = fcd_cmd_spawn(&child, fcd_hddtemp_cmd);
		if (fp == NULL)
			fcd_disable_monitor(mon);

		while (1)
		{
			ret = fscanf(fp, "/dev/sd%c:%*[^:]: %d%*[^\n]\n",
				     &c, &temp);
			if (ret == EOF) {
				if (feof(fp))
					break;
				FCD_ERR("fscanf: %m\n");
				fcd_cmd_cleanup(fp, child);
				fcd_disable_monitor(mon);
			}

			if (ret == 2 && c >= 'b' && c <= 'f')
				temps[c - 'b'] = temp;
			else
				FCD_WARN("Error parsing hddtemp output\n");
		}

		if (fcd_cmd_cleanup(fp, child) == -1)
			fcd_disable_monitor(mon);

		for (i = 0, b = buf; i < 5; ++i)
		{
			if (temps[i] >= -99 && temps[i] <= 999) {
				ret = sprintf(b, "%d ", temps[i]);
				if (ret == EOF) {
					FCD_ERR("sprintf: %m\n");
					fcd_disable_monitor(mon);
				}
				b += ret;
			}
			else {
				if (disk_presence[i] == 0)
					memset(b, '-', 2);
				else if (temps[i] == INT_MIN)
					memset(b, '?', 2);
				else
					memset(b, '*', 2);
				b += 3;
			}
		}

		fcd_copy_buf(buf, mon);

	} while (fcd_sleep_and_check_exit(30) == 0);

	pthread_exit(NULL);
}

struct fcd_monitor fcd_hddtemp_monitor = {
	.mutex		= PTHREAD_MUTEX_INITIALIZER,
	.name		= "HDD temperature",
	.monitor_fn	= fcd_hddtemp_fn,
	.buf		= "....."
			  "HDD TEMPERATURE     "
			  "                    ",
};
