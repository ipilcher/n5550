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

static const char *fcd_cputemp_input[2] = {
	"/sys/devices/platform/coretemp.0/temp2_input",
	"/sys/devices/platform/coretemp.0/temp3_input"
};

__attribute__((noreturn))
static void fcd_cputemp_close_and_disable(FILE **fp, struct fcd_monitor *mon)
{
	int i;

	for (i = 0; i < 2; ++i) {
		if (fclose(fp[i]) == EOF)
			FCD_ERR("fclose: %m\n");
	}

	fcd_disable_monitor(mon);

}

__attribute__((noreturn))
static void *fcd_cputemp_fn(void *arg)
{
	struct fcd_monitor *mon = arg;
	double temp[2];
	char buf[21];
	FILE *fp[2];
	int i, ret;

	for (i = 0; i < 2; ++i) {
		fp[i] = fopen(fcd_cputemp_input[i], "r");
		if (fp[i] == NULL) {
			FCD_ERR("fopen: %m\n");
			if (i == 1 && fclose(fp[0]) == EOF)
				FCD_ERR("fclose: %m\n");
			fcd_disable_monitor(mon);
		}
	}

	for (i = 0; i < 2; ++i) {
		if (setvbuf(fp[i], NULL, _IONBF, 0) != 0) {
			FCD_ERR("setvbuf: %m\n");
			fcd_cputemp_close_and_disable(fp, mon);
		}
	}

	do {
		memset(buf, ' ', sizeof buf);

		for (i = 0; i < 2; ++i)
		{
			rewind(fp[i]);

			ret = fscanf(fp[i], "%lf", &temp[i]);
			if (ret == EOF) {
				FCD_ERR("fscanf: %m\n");
				fcd_cputemp_close_and_disable(fp, mon);
			}
			else if (ret != 1) {
				FCD_WARN("Failed to parse contents of %s\n",
					 fcd_cputemp_input[i]);
				fcd_cputemp_close_and_disable(fp, mon);
			}
		}

		ret = snprintf(buf, sizeof buf, "%.1f %.1f",
			       temp[0] / 1000.0, temp[1] / 1000.0);
		if (ret < 0) {
			FCD_ERR("snprintf: %m\n");
			fcd_cputemp_close_and_disable(fp, mon);
		}

		if (ret < (int)sizeof buf)
			buf[ret] = ' ';

		fcd_copy_buf(buf, mon);

	} while (fcd_sleep_and_check_exit(30) == 0);

	for (i = 0; i < 2; ++i) {
		if (fclose(fp[i]) == EOF)
			FCD_ERR("fclose: %m\n");
	}

	pthread_exit(NULL);
}

struct fcd_monitor fcd_cputemp_monitor = {
	.mutex		= PTHREAD_MUTEX_INITIALIZER,
	.name		= "CPU temperature",
	.monitor_fn	= fcd_cputemp_fn,
	.buf		= "....."
			  "CPU TEMPERATURE     "
			  "                    ",
};
