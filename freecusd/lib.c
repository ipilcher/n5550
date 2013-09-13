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

#define _GNU_SOURCE	/* for pipe2 */

#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "freecusd.h"

int fcd_sleep_and_check_exit(time_t seconds)
{
	struct timespec abstime;
	int err, ret;

	abstime.tv_sec = time(NULL) + seconds;
	abstime.tv_nsec = 0;

	err = pthread_mutex_lock(&fcd_thread_exit_mutex);
	if (err != 0)
		FCD_ABORT("pthread_mutex_lock: %s\n", strerror(err));

	err = pthread_cond_timedwait(&fcd_thread_exit_cond,
				     &fcd_thread_exit_mutex, &abstime);
	if (err != 0 && err != ETIMEDOUT)
		FCD_ABORT("pthread_cond_timedwait: %s\n", strerror(err));

	ret = fcd_thread_exit_flag;

	err = pthread_mutex_unlock(&fcd_thread_exit_mutex);
	if (err != 0)
		FCD_ABORT("pthread_mutex_unlock: %s\n", strerror(err));

	return ret;
}

void fcd_disable_monitor(struct fcd_monitor *mon)
{
	static const char disabled_msg[20] = "ERROR: NOT AVAILABLE";
	int ret;

	FCD_WARN("Disabling %s monitor\n", mon->name);

	/* TODO - Set alarm, blink LED, etc. */

	ret = pthread_mutex_lock(&mon->mutex);
	if (ret != 0)
		FCD_ABORT("pthread_mutex_lock: %s\n", strerror(ret));

	memcpy(mon->buf + 45, disabled_msg, 20);

	ret = pthread_mutex_unlock(&mon->mutex);
	if (ret != 0)
		FCD_ABORT("pthread_mutex_unlock: %s\n", strerror(ret));

	pthread_exit(NULL);
}

void fcd_copy_buf(const char *buf, struct fcd_monitor *mon)
{
	int ret;

	ret = pthread_mutex_lock(&mon->mutex);
	if (ret != 0)
		FCD_ABORT("pthread_mutex_lock: %s\n", strerror(ret));

	memcpy(mon->buf + 45, buf, 20);

	ret = pthread_mutex_unlock(&mon->mutex);
	if (ret != 0)
		FCD_ABORT("pthread_mutex_unlock: %s\n", strerror(ret));
}

int fcd_update_disk_presence(int *presence)
{
	int i, present, changed = 0;
	char dev[] = "/dev/sdX";

	for (i = 0; i < 5; ++i)
	{
		dev[7] = 'b' + i;

		if (access(dev, F_OK) == -1) {
			if (errno == ENOENT) {
				present = 0;
			}
			else {
				FCD_ERR("access: %m\n");
				return -1;
			}
		}
		else {
			present = 1;
		}

		if (presence[i] != present) {
			presence[i] = present;
			changed = 1;
		}
	}

	return changed;
}

__attribute__((noreturn))
static void fcd_cmd_child_suicide()
{
	/* Got a better idea? */

	while (1)
	{
		if (kill(getpid(), SIGKILL) == -1)
			FCD_ERR("kill: %m\n");
		sleep(5);
	}
}

__attribute__((noreturn))
static void fcd_cmd_child(int fd, char **cmd)
{
	int flags;

	if (dup2(fd, STDOUT_FILENO) == -1) {
		FCD_ERR("dup2: %m\n");
		fcd_cmd_child_suicide();
	}

	if (!fcd_foreground)
	{
		flags = fcntl(STDERR_FILENO, F_GETFD);
		if (flags == -1) {
			FCD_ERR("fcntl: %m\n");
			fcd_cmd_child_suicide();
		}

		if (fcntl(STDERR_FILENO, F_SETFD, flags | FD_CLOEXEC) == -1) {
			FCD_ERR("fcntl: %m\n");
			fcd_cmd_child_suicide();
		}
	}

	execv(cmd[0], cmd + 1);

	FCD_ERR("execv: %m\n");
	fcd_cmd_child_suicide();
}

FILE *fcd_cmd_spawn(pid_t *child, char **cmd)
{
	int fd[2];
	FILE *fp;

	if (pipe2(fd, O_CLOEXEC) == -1) {
		FCD_ERR("pipe2: %m\n");
		return NULL;
	}

	*child = fork();
	if (*child == -1) {
		FCD_ERR("fork: %m\n");
		if (close(fd[1]) == -1)
			FCD_ERR("close: %m\n");
		goto fcd_cmd_spawn_error;
	}

	if (*child == 0)
		fcd_cmd_child(fd[1], cmd);

	if (close(fd[1]) == -1) {
		FCD_ERR("close: %m\n");
		goto fcd_cmd_spawn_error;
	}

	fp = fdopen(fd[0], "r");
	if (fp == NULL) {
		FCD_ERR("fdopen: %m\n");
		goto fcd_cmd_spawn_error;
	}

	return fp;

fcd_cmd_spawn_error:

	if (close(fd[0]) == -1)
		FCD_ERR("close: %m\n");
	return NULL;
}

int fcd_cmd_cleanup(FILE *fp, pid_t child)
{
	int status;

	if (fclose(fp) == EOF) {
		FCD_ERR("fclose: %m\n");
		return -1;
	}

	if (waitpid(child, &status, 0) == -1) {
		FCD_ERR("waitpid: %m\n");
		return -1;
	}

	if (!WIFEXITED(status)) {
		FCD_WARN("Child process terminated abnormally\n");
		return -1;
	}

	return WEXITSTATUS(status);
}
