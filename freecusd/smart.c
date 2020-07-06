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
#include "smart/status.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>

#define FCD_SMART_BUF_MAX	100

static char *fcd_smart_cmd[] = {
	[0] = "/usr/libexec/freecusd-smart-helper",
	[1] = "freecusd-smart-helper",
	[2] = NULL,			/* disk goes here */
	[3] = NULL
};

/* Alert & PWM thresholds */
static const int fcd_smart_temp_defaults[FCD_CONF_TEMP_ARRAY_SIZE] = {
	[FCD_CONF_TEMP_WARN]		= 45,		/* hdd_temp_warn */
	[FCD_CONF_TEMP_FAIL]		= 50,		/* hdd_temp_crit */
	[FCD_CONF_TEMP_FAN_MAX_ON]	= 43,		/* hdd_temp_fan_max_on */
	[FCD_CONF_TEMP_FAN_MAX_HYST]	= 41,		/* hdd_temp_fan_max_hyst */
	[FCD_CONF_TEMP_FAN_HIGH_ON]	= 40,		/* hdd_temp_fan_high_on */
	[FCD_CONF_TEMP_FAN_HIGH_HYST]	= 38		/* hdd_temp_fan_high_hyst */
};

static int fcd_smart_temp_cb();
static int fcd_smart_temp_disk_cb();
static int fcd_smart_ignore_cb();

static const cip_opt_info fcd_smart_disk_opts[] = {
	{
		.name			= "smart_monitor_ignore",
		.type			= CIP_OPT_TYPE_BOOL,
		.post_parse_fn		= fcd_smart_ignore_cb,
		.post_parse_data	= &fcd_smart_monitor,
	},
	{
		.name			= NULL
	}
};

static const cip_opt_info fcd_smart_temp_opts[] = {
	{
		.name			= "hdd_temp_warn",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_smart_temp_cb,
		.post_parse_data	= FCD_CONF_TEMP_WARN,
		.flags			= CIP_OPT_DEFAULT,
		.default_value		= &fcd_smart_temp_defaults[FCD_CONF_TEMP_WARN],
	},
	{
		.name			= "hdd_temp_crit",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_smart_temp_cb,
		.post_parse_data	= (void *)FCD_CONF_TEMP_FAIL,
		.flags			= CIP_OPT_DEFAULT,
		.default_value		= &fcd_smart_temp_defaults[FCD_CONF_TEMP_FAIL],
	},
	{
		.name			= "hdd_temp_fan_max_on",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_smart_temp_cb,
		.post_parse_data	= (void *)FCD_CONF_TEMP_FAN_MAX_ON,
		.flags			= CIP_OPT_DEFAULT,
		.default_value		= &fcd_smart_temp_defaults[FCD_CONF_TEMP_FAN_MAX_ON],
	},
	{
		.name			= "hdd_temp_fan_max_hyst",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_smart_temp_cb,
		.post_parse_data	= (void *)FCD_CONF_TEMP_FAN_MAX_HYST,
		.flags			= CIP_OPT_DEFAULT,
		.default_value		= &fcd_smart_temp_defaults[FCD_CONF_TEMP_FAN_MAX_HYST],
	},
	{
		.name			= "hdd_temp_fan_high_on",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_smart_temp_cb,
		.post_parse_data	= (void *)FCD_CONF_TEMP_FAN_HIGH_ON,
		.flags			= CIP_OPT_DEFAULT,
		.default_value		= &fcd_smart_temp_defaults[FCD_CONF_TEMP_FAN_HIGH_ON],
	},
	{
		.name			= "hdd_temp_fan_high_hyst",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_smart_temp_cb,
		.post_parse_data	= (void *)FCD_CONF_TEMP_FAN_HIGH_HYST,
		.flags			= CIP_OPT_DEFAULT,
		.default_value		= &fcd_smart_temp_defaults[FCD_CONF_TEMP_FAN_HIGH_HYST],
	},
	{
		.name			= NULL
	}
};

