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

#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <poll.h>

struct fcd_proc_child {
	pid_t child;
	int pipe_fds[2];
};

sigset_t fcd_proc_ppoll_sigmask;

static pthread_mutex_t fcd_proc_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Currently the hddtemp, smart, and raid monitors spawn children */
static struct fcd_proc_child fcd_proc_children[3] = {
	{ .child = -1 },
	{ .child = -1 },
	{ .child = -1 },
};

/*******************************************************************************
 *
 * Functions called in other threads
 *
 ******************************************************************************/

pid_t fcd_proc_fork(const int *pipe_fds)
{
	pid_t child;
	size_t i;
	int ret;

	ret = pthread_mutex_lock(&fcd_proc_mutex);
	if (ret != 0) {
		FCD_PT_ERR("pthread_mutex_lock", ret);
		return -1;
	}

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_proc_children); ++i) {
		if (fcd_proc_children[i].child == -1)
			break;
	}

	if (i >= FCD_ARRAY_SIZE(fcd_proc_children)) {
		FCD_WARN("No free slot in child array\n");
		child = -1;
		goto unlock_mutex;
	}

	child = fork();
	if (child == -1) {
		FCD_PERROR("fork");
		child = -1;
		goto unlock_mutex;
	}

	if (child == 0)
		return 0;

	fcd_proc_children[i].child = child;
	memcpy(fcd_proc_children[i].pipe_fds, pipe_fds,
	       sizeof fcd_proc_children[i].pipe_fds);

unlock_mutex:

	ret = pthread_mutex_unlock(&fcd_proc_mutex);
	if (ret == -1)
		FCD_PT_ABRT("pthread_mutex_unlock", ret);

	return child;
}

int fcd_proc_wait(int *status, const int *pipe_fds, struct timespec *timeout)
{
	ssize_t ret;

	ret = fcd_read(pipe_fds[0], status, sizeof *status, timeout);
	if (ret < 0)
		return ret;

	if (ret != (ssize_t)sizeof *status) {
		FCD_ERR("Incomplete read (%zd bytes)\n", ret);
		return -1;
	}

	return 0;
}

int fcd_proc_kill(pid_t pid, const int *pipe_fds)
{
	int result, ret;
	size_t i;

	ret = pthread_mutex_lock(&fcd_proc_mutex);
	if (ret != 0) {
		FCD_PT_ERR("pthread_mutex_lock", ret);
		return -1;
	}

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_proc_children); ++i) {

		if (fcd_proc_children[i].child == pid) {

			fcd_proc_children[i].child = -1;
			break;
		}
	}

	if (i >= FCD_ARRAY_SIZE(fcd_proc_children)) {
		result = 1;		/* Child already exited & reaped */
		goto unlock_mutex;
	}

	if (kill(pid, SIGKILL) == -1) {
		FCD_PERROR("kill");
		result = -1;
		goto unlock_mutex;
	}

	result = 0;

unlock_mutex:

	ret = pthread_mutex_unlock(&fcd_proc_mutex);
	if (ret != 0)
		FCD_PT_ABRT("pthread_mutex_unlock", ret);

	if (result == 1) {

		/* Child has been reaped, so clear its status from the pipe */

		ret = read(pipe_fds[0], &result, sizeof result);
		if (ret == -1) {
			FCD_PERROR("read");
			return -1;
		}

		if (ret != (int)sizeof result) {
			FCD_ERR("Incomplete read (%d bytes)\n", ret);
			return -1;
		}

		return 0;
	}

	return result;
}

int fcd_proc_close_pipe(const int *pipe_fds)
{
	int ret;

	ret = close(pipe_fds[0]);
	if (ret == -1)
		FCD_PERROR("close");

	if (close(pipe_fds[1]) == -1) {
		FCD_PERROR("close");
		return -1;
	}

	return ret;
}

/*******************************************************************************
 *
 * The actual reaper thread
 *
 ******************************************************************************/

static void fcd_proc_send(pid_t pid, int status)
{
	size_t i;
	int ret;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_proc_children); ++i) {

		if (fcd_proc_children[i].child == pid) {

			fcd_proc_children[i].child = -1;
			break;
		}
	}

	if (i >= FCD_ARRAY_SIZE(fcd_proc_children)) {

		/*
		 * This can happen if the following sequence occurs:
		 *
		 * 	1. fcd_proc_wait (in monitor thread) times out
		 * 	2. Monitor thread calls fcd_proc_kill, which acquires
		 * 		fcd_proc_mutex
		 * 	3. Child process terminates; reaper thread is woken up
		 * 		by SIGCHLD
		 * 	4. Reaper thread blocks, waiting to acquire the mutex
		 * 	5. fcd_proc_kill (in monitor thread) removes the child
		 * 		from fcd_proc_children
		 * 	6. Monitor thread releases the mutex
		 * 	7. Reaper thread cannot find the child in
		 * 		fcd_proc_children
		 */

		FCD_WARN("PID %lu not found in child array\n",
			 (unsigned long)pid);
		return;
	}

	ret = write(fcd_proc_children[i].pipe_fds[1], &status, sizeof status);
	if (ret == -1)
		FCD_PABORT("write");

	if (ret != (int)sizeof status)
		FCD_ABORT("Incomplete write (%d bytes)\n", ret);
}

