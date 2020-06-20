/*
 * Copyright 2013-2014, 2016-2017, 2020 Ian Pilcher <arequipeno@gmail.com>
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
#include <errno.h>
#include <fcntl.h>

#include "smart/status.h"

#define FCD_SMART_BUF_MAX	100

static char *fcd_smart_cmd[] = {
	[0] = "/usr/libexec/freecusd-smart-helper",
	[1] = "freecusd-smart-helper",
	[2] = NULL,			/* disk goes here */
	[3] = NULL
};

/* Alert thresholds */
static const int fcd_hddtemp_warn_def = 45;
static const int fcd_hddtemp_crit_def = 50;

static const cip_opt_info fcd_smart_opts[] = {
	{
		.name			= "smart_monitor_ignore",
		.type			= CIP_OPT_TYPE_BOOL,
		.post_parse_fn		= fcd_conf_disk_bool_cb,
		.post_parse_data	= &fcd_conf_disks[0].smart_ignore,
	},
	{	.name			= NULL		}
};
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

__attribute__((noreturn))
static void fcd_smart_disable(char *restrict cmd_buf,
			      const int *const restrict pipe_fds)
{
	fcd_lib_disable_slave(&fcd_hddtemp_monitor);
	fcd_lib_disable_cmd_mon(&fcd_smart_monitor, pipe_fds, cmd_buf);
}

static int fcd_smart_exec(const int disk,
			  char **const restrict cmd_buf,
			  size_t *const restrict buf_size,
			  const int *const restrict pipe_fds)

{
	struct timespec timeout;
	int ret, status;

	timeout.tv_sec = 5;
	timeout.tv_nsec = 0;

	fcd_smart_cmd[2] = fcd_conf_disks[disk].name;

	ret = fcd_lib_cmd_output(&status,
				 fcd_smart_cmd,
				 cmd_buf,
				 buf_size,
				 FCD_SMART_BUF_MAX,
				 &timeout,
				 pipe_fds);

	switch (ret) {

		case -4:		/* Max buffer size exceeded */
			fcd_smart_disable(*cmd_buf, pipe_fds);

		case -3:		/* Got exit signal */
			return -3;

		case -2:
			FCD_WARN("%s timed out\n", fcd_smart_cmd[1]);
			return -2;

		case -1:		/* Error spawning helper */
			fcd_smart_disable(*cmd_buf, pipe_fds);
	}

	if (status != 0) {
		FCD_WARN("Non-zero %s exit status: %d\n",
			 fcd_smart_cmd[1],
			 status);
		return -2;		/* Treat like a timeout */
	}

	return 0;
}

static void fcd_smart_parse(const int disk,
			    int *const restrict status,
			    int *const restrict temps,
			    char *const restrict cmd_buf,
			    const int *const restrict pipe_fds)
{
	int ret;

	errno = 0;
	ret = sscanf(cmd_buf, "%d\n%d\n", &status[disk], &temps[disk]);
	if (ret == 2 && errno == 0)
		return;

	if (errno != 0)
		FCD_ABORT("Error parsing %s output: %m\n", fcd_smart_cmd[1]);
	else if (ret == EOF)
		FCD_ABORT("Unexpected end of %s output\n", fcd_smart_cmd[1]);
	else
		FCD_ABORT("Error parsing %s output\n", fcd_smart_cmd[1]);

	fcd_smart_disable(cmd_buf, pipe_fds);
}

static void process_status(int *const restrict status)
{
	int alerts[FCD_MAX_DISK_COUNT], warn, fail;
	char buf[21], *c;
	unsigned i;

	memset(alerts, 0, sizeof alerts);
	memset(buf, ' ', sizeof buf);
	warn = 0;
	fail = 0;

	for (i = 0; i < fcd_conf_disk_count; ++i) {

		c = buf + (fcd_conf_disks[i].port_no - 2) * 4;

		if (fcd_conf_disks[i].smart_ignore) {
			memset(c, '.', 2);
		}
		else if (status[i] == FCD_SMART_ASLEEP) {
			memset(c, '-', 2);
		}
		else if (status[i] == FCD_SMART_FAIL) {
			memset(c, '*', 2);
			alerts[i] = 1;
			fail = 1;
			warn = 0;
		}
		else if (status[i] == FCD_SMART_ERROR) {
			memset(c, 'X', 2);
			alerts[i] = 1;
			warn = !fail;
		}
		else if (status[i] == FCD_SMART_WARN) {
			memset(c, '?', 2);
			alerts[i] = 1;
			warn = !fail;
		}
		else {
			memcpy(c, "OK", 2);
		}
	}

	fcd_lib_set_mon_status(&fcd_smart_monitor, buf, warn, fail, alerts, 0);
}