static const cip_opt_info fcd_smart_temp_disk_opts[] = {
	{
		.name			= "hddtemp_monitor_ignore",
		.type			= CIP_OPT_TYPE_BOOL,
		.post_parse_fn		= fcd_smart_ignore_cb,
		.post_parse_data	= &fcd_hddtemp_monitor,
	},
	{
		.name			= "hdd_temp_warn",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_smart_temp_disk_cb,
		.post_parse_data	= (void *)FCD_CONF_TEMP_WARN,
	},
	{
		.name			= "hdd_temp_crit",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_smart_temp_disk_cb,
		.post_parse_data	= (void *)FCD_CONF_TEMP_FAIL,
	},
	{
		.name			= "hdd_temp_fan_max_on",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_smart_temp_disk_cb,
		.post_parse_data	= (void *)FCD_CONF_TEMP_FAN_MAX_ON,
	},
	{
		.name			= "hdd_temp_fan_max_hyst",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_smart_temp_disk_cb,
		.post_parse_data	= (void *)FCD_CONF_TEMP_FAN_MAX_HYST,
	},
	{
		.name			= "hdd_temp_fan_high_on",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_smart_temp_disk_cb,
		.post_parse_data	= (void *)FCD_CONF_TEMP_FAN_HIGH_ON,
	},
	{
		.name			= "hdd_temp_fan_high_hyst",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_smart_temp_disk_cb,
		.post_parse_data	= (void *)FCD_CONF_TEMP_FAN_HIGH_HYST,
	},
	{
		.name			= NULL
	}
};

#if 0
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
#endif

/*
 * Gets a disk temperature setting (int) out of a cip_ini_value and
 * validates it.  Returns the temperature (or INT_MIN on error).
 */
static int fcd_smart_temp_get_conf(cip_err_ctx *const ctx,
				   const cip_ini_value *const value)
{
	int temp;

	memcpy(&temp, value->value, sizeof temp);

	if (temp < -273) {
		cip_err(ctx, "Invalid temperature (below absolute zero): %d", temp);
		return INT_MIN;
	}

	if (temp <= 0 || temp >= 1000)
		cip_err(ctx, "Probably not a useful HDD temperature: %d", temp);

	return temp;
}

/*
 * Callback for disk temperatures in the main ([freecusd]) config section
 * Each value is copied to the fcd_conf_disks member for all detected
 * RAID disks, where it may subsequently be overwritten by a disk-specific
 * override.
 */
static int fcd_smart_temp_cb(cip_err_ctx *const ctx,
			     const cip_ini_value *const value,
			     const cip_ini_sect *const sect __attribute__((unused)),
			     const cip_ini_file *const file __attribute__((unused)),
			     void *const post_parse_data)
{
	enum fcd_conf_temp_type temp_type;
	int temp, i;

	if ((temp = fcd_smart_temp_get_conf(ctx, value)) == INT_MIN)
		return -1;

	temp_type = (enum fcd_conf_temp_type)post_parse_data;

	for (i = 0; i < FCD_MAX_DISK_COUNT; ++i)
		fcd_conf_disks[i].temps[temp_type] = temp;

	return 0;
}

/*
 * Parse a RAID disk "name" (the X in a [raid_disk:X] config section).
 * X must a decimal integer in the range 1 - FCD_MAX_DISK_COUNT; any
 * whitespace or extra characters are an error.
 *
 * Returns the parsed integer or -1 to indicate a parsing or out of
 * range error.
 */
static int fcd_smart_parse_raid_num(const char *name)
{
	int i = 0;

	while (*name != 0) {

		i *= 10;

		if (!isdigit(*name))
			return -1;

		i += *name - '0';

		/* Leading zeroes not allowed, so i should never be 0 here */
		if (i == 0 || i > FCD_MAX_DISK_COUNT)
			return -1;

		++name;
	}

	/* Empty string? */
	if (i == 0)
		return -1;

	return i;
}

