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

#include "freecusd.h"

#include <syslog.h>
#include <stdarg.h>
#include <string.h>

int fcd_err_foreground = 0;

static const char *const fcd_err_severities[] = {
	"ERROR",
	"FATAL",
	"ABORT",
};

void fcd_err_msg(int priority, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	if (fcd_err_foreground)
		vfprintf(stderr, format, ap);
	else
		vsyslog(priority, format, ap);

	va_end(ap);
}

void fcd_err_perror(const char *msg, const char *file, int line, int sev)
{
	fcd_err_msg(LOG_ERR, "%s: %s:%d: %s: %m\n", fcd_err_severities[sev],
		    file, line, msg);
}

void fcd_err_pt_err(const char *msg, int err, const char *file, int line,
		    int sev)
{
	fcd_err_msg(LOG_ERR, "%s: %s:%d: %s: %s\n", fcd_err_severities[sev],
		    file, line, msg, strerror(err));
}
