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

#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#define FCD_HDDTEMP_BUF_MAX	1000

static char *fcd_hddtemp_cmd[FCD_MAX_DISK_COUNT + 3] = {
	"/usr/sbin/hddtemp", "hddtemp"
	/* remaining array members are automatically set to NULL */
};

/* Alert thresholds */
static const int fcd_hddtemp_warn_def = 45;
static const int fcd_hddtemp_crit_def = 50;

static int fcd_hddtemp_freecusd_cb();

static const cip_opt_info fcd_hddtemp_freecusd_opts[] = {
	{
		.name			= "hdd_temp_warn",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_hddtemp_freecusd_cb,
		.post_parse_data	= &fcd_conf_disks[0].temp_warn,
		.flags			= CIP_OPT_DEFAULT,
		.default_value		= &fcd_hddtemp_warn_def,
	},
	{
		.name			= "hdd_temp_crit",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_hddtemp_freecusd_cb,
		.post_parse_data	= &fcd_conf_disks[0].temp_crit,
		.flags			= CIP_OPT_DEFAULT,
		.default_value		= &fcd_hddtemp_crit_def,
	},
	{
		.name			= NULL
	}
};

static int fcd_hddtemp_raiddisk_cb();

static const cip_opt_info fcd_hddtemp_raiddisk_opts[] = {
	{
		.name			= "hddtemp_monitor_ignore",
		.type			= CIP_OPT_TYPE_BOOL,
		.post_parse_fn		= fcd_conf_disk_bool_cb,
		.post_parse_data	= &fcd_conf_disks[0].temp_ignore,
	},
	{
		.name			= "hdd_temp_warn",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_hddtemp_raiddisk_cb,
		.post_parse_data	= &fcd_conf_disks[0].temp_warn,
	},
	{
		.name			= "hdd_temp_crit",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_hddtemp_raiddisk_cb,
		.post_parse_data	= &fcd_conf_disks[0].temp_crit,
	},
	{
		.name			= NULL
	}
};

static int fcd_hddtemp_check_temp(cip_err_ctx *ctx, int temp)
{
	if (temp < -273) {
		cip_err(ctx, "Invalid temperature (below absolute zero): %d",
			temp);
		return -1;
	}

	if (temp <= 0 || temp >= 1000)
		cip_err(ctx, "Probably not a useful HDD temperature: %d", temp);

	return 0;
}

static int fcd_hddtemp_freecusd_cb(cip_err_ctx *ctx, const cip_ini_value *value,
				   const cip_ini_sect *sect
						__attribute__((unused)),
				   const cip_ini_file *file
						__attribute__((unused)),
				   void *post_parse_data)
{
	int temp, *p;
	unsigned i;

	p = (int *)(value->value);
	temp = *p;

	if (fcd_hddtemp_check_temp(ctx, temp) == -1)
		return -1;

	for (i = 0; i < FCD_MAX_DISK_COUNT; ++i) {
		p = fcd_conf_disk_member(post_parse_data, i);
		*p = temp;
	}

	return 0;
}

static int fcd_hddtemp_raiddisk_cb(cip_err_ctx *ctx, const cip_ini_value *value,
				   const cip_ini_sect *sect,
				   const cip_ini_file *file,
				   void *post_parse_data)
{
	int ret, temp;

	/*
	 * If we're in the [raid_disk:X] section for a missing disk,
	 * fcd_conf_disk_int_cb_help() will return 0, but temp won't be changed.
	 * Set it to something that won't trigger a spurious warning/error.
	 */
	temp = 25;

	ret = fcd_conf_disk_int_cb_help(ctx, value, sect, file, post_parse_data,
					&temp);
	if (ret != 0)
		return ret;

	if (fcd_hddtemp_check_temp(ctx, temp) == -1)
		return -1;

	return 0;
}

static void fcd_hddtemp_mkcmd(void)
{
	unsigned i, j;

	for (i = 0, j = 0; i < fcd_conf_disk_count; ++i) {

		if (fcd_conf_disks[i].temp_ignore)
			continue;

		fcd_hddtemp_cmd[j + 2] = fcd_conf_disks[i].name;
		++j;
	}
}

static int fcd_hddtemp_exec(char **cmd_buf, size_t *buf_size,
			    const int *pipe_fds, struct fcd_monitor *mon)
{
	struct timespec timeout;
	int ret, status;

	/* May need to increase timeout for more than 5 disks */

	timeout.tv_sec = 5;
	timeout.tv_nsec = 0;

	ret = fcd_lib_cmd_output(&status, fcd_hddtemp_cmd, cmd_buf, buf_size,
				 FCD_HDDTEMP_BUF_MAX, &timeout, pipe_fds);

	switch (ret) {

		case -4:
			fcd_lib_disable_cmd_mon(mon, pipe_fds, *cmd_buf);

		case -3:
			return -3;

		case -2:
			FCD_WARN("hddtemp command timed out\n");

		case -1:
			fcd_lib_disable_cmd_mon(mon, pipe_fds, *cmd_buf);
	}

	if (status != 0) {
		FCD_WARN("Non-zero hddtemp exit status: %d\n", status);
		fcd_lib_disable_cmd_mon(mon, pipe_fds, *cmd_buf);
	}

	return 0;
}

