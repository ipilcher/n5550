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

#define _GNU_SOURCE	/* for pipe2 and ppoll */

#include "freecusd.h"

#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>

#define FCD_BUF_CHUNK	2000

sigset_t fcd_mon_ppoll_sigmask;

/*
 * Sleeps for the specified number of seconds, unless interrupted by a signal
 * (SIGUSR1). Returns the thread-local value of fcd_thread_exit_flag (or -1 on
 * error).
 *
 * NOTE: Does not check fcd_thread_exit_flag before sleeping (assumes that
 * 	 SIGUSR1 has been blocked).
 */
int fcd_sleep_and_check_exit(time_t seconds)
{
	struct timespec ts;

	ts.tv_sec = seconds;
	ts.tv_nsec = 0;

	if (ppoll(NULL, 0, &ts, &fcd_mon_ppoll_sigmask) == -1
						&& errno != EINTR) {
		FCD_PERROR("ppoll");
		return -1;
	}

	return fcd_thread_exit_flag;
}

/*
 * Calculates *deadline, based on current time and timeout. Returns 0 on
 * success, -1 on error.
 */
static int fcd_lib_deadline(struct timespec *deadline,
			    const struct timespec *timeout)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC_COARSE, &now) == -1) {
		FCD_PERROR("clock_gettime");
		return -1;
	}

	deadline->tv_sec  = now.tv_sec  + timeout->tv_sec;
	deadline->tv_nsec = now.tv_nsec + timeout->tv_nsec;

	if (deadline->tv_nsec >= 1000000000L)
	{
		deadline->tv_nsec -= 1000000000L;
		++(deadline->tv_sec);
	}

	return 0;
}

/*
 * Calculates *remaining time, based on current time and deadline (but "rounds"
 * negative result up to zero). Returns 0 on success, -1 on error.
 */
static int fcd_lib_remaining(struct timespec *remaining,
			     const struct timespec *deadline)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC_COARSE, &now) == -1) {
		FCD_PERROR("clock_gettime");
		return -1;
	}

	/* Linux time_t is signed */

	remaining->tv_sec  = deadline->tv_sec  - now.tv_sec;
	remaining->tv_nsec = deadline->tv_nsec - now.tv_nsec;

	if (remaining->tv_nsec < 0)
	{
		remaining->tv_nsec += 1000000000L;
		--(remaining->tv_sec);
	}

	if (remaining->tv_sec < 0)
	{
		remaining->tv_sec = 0;
		remaining->tv_nsec = 0;
	}

	return 0;
}

/*
 * Acts as a wrapper around read(2) with a timeout. Updates *timeout with
 * remaining time on successful return (>= 0). Returns # of bytes read (0 = EOF,
 * -1 = error, -2 = timeout, -3 = thread exit signal received).
 */
ssize_t fcd_lib_read(int fd, void *buf, size_t count, struct timespec *timeout)
{
	struct timespec deadline;
	struct pollfd pfd;
	ssize_t ret;

	if (fcd_lib_deadline(&deadline, timeout) == -1)
		return -1;

	pfd.fd = fd;
	pfd.events = POLLIN;

	while (!fcd_thread_exit_flag)
	{
		if (fcd_lib_remaining(timeout, &deadline) == -1)
			return -1;

		ret = ppoll(&pfd, 1, timeout, &fcd_mon_ppoll_sigmask);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			FCD_PERROR("ppoll");
			return -1;
		}

		if (ret == 0)
			return -2;

		/*
		 * Different file descriptors (regular files, pipes, sysfs/proc
		 * files, etc.) behave so differently that it's impossible to
		 * check revents in a meaningful way.
		 */

		ret = read(fd, buf, count);
		if (ret == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			FCD_PERROR("read");
			return -1;
		}
		else {
			if (fcd_lib_remaining(timeout, &deadline) == -1)
				return -1;
			return ret;
		}
	}

	return -3;
}

static int fcd_set_fd_flags(int fd, int get_cmd, int set_cmd, int flags)
{
	int current_flags;

	current_flags = fcntl(fd, get_cmd);
	if (current_flags == -1) {
		FCD_PERROR("fcntl");
		return -1;
	}

	if (fcntl(fd, set_cmd, current_flags | flags) == -1) {
		FCD_PERROR("fcntl");
		return -1;
	}

	return 0;
}

static int fcd_cmd_set_fd_cloexec(int fd)
{
	return fcd_set_fd_flags(fd, F_GETFD, F_SETFD, FD_CLOEXEC);
}
#if 0
static int fcd_set_fd_nonblock(int fd)
{
	return fcd_set_fd_flags(fd, F_GETFL, F_SETFL, O_NONBLOCK);
}
#endif
/*
 * Returns 0 on success, -1 on error, -4 if max buffer size would be exceeded
 */
