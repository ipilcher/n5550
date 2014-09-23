/*
 * Copyright 2013-2014 Ian Pilcher <arequipeno@gmail.com>
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
#include <errno.h>

int fcd_err_child_errfd = STDERR_FILENO;
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

/*******************************************************************************
 *
 * Pre-exec child process error reporting stuff
 *
 ******************************************************************************/

/*
 * All of the functions below write to this buffer.  This works because a
 * fork()ed child always has only a single thread (and the child has its own
 * address space).
 */
static char fcd_err_buf[1000];
static size_t fcd_err_len;

/*
 * Copy a C-style string (not including the terminating 0) to the error buffer,
 * truncating if necessary, and update the length.  Returns -1 if truncated (0
 * otherwise).
 */
static int fcd_err_str(const char *src)
{
	while (*src != 0) {

		if (fcd_err_len >= sizeof fcd_err_buf)
			return -1;

		fcd_err_buf[fcd_err_len++] = *(src++);
	}

	return 0;
}

/*
 * Copy a known number of characters to the error buffer, truncating if
 * necessary, and update the length.  Returns -1 if truncated (0 otherwise).
 */
static int fcd_err_cpy(const char *src, size_t n)
{
	size_t i;

	for (i = 0; i < n; ++i) {

		if (fcd_err_len >= sizeof fcd_err_buf)
			return -1;

		fcd_err_buf[fcd_err_len++] = *(src++);
	}

	return 0;
}

/* fcd_err_cpy wrapper for string literals. */
#define fcd_err_lit(s)	fcd_err_cpy((s), sizeof (s) - 1)

/*
 * Write a formatted unsigned integer to the error buffer, truncating if
 * necessary, and update the length.  Returns -1 if truncated (0 otherwise).
 */
static int fcd_err_uint(uintmax_t u)
{
	static uintmax_t max_divisor = 0;

	uintmax_t divisor;
	int printing = 0;

	if (max_divisor == 0) {

		/*
		 * uintmax_t must be at least a 64-bit type, so UINTMAX_MAX
		 * must be at least 18,446,744,073,709,551,615 (2^64 - 1).
		 * Therefore the smallest possible max_divisor is
		 * 10,000,000,000,000,000,000 (10^19).
		 */

		divisor = 10000000000000000000ULL;

		while (divisor < UINTMAX_MAX / 10)
			divisor *= 10;

		max_divisor = divisor;
	}
	else {
		divisor = max_divisor;
	}

	while (divisor > 0) {

		char digit = '0' + (char)(u / divisor);

		if (printing || digit > '0' || divisor == 1) {

			if (fcd_err_len >= sizeof fcd_err_buf)
				return -1;

			fcd_err_buf[fcd_err_len++] = digit;
			printing = 1;
		}

		u %= divisor;
		divisor /= 10;
	}

	return 0;
}

/*
 * Write a formatted signed integer to the error buffer, truncating if
 * necessary, and update the length.  Returns -1 if truncated (0 otherwise).
 */
static int fcd_err_int(intmax_t i)
{
	uintmax_t u;

	if (fcd_err_len >= sizeof fcd_err_buf)
		return -1;

	if (i < 0) {
		fcd_err_buf[fcd_err_len++] = '-';
		u = -i;
	}
	else {
		u = i;
	}

	return  fcd_err_uint(u);
}

/*
 * Write the error text corresponding to the current value of errno -- i.e.
 * strerror(errno) -- to the error buffer, truncating if necessary, and update
 * the length.   Returns -1 if truncated (0 otherwise).
 */
static int fcd_err_txt(void)
{
	if (errno >= 0 && errno < sys_nerr)
		return fcd_err_str(sys_errlist[errno]);

	if (fcd_err_lit("Unknown error: ") == -1)
		return -1;

	return fcd_err_int(errno);
}

void fcd_err_child_pabort(const char *msg, const char *file, int line)
{
	if (!fcd_err_foreground) {

		/* syslog header without timestamp and hostname */

		if (fcd_err_lit("<") == -1)			goto truncated;
		if (fcd_err_uint(LOG_DAEMON | LOG_ERR) == -1)	goto truncated;
		if (fcd_err_lit(">freecusd[") == -1)		goto truncated;
		if (fcd_err_int(getpid()) == -1)		goto truncated;
		if (fcd_err_lit("]: ") == -1)			goto truncated;
	}

	/* Now write the actual error message to the buffer */

	if (fcd_err_lit("ABORT: ") == -1)	goto truncated;
	if (fcd_err_str(file) == -1)		goto truncated;
	if (fcd_err_lit(":") == -1)		goto truncated;
	if (fcd_err_int(line) == -1)		goto truncated;
	if (fcd_err_lit(": ") == -1)		goto truncated;
	if (fcd_err_str(msg) == -1)		goto truncated;
	if (fcd_err_lit(": ") == -1)		goto truncated;
	if (fcd_err_txt() == -1)		goto truncated;
	if (fcd_err_lit("\n") == -1)		goto truncated;

	goto write_msg;

truncated:
	fcd_err_len = sizeof fcd_err_buf - (sizeof "...\n" - 1);
	fcd_err_lit("...\n");

write_msg:

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
	write(fcd_err_child_errfd, fcd_err_buf, fcd_err_len);
#pragma GCC diagnostic pop

	abort();
}