__attribute__((noreturn))
void *fcd_proc_fn(void *arg __attribute__((unused)))
{
	int status, ret;
	pid_t pid;

	while (!fcd_thread_exit_flag) {

		/* Wait to be interrupted by a signal (SIGCHLD or SIGUSR1) */
		ret = ppoll(NULL, 0, NULL, &fcd_proc_ppoll_sigmask);
		if (ret != -1)
			FCD_ABORT("Unexpected ppoll return value: %d\n", ret);
		if (errno != EINTR)
			FCD_PABORT("ppoll");

		ret = pthread_mutex_lock(&fcd_proc_mutex);
		if (ret != 0)
			FCD_PT_ABRT("pthread_mutex_lock", ret);

		do {
			pid = waitpid(-1, &status, WNOHANG);
			if (pid == -1) {
				if (errno == ECHILD)
					pid = 0;
				else
					FCD_PABORT("waitpid");
			}

			if (pid > 0)
				fcd_proc_send(pid, status);

		} while (pid != 0);

		ret = pthread_mutex_unlock(&fcd_proc_mutex);
		if (ret != 0)
			FCD_PT_ABRT("pthread_mutex_unlock", ret);
	}

	pthread_exit(NULL);
}

/*******************************************************************************
 *
 * Testing stuff
 *
 ******************************************************************************/

#if 0
#include <fcntl.h>

__thread volatile sig_atomic_t fcd_thread_exit_flag = 0;
int fcd_foreground = 1;
sigset_t fcd_reaper_ppoll_sigmask;

static void sigchld_handler(int signum __attribute__((unused)))
{
	/* Nothing to see here */
}

int main(int argc __attribute__((unused)), char *argv[])
{
	pthread_t reaper_thread;
	int status, ret, fds[2];
	struct sigaction sa;
	struct pollfd pfd;
	sigset_t mask;
	pid_t child;

	if (sigemptyset(&sa.sa_mask) == -1)
		FCD_ABORT("sigemptyset: %m\n");
	if (sigemptyset(&fcd_reaper_ppoll_sigmask) == -1)
		FCD_ABORT("sigemptyset: %m\n");
	if (sigemptyset(&mask) == -1)
		FCD_ABORT("sigemptyset: %m\n");

	sa.sa_handler = sigchld_handler;
	sa.sa_flags = 0;

	if (sigaction(SIGCHLD, &sa, NULL) == -1)
		FCD_ABORT("sigaction: %m\n");

	if (sigaddset(&mask, SIGCHLD) == -1)
		FCD_ABORT("sigaddset: %m\n");

	ret = pthread_sigmask(SIG_BLOCK, &mask, &fcd_reaper_ppoll_sigmask);
	if (ret != 0)
		FCD_ABORT("pthread_sigmask: %s\n", strerror(ret));

	ret = pthread_create(&reaper_thread, NULL, fcd_reaper_fn, NULL);
	if (ret != 0)
		FCD_ABORT("pthread_create: %s\n", strerror(ret));

	if (pipe2(fds, O_CLOEXEC) == -1)
		FCD_ABORT("pipe2: %m\n");

	child = fcd_reaper_fork(fds);
	if (child == -1)
		FCD_ABORT("Failed to spawn child process\n");

	if (child == 0) {
		execv(argv[1], argv + 2);
		FCD_ABORT("execv: %m\n");
	}

	pfd.fd = fds[0];
	pfd.events = POLLIN;

	ret = poll(&pfd, 1, 2000);
	if (ret == -1)
		FCD_ABORT("poll: %m\n");

	if (ret == 0) {
		puts("Timed out; killing child");
		kill(child, SIGKILL);
	}
	else {
		if (read(fds[0], &status, sizeof status) == -1)
			FCD_ABORT("read: %m\n");
		if (!WIFEXITED(status))
			FCD_ABORT("Child process did not terminate normally\n");
		printf("Child exit code = %d\n", WEXITSTATUS(status));
	}

	return WEXITSTATUS(status);
}
#endif
