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
static const int fcd_cputemp_warn = 47000;
static const int fcd_cputemp_fail = 52000;

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
			FCD_PERROR("fclose");
	}

	fcd_disable_monitor(mon);

}

__attribute__((noreturn))
static void *fcd_cputemp_fn(void *arg)
{
	struct fcd_monitor *mon = arg;
	int warn, fail, i, ret, max, temps[2];
	char buf[21];
	FILE *fps[2];

	for (i = 0; i < 2; ++i) {
		fps[i] = fopen(fcd_cputemp_input[i], "re");
		if (fps[i] == NULL) {
			FCD_PERROR(fcd_cputemp_input[i]);
			if (i == 1 && fclose(fps[0]) == EOF)
				FCD_PERROR("fclose");
			fcd_disable_monitor(mon);
		}
	}

	for (i = 0; i < 2; ++i) {
		if (setvbuf(fps[i], NULL, _IONBF, 0) != 0) {
			FCD_PERROR("setvbuf");
			fcd_cputemp_close_and_disable(fps, mon);
		}
	}

	do {
		memset(buf, ' ', sizeof buf);

		for (i = 0; i < 2; ++i)
		{
			rewind(fps[i]);

			ret = fscanf(fps[i], "%d", &temps[i]);
			if (ret == EOF) {
				FCD_PERROR("fscanf");
				fcd_cputemp_close_and_disable(fps, mon);
			}
			else if (ret != 1) {
				FCD_WARN("Failed to parse contents of %s\n",
					 fcd_cputemp_input[i]);
				fcd_cputemp_close_and_disable(fps, mon);
			}
		}

		max = (temps[0] > temps[1]) ? temps[0] : temps[1];
		fail = (max >= fcd_cputemp_fail);
		warn = fail ? 0 : (max >= fcd_cputemp_warn);

		ret = snprintf(buf, sizeof buf, "%.1f %.1f",
			       ((double)temps[0]) / 1000.0,
			       ((double)temps[1]) / 1000.0);
		if (ret < 0) {
			FCD_PERROR("snprintf");
			fcd_cputemp_close_and_disable(fps, mon);
		}

		if (ret < (int)sizeof buf)
			buf[ret] = ' ';

		fcd_copy_buf_and_alerts(mon, buf, warn, fail, NULL);

	} while (fcd_sleep_and_check_exit(30) == 0);

	for (i = 0; i < 2; ++i) {
		if (fclose(fps[i]) == EOF)
			FCD_PERROR("fclose");
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