/*
 * Finds the index (in fcd_conf_disks) for a disk-specific override section in
 * the config file (e.g. [raid_disk:X], where X is the disk number).
 *
 * Returns one of the following:
 *
 * 	* The index, which is a positive integer in the range 1 through fcd_conf_disk_count - 1
 * 	* -1 indicates that "X" is not a valid integer or is out of range.
 * 	* -2 indicates that the main ([freecusd]) section has not yet been processed, so it's
 * 		too early to process disk-specific overrides.
 * 	* -3 indicates that no RAID disk is connected in position X
 */
static int fcd_smart_disk_index(cip_err_ctx *const ctx,
				const cip_ini_sect *const sect)
{
	/* Save our results to avoid repeatedly searching for the same disk */
	static const cip_ini_sect *last_sect = NULL;
	static int last_index;

	int disk, i;

	/*
	 * fcd_conf_disks[0].temps[FCD_CONF_TEMP_WARN] will be changed to a valid
	 * value when the main (i.e. [freecusd]) config section is processed.  If
	 * it's still INT_MIN, the main section hasn't been processed yet, so it's
	 * too early to process disk-specific overrides.
	 *
	 * Callback needs to return 1 to libcip, to defer processing, but 1 is a
	 * valid index, so return -2.  (-1 indicates error.)
	 */
	if (fcd_conf_disks[0].temps[FCD_CONF_TEMP_WARN] == INT_MIN)
		return -2;

	/* Same section as last time? */
	if (sect == last_sect)
		return last_index;

	if ((disk = fcd_smart_parse_raid_num(sect->node.name)) == -1) {
		cip_err(ctx, "Invalid RAID disk number: %s (must be 1 - %d)",
			sect->node.name, FCD_MAX_DISK_COUNT);
		return -1;
	}

	for (i = 0; i < (int)fcd_conf_disk_count; ++i) {
		/* DOM is on port 1; RAID disks are on ports 2+ */
		if ((int)fcd_conf_disks[i].port_no - 1 == disk)
			break;
	}

	if (i == (int)fcd_conf_disk_count) {
		cip_err(ctx, "Ignoring section: [raid_disk:%s]: no such disk", sect->node.name);
		i = -3;
	}

	/* Save the result, including "missing disk" results */
	last_sect = sect;
	last_index = i;

	return i;
}

/*
 * Callback for disk temperatures in disk-specific override sections (i.e.
 * [raid_disk:X]).  May be called before main ([freecusd]) section has been
 * processed.
 */
static int fcd_smart_temp_disk_cb(cip_err_ctx *const ctx,
				  const cip_ini_value *const value,
				  const cip_ini_sect *const sect,
				  const cip_ini_file *const file __attribute__((unused)),
				  void *const post_parse_data)
{
	enum fcd_conf_temp_type temp_type;
	int temp, disk;

	switch (disk = fcd_smart_disk_index(ctx, sect)) {
		case -1:	return -1;	/* error */
		case -2:	return  1;	/* main section not yet proecessed; defer */
		case -3:	return  0;	/* "missing" disk; nothing to do */
	}

	if ((temp = fcd_smart_temp_get_conf(ctx, value)) == INT_MIN)
		return -1;

	temp_type = (enum fcd_conf_temp_type)post_parse_data;

	fcd_conf_disks[disk].temps[temp_type] = temp;

	return 0;
}

/*
 * Callback for temp_ignore/disk_ignore booleans in [raid_disk:X] sections.
 */
static int fcd_smart_ignore_cb(cip_err_ctx *const ctx,
			       const cip_ini_value *const value,
			       const cip_ini_sect *const sect,
			       const cip_ini_file *const file __attribute__((unused)),
			       void *const post_parse_data)
{
	_Bool ignore;
	int disk;

	switch (disk = fcd_smart_disk_index(ctx, sect)) {
		case -1:	return -1;	/* error */
		case -2:	return  1;	/* main section not yet proecessed; defer */
		case -3:	return  0;	/* "missing" disk; nothing to do */
	}

	memcpy(&ignore, value->value, sizeof ignore);

	/* Monitor struct addresses used as "magic" numbers to indicate which flag */

	if (post_parse_data == &fcd_smart_monitor) {

		fcd_conf_disks[disk].smart_ignore = ignore;
	}
	else if (post_parse_data == &fcd_hddtemp_monitor) {

		fcd_conf_disks[disk].temp_ignore = ignore;
	}
	else {
		FCD_ABORT("This should never happen!\n");
	}

	return 0;
}

