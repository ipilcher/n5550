/*
 * Copyright 2013, 2020 Ian Pilcher <arequipeno@gmail.com>
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

#include <stddef.h>
#include <fcntl.h>

/* /sys/class/leds/<NAME>/brightness; <NAME> is 21 characters max */
#define FCD_ALERT_LED_BUF_SIZE	49

struct fcd_alert {
	const char *led_name;
	size_t mon_offset;
	int led_fd;
	int counter;
};

static struct fcd_alert fcd_alerts[] = {
	{
		.led_name	= "n5550:orange:busy",
		.mon_offset	= offsetof(struct fcd_monitor, sys_warn),
		.counter	= 0,
	},
	{
		.led_name	= "n5550:red:fail",
		.mon_offset	= offsetof(struct fcd_monitor, sys_fail),
		.counter	= 0,
	},
	{
		.led_name	= "n5550:red:disk-stat-0",
		.mon_offset	= offsetof(struct fcd_monitor, disk_alerts[0]),
		.counter	= 0,
	},
	{
		.led_name	= "n5550:red:disk-stat-1",
		.mon_offset	= offsetof(struct fcd_monitor, disk_alerts[1]),
		.counter	= 0,
	},
	{
		.led_name	= "n5550:red:disk-stat-2",
		.mon_offset	= offsetof(struct fcd_monitor, disk_alerts[2]),
		.counter	= 0,
	},
	{
		.led_name	= "n5550:red:disk-stat-3",
		.mon_offset	= offsetof(struct fcd_monitor, disk_alerts[3]),
		.counter	= 0,
	},
	{
		.led_name	= "n5550:red:disk-stat-4",
		.mon_offset	= offsetof(struct fcd_monitor, disk_alerts[4]),
		.counter	= 0,
	},
};

/*******************************************************************************
 *
 * Called in monitor threads
 *
 ******************************************************************************/

static _Bool fcd_alert_set(enum fcd_alert_msg *status)
{
	switch (*status) {

		case FCD_ALERT_SET_REQ:
			/*
			 * Main thread has not yet acknowledged previous set
			 * request; keep set request pending.
			 */
			return 0;

		case FCD_ALERT_CLR_REQ:
			/*
			 * Main thread has not yet acknowledged previous clear
			 * request.  Clear request indicates that alert is set,
			 * so restore set ACK.
			 */
			*status = FCD_ALERT_SET_ACK;
			return 1;

		case FCD_ALERT_SET_ACK:
			/*
			 * Alert is currently set; no action required.
			 */
			return 0;

		case FCD_ALERT_CLR_ACK:
			/*
			 * Alert is currently not set; set it.
			 */
			*status = FCD_ALERT_SET_REQ;
			return 1;
	}

	FCD_ABORT("Invalid enum value\n");
}

static _Bool fcd_alert_clear(enum fcd_alert_msg *status)
{
	switch (*status) {

		case FCD_ALERT_SET_REQ:
			/*
			 * Main thread has not yet acknowledged previous set
			 * request.  Set request indicates that alear is not
			 * set, so restore clear ACK.
			 */
			*status = FCD_ALERT_CLR_ACK;
			return 1;

		case FCD_ALERT_CLR_REQ:
			/*
			 * Main thread has not yet acknowledged previous clear
			 * request; keep clear request pending.
			 */
			return 0;

		case FCD_ALERT_SET_ACK:
			/*
			 * Alert is currently set; clear it.
			 */
			*status = FCD_ALERT_CLR_REQ;
			return 1;

		case FCD_ALERT_CLR_ACK:
			/*
			 * Alert is currently not set; no action required.
			 */
			return 0;
	}

	FCD_ABORT("Invalid enum value\n");
}

_Bool fcd_alert_update(enum fcd_alert_msg new, enum fcd_alert_msg *status)
{
	if (new == FCD_ALERT_SET_REQ)
		return fcd_alert_set(status);
	else if (new == FCD_ALERT_CLR_REQ)
		return fcd_alert_clear(status);
	else
		FCD_ABORT("Invalid alert status\n");
}

/*******************************************************************************
 *
 * Called in the main thread
 *
 ******************************************************************************/

static void fcd_alert_led_on(const struct fcd_alert *alert)
{
	ssize_t ret;

	ret = write(alert->led_fd, "255", 3);
	if (ret == -1)
		FCD_PABORT("write");
	if (ret != 3)
		FCD_ABORT("Incomplete write (%zd bytes)\n", ret);
}

static void fcd_alert_led_off(const struct fcd_alert *alert)
{
	ssize_t ret;

	ret = write(alert->led_fd, "0", 1);
	if (ret == -1)
		FCD_PABORT("write");
	if (ret != 1)
		FCD_ABORT("Incomplete write (%zd bytes)\n", ret);
}

void fcd_alert_read_monitor(struct fcd_monitor *mon)
{
	enum fcd_alert_msg *msg;
	unsigned char *mon_base;
	struct fcd_alert *alert;
	size_t i;

	mon_base = (unsigned char *)mon;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_alerts); ++i) {

		alert = &fcd_alerts[i];
		msg = (enum fcd_alert_msg *)(mon_base + alert->mon_offset);

		if (*msg == FCD_ALERT_SET_REQ) {
			++(alert->counter);
			*msg = FCD_ALERT_SET_ACK;
			if (alert->counter == 1)
				fcd_alert_led_on(alert);
		}
		else if (*msg == FCD_ALERT_CLR_REQ) {
			--(alert->counter);
			if (alert->counter < 0)
				FCD_ABORT("Negative alert counter\n");
			*msg = FCD_ALERT_CLR_ACK;
			if (alert->counter == 0)
				fcd_alert_led_off(alert);
		}
	}
}

void fcd_alert_leds_close(void)
{
	size_t i;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_alerts); ++i) {

		if (close(fcd_alerts[i].led_fd) == -1)
			FCD_PERROR("close");
	}
}

void fcd_alert_leds_open(void)
{
	char buf[FCD_ALERT_LED_BUF_SIZE];
	size_t i;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_alerts); ++i) {

		sprintf(buf, "/sys/class/leds/%s/brightness",
			fcd_alerts[i].led_name);

		fcd_alerts[i].led_fd = open(buf, O_WRONLY | O_CLOEXEC);
		if (fcd_alerts[i].led_fd == -1)
			FCD_PFATAL(buf);

		fcd_alert_led_off(&fcd_alerts[i]);
	}
}
