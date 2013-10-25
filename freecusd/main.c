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

#include <sys/resource.h>
#include <stdarg.h>
#include <locale.h>
#include <string.h>
#include <errno.h>

int fcd_foreground = 0;

static struct fcd_monitor fcd_freecus_logo = {
	.monitor_fn	= 0,
	.buf		= "....."
			  "FreeCUS             "
			  "                    "
			  "Free Your NAS!      ",
};

static struct fcd_monitor *fcd_monitors[] = {
	&fcd_freecus_logo,
	&fcd_loadavg_monitor,
	&fcd_cputemp_monitor,
	&fcd_sysfan_monitor,
	&fcd_hddtemp_monitor,
	&fcd_smart_monitor,
	&fcd_raid_monitor,
};

static volatile sig_atomic_t fcd_got_exit_signal = 0;

/*
 * See https://sourceware.org/ml/libc-alpha/2012-06/msg00335.html for a
 * discussion of accessing thread-local variables in signal handlers.
 */
__thread volatile sig_atomic_t fcd_thread_exit_flag = 0;

static const struct timespec fcd_main_sleep = {
	.tv_sec		= 3,
	.tv_nsec	= 0,
};

static void fcd_sig_handler(int signum)
{
	/*
	 * Trigger a core dump with a second ctrl-C when running in foreground
	 * mode.
	 */

	if (signum == SIGINT && fcd_got_exit_signal && fcd_foreground)
		abort();

	if (signum == SIGINT || signum == SIGTERM)
		fcd_got_exit_signal = 1;

	if (signum == SIGUSR1)
		fcd_thread_exit_flag = 1;
}

static void fcd_main_enable_coredump(void)
{
	static const struct rlimit unlimited = { RLIM_INFINITY, RLIM_INFINITY};

	if (setrlimit(RLIMIT_CORE, &unlimited) == -1) {
		FCD_PERROR("setrlimit");
		FCD_WARN("Failed to enable core dumps\n");
	}
	else {
		FCD_INFO("Enabled core dumps\n");
	}
}

static void fcd_parse_args(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; ++i) {

		if (strcmp("-f", argv[i]) == 0) {
			fcd_foreground = 1;
			fcd_main_enable_coredump();
		}
		else {
			FCD_WARN("Unknown option: '%s'\n", argv[i]);
		}
	}
}

static void fcd_start_monitor_threads(void)
{
	struct fcd_monitor *mon;
	size_t i;
	int ret;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_monitors); ++i) {

		mon = fcd_monitors[i];

		if (mon->monitor_fn != 0) {

			ret = pthread_create(&mon->tid, NULL,
					     mon->monitor_fn, mon);
			if (ret != 0)
				FCD_PT_ABRT("pthread_create", ret);
		}
	}
}

static void fcd_stop_thread(pthread_t thread)
{
	int ret;

	ret = pthread_kill(thread, SIGUSR1);
	if (ret != 0 && ret != ESRCH) {
		FCD_PT_ERR("pthread_kill", ret);
	}
	else {
		ret = pthread_join(thread, NULL);
		if (ret != 0)
			FCD_PT_ERR("pthread_join", ret);
	}
}

static void fcd_stop_monitor_threads(void)
{
	size_t i;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_monitors); ++i) {

		if (fcd_monitors[i]->monitor_fn != 0)
			fcd_stop_thread(fcd_monitors[i]->tid);
	}
}

static void fcd_sigmask(sigset_t *mask, ...)
{
	va_list ap;
	int i;

	if (sigemptyset(mask) == -1)
		FCD_PABORT("sigemptyset");

	i = pthread_sigmask(0, NULL, mask);
	if (i != 0)
		FCD_PT_ABRT("pthread_sigmask", i);

	va_start(ap, mask);

	while (1) {

		i = va_arg(ap, int);
		if (i == 0)
			break;

		if (i > 0) {
			if (sigaddset(mask, i) == -1)
				FCD_PABORT("sigaddset");
		}
		else {
			if (sigdelset(mask, -i) == -1)
				FCD_PABORT("sigdelset");
		}
	}

	va_end(ap);
}

static void fcd_set_sig_handler(void)
{
	struct sigaction sa;

	sa.sa_handler = fcd_sig_handler;
	sa.sa_flags = 0;
	if (sigemptyset(&sa.sa_mask) == -1)
		FCD_PABORT("sigemptyset");

	if (sigaction(SIGINT, &sa, NULL) == -1)
		FCD_PABORT("sigaction");
	if (sigaction(SIGTERM, &sa, NULL) == -1)
		FCD_PABORT("sigaction");
	if (sigaction(SIGUSR1, &sa, NULL) == -1)
		FCD_PABORT("sigaction");
	if (sigaction(SIGCHLD, &sa, NULL) == -1)
		FCD_PABORT("sigaction");
}

static void fcd_main_read_monitor(int tty_fd, struct fcd_monitor *mon)
{
	int ret;

	if (mon->monitor_fn != 0) {
		ret = pthread_mutex_lock(&mon->mutex);
		if (ret != 0)
			FCD_PT_ABRT("pthread_mutex_lock", ret);
	}

	fcd_tty_write_msg(tty_fd, mon);

	if (mon->monitor_fn != 0) {

		fcd_alert_read_monitor(mon);

		ret = pthread_mutex_unlock(&mon->mutex);
		if (ret != 0)
			FCD_PT_ABRT("pthread_mutex_unlock", ret);
	}
}

