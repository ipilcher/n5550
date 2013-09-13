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

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#include "freecusd.h"

static int fcd_pic_gpio_is_exported(void)
{
	int ret;

	ret = access("/sys/class/gpio/gpio31", F_OK);
	if (ret == -1) {
		if (errno != ENOENT)
			FCD_ERR("access: %m\n");
		return 0;
	}

	return 1;
}

static void fcd_export_pic_gpio(void)
{
	int fd, warn = 0;

	fd = open("/sys/class/gpio/export", O_WRONLY);
	if (fd == -1) {
		FCD_ERR("open: %m\n");
		warn = 1;
	}
	else {
		if (write(fd, "31", 2) != 2) {
			FCD_ERR("write: %m\n");
			warn = 1;
		}

		if (close(fd) == -1)
			FCD_ERR("close: %m\n");
	}

	if (warn)
		FCD_WARN("Failed to export LCD PIC GPIO\n");
}

static void fcd_set_pic_gpio_direction(void)
{
	int fd, warn = 0;

	fd = open("/sys/class/gpio/gpio31/direction", O_WRONLY);
	if (fd == -1) {
		FCD_ERR("open: %m");
		warn = 1;
	}
	else {
		if (write(fd, "out", 3) != 3) {
			FCD_ERR("write: %m\n");
			warn = 1;
		}

		if (close(fd) == -1)
			FCD_ERR("close: %m\n");
	}

	if (warn)
		FCD_WARN("Failed to set LCD PIC GPIO direction\n");
}

void fcd_setup_pic_gpio(void)
{
	if (!fcd_pic_gpio_is_exported())
		fcd_export_pic_gpio();
	fcd_set_pic_gpio_direction();
}

void fcd_reset_pic(void)
{
	struct timespec req, rem;
	int fd, ret, warn = 1;

	fd = open("/sys/class/gpio/gpio31/value", O_WRONLY);
	if (fd == -1) {
		FCD_ERR("open: %m\n");
	}
	else {
		if (write(fd, "1", 1) != 1) {
			FCD_ERR("write: %m\n");
		}
		else {
			req.tv_sec = 0;
			req.tv_nsec = 60000; /* 60 usec */

			while (1) {
				ret = nanosleep(&req, &rem);
				if (ret == 0)
					break;
				if (errno != EINTR)
					FCD_ABORT("nanosleep: %m\n");
				req = rem;
			}

			if (write(fd, "0", 1) != 1) {
				FCD_ERR("write: %m\n");
			}
			else {
				/* Wait for PIC reset to complete */
				req.tv_sec = 2;
				req.tv_nsec = 0;

				while (1) {
					ret = nanosleep(&req, &rem);
					if (ret == 0)
						break;
					if (errno != EINTR)
						FCD_ABORT("nanosleep: %m\n");
					req = rem;
				}

				warn = 0;
			}
		}

		if (close(fd) == -1)
			FCD_ERR("close: %m\n");
	}

	if (warn)
		FCD_WARN("Failed to reset LCD PIC\n");

}
