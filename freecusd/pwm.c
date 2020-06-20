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

static struct fcd_pwm_value fcd_pwm_values[FCD_PWM_MAX + 1] = {
	[FCD_PWM_NORMAL]	= { .value = 170, .s = "170", .len = 3 },
	[FCD_PWM_HIGH]		= { .value = 215, .s = "215", .len = 3 },
	[FCD_PWM_MAX]		= { .value = 255, .s = "255", .len = 3 }
};

static int fcd_pwm_cb();

static const cip_opt_info fcd_pwm_opts[] = {
	{
		.name			= "sysfan_pwm_normal",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_pwm_cb,
		.post_parse_data	= &fcd_pwm_values[FCD_PWM_NORMAL],
	},
	{
		.name			= "sysfan_pwm_high",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_pwm_cb,
		.post_parse_data	= &fcd_pwm_values[FCD_PWM_HIGH],
	},
	{
		.name			= "sysfan_pwm_max",
		.type			= CIP_OPT_TYPE_INT,
		.post_parse_fn		= fcd_pwm_cb,
		.post_parse_data	= &fcd_pwm_values[FCD_PWM_MAX],
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

void fcd_pwm_update(struct fcd_monitor *const mon)
{

}

void fcd_pwm_init(void)
{

}

void fcd_pwm_fini(void)
{

}

struct fcd_monitor fcd_pwm_monitor = {
	.mutex			= PTHREAD_MUTEX_INITIALIZER,
	.name			= "PWM",
	.enabled		= 1,
	.silent			= 1,
	.enabled_opt_name	= "enable_sysfan_pwm",
	.freecusd_opts		= fcd_pwm_opts,
};
