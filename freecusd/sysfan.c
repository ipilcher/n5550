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

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "freecusd.h"

static const char fcd_sysfan_input[] =
		"/sys/devices/platform/it87.656/fan3_input";

__attribute__((noreturn))
static void fcd_sysfan_close_and_disable(FILE *fp, struct fcd_monitor *mon)
{
	if (fclose(fp) != 0)
		FCD_ERR("fclose: %m\n");
	fcd_disable_monitor(mon);

}

__attribute__((noreturn))
static void *fcd_sysfan_fn(void *arg)
{
	struct fcd_monitor *mon = arg;
	char buf[21];
	FILE *fp;
	int rpm, ret;

	fp = fopen(fcd_sysfan_input, "r");
	if (fp == NULL) {
		FCD_ERR("fopen: %m\n");
		fcd_disable_monitor(mon);
	}

	if (setvbuf(fp, NULL, _IONBF, 0) != 0) {
		FCD_ERR("setvbuf: %m\n");
		fcd_sysfan_close_and_disable(fp, mon);
	}

	do {
		rewind(fp);
		memset(buf, ' ', sizeof buf);

		ret = fscanf(fp, "%d", &rpm);
		if (ret == EOF) {
			FCD_ERR("fscanf: %m\n");
			fcd_sysfan_close_and_disable(fp, mon);
		}
		else if (ret != 1) {
			FCD_WARN("Failed to parse contents of %s\n",
				 fcd_sysfan_input);
			fcd_sysfan_close_and_disable(fp, mon);
		}

		ret = snprintf(buf, sizeof buf, "%'d RPM", rpm);
		if (ret < 0) {
			FCD_ERR("snprintf: %m\n");
			fcd_sysfan_close_and_disable(fp, mon);
		}

		if (ret < (int)sizeof buf)
			buf[ret] = ' ';

		fcd_copy_buf(buf, mon);

	} while (fcd_sleep_and_check_exit(30) == 0);

	if (fclose(fp) != 0)
		FCD_ERR("fclose: %m\n");

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