static void process_temps(int *const restrict status,
			  int *const restrict temps,
			  char *const restrict cmd_buf,
			  const int *const restrict pipe_fds)
{
	int alerts[FCD_MAX_DISK_COUNT], warn, fail;
	char buf[21], *c;
	unsigned i;
	int ret;

	memset(alerts, 0, sizeof alerts);
	memset(buf, ' ', sizeof buf);
	warn = 0;
	fail = 0;

	for (i = 0; i < fcd_conf_disk_count; ++i) {

		c = buf + (fcd_conf_disks[i].port_no - 2) * 4;

		if (fcd_conf_disks[i].temp_ignore) {
			memset(c, '.', 3);
		}
		else if (status[i] == FCD_SMART_ASLEEP) {
			memset(c, '-', 3);
		}
		else if (status[i] == FCD_SMART_ERROR) {
			memset(c, 'X', 3);
			alerts[i] = 1;
			warn = !fail;
		}
		else if (temps[i] < -99) {
			memcpy(c, "-**", 3);
			alerts[i] = 1;
			warn = !fail;
		}
		else if (temps[i] > 999) {
			memset(c, '*', 3);
			alerts[i] = 1;
			warn = !fail;
		}
		else {
			ret = sprintf(c, "%d", temps[i]);
			if (ret < 0) {
				FCD_PERROR("sprintf");
				fcd_smart_disable(cmd_buf, pipe_fds);
			}

			c[ret] = ' ';	/* sprintf 0-terminates */

			if (temps[i] >= fcd_conf_disks[i].temp_crit) {
				alerts[i] = 1;
				fail = 1;
				warn = 0;
			}
			else if (temps[i] >= fcd_conf_disks[i].temp_warn
							|| temps[i] <= 0) {
				alerts[i] = 1;
				warn = !fail;
			}
		}
	}

	fcd_lib_set_mon_status(&fcd_hddtemp_monitor, buf, warn, fail, alerts, 0);
}


__attribute__((noreturn))
static void *fcd_smart_fn(void *arg __attribute__((unused)))
{
	int status[FCD_MAX_DISK_COUNT], temps[FCD_MAX_DISK_COUNT];
	int pipe_fds[2];
	int ret;
	char *cmd_buf;
	size_t buf_size;
	unsigned i;

	if (pipe2(pipe_fds, O_CLOEXEC) == -1) {
		FCD_PERROR("pipe2");
		fcd_lib_disable_slave(&fcd_hddtemp_monitor);
		fcd_lib_disable_monitor(&fcd_smart_monitor);
	}

	cmd_buf = NULL;
	buf_size = 0;

	do {
		for (i = 0; i < fcd_conf_disk_count; ++i) {

			if (!fcd_conf_disks[i].smart_ignore
					|| !fcd_conf_disks[i].temp_ignore) {

				ret = fcd_smart_exec(i,
						     &cmd_buf,
						     &buf_size,
						     pipe_fds);
				if (ret == -3) {
					goto break_outer_loop;
				}
				else if (ret == -2) {
					status[i] = FCD_SMART_ERROR;
					continue;
				}

				fcd_smart_parse(i,
						status,
						temps,
						cmd_buf,
						pipe_fds);
			}
		}

		process_status(status);
		process_temps(status, temps, cmd_buf, pipe_fds);

		ret = fcd_lib_monitor_sleep(30);
		if (ret == -1)
			fcd_smart_disable(cmd_buf, pipe_fds);

	} while (ret == 0);

break_outer_loop:
	free(cmd_buf);
	fcd_proc_close_pipe(pipe_fds);
	pthread_exit(NULL);
}

struct fcd_monitor fcd_smart_monitor = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,
	.name			= "SMART status",
	.monitor_fn		= fcd_smart_fn,
	.buf			= "....."
				  "S.M.A.R.T. STATUS   "
				  "                    ",
	.enabled		= true,
	.enabled_opt_name	= "enable_smart_monitor",
	.raiddisk_opts		= fcd_smart_opts,
};

struct fcd_monitor fcd_hddtemp_monitor = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,
	.name			= "HDD temperature",
	.monitor_fn		= 0,
	.buf			= "....."
				  "HDD TEMPERATURE     "
				  "                    ",
	.enabled		= true,
	.enabled_opt_name	= "enable_hddtemp_monitor",
	.raiddisk_opts		= fcd_hddtemp_raiddisk_opts,
	.freecusd_opts		= fcd_hddtemp_freecusd_opts,
};
