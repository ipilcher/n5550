/*
 * Copyright 2013, 2020, 2022 Ian Pilcher <arequipeno@gmail.com>
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

#include <errno.h>
#include <gpiod.h>

#ifdef FCD_NEW_OS

void fcd_pic_setup_gpio(void)
{
	/* Do everything in fcd_pic_reset() */
}

void fcd_pic_reset(void)
{
	static const struct gpiod_line_request_config rc = {
		.consumer	= "freecusd",
		.request_type	= GPIOD_LINE_REQUEST_DIRECTION_OUTPUT
	};

	struct timespec req, rem;
	struct gpiod_chip *chip;
	struct gpiod_line *line;
	int err;

	if ((chip = gpiod_chip_open_by_label("gpio-pca9532")) == NULL)
		FCD_PFATAL("Failed to open LCD controller GPIO chip");

	if ((line = gpiod_chip_get_line(chip, 15)) == NULL)
		FCD_PFATAL("Failed to get LCD controller GPIO line");

	if (gpiod_line_request(line, &rc, 0) != 0)
		FCD_PFATAL("Failed to reserve LCD controller GPIO line");

	if (gpiod_line_set_value(line, 1) != 0)
		FCD_PFATAL("Failed to set LCD controller GPIO line HIGH");

	req = (struct timespec){ .tv_sec = 0, .tv_nsec = 60000 };

	while ((err = nanosleep(&req, &rem)) != 0) {

		if (errno == EINTR)
			req = rem;
		else
			FCD_PFATAL("Failed to sleep");
	}

	if (gpiod_line_set_value(line, 0) != 0)
		FCD_PFATAL("Failed to set LCD controller GPIO line LOW");

	req = (struct timespec){ .tv_sec = 2, .tv_nsec = 0 };

	while ((err = nanosleep(&req, &rem)) != 0) {

		if (errno == EINTR)
			req = rem;
		else
			FCD_PFATAL("Failed to sleep");
	}

	gpiod_line_release(line);
	gpiod_chip_close(chip);
}

#else  /* !FCD_NEW_OS */

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#include <selinux/restorecon.h>
#include <selinux/selinux.h>

static int fcd_pic_gpio_is_exported(void)
{
	static const char path[] = "/sys/class/gpio/gpio31";
	int ret;

	ret = access(path, F_OK);
	if (ret == -1) {
		if (errno != ENOENT)
			FCD_PERROR(path);
		return 0;
	}

	return 1;
}

static int fcd_pic_selinux_log(const int type, const char *const format, ...)
{
	va_list ap;
	int priority;

	switch (type) {

		case SELINUX_INFO:
			priority = LOG_DEBUG;
			break;

		case SELINUX_WARNING:
			priority = LOG_WARNING;
			break;

		default:
			FCD_WARN("Unknown libselinux message type: %d\n", type);
		/* fallthrough */
		case SELINUX_ERROR:
		/* fallthrough */
		case SELINUX_AVC:
			priority = LOG_ERR;
	}

	va_start(ap, format);
	fcd_err_vmsg(priority, format, ap);
	va_end(ap);

	return 0;
}

static void fcd_pic_export_gpio(void)
{
	static const char export_path[] = "/sys/class/gpio/export";

	static const char *const restorecon_paths[2] = {
		"/sys/devices/pci0000:00/0000:00:1f.3/i2c-0/0-0062/gpiochip1/gpio/gpio31/direction",
		"/sys/devices/pci0000:00/0000:00:1f.3/i2c-0/0-0062/gpiochip1/gpio/gpio31/value"
	};

	int i, fd, warn = 0;

	fd = open(export_path, O_WRONLY | O_CLOEXEC);
	if (fd == -1) {
		FCD_PERROR(export_path);
		warn = 1;
	}
	else {
		if (write(fd, "31", 2) != 2) {
			FCD_PERROR("write");
			warn = 1;
		}

		if (close(fd) == -1)
			FCD_PERROR("close");
	}

	if (warn) {
		FCD_WARN("Failed to export LCD PIC GPIO\n");
		return;
	}

	if (!is_selinux_enabled())
		return;

	selinux_set_callback(SELINUX_CB_LOG, (union selinux_callback)fcd_pic_selinux_log);

	/*
	 * Despite what the man page says, selinux_restorecon doesn't seem to
	 * actually set errno on CentOS 7, but it does log its errors via the
	 * callback.
	 */

	for (i = 0; i < 2; ++i) {
		if (selinux_restorecon(restorecon_paths[i], 0) != 0)
			FCD_WARN("Failed to restore SELinux context: %s\n", restorecon_paths[i]);
	}
}

static void fcd_pic_set_gpio_direction(void)
{
	static const char path[] = "/sys/class/gpio/gpio31/direction";
	int fd, warn = 0;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd == -1) {
		FCD_PERROR(path);
		warn = 1;
	}
	else {
		if (write(fd, "out", 3) != 3) {
			FCD_PERROR("write");
			warn = 1;
		}

		if (close(fd) == -1)
			FCD_PERROR("close");
	}

	if (warn)
		FCD_WARN("Failed to set LCD PIC GPIO direction\n");
}

void fcd_pic_setup_gpio(void)
{
	if (!fcd_pic_gpio_is_exported())
		fcd_pic_export_gpio();
	fcd_pic_set_gpio_direction();
}

void fcd_pic_reset(void)
{
	static const char path[] = "/sys/class/gpio/gpio31/value";
	struct timespec req, rem;
	int fd, ret, warn = 1;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd == -1) {
		FCD_PERROR(path);
	}
	else {
		if (write(fd, "1", 1) != 1) {
			FCD_PERROR("write");
		}
		else {
			req.tv_sec = 0;
			req.tv_nsec = 60000; /* 60 usec */

			while (1) {
				ret = nanosleep(&req, &rem);
				if (ret == 0)
					break;
				if (errno != EINTR)
					FCD_PABORT("nanosleep");
				req = rem;
			}

			if (write(fd, "0", 1) != 1) {
				FCD_PERROR("write");
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
						FCD_PABORT("nanosleep");
					req = rem;
				}

				warn = 0;
			}
		}

		if (close(fd) == -1)
			FCD_PERROR("close");
	}

	if (warn)
		FCD_WARN("Failed to reset LCD PIC\n");

}

#endif

