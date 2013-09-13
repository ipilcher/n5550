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

#define _BSD_SOURCE	/* for vsyslog */

#include <syslog.h>
#include <stdio.h>
#include <stdarg.h>

#include "freecusd.h"

void fcd_err(int priority, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	if (fcd_foreground)
		vfprintf(stderr, format, ap);
	else
		vsyslog(priority, format, ap);

	va_end(ap);
}
