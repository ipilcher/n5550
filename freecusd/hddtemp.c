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

#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

/* Alert thresholds */
static const int fcd_hddtemp_warn = 45;
static const int fcd_hddtemp_fail = 50;

#define FCD_HDDTEMP_BUF_MAX	1000

static char *fcd_hddtemp_cmd[] = {
	"/usr/sbin/hddtemp",
	"hddtemp",
	"/dev/sdb",
	"/dev/sdc",
	"/dev/sdd",
	"/dev/sde",
	"/dev/sdf",
	NULL
};

static int fcd_hddtemp_exec(char **cmd_buf, size_t *buf_size,
			    const int *pipe_fds, struct fcd_monitor *mon)
{
	struct timespec timeout;
	int ret, status;

	/* May need to increase timeout for more than 5 disks */

	timeout.tv_sec = 5;
	timeout.tv_nsec = 0;

	ret = fcd_cmd_output(&status, fcd_hddtemp_cmd, cmd_buf, buf_size,
			     FCD_HDDTEMP_BUF_MAX, &timeout, pipe_fds);

	switch (ret) {
		case -4:	fcd_disable_mon_cmd(mon, pipe_fds, *cmd_buf);
		case -3:	return -3;
		case -2:	FCD_WARN("hddtemp command timed out\n");
		case -1:	fcd_disable_mon_cmd(mon, pipe_fds, *cmd_buf);
	}

	if (status != 0) {
		FCD_WARN("Non-zero hddtemp exit status: %d\n", status);
		fcd_disable_mon_cmd(mon, pipe_fds, *cmd_buf);
	}

	return 0;
}

static void fcd_hddtemp_parse(char *cmd_buf, int *temps, const int *pipe_fds,
			      struct fcd_monitor *mon)
{
	int ret, temp, n;
	const char *s;
	char c;

	s = cmd_buf;

	for (s = cmd_buf; *s != 0; s += n) {

		n = -1;
		errno = 0;

		ret = sscanf(s, "/dev/sd%c:%*[^:]: %d%*[^\n]%*1[\n]%n",
			     &c, &temp, &n);
		if (ret == EOF) {
			if (errno == 0)
				FCD_WARN("Unexpected end of hddtemp output\n");
			else
				FCD_PERROR("sscanf");
			fcd_disable_mon_cmd(mon, pipe_fds, cmd_buf);
		}

		if (ret != 2 || n == -1 || errno != 0 || c < 'b' || c > 'f') {
			FCD_WARN("Error parsing hddtemp output\n");
			fcd_disable_mon_cmd(mon, pipe_fds, cmd_buf);
		}

		temps[c - 'b'] = temp;
	}
}


__attribute__((noreturn))
static void *fcd_hddtemp_fn(void *arg)
{
	struct fcd_monitor *mon = arg;
	int ret, i, temps[5], disk_presence[5], pipe_fds[2];
	int max, warn, fail, disk_alerts[5];
	char *b, buf[21], *cmd_buf;
	size_t buf_size;

	if (pipe2(pipe_fds, O_CLOEXEC) == -1) {
		FCD_PERROR("pipe2");
		fcd_lib_disable_monitor(mon);
	}

	cmd_buf = NULL;
	buf_size = 0;

	memset(disk_presence, 0, sizeof disk_presence);

	do {
		temps[0] = temps[1] = temps[2] = temps[3] = temps[4] = INT_MIN;
		memset(buf, ' ', sizeof buf);

		if (fcd_update_disk_presence(disk_presence) == -1)
			fcd_disable_mon_cmd(mon, pipe_fds, cmd_buf);

		if (fcd_hddtemp_exec(&cmd_buf, &buf_size, pipe_fds, mon) == -3)
			continue;

		fcd_hddtemp_parse(cmd_buf, temps, pipe_fds, mon);

		memset(disk_alerts, 0, sizeof disk_alerts);
		max = 0;

		for (i = 0, b = buf; i < 5; ++i)
		{
			if (temps[i] >= -99 && temps[i] <= 999) {

				disk_alerts[i] = (temps[i] >= fcd_hddtemp_warn);
				if (temps[i] > max)
					max = temps[i];

				ret = sprintf(b, "%d ", temps[i]);
				if (ret == EOF) {
					FCD_PERROR("sprintf");
					fcd_disable_mon_cmd(mon, pipe_fds,
							    cmd_buf);
				}
				b += ret;
			}
			else {
				if (disk_presence[i] == 0)
					memset(b, '-', 2);
				else if (temps[i] == INT_MIN)
					memset(b, '?', 2);
				else
					memset(b, '*', 2);
				b += 3;
			}
		}

		fail = (max >= fcd_hddtemp_fail);
		warn = fail ? 0 : (max >= fcd_hddtemp_warn);

		fcd_copy_buf_and_alerts(mon, buf, warn, fail, disk_alerts);

		ret = fcd_lib_monitor_sleep(30);
		if (ret == -1)
			fcd_disable_mon_cmd(mon, pipe_fds, cmd_buf);

	} while (ret == 0);

	free(cmd_buf);
	fcd_proc_close_pipe(pipe_fds);
	pthread_exit(NULL);
}

struct fcd_monitor fcd_hddtemp_monitor = {
	.mutex		= PTHREAD_MUTEX_INITIALIZER,
	.name		= "HDD temperature",
	.monitor_fn	= fcd_hddtemp_fn,
	.buf		= "....."
			  "HDD TEMPERATURE     "
			  "                    ",
};
