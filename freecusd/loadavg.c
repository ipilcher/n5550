/*
 * Copyright 2013-2014, 2020 Ian Pilcher <arequipeno@gmail.com>
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
static double fcd_loadavg_warn[3] = { 12.0, 12.0, 12.0 };
static double fcd_loadavg_crit[3] = { 16.0, 16.0, 16.0 };

static int fcd_loadavg_cb();

static const cip_opt_info fcd_loadavg_opts[] = {
	{
		.name			= "load_avg_warn",
		.type			= CIP_OPT_TYPE_FLOAT_LIST,
		.post_parse_fn		= fcd_loadavg_cb,
		.post_parse_data	= fcd_loadavg_warn,
	},
	{
		.name			= "load_avg_crit",
		.type			= CIP_OPT_TYPE_FLOAT_LIST,
		.post_parse_fn		= fcd_loadavg_cb,
		.post_parse_data	= fcd_loadavg_crit,
	},
	{	.name			= NULL		}
};

/*
 * Configuration callback for alert thresholds
 */
static int fcd_loadavg_cb(cip_err_ctx *ctx, const cip_ini_value *value,
			  const cip_ini_sect *sect __attribute__((unused)),
			  const cip_ini_file *file __attribute__((unused)),
			  void *post_parse_data)
{
	const cip_float_list *list;
	double avg, *p;
	unsigned i;

	list = (const cip_float_list *)(value->value);
	if (list->count != 3) {
		cip_err(ctx, "Must specify 3 load average values");
		return -1;
	}

	p = post_parse_data;

	for (i = 0; i < 3; ++i) {

		avg = list->values[i];

		if (avg <= 0.0 || avg >= 100.0) {
			cip_err(ctx,
				"Probably not a useful load average value: %g",
				avg);
		}

		p[i] = avg;
	}

	return 0;
}

__attribute__((noreturn))
static void fcd_loadavg_close_and_disable(FILE *fp, struct fcd_monitor *mon)
{
	if (fclose(fp) != 0)
		FCD_PERROR("fclose");
	fcd_lib_fail_and_exit(mon);

}

__attribute__((noreturn))
static void *fcd_loadavg_fn(void *arg)
{
	static const char path[] = "/proc/loadavg";
	struct fcd_monitor *mon = arg;
	int warn, fail, ret;
	double avgs[3];
	char buf[21];
	unsigned i;
	FILE *fp;

	fp = fopen(path, "re");
	if (fp == NULL) {
		FCD_PERROR(path);
		fcd_lib_fail_and_exit(mon);
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

		for (fail = 0, warn = 0, i = 0; i < FCD_ARRAY_SIZE(avgs); ++i) {

			if (avgs[i] >= fcd_loadavg_crit[i]) {
				fail = 1;
				warn = 0;
				break;
			}

			if (avgs[i] >= fcd_loadavg_warn[i])
				warn = 1;
		}

		ret = fcd_lib_snprintf(buf, sizeof buf, "%.2f %.2f %.2f",
				       avgs[0], avgs[1], avgs[2]);
		if (ret < 0)
			fcd_loadavg_close_and_disable(fp, mon);

		fcd_lib_set_mon_status(mon, buf, warn, fail, NULL, 0);

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
	.freecusd_opts		= fcd_loadavg_opts,
};