static int fcd_grow_buf(char **buf, size_t *buf_size, size_t max_size)
{
	size_t new_size;
	char *new_buf;

	if (*buf == NULL || *buf_size == 0)
		new_size = FCD_BUF_CHUNK;
	else
		new_size = *buf_size + FCD_BUF_CHUNK;

	/*
	 * Ensure max_size is a multiple of FCD_BUF_CHUNK, rounding up if
	 * necessary
	 */

	max_size = ((max_size + FCD_BUF_CHUNK - 1) / FCD_BUF_CHUNK)
							* FCD_BUF_CHUNK;

	if (new_size > max_size)
		return -4;

	new_buf = realloc(*buf, new_size);
	if (new_buf == NULL) {
		FCD_PERROR("realloc");
		return -1;
	}

	*buf = new_buf;
	*buf_size = new_size;

	return 0;
}

/*
 * Returns # of bytes read, which may be 0 (-1 = error, -2 = timeout,
 * -3 = thread exit signal received, -4 = max buffer size would be exceeded).
 * Updates *timeout with remaining time.  (*timeout is undefined on error.)
 */
ssize_t fcd_read_all(int fd, char **buf, size_t *buf_size, size_t max_size,
		     struct timespec *timeout)
{
	size_t total;
	ssize_t ret;

	total = 0;

	do {
		if (total == *buf_size) {
			ret = fcd_grow_buf(buf, buf_size, max_size);
			if (ret < 0)
				return ret;	/* -1 or -4 */
		}

		ret = fcd_lib_read(fd, *buf + total, *buf_size - total,
				   timeout);
		if (ret < 0)
			return ret;	/* -1, -2, or -3 */

		total += ret;

	} while (ret != 0);

	/*
	 * If the number of bytes read is an exact multiple of FCD_BUF_CHUNK,
	 * buffer will have been grown immediately before fcd_read returned 0.
	 */

	(*buf)[total] = 0;

	return total;
}

void fcd_disable_monitor(struct fcd_monitor *mon)
{
	static const char disabled_msg[20] = "ERROR: NOT AVAILABLE";
	int ret;

	FCD_WARN("Disabling %s monitor\n", mon->name);

	ret = pthread_mutex_lock(&mon->mutex);
	if (ret != 0)
		FCD_PT_ABRT("pthread_mutex_lock", ret);

	fcd_alert_update(FCD_ALERT_SET_REQ, &mon->sys_fail);
	memcpy(mon->buf + 45, disabled_msg, 20);

	ret = pthread_mutex_unlock(&mon->mutex);
	if (ret != 0)
		FCD_PT_ABRT("pthread_mutex_unlock", ret);

	pthread_exit(NULL);
}

void fcd_disable_mon_cmd(struct fcd_monitor *mon, const int *pipe_fds,
			 char *buf)
{
	free(buf);
	fcd_proc_close_pipe(pipe_fds);
	fcd_disable_monitor(mon);
}

