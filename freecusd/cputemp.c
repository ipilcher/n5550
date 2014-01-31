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
#include <limits.h>

/* Alert thresholds */
static int fcd_cputemp_warn = 47000;	/* cpu_temp_warn */
static int fcd_cputemp_fail = 52000;	/* cpu_temp_crit */

static int fcd_cputemp_cb();

static const cip_opt_info fcd_cputemp_opts[] = {
	{
		.name			= "cpu_temp_warn",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_cputemp_cb,
		.post_parse_data	= &fcd_cputemp_warn,
	},
	{
		.name			= "cpu_temp_crit",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_cputemp_cb,
		.post_parse_data	= &fcd_cputemp_fail,
	},
	{	.name			= NULL		}
};

static const char *fcd_cputemp_input[2] = {
	"/sys/devices/platform/coretemp.0/temp2_input",
	"/sys/devices/platform/coretemp.0/temp3_input"
};

/*
 * Configuration callback for alert thresholds
 */
static int fcd_cputemp_cb(cip_err_ctx *ctx, const cip_ini_value *value,
			  const cip_ini_sect *sect __attribute__((unused)),
			  const cip_ini_file *file __attribute__((unused)),
			  void *post_parse_data)
{
	double temp;
	float *p;

	p = (float *)(value->value);
	temp = *p;

	if (temp < INT_MIN / 1000 || temp > INT_MAX / 1000) {
		cip_err(ctx,
			"CPU temperature (%g) outside valid range (%d - %d)",
			temp, INT_MIN / 1000, INT_MAX / 1000);
		return -1;
	}

	if (temp <= 0.0 || temp >= 1000.0)
		cip_err(ctx, "Probably not a useful CPU temperature: %g", temp);

	*(int *)post_parse_data = (int)(temp * 1000.0);

	return 0;
}

__attribute__((noreturn))
static void fcd_cputemp_close_and_disable(FILE **fp, struct fcd_monitor *mon)
{
	int i;

	for (i = 0; i < 2; ++i) {
		if (fclose(fp[i]) == EOF)
			FCD_PERROR("fclose");
	}

	fcd_lib_disable_monitor(mon);

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
			fcd_lib_disable_monitor(mon);
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

		fcd_lib_set_mon_status(mon, buf, warn, fail, NULL);

		ret = fcd_lib_monitor_sleep(30);
		if (ret == -1)
			fcd_cputemp_close_and_disable(fps, mon);

	} while (ret == 0);

	for (i = 0; i < 2; ++i) {
		if (fclose(fps[i]) == EOF)
			FCD_PERROR("fclose");
	}

	pthread_exit(NULL);
}

struct fcd_monitor fcd_cputemp_monitor = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,
	.name			= "CPU temperature",
	.monitor_fn		= fcd_cputemp_fn,
	.buf			= "....."
				  "CPU TEMPERATURE     "
				  "                    ",
	.enabled		= true,
	.enabled_opt_name	= "enable_cputemp_monitor",
	.freecusd_opts		= fcd_cputemp_opts,
};
