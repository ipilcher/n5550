/*
 * Copyright 2013-2014, 2016, 2020 Ian Pilcher <arequipeno@gmail.com>
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

/* Alert & PWM thresholds */
static int fcd_temp_core_cfg[FCD_CONF_TEMP_ARRAY_SIZE] = {
	[FCD_CONF_TEMP_WARN]		= 43000,	/* cpu_core_temp_warn */
	[FCD_CONF_TEMP_FAIL]		= 45000,	/* cpu_core_temp_crit */
	[FCD_CONF_TEMP_FAN_MAX_ON]	= 42000,	/* cpu_core_temp_fan_max_on */
	[FCD_CONF_TEMP_FAN_MAX_HYST]	= 39000,	/* cpu_core_temp_fan_max_hyst */
	[FCD_CONF_TEMP_FAN_HIGH_ON]	= 40000,	/* cpu_core_temp_fan_high_on */
	[FCD_CONF_TEMP_FAN_HIGH_HYST]	= 37000		/* cpu_core_temp_fan_high_hyst */
};

static int fcd_temp_cpu_cfg[FCD_CONF_TEMP_ARRAY_SIZE] = {
	[FCD_CONF_TEMP_WARN]		= 43000,	/* cpu_temp_warn */
	[FCD_CONF_TEMP_FAIL]		= 45000,	/* cpu_temp_crit */
	[FCD_CONF_TEMP_FAN_MAX_ON]	= 42000,	/* cpu_temp_fan_max_on */
	[FCD_CONF_TEMP_FAN_MAX_HYST]	= 39000,	/* cpu_temp_fan_max_hyst */
	[FCD_CONF_TEMP_FAN_HIGH_ON]	= 40000,	/* cpu_temp_fan_high_on */
	[FCD_CONF_TEMP_FAN_HIGH_HYST]	= 37000		/* cpu_temp_fan_high_hyst */
};

static int fcd_temp_sys_cfg[FCD_CONF_TEMP_ARRAY_SIZE] = {
	[FCD_CONF_TEMP_WARN]		= 39000,	/* sys_temp_warn */
	[FCD_CONF_TEMP_FAIL]		= 40000,	/* sys_temp_crit */
	[FCD_CONF_TEMP_FAN_MAX_ON]	= 39000,	/* sys_temp_fan_max_on */
	[FCD_CONF_TEMP_FAN_MAX_HYST]	= 37000,	/* sys_temp_fan_max_hyst */
	[FCD_CONF_TEMP_FAN_HIGH_ON]	= 38000,	/* sys_temp_fan_high_on */
	[FCD_CONF_TEMP_FAN_HIGH_HYST]	= 36000		/* sys_temp_fan_high_hyst */
};

static int fcd_temp_ich_cfg[FCD_CONF_TEMP_ARRAY_SIZE] = {
	[FCD_CONF_TEMP_WARN]		= 39000,	/* ich_temp_warn */
	[FCD_CONF_TEMP_FAIL]		= 40000,	/* ich_temp_crit */
	[FCD_CONF_TEMP_FAN_MAX_ON]	= 39000,	/* ich_temp_fan_max_on */
	[FCD_CONF_TEMP_FAN_MAX_HYST]	= 37000,	/* ich_temp_fan_max_hyst */
	[FCD_CONF_TEMP_FAN_HIGH_ON]	= 38000,	/* ich_temp_fan_high_on */
	[FCD_CONF_TEMP_FAN_HIGH_HYST]	= 36000		/* ich_temp_fan_high_hyst */
};

static int fcd_temp_cb();

static const cip_opt_info fcd_temp_core_opts[] = {
	{
		.name			= "cpu_core_temp_warn",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_core_cfg[FCD_CONF_TEMP_WARN],
	},
	{
		.name			= "cpu_core_temp_crit",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_core_cfg[FCD_CONF_TEMP_FAIL],
	},
	{
		.name			= "cpu_core_temp_fan_max_on",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_core_cfg[FCD_CONF_TEMP_FAN_MAX_ON],
	},
	{
		.name			= "cpu_core_temp_fan_max_hyst",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_core_cfg[FCD_CONF_TEMP_FAN_MAX_HYST],
	},
	{
		.name			= "cpu_core_temp_fan_high_on",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_core_cfg[FCD_CONF_TEMP_FAN_HIGH_ON],
	},
	{
		.name			= "cpu_core_temp_fan_high_hyst",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_core_cfg[FCD_CONF_TEMP_FAN_HIGH_HYST],
	},
	{
		.name			= NULL
	}
};

