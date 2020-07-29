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

#define _GNU_SOURCE	/* for vsyslog, asprintf, strptime, and timegm */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define RAIDCHECK_GLOB_FLAGS	(GLOB_ERR | GLOB_MARK | GLOB_NOSORT | GLOB_ONLYDIR)

/*
 * RAID check timing for a particular device is determined by 3 values.
 *
 * 	* Frequency - the number of days between checks of this device.
 *
 * 	* Cycle date - a date on which this device should be (or theoretically
 * 	  should have been) checked.  The check will be performed on any day
 * 	  that is an even multiple of frequency days after the cycle date.
 *
 * 	* Time - the (approximate) time at which the check should be started.
 * 	  If this program runs within 15 minutes of this time, on a date that
 * 	  matches the cycle date and frequency, the check will be started
 * 	  (assuming that no other checks are in progress and no arrays are
 * 	  degraded).
 */

struct cfg_dev {
	struct cfg_dev *next;
	char name[8];
	unsigned check_freq;	/* days */
	unsigned cycle_date;	/* days since 1 January 1970 */
	unsigned check_time;	/* minutes after midnight */
};

static _Bool use_syslog;
static _Bool debug;

static void vmsg(const int priority, const char *const format, va_list ap)
{
	if (use_syslog)
		vsyslog(priority, format, ap);
	else
		vfprintf(stderr, format, ap);
}

__attribute__((format(printf, 1, 2)))
static void dbug(const char *const restrict format, ...)
{
	va_list ap;

	if (debug) {
		va_start(ap, format);
		vmsg(LOG_INFO, format, ap);
		va_end(ap);
	}
}

__attribute__((format(printf, 1, 2)))
static void info(const char *const restrict format, ...)
{
	va_list ap;

	va_start(ap, format);
	vmsg(LOG_INFO, format, ap);
	va_end(ap);
}

__attribute__((format(printf, 1, 2)))
static void err(const char *const restrict format, ...)
{
	va_list ap;

	va_start(ap, format);
	vmsg(LOG_ERR, format, ap);
	va_end(ap);
}

__attribute__((format(printf, 1, 2), noreturn))
static void abrt(const char *const restrict format, ...)
{
	va_list ap;

	va_start(ap, format);
	vmsg(LOG_ERR, format, ap);
	va_end(ap);

	abort();
}

__attribute__((format(printf, 1, 2), noreturn))
static void fail(const char *const restrict format, ...)
{
	va_list ap;

	va_start(ap, format);
	vmsg(LOG_ERR, format, ap);
	va_end(ap);

	exit(EXIT_FAILURE);
}

__attribute__((format(printf, 1, 2), noreturn))
static void bail(const char *const restrict format, ...)
{
	va_list ap;

	va_start(ap, format);
	vmsg(LOG_WARNING, format, ap);
	va_end(ap);

	exit(EXIT_FAILURE);
}

static struct cfg_dev *parse_cfg_line(const char *const restrict cfg_file, const unsigned cfg_line,
				      const char *const restrict line)
{
	struct cfg_dev *dev;
	char *cdate, *ctime;
	struct tm tm;
	time_t t;

	if ((dev = calloc (1, sizeof *dev)) == NULL)
		abrt("calloc: %m\n");

	if (sscanf(line, "%7s %u %ms %ms", dev->name, &dev->check_freq, &cdate, &ctime) != 4)
		fail("%s:%u: failed to parse line\n", cfg_file, cfg_line);

	memset(&tm, 0, sizeof tm);

	if (strptime(cdate, "%Y-%m-%d", &tm) == NULL)
		fail("%s:%u: invalid date (%s)\n", cfg_file, cfg_line, cdate);

	if (tm.tm_year < 70 || tm.tm_year > 1100) {
		fail("%s:%u: year (%d) out of range (1970-3000)\n",
		     cfg_file, cfg_line, tm.tm_year + 1900);
	}

	tm.tm_isdst = 0;

	if ((t = timegm(&tm)) == -1)
		abrt("timegm: %m\n");

	assert(t >= 0 && t % 86400 == 0);

	dev->cycle_date = (unsigned)(t / 86400);

	memset(&tm, 0, sizeof tm);

	if (strptime(ctime, "%H:%M", &tm) == NULL)
		fail("%s:%u: invalid time (%s)\n", cfg_file, cfg_line, ctime);

	assert(tm.tm_hour >= 0 && tm.tm_hour < 24 && tm.tm_min >= 0 && tm.tm_min < 60);

	dev->check_time = tm.tm_hour * 60 + tm.tm_min;

	free(cdate);
	free(ctime);

	return dev;
}