__attribute__((noreturn))
static void fcd_smart_disable(char *restrict cmd_buf,
			      const int *const restrict pipe_fds)
{
	fcd_lib_fail(&fcd_hddtemp_monitor);
	fcd_lib_parent_fail_and_exit(&fcd_smart_monitor, pipe_fds, cmd_buf);
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
	uint8_t pwm_flags;
	unsigned i;
	int ret;

	memset(alerts, 0, sizeof alerts);
	memset(buf, ' ', sizeof buf);
	warn = 0;
	fail = 0;
	pwm_flags = 0;

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

			if (temps[i] >= fcd_conf_disks[i].temps[FCD_CONF_TEMP_FAIL]) {
				alerts[i] = 1;
				fail = 1;
				warn = 0;
			}
			else if (temps[i] >= fcd_conf_disks[i].temps[FCD_CONF_TEMP_WARN]
							|| temps[i] <= 0) {
				alerts[i] = 1;
				warn = !fail;
			}

			pwm_flags |= fcd_pwm_temp_flags(temps[i], fcd_conf_disks[i].temps);
		}
	}

	fcd_lib_set_mon_status(&fcd_hddtemp_monitor, buf, warn, fail, alerts, pwm_flags);
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
		fcd_lib_fail(&fcd_hddtemp_monitor);
		fcd_lib_fail_and_exit(&fcd_smart_monitor);
	}

	cmd_buf = NULL;
	buf_size = 0;

	do {
		for (i = 0; i < fcd_conf_disk_count; ++i) {

			if (fcd_conf_disks[i].smart_ignore && fcd_conf_disks[i].temp_ignore)
				continue;

			ret = fcd_smart_exec(i,	&cmd_buf, &buf_size, pipe_fds);
			if (ret == -3) {
				goto break_outer_loop;
			}
			else if (ret == -2) {
				status[i] = FCD_SMART_ERROR;
				continue;
			}

			fcd_smart_parse(i, status, temps, cmd_buf, pipe_fds);
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

static void fcd_smart_dump_smart_cfg(void)
{
	int i;

	for (i = 0; i < (int)fcd_conf_disk_count; ++i) {
		FCD_DUMP("\t%s:\n", fcd_conf_disks[i].name);
		FCD_DUMP("\t\tignore: %s\n", fcd_conf_disks[i].smart_ignore ? "true" : "false");
	}
}

static void fcd_smart_dump_temp_cfg(void)
{
	int i;

	for (i = 0; i < (int)fcd_conf_disk_count; ++i) {
		FCD_DUMP("\t%s:\n", fcd_conf_disks[i].name);
		FCD_DUMP("\t\tignore: %s\n", fcd_conf_disks[i].temp_ignore ? "true" : "false");
		fcd_lib_dump_temp_cfg(fcd_conf_disks[i].temps);
	}
}

struct fcd_monitor fcd_smart_monitor = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,
	.name			= "SMART status",
	.monitor_fn		= fcd_smart_fn,
	.cfg_dump_fn		= fcd_smart_dump_smart_cfg,
	.buf			= "....."
				  "S.M.A.R.T. STATUS   "
				  "                    ",
	.enabled		= true,
	.enabled_opt_name	= "enable_smart_monitor",
	.raiddisk_opts		= fcd_smart_disk_opts,
};

struct fcd_monitor fcd_hddtemp_monitor = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,
	.name			= "HDD temperature",
	.monitor_fn		= 0,
	.cfg_dump_fn		= fcd_smart_dump_temp_cfg,
	.buf			= "....."
				  "HDD TEMPERATURE     "
				  "                    ",
	.enabled		= true,
	.enabled_opt_name	= "enable_hddtemp_monitor",
	.raiddisk_opts		= fcd_smart_temp_disk_opts,
	.freecusd_opts		= fcd_smart_temp_opts,
	.current_pwm_flags	= FCD_FAN_HIGH_ON,
};
