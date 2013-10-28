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

/* Alert thresholds */
static const int fcd_sysfan_warn = 1200;
static const int fcd_sysfan_fail = 500;

static const char fcd_sysfan_input[] =
		"/sys/devices/platform/it87.656/fan3_input";

__attribute__((noreturn))
static void fcd_sysfan_close_and_disable(FILE *fp, struct fcd_monitor *mon)
{
	if (fclose(fp) != 0)
		FCD_PERROR("fclose");
	fcd_lib_disable_monitor(mon);

}

__attribute__((noreturn))
static void *fcd_sysfan_fn(void *arg)
{
	struct fcd_monitor *mon = arg;
	int warn, fail, rpm, ret;
	char buf[21];
	FILE *fp;

	fp = fopen(fcd_sysfan_input, "re");
	if (fp == NULL) {
		FCD_PERROR("fopen");
		fcd_lib_disable_monitor(mon);
	}

	if (setvbuf(fp, NULL, _IONBF, 0) != 0) {
		FCD_PERROR("setvbuf");
		fcd_sysfan_close_and_disable(fp, mon);
	}

	do {
		rewind(fp);
		memset(buf, ' ', sizeof buf);

		ret = fscanf(fp, "%d", &rpm);
		if (ret == EOF) {
			FCD_PERROR("fscanf");
			fcd_sysfan_close_and_disable(fp, mon);
		}
		else if (ret != 1) {
			FCD_WARN("Failed to parse contents of %s\n",
				 fcd_sysfan_input);
			fcd_sysfan_close_and_disable(fp, mon);
		}

		fail = (rpm <= fcd_sysfan_fail);
		warn = fail ? 0 : (rpm <= fcd_sysfan_warn);

		ret = snprintf(buf, sizeof buf, "%'d RPM", rpm);
		if (ret < 0) {
			FCD_PERROR("snprintf");
			fcd_sysfan_close_and_disable(fp, mon);
		}

		if (ret < (int)sizeof buf)
			buf[ret] = ' ';

		fcd_lib_set_mon_status(mon, buf, warn, fail, NULL);

		ret = fcd_lib_monitor_sleep(30);
		if (ret == -1)
			fcd_sysfan_close_and_disable(fp, mon);

	} while (ret == 0);

	if (fclose(fp) != 0)
		FCD_PERROR("fclose");

	pthread_exit(NULL);
}

struct fcd_monitor fcd_sysfan_monitor = {
	.mutex		= PTHREAD_MUTEX_INITIALIZER,
	.name		= "system fan",
	.monitor_fn	= fcd_sysfan_fn,
	.buf		= "....."
			  "SYSTEM FAN          "
			  "                    ",
};