static struct cfg_dev *parse_cfg(const char *const cfg_file)
{
	struct cfg_dev *list, **tail;
	char *line;
	FILE *fp;
	unsigned cfg_line;
	ssize_t ret;
	size_t len;

	dbug("Parsing configuration from %s\n", cfg_file);

	if ((fp = fopen(cfg_file, "r")) == NULL)
		fail("%s: %m\n", cfg_file);

	for (list = NULL, tail = &list, cfg_line = 1; ; ++cfg_line) {

		line = NULL;
		len = 0;

		/* getline() probably never returns 0, but treat it like EOF just in case */
		if ((ret = getline(&line, &len, fp)) <= 0) {
			if (ret == 0 || feof(fp)) {
				free(line);
				break;
			}
			else {
				abrt("getline: %m\n");
			}
		}

		--ret;

		if (line[ret] == '\n')
			line[ret] = 0;

		if (*line == '#' || *line == 0) {
			free(line);
			continue;
		}

		*tail = parse_cfg_line(cfg_file, cfg_line, line);
		tail = &(*tail)->next;

		free(line);
	}

	if (fclose(fp) != 0)
		abrt("fclose: %m\n");

	return list;
}

static int glob_err(const char *const epath, const int eerno)
{
	errno = eerno;
	err("%s: %m\n", epath);
	return 0;
}

