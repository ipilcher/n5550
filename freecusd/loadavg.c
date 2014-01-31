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

/* Alert thresholds */
static const double fcd_loadavg_warn = 12.0;
static const double fcd_loadavg_fail = 16.0;

__attribute__((noreturn))
static void fcd_loadavg_close_and_disable(FILE *fp, struct fcd_monitor *mon)
{
	if (fclose(fp) != 0)
		FCD_PERROR("fclose");
	fcd_lib_disable_monitor(mon);

}

__attribute__((noreturn))
static void *fcd_loadavg_fn(void *arg)
{
	static const char path[] = "/proc/loadavg";
	struct fcd_monitor *mon = arg;
	int warn, fail, ret;
	double avgs[3];
	char buf[21];
	FILE *fp;

	fp = fopen(path, "re");
	if (fp == NULL) {
		FCD_PERROR(path);
		fcd_lib_disable_monitor(mon);
	}

	if (setvbuf(fp, NULL, _IONBF, 0) != 0) {
		FCD_PERROR("setvbuf");
		fcd_loadavg_close_and_disable(fp, mon);
	}

	do {
		rewind(fp);
		memset(buf, ' ', sizeof buf);

		ret = fscanf(fp, "%lf %lf %lf", &avgs[0], &avgs[1], &avgs[2]);
		if (ret == EOF) {
			FCD_PERROR("fscanf");
			fcd_loadavg_close_and_disable(fp, mon);
		}
		else if (ret != 3) {
			FCD_WARN("Failed to parse contents of /proc/loadavg\n");
			fcd_loadavg_close_and_disable(fp, mon);
		}

		fail = (avgs[0] >= fcd_loadavg_fail);
		warn = fail ? 0 : (avgs[0] >= fcd_loadavg_warn);

		ret = snprintf(buf, sizeof buf, "%.2f %.2f %.2f",
			       avgs[0], avgs[1], avgs[2]);
		if (ret < 0) {
			FCD_PERROR("snprintf");
			fcd_loadavg_close_and_disable(fp, mon);
		}

		if (ret < (int)sizeof buf)
			buf[ret] = ' ';

		fcd_lib_set_mon_status(mon, buf, warn, fail, NULL);

		ret = fcd_lib_monitor_sleep(30);
		if (ret == -1)
			fcd_loadavg_close_and_disable(fp, mon);

	} while (ret == 0);

	if (fclose(fp) != 0)
		FCD_PERROR("fclose");

	pthread_exit(NULL);
}

struct fcd_monitor fcd_loadavg_monitor = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,
	.name			= "load average",
	.monitor_fn		= fcd_loadavg_fn,
	.buf			= "....."
				  "LOAD AVERAGE        "
				  "                    ",
	.enabled		= true,
	.enabled_opt_name	= "enable_loadavg_monitor",
};