int main(int argc, char *argv[])
{
	sigset_t worker_sigmask, main_sigmask;
	pthread_t reaper_thread;
	int tty_fd, ret;
	size_t i;

	openlog("freecusd", LOG_PID, LOG_DAEMON);
	fcd_parse_args(argc, argv);
	setlocale(LC_NUMERIC, "");

	fcd_sigmask(&worker_sigmask, SIGINT, SIGTERM, SIGCHLD, SIGUSR1, 0);
	fcd_sigmask(&main_sigmask, -SIGINT, -SIGTERM, SIGCHLD, SIGUSR1, 0);
	fcd_sigmask(&fcd_mon_ppoll_sigmask,
		    SIGINT, SIGTERM, SIGCHLD, -SIGUSR1, 0);
	fcd_sigmask(&fcd_proc_ppoll_sigmask,
		    SIGINT, SIGTERM, -SIGCHLD, -SIGUSR1, 0);

	ret = pthread_sigmask(SIG_SETMASK, &worker_sigmask, NULL);
	if (ret != 0)
		FCD_PT_ABRT("pthread_sigmask", ret);

	fcd_set_sig_handler();

	ret = pthread_create(&reaper_thread, NULL, fcd_proc_fn, NULL);
	if (ret != 0)
		FCD_PT_ABRT("pthread_create", ret);

	fcd_start_monitor_threads();

	ret = pthread_sigmask(SIG_SETMASK, &main_sigmask, NULL);
	if (ret != 0)
		FCD_PT_ABRT("pthread_sigmask", ret);

	fcd_pic_setup_gpio();
	fcd_pic_reset();
	tty_fd = fcd_tty_open("/dev/ttyS0");
	fcd_alert_leds_open();

	while (!fcd_got_exit_signal) {

		for (i = 0; i < FCD_ARRAY_SIZE(fcd_monitors); ++i) {

			fcd_main_read_monitor(tty_fd, fcd_monitors[i]);

			ret = nanosleep(&fcd_main_sleep, NULL);
			if (ret == -1 && errno != EINTR)
				FCD_PABORT("nanosleep");
			if (fcd_got_exit_signal)
				break;
		}
	}

	fcd_alert_leds_close();
	if (close(tty_fd) == -1)
		FCD_PERROR("close");

	fcd_stop_monitor_threads();
	fcd_stop_thread(reaper_thread);

	FCD_INFO("Exiting\n");
	return 0;
}

#if 0
#include <fcntl.h>

int main(int argc __attribute__((unused)), char *argv[])
{
	struct timespec timeout = { 5, 0 };
	pthread_t reaper_thread;
	sigset_t oldmask;
	char *buf = NULL;
	size_t buf_size = 0;
	int fds[2], ret, status;

	fcd_foreground = 1;

	fcd_set_sigmask(&oldmask);
	fcd_init_ppoll_sigmasks();
	fcd_set_sig_handler();

	ret = pthread_create(&reaper_thread, NULL, fcd_reaper_fn, NULL);
	if (ret != 0)
		FCD_ABORT("pthread_create: %s\n", strerror(ret));

	ret = pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
	if (ret != 0)
		FCD_ABORT("pthread_sigmask: %s\n", strerror(ret));

	if (pipe2(fds, O_CLOEXEC) == -1)
		FCD_ABORT("pipe2: %m\n");

	ret = fcd_cmd_output(&status, argv + 1, &buf, &buf_size,
			     32000, &timeout, fds);

	printf("fcd_cmd_exec returned %d\n", ret);
	if (ret >= 0) {
		if (WIFEXITED(status))
			printf("Child exit code: %d\n", WEXITSTATUS(status));
		puts(buf);
	}

	fcd_stop_thread(reaper_thread);

	return 0;
}
#endif
#if 0
extern void fcd_raid_test(void);

int main(int argc, char *argv[])
{
	sigset_t worker_sigmask, main_sigmask;
	pthread_t reaper_thread;
	int ret;

	openlog("freecusd", LOG_PID, LOG_DAEMON);
	fcd_parse_args(argc, argv);
	setlocale(LC_NUMERIC, "");

	fcd_sigmask(&worker_sigmask, SIGINT, SIGTERM, SIGCHLD, SIGUSR1, 0);
	fcd_sigmask(&main_sigmask, -SIGINT, -SIGTERM, SIGCHLD, SIGUSR1, 0);
	fcd_sigmask(&fcd_monitor_ppoll_sigmask,
		    SIGINT, SIGTERM, SIGCHLD, -SIGUSR1, 0);
	fcd_sigmask(&fcd_reaper_ppoll_sigmask,
		    SIGINT, SIGTERM, -SIGCHLD, -SIGUSR1, 0);

	ret = pthread_sigmask(SIG_SETMASK, &worker_sigmask, NULL);
	if (ret != 0)
		FCD_ABORT("pthread_sigmask: %s\n", strerror(ret));

	fcd_set_sig_handler();

	ret = pthread_create(&reaper_thread, NULL, fcd_reaper_fn, NULL);
	if (ret != 0)
		FCD_ABORT("pthread_create: %s\n", strerror(ret));

	ret = pthread_sigmask(SIG_SETMASK, &main_sigmask, NULL);
	if (ret != 0)
		FCD_ABORT("pthread_sigmask: %s\n", strerror(ret));

	fcd_raid_test();

	fcd_stop_thread(reaper_thread);

	FCD_INFO("Exiting\n");
	return 0;
}
#endif
