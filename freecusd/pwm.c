/*
 * Copyright 2020 Ian Pilcher <arequipeno@gmail.com>
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

#include <fcntl.h>

/* Parsed PWM value */
struct fcd_pwm_value {
	size_t	len;		/* strlen(s) */
	int	value;		/* 0 - 255 */
	char	s[4];		/* value as a string */
};

const char *const fcd_pwm_state_names[FCD_PWM_STATE_ARRAY_SIZE] = {
	"NORMAL",
	"HIGH",
	"MAXIMUM"
};

static const char fcd_pwm_file[] = "/sys/devices/platform/it87.656/pwm3";
static enum fcd_pwm_state fcd_pwm_current_state = FCD_PWM_STATE_NORMAL;
static int fcd_pwm_fd;

static struct fcd_pwm_value fcd_pwm_values[FCD_PWM_STATE_ARRAY_SIZE] = {
	[FCD_PWM_STATE_NORMAL]	= { .value = 170, .s = "170", .len = 3 },
	[FCD_PWM_STATE_HIGH]	= { .value = 215, .s = "215", .len = 3 },
	[FCD_PWM_STATE_MAX]	= { .value = 255, .s = "255", .len = 3 }
};

static int fcd_pwm_cb();

static const cip_opt_info fcd_pwm_opts[] = {
	{
		.name			= "sysfan_pwm_normal",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_pwm_cb,
		.post_parse_data	= &fcd_pwm_values[FCD_PWM_STATE_NORMAL],
	},
	{
		.name			= "sysfan_pwm_high",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_pwm_cb,
		.post_parse_data	= &fcd_pwm_values[FCD_PWM_STATE_HIGH],
	},
	{
		.name			= "sysfan_pwm_max",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_pwm_cb,
		.post_parse_data	= &fcd_pwm_values[FCD_PWM_STATE_MAX],
	},
	{	.name			= NULL		}
};

/*
 * Configuration callback for PWM values
 */
static int fcd_pwm_cb(cip_err_ctx *const ctx, const cip_ini_value *const value,
		      const cip_ini_sect *const sect __attribute__((unused)),
		      const cip_ini_file *const file __attribute__((unused)),
		      void *const post_parse_data)
{
	struct fcd_pwm_value *pwm;
	const int *p;

	p = (const int *)(value->value);
	pwm = post_parse_data;
	pwm->value = *p;

	if (pwm->value < 0 || pwm->value > 255) {
		cip_err(ctx, "PWM value (%d) outside value range (0 - 255)", pwm->value);
		return -1;
	}

	pwm->len = sprintf(pwm->s, "%d", pwm->value);

	return 0;
}

static void fcd_pwm_set(const enum fcd_pwm_state new)
{
	ssize_t ret;

	if (fcd_pwm_current_state == new)
		return;

	FCD_INFO("Changing fan speed from %s to %s\n",
		 fcd_pwm_state_names[fcd_pwm_current_state], fcd_pwm_state_names[new]);

	ret = write(fcd_pwm_fd, fcd_pwm_values[new].s, fcd_pwm_values[new].len);
	if (ret < 0)
		FCD_PABORT(fcd_pwm_file);
	if ((size_t)ret != fcd_pwm_values[new].len)
		FCD_ABORT("Incomplete write (%zd bytes)\n", ret);

	fcd_pwm_current_state = new;
}

void fcd_pwm_update(struct fcd_monitor *const mon)
{
	uint8_t flags;
	int i;

	if (!fcd_pwm_monitor.enabled)
		return;

	if (mon->current_pwm_flags == mon->new_pwm_flags)
		return;

	mon->current_pwm_flags = mon->new_pwm_flags;

	for (flags = 0, i = 0; fcd_monitors[i] != NULL; ++i)
		flags |= fcd_monitors[i]->current_pwm_flags;

	/* Should fan be set to max speed? */

	if (flags & FCD_FAN_MAX_ON) {
		fcd_pwm_set(FCD_PWM_STATE_MAX);
		return;
	}

	if (flags & FCD_FAN_MAX_HYST && fcd_pwm_current_state == FCD_PWM_STATE_MAX) {
		/* Already set to max; nothing to do */
		return;
	}

	/* NOT max speed; what about high speed? */

	if (flags & FCD_FAN_HIGH_ON) {
		fcd_pwm_set(FCD_PWM_STATE_HIGH);
		return;
	}

	if (flags & FCD_FAN_HIGH_HYST && fcd_pwm_current_state >= FCD_PWM_STATE_HIGH) {
		/* Not necessarily a no-op; fan may be set to max */
		fcd_pwm_set(FCD_PWM_STATE_HIGH);
		return;
	}

	/* Normal speed it is */

	fcd_pwm_set(FCD_PWM_STATE_NORMAL);
}

void fcd_pwm_init(void)
{
	if (fcd_pwm_monitor.enabled) {

		if ((fcd_pwm_fd = open(fcd_pwm_file, O_WRONLY | O_CLOEXEC)) < 0)
			FCD_PFATAL(fcd_pwm_file);

		fcd_pwm_set(FCD_PWM_STATE_MAX);
	}
	else {
		FCD_INFO("System fan speed management (PWM) disabled\n");
	}
}

void fcd_pwm_fini(void)
{
	if (fcd_pwm_monitor.enabled) {
		if (close(fcd_pwm_fd) != 0)
			FCD_PERROR(fcd_pwm_file);
	}
}

struct fcd_monitor fcd_pwm_monitor = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,
	.name			= "PWM",
	.enabled		= 1,
	.silent			= 1,
	.enabled_opt_name	= "enable_sysfan_pwm",
	.freecusd_opts		= fcd_pwm_opts,
};