static void check_sys_status(void)
{
	static const char glob_pattern[] = "/sys/devices/virtual/block/*/md/";

	int dirfd, fd;
	char buf[32];
	unsigned i;
	ssize_t s;
	glob_t gl;

	switch (glob(glob_pattern, RAIDCHECK_GLOB_FLAGS, glob_err, &gl)) {
		case 0:			break;
		case GLOB_NOSPACE:	abrt("glob: out of memory\n");
		case GLOB_ABORTED:	fail("glob: read error\n");
		case GLOB_NOMATCH:	fail("No RAID devices found\n");
		default:		abrt("glob: unknown error\n");
	}

	for (i = 0; i < gl.gl_pathc; ++i) {

		if ((dirfd = open(gl.gl_pathv[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW)) < 0)
			fail("%s: %m\n", gl.gl_pathv[i]);

		if ((fd = openat(dirfd, "degraded", O_RDONLY | O_NOFOLLOW)) < 0)
			fail("%sdegraded: %m\n", gl.gl_pathv[i]);

		if ((s = read(fd, buf, sizeof buf - 1)) < 0)
			fail("%sdegraded: %m\n", gl.gl_pathv[i]);

		if (s > 0 && buf[s - 1] == '\n')
			buf[s - 1] = 0;
		else
			buf[s] = 0;

		if (strcmp(buf, "0") != 0)
			bail("%sdegraded is non-zero (%s); aborting\n", gl.gl_pathv[i], buf);

		if (close(fd) != 0)
			abrt("close: %m\n");

		if ((fd = openat(dirfd, "sync_action", O_RDONLY | O_NOFOLLOW)) < 0)
			fail("%ssync_action: %m\n", gl.gl_pathv[i]);

		if ((s = read(fd, buf, sizeof buf - 1)) < 0)
			fail("%ssync_action: %m\n", gl.gl_pathv[i]);

		if (s > 0 && buf[s - 1] == '\n')
			buf[s - 1] = 0;
		else
			buf[s] = 0;

		if (strcmp(buf, "idle") != 0)
			bail("%ssync_action is not 'idle' ('%s'); aborting\n", gl.gl_pathv[i], buf);

		if (close(fd) != 0)
			abrt("close: %m\n");

		if (close(dirfd) != 0)
			abrt("close: %m\n");
	}

	globfree(&gl);
}

static void get_current_time(unsigned *const restrict days, unsigned *const restrict mins)
{
	const char *override;
	struct tm tm, *lt;
	time_t t;

	if ((override = getenv("RAIDCHECK_TIME_OVERRIDE")) == NULL) {
		t = time(NULL);
	}
	else {
		t = atol(override);
		info("Time set via RAIDCHECK_TIME_OVERRIDE (%ld)\n", t);
	}

	if ((lt = localtime(&t)) == NULL)
		abrt("localtime: %m\n");

	dbug("Current time is %s", asctime(lt));

	if (lt->tm_year >= 1100)
		bail("Current year (%d) out of range (1970-3000)\n", lt->tm_year + 1900);

	assert(lt->tm_hour >= 0 && lt->tm_hour < 24 && lt->tm_min >= 0 && lt->tm_min < 60);

	*mins = lt->tm_hour * 60 + lt->tm_min;

	memset(&tm, 0, sizeof tm);

	tm.tm_mday = lt->tm_mday;
	tm.tm_mon = lt->tm_mon;
	tm.tm_year = lt->tm_year;

	if ((t = timegm(&tm)) == -1)
		abrt("timegm: %m\n");

	assert(t >= 0 && t % 86400 == 0);

	*days = t / 86400;
}

static void handle_dev(const struct cfg_dev *const dev, const unsigned days, const unsigned mins)
{
	char *sa_path;
	ssize_t s;
	int fd;

	dbug("Considering %s ...\n", dev->name);

	if (days < dev->cycle_date) {
		dbug("Current date is before %s cycle date; ignoring\n", dev->name);
		return;
	}

	if ((days - dev->cycle_date) % dev->check_freq != 0) {
		dbug("Date difference (%u days) not a multiple of frequency (%u days); ignoring\n",
		     days - dev->cycle_date, dev->check_freq);
		return;
	}

	if (abs((int)mins - (int)dev->check_time) > 15) {
		dbug("Time difference (%d minutes) greater than 15 minutes; ignoring\n",
		     abs((int)mins - (int)dev->check_time));
		return;
	}

	dbug("Starting check of %s\n", dev->name);

	if (asprintf(&sa_path, "/sys/devices/virtual/block/%s/md/sync_action", dev->name) < 0)
		abrt("asprintf: %m\n");

	if ((fd = open(sa_path, O_WRONLY | O_NOFOLLOW)) < 0)
		fail("%s: %m\n", sa_path);

	if ((s = write(fd, "check\n", sizeof "check\n")) < 0)
		fail("%s: %m\n", sa_path);

	if ((size_t)s != sizeof "check\n")
		fail("%s: incomplete write\n", sa_path);

	info("Started check of RAID device %s\n", dev->name);

	if (close(fd) != 0)
		abrt("close: %m");

	free(sa_path);
}

int main(void)
{
	unsigned days, mins;
	struct cfg_dev *dev_list, *dev;
	const char *cfg_file;

	use_syslog = !isatty(STDERR_FILENO);
	debug = (getenv("RAIDCHECK_DEBUG") != NULL);

	get_current_time(&days, &mins);

	if ((cfg_file = getenv("RAIDCHECK_CONFIG")) == NULL)
		cfg_file = "/etc/raidcheck.conf";

	dev_list = parse_cfg(cfg_file);

	check_sys_status();

	while (dev_list != NULL) {

		dev = dev_list;
		handle_dev(dev, days, mins);
		dev_list = dev->next;
		free(dev);
	}

	dbug("Sleeping 60 seconds so journald can match log messages\n");
	sleep(60);

	return 0;
}