static const cip_opt_info fcd_temp_it87_opts[] = {
	{
		.name			= "cpu_temp_warn",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_cpu_cfg[FCD_CONF_TEMP_WARN],
	},
	{
		.name			= "cpu_temp_crit",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_cpu_cfg[FCD_CONF_TEMP_FAIL],
	},
	{
		.name			= "cpu_temp_fan_max_on",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_cpu_cfg[FCD_CONF_TEMP_FAN_MAX_ON],
	},
	{
		.name			= "cpu_temp_fan_max_hyst",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_cpu_cfg[FCD_CONF_TEMP_FAN_MAX_HYST],
	},
	{
		.name			= "cpu_temp_fan_high_on",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_cpu_cfg[FCD_CONF_TEMP_FAN_HIGH_ON],
	},
	{
		.name			= "cpu_temp_fan_high_hyst",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_cpu_cfg[FCD_CONF_TEMP_FAN_HIGH_HYST],
	},
	{
		.name			= "sys_temp_warn",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_sys_cfg[FCD_CONF_TEMP_WARN],
	},
	{
		.name			= "sys_temp_crit",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_sys_cfg[FCD_CONF_TEMP_FAIL],
	},
	{
		.name			= "sys_temp_fan_max_on",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_sys_cfg[FCD_CONF_TEMP_FAN_MAX_ON],
	},
	{
		.name			= "sys_temp_fan_max_hyst",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_sys_cfg[FCD_CONF_TEMP_FAN_MAX_HYST],
	},
	{
		.name			= "sys_temp_fan_high_on",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_sys_cfg[FCD_CONF_TEMP_FAN_HIGH_ON],
	},
	{
		.name			= "sys_temp_fan_high_hyst",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_sys_cfg[FCD_CONF_TEMP_FAN_HIGH_HYST],
	},
	{
		.name			= "ich_temp_warn",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_ich_cfg[FCD_CONF_TEMP_WARN],
	},
	{
		.name			= "ich_temp_crit",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_ich_cfg[FCD_CONF_TEMP_FAIL],
	},
	{
		.name			= "ich_temp_fan_max_on",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_ich_cfg[FCD_CONF_TEMP_FAN_MAX_ON],
	},
	{
		.name			= "ich_temp_fan_max_hyst",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_ich_cfg[FCD_CONF_TEMP_FAN_MAX_HYST],
	},
	{
		.name			= "ich_temp_fan_high_on",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_ich_cfg[FCD_CONF_TEMP_FAN_HIGH_ON],
	},
	{
		.name			= "ich_temp_fan_high_hyst",
		.type			= CIP_OPT_TYPE_FLOAT,
		.post_parse_fn		= fcd_temp_cb,
		.post_parse_data	= &fcd_temp_ich_cfg[FCD_CONF_TEMP_FAN_HIGH_HYST],
	},
	{
		.name			= NULL
	}
};

struct fcd_temp_input {
	const char *path;
	FILE *fp;
	const int *cfg;
	struct fcd_monitor *mon;
};

enum fcd_temp_id {
	FCD_TEMP_ID_CORE0	= 0,
	FCD_TEMP_ID_CORE1,
	FCD_TEMP_ID_CPU,
	FCD_TEMP_ID_ICH,
	FCD_TEMP_ID_SYS
};

#define FCD_TEMP_ID_ARRAY_SIZE	(FCD_TEMP_ID_SYS + 1)

static struct fcd_temp_input fcd_temp_inputs[FCD_TEMP_ID_ARRAY_SIZE] = {
	[FCD_TEMP_ID_CORE0] = {
		.path	= "/sys/devices/platform/coretemp.0/hwmon/hwmon1/temp2_input",
		.cfg	= fcd_temp_core_cfg,
		.mon	= &fcd_temp_core_monitor,
	},
	[FCD_TEMP_ID_CORE1] = {
		.path	= "/sys/devices/platform/coretemp.0/hwmon/hwmon1/temp3_input",
		.cfg	= fcd_temp_core_cfg,
		.mon	= &fcd_temp_core_monitor,
	},
	[FCD_TEMP_ID_CPU] = {
		.path	= "/sys/devices/platform/it87.656/temp1_input",
		.cfg	= fcd_temp_cpu_cfg,
		.mon	= &fcd_temp_it87_monitor,
	},
	[FCD_TEMP_ID_ICH] = {
		.path	= "/sys/devices/platform/it87.656/temp2_input",
		.cfg	= fcd_temp_ich_cfg,
		.mon	= &fcd_temp_it87_monitor,
	},
	[FCD_TEMP_ID_SYS] = {
		.path	= "/sys/devices/platform/it87.656/temp3_input",
		.cfg	= fcd_temp_sys_cfg,
		.mon	= &fcd_temp_it87_monitor,
	}
};