void fcd_copy_buf_and_alerts(struct fcd_monitor *mon, const char *buf,
			     int warn, int fail, const int *disks)
{
	size_t i;
	int ret;

	ret = pthread_mutex_lock(&mon->mutex);
	if (ret != 0)
		FCD_PT_ABRT("pthread_mutex_lock", ret);

	memcpy(mon->buf + 45, buf, 20);

	fcd_alert_update(warn ? FCD_ALERT_SET_REQ : FCD_ALERT_CLR_REQ,
			 &mon->sys_warn);
	fcd_alert_update(fail ? FCD_ALERT_SET_REQ : FCD_ALERT_CLR_REQ,
			 &mon->sys_fail);

	if (disks != NULL) {

		for (i = 0; i < FCD_ARRAY_SIZE(mon->disk_alerts); ++i) {

			fcd_alert_update(disks[i] ? FCD_ALERT_SET_REQ :
						    FCD_ALERT_CLR_REQ,
					 &mon->disk_alerts[i]);
		}
	}

	ret = pthread_mutex_unlock(&mon->mutex);
	if (ret != 0)
		FCD_PT_ABRT("pthread_mutex_unlock", ret);
}
#if 0
void fcd_copy_buf(const char *buf, struct fcd_monitor *mon)
{
	fcd_copy_buf_and_alerts(mon, buf, 0, 0, NULL);
}
#endif
int fcd_update_disk_presence(int *presence)
{
	char dev[sizeof "/dev/sdX"];
	int i, present, changed;

	for (changed = 0, i = 0; i < 5; ++i) {

		sprintf(dev, "/dev/sd%c", 'b' + i);

		if (access(dev, F_OK) == -1) {
			if (errno == ENOENT) {
				present = 0;
			}
			else {
				FCD_PERROR(dev);
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
static void fcd_cmd_child(int fd, char **cmd)
{
	/*
	 * This flow is a bit ugly.  If we created an output pipe (fd != -1),
	 * then replace STDOUT with the pipe.  If we did NOT create an output
	 * pipe (fd == -1), then set the CLOEXEC flag on STDOUT -- unless we're
	 * running in the foreground.
	 *
	 * STDERR also gets its CLOEXEC flag set, unless we're running in the
	 * foreground.  (It doesn't matter if we're creating an output pipe or
	 * not.)
	 */

	if (fd != -1) {

		/* CLOEXEC is NOT inherited by dup2'ed descriptor */
		if (dup2(fd, STDOUT_FILENO) == -1)
			FCD_PABORT("dup2");
	}

	if (!fcd_foreground) {

		if (fd == -1 && fcd_cmd_set_fd_cloexec(STDOUT_FILENO) == -1)
			FCD_ABORT();

		if (fcd_cmd_set_fd_cloexec(STDERR_FILENO) == -1)
			FCD_ABORT();
	}

	execv(cmd[0], cmd + 1);

	FCD_PABORT("execv");
}

static int fcd_cmd_spawn(pid_t *child, char **cmd, const int *reaper_pipe,
			 int create_output_pipe)
{
	int output_pipe[2];

	if (create_output_pipe) {

		/* CLOEXEC will not be inherited by dup2'ed file descriptor */
		if (pipe2(output_pipe, O_CLOEXEC) == -1) {
			FCD_PERROR("pipe2");
			return -1;
		}
	}

	*child = fcd_proc_fork(reaper_pipe);
	if (*child == -1) {
		FCD_PERROR("fork");
		if (create_output_pipe) {
			if (close(output_pipe[0]) == -1)
				FCD_PERROR("close");
			if (close(output_pipe[1]) == -1)
				FCD_PERROR("close");
		}
		return -1;
	}

	if (*child == 0)
		fcd_cmd_child(create_output_pipe ? output_pipe[1] : -1, cmd);

	if (create_output_pipe)	{

		if (close(output_pipe[1]) == -1) {
			FCD_PERROR("close");
			if (close(output_pipe[0]) == -1) {
				FCD_PERROR("close");
				FCD_ABORT("Failed to close child pipe\n");
			}
			fcd_proc_kill(*child, reaper_pipe);
			return -1;
		}
	}

	return create_output_pipe ? output_pipe[0] : 0;
}

/*
 * Returns # of bytes read, which may be 0 (-1 = error, -2 = timeout,
 * -3 = thread exit signal received, -4 = max buffer size would be exceeded).
 * Updates *timeout with remaining time.  (*timeout is undefined on error.)
 */
ssize_t fcd_cmd_output(int *status, char **cmd, char **buf, size_t *buf_size,
		       size_t max_size, struct timespec *timeout,
		       const int *pipe_fds)
{
	ssize_t bytes_read;
	int ret, fd;
	pid_t child;

	fd = fcd_cmd_spawn(&child, cmd, pipe_fds, 1);
	if (fd == -1)
		return -1;

	bytes_read = fcd_read_all(fd, buf, buf_size, max_size, timeout);
	if (bytes_read < 0) {
		if (close(fd) == -1)
			FCD_PERROR("close");
		fcd_proc_kill(child, pipe_fds);
		return bytes_read;
	}

	if (close(fd) == -1) {
		FCD_PERROR("close");
		fcd_proc_kill(child, pipe_fds);
		return -1;
	}

	ret = fcd_proc_wait(status, pipe_fds, timeout);
	if (ret < 0) {
		fcd_proc_kill(child, pipe_fds);
		return ret;
	}

	if (!WIFEXITED(*status)) {
		FCD_WARN("Child process did not terminate normally\n");
		return -1;
	}

	*status = WEXITSTATUS(*status);

	return bytes_read;
}

int fcd_cmd_status(char **cmd, struct timespec *timeout,
		   const int *pipe_fds)
{
	int status, ret;
	pid_t child;

	if (fcd_cmd_spawn(&child, cmd, pipe_fds, 0) == -1)
		return -1;

	ret = fcd_proc_wait(&status, pipe_fds, timeout);
	if (ret < 0) {
		fcd_proc_kill(child, pipe_fds);
		return ret;
	}

	if (!WIFEXITED(status)) {
		FCD_WARN("Child process did not terminate normally\n");
		return -1;
	}

	return WEXITSTATUS(status);
}

#if 0
#include <stdlib.h>

int fcd_foreground = 1;

static void sigint_handler(int signum __attribute__((unused)))
{
	fcd_thread_exit_flag = 1;
}

int main(int argc __attribute__((unused)), char *argv[])
{
	struct timespec timeout;
	struct sigaction act;
	static char *buf;
	size_t buf_size;
	ssize_t ret;
	int fd;

	act.sa_handler = sigint_handler;
	if (sigemptyset(&act.sa_mask)) abort();
	act.sa_flags = 0;
	if (sigaction(SIGINT, &act, NULL)) abort();

	timeout.tv_sec = 10;
	timeout.tv_nsec = 0;

	if ((fd = open(argv[1], O_RDONLY | O_NONBLOCK)) == -1) abort();

	buf = NULL;
	buf_size = 0;
	ret = fcd_read_all(fd, &buf, &buf_size, 32000, &timeout);

	printf("fcd_read_all returned %zd\n", ret);

	return 0;
}
#endif