static void fcd_hddtemp_parse(char *cmd_buf, int *temps, const int *pipe_fds,
			      struct fcd_monitor *mon)
{
	int ret, temp, n;
	const char *s;
	char c;

	s = cmd_buf;

	for (s = cmd_buf; *s != 0; s += n) {

		n = -1;
		errno = 0;

		ret = sscanf(s, "/dev/sd%c:%*[^:]: %d%*[^\n]%*1[\n]%n",
			     &c, &temp, &n);
		if (ret == EOF) {
			if (errno == 0)
				FCD_WARN("Unexpected end of hddtemp output\n");
			else
				FCD_PERROR("sscanf");
			fcd_lib_disable_cmd_mon(mon, pipe_fds, cmd_buf);
		}

		if (ret != 2 || n == -1 || errno != 0)
			goto parse_error;

		ret = fcd_lib_disk_index(c);
		if (ret == -1)
			goto parse_error;

		temps[ret] = temp;
	}

	return;

parse_error:
	FCD_WARN("Error parsing hddtemp output\n");
	fcd_lib_disable_cmd_mon(mon, pipe_fds, cmd_buf);
}


__attribute__((noreturn))
static void *fcd_hddtemp_fn(void *arg)
{
	struct fcd_monitor *mon = arg;
	int ret, i, temps[FCD_MAX_DISK_COUNT], pipe_fds[2];
	int max, warn, fail, disk_alerts[FCD_MAX_DISK_COUNT];
	char *b, buf[21], *cmd_buf;
	size_t buf_size;

	if (pipe2(pipe_fds, O_CLOEXEC) == -1) {
		FCD_PERROR("pipe2");
		fcd_lib_disable_monitor(mon);
	}

	fcd_hddtemp_mkcmd();
	cmd_buf = NULL;
	buf_size = 0;

	do {
		memset(buf, ' ', sizeof buf);
		for (i = 0; i < (int)fcd_conf_disk_count; ++i)
			temps[i] = INT_MIN;

		if (fcd_hddtemp_exec(&cmd_buf, &buf_size, pipe_fds, mon) == -3)
			break;

		fcd_hddtemp_parse(cmd_buf, temps, pipe_fds, mon);

		memset(disk_alerts, 0, sizeof disk_alerts);
		max = 0;

		for (i = 0, b = buf; i < (int)fcd_conf_disk_count; ++i) {

			if (fcd_conf_disks[i].temp_ignore) {
				memset(b, '.', 2);
				b += 3;
			}
			else if (temps[i] >= -99 && temps[i] <= 999) {

				disk_alerts[i] =
				      (temps[i] >= fcd_conf_disks[i].temp_warn);
				if (temps[i] > max)
					max = temps[i];

				ret = sprintf(b, "%d ", temps[i]);
				if (ret == EOF) {
					FCD_PERROR("sprintf");
					fcd_lib_disable_cmd_mon(mon, pipe_fds,
								cmd_buf);
				}
				b += ret;
			}
			else {
				if (temps[i] == INT_MIN)
					memset(b, '?', 2);
				else
					memset(b, '*', 2);
				b += 3;
			}
		}

		fail = (max >= fcd_conf_disks[0].temp_crit);
		warn = fail ? 0 : (max >= fcd_conf_disks[0].temp_warn);

		fcd_lib_set_mon_status(mon, buf, warn, fail, disk_alerts);

		ret = fcd_lib_monitor_sleep(30);
		if (ret == -1)
			fcd_lib_disable_cmd_mon(mon, pipe_fds, cmd_buf);

	} while (ret == 0);

	free(cmd_buf);
	fcd_proc_close_pipe(pipe_fds);
	pthread_exit(NULL);
}

struct fcd_monitor fcd_hddtemp_monitor = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,
	.name			= "HDD temperature",
	.monitor_fn		= fcd_hddtemp_fn,
	.buf			= "....."
				  "HDD TEMPERATURE     "
				  "                    ",
	.enabled		= true,
	.enabled_opt_name	= "enable_hddtemp_monitor",
	.raiddisk_opts		= fcd_hddtemp_raiddisk_opts,
	.freecusd_opts		= fcd_hddtemp_freecusd_opts,
};