static int fcd_temp_active_monitors;
static _Bool fcd_temp_core_failed = 0;
static _Bool fcd_temp_it87_failed = 0;

/*
 * Configuration callback for alert & PWM thresholds
 */
static int fcd_temp_cb(cip_err_ctx *ctx, const cip_ini_value *value,
		       const cip_ini_sect *sect __attribute__((unused)),
		       const cip_ini_file *file __attribute__((unused)),
		       void *post_parse_data)
{
	const float *p;
	double temp;

	p = (const float *)(value->value);
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

static void fcd_temp_exit_if_dupe_thread(void)
{
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	static _Bool already_running = 0;

	_Bool dupe_thread;
	int ret;

	if ((ret = pthread_mutex_lock(&mutex)) != 0)
		FCD_PT_ABRT("pthread_mutex_lock", ret);

	if (already_running) {
		dupe_thread = 1;
	}
	else {
		already_running = 1;
		dupe_thread = 0;
	}

	if ((ret = pthread_mutex_unlock(&mutex)) != 0)
		FCD_PT_ABRT("pthread_mutex_unlock", ret);

	if (dupe_thread)
		pthread_exit(NULL);

	fcd_temp_active_monitors = fcd_temp_core_monitor.enabled + fcd_temp_it87_monitor.enabled;
}

static void fcd_temp_close_inputs(const struct fcd_monitor *const mon)
{
	int i;

	for (i = 0; i < FCD_TEMP_ID_ARRAY_SIZE; ++i) {

		if (mon != NULL && mon != fcd_temp_inputs[i].mon)
			continue;

		if (fcd_temp_inputs[i].fp == NULL)
			continue;

		if (fclose(fcd_temp_inputs[i].fp) == EOF)
			FCD_PERROR(fcd_temp_inputs[i].path);

		fcd_temp_inputs[i].fp = NULL;
	}
}

static void fcd_temp_fail(struct fcd_monitor *const mon)
{
	fcd_temp_close_inputs(mon);

	if (mon == &fcd_temp_core_monitor)
		fcd_temp_core_failed = 1;
	else if (mon == &fcd_temp_it87_monitor)
		fcd_temp_it87_failed = 1;
	else
		FCD_ABORT("Aaaaaaaaaaaargh!\n");

	if (--fcd_temp_active_monitors == 0)
		fcd_lib_fail_and_exit(mon);
	else
		fcd_lib_fail(mon);
}

static void fcd_temp_fail_both(void)
{
	if (fcd_temp_core_monitor.enabled)
		fcd_temp_fail(&fcd_temp_core_monitor);

	if (fcd_temp_it87_monitor.enabled)
		fcd_temp_fail(&fcd_temp_it87_monitor);
}

static void fcd_temp_open_inputs(struct fcd_monitor *const mon)
{
	int i;

	if (!mon->enabled)
		return;

	for (i = 0; i < FCD_TEMP_ID_ARRAY_SIZE; ++i) {

		if (fcd_temp_inputs[i].mon != mon)
			continue;

		if ((fcd_temp_inputs[i].fp = fopen(fcd_temp_inputs[i].path, "re")) == NULL) {
			FCD_PERROR(fcd_temp_inputs[i].path);
			fcd_temp_fail(mon);
		}

		if (setvbuf(fcd_temp_inputs[i].fp, NULL, _IONBF, 0) != 0) {
			FCD_PERROR(fcd_temp_inputs[i].path);
			fcd_temp_fail(mon);
		}
	}
}

static void fcd_temp_process(const struct fcd_monitor *const mon,
			     const int *const restrict temps,
			     int *const restrict warn,
			     int *const restrict fail,
			     uint8_t *const restrict pwm_flags)
{
	int i;

	*fail = 0;
	*warn = 0;
	*pwm_flags = 0;

	for (i = 0; i < FCD_TEMP_ID_ARRAY_SIZE; ++i) {

		if (fcd_temp_inputs[i].mon != mon)
			continue;

		if (temps[i] >= fcd_temp_inputs[i].cfg[FCD_CONF_TEMP_FAIL]) {
			*fail = 1;
			*warn = 0;
		}
		else if (temps[i] >= fcd_temp_inputs[i].cfg[FCD_CONF_TEMP_WARN]) {
			*warn = !(*fail);
		}

		*pwm_flags |= fcd_pwm_temp_flags(temps[i], fcd_temp_inputs[i].cfg);
	}
}

__attribute__((noreturn))
static void *fcd_temp_fn(void *arg)
{
	int warn, fail, i, ret, temps[FCD_TEMP_ID_ARRAY_SIZE];
	uint8_t pwm_flags;
	char upper[21], lower[21];

	fcd_temp_exit_if_dupe_thread();
	fcd_temp_open_inputs(&fcd_temp_core_monitor);
	fcd_temp_open_inputs(&fcd_temp_it87_monitor);

	do {
		for (i = 0; i < FCD_TEMP_ID_ARRAY_SIZE; ++i) {

			if (fcd_temp_inputs[i].fp == NULL)
				continue;

			rewind(fcd_temp_inputs[i].fp);

			ret = fscanf(fcd_temp_inputs[i].fp, "%d", &temps[i]);
			if (ret == EOF) {
				FCD_PERROR(fcd_temp_inputs[i].path);
				fcd_temp_fail(fcd_temp_inputs[i].mon);
			}
			else if (ret != 1) {
				FCD_WARN("Failed to parse contents of %s\n",
					 fcd_temp_inputs[i].path);
				fcd_temp_fail(fcd_temp_inputs[i].mon);
			}
		}

		if (fcd_temp_core_monitor.enabled && !fcd_temp_core_failed) {

			fcd_temp_process(&fcd_temp_core_monitor, temps, &warn, &fail, &pwm_flags);

			memset(lower, ' ', sizeof lower);

			ret = fcd_lib_snprintf(lower, sizeof lower, "CORE0: %d  CORE1: %d",
					       temps[FCD_TEMP_ID_CORE0] / 1000,
					       temps[FCD_TEMP_ID_CORE1] / 1000);
			if (ret < 0) {
				fcd_temp_fail(&fcd_temp_core_monitor);
			}
			else {
				fcd_lib_set_mon_status(&fcd_temp_core_monitor,
						       lower, warn, fail, NULL, pwm_flags);
			}
		}

		if (fcd_temp_it87_monitor.enabled && ! fcd_temp_it87_failed) {

			fcd_temp_process(&fcd_temp_it87_monitor, temps, &warn, &fail, &pwm_flags);

			memset(upper, ' ', sizeof upper);
			memset(lower, ' ', sizeof lower);

			ret = fcd_lib_snprintf(upper, sizeof upper, "TEMPERATURE  CPU: %d",
					       temps[FCD_TEMP_ID_CPU] / 1000);
			if (ret > 0) {
				ret = fcd_lib_snprintf(lower, sizeof lower, "ICH: %d  SYS: %d",
						       temps[FCD_TEMP_ID_ICH] / 1000,
						       temps[FCD_TEMP_ID_SYS] / 1000);
			}

			if (ret < 0) {
				fcd_temp_fail(&fcd_temp_it87_monitor);
			}
			else {
				fcd_lib_set_mon_status2(&fcd_temp_it87_monitor,
							upper, lower, warn, fail, NULL, pwm_flags);
			}
		}

		ret = fcd_lib_monitor_sleep(30);
		if (ret == -1)
			fcd_temp_fail_both();

	} while (ret == 0);

	fcd_temp_close_inputs(NULL);
	pthread_exit(NULL);
}

static void fcd_temp_dump_core_config(void)
{
	FCD_DUMP("\tcore temperature thresholds:\n");
	fcd_lib_dump_temp_cfg(fcd_temp_core_cfg);
}

static void fcd_temp_dump_it87_config(void)
{
	FCD_DUMP("\tCPU temperature thresholds:\n");
	fcd_lib_dump_temp_cfg(fcd_temp_cpu_cfg);
	FCD_DUMP("\tsystem temperature thresholds:\n");
	fcd_lib_dump_temp_cfg(fcd_temp_sys_cfg);
	FCD_DUMP("\tICH temperature thresholds:\n");
	fcd_lib_dump_temp_cfg(fcd_temp_ich_cfg);
}

struct fcd_monitor fcd_temp_core_monitor = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,
	.name			= "CPU core temperature",
	.monitor_fn		= fcd_temp_fn,
	.cfg_dump_fn		= fcd_temp_dump_core_config,
	.buf			= "....."
				  "CPU CORE TEMPERATURE"
				  "                    ",
	.enabled		= true,
	.enabled_opt_name	= "enable_cpu_core_temp_monitor",
	.freecusd_opts		= fcd_temp_core_opts,
	.current_pwm_flags	= FCD_FAN_HIGH_ON,
};

struct fcd_monitor fcd_temp_it87_monitor = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,
	.name			= "IT87 temperature",
	.monitor_fn		= fcd_temp_fn,
	.cfg_dump_fn		= fcd_temp_dump_it87_config,
	.buf			= "....."
				  "CPU CORE TEMPERATURE"
				  "                    ",
	.enabled		= true,
	.enabled_opt_name	= "enable_sys_temp_monitor",
	.freecusd_opts		= fcd_temp_it87_opts,
	.current_pwm_flags	= FCD_FAN_HIGH_ON,
};
