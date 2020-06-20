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

static const cip_opt_info fcd_pwm_opts[] = {
	{	.name		= NULL		}
};

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
