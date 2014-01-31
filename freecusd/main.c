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

#include <sys/resource.h>
#include <stdarg.h>
#include <locale.h>
#include <string.h>
#include <errno.h>

static struct fcd_monitor fcd_main_logo = {
	.monitor_fn	= 0,
	.buf		= "....."
			  "FreeCUS             "
			  "                    "
			  "Free Your NAS!      ",
};

struct fcd_monitor *fcd_monitors[] = {
	&fcd_main_logo,
	&fcd_loadavg_monitor,
	&fcd_cputemp_monitor,
	&fcd_sysfan_monitor,
	&fcd_hddtemp_monitor,
	&fcd_smart_monitor,
	&fcd_raid_monitor,
	NULL
};

static volatile sig_atomic_t fcd_main_got_exit_signal = 0;

/*
 * See https://sourceware.org/ml/libc-alpha/2012-06/msg00335.html for a
 * discussion of accessing thread-local variables in signal handlers.
 */
__thread volatile sig_atomic_t fcd_thread_exit_flag = 0;

static const struct timespec fcd_main_sleep = {
	.tv_sec		= 3,
	.tv_nsec	= 0,
};

static void fcd_main_sig_handler(int signum)
{
	/*
	 * Trigger a core dump with a second ctrl-C when running in foreground
	 * mode.
	 */

	if (signum == SIGINT && fcd_main_got_exit_signal && fcd_err_foreground)
		abort();

	if (signum == SIGINT || signum == SIGTERM)
		fcd_main_got_exit_signal = 1;

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

static void fcd_main_parse_args(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; ++i) {

		if (strcmp("-f", argv[i]) == 0) {
			fcd_err_foreground = 1;
		}
		else {
			FCD_WARN("Unknown option: '%s'\n", argv[i]);
		}
	}
}

static void fcd_main_start_mon_threads(void)
{
	struct fcd_monitor *mon, **m;
	int ret;

	for (m = fcd_monitors; mon = *m, mon != NULL; ++m) {

		if (mon->monitor_fn != 0) {

			ret = pthread_create(&mon->tid, NULL,
					     mon->monitor_fn, mon);
			if (ret != 0)
				FCD_PT_ABRT("pthread_create", ret);
		}
	}
}

static void fcd_main_stop_thread(pthread_t thread)
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

static void fcd_main_stop_mon_threads(void)
{
	struct fcd_monitor **mon;

	for (mon = fcd_monitors; *mon != NULL; ++mon) {

		if ((*mon)->monitor_fn != 0)
			fcd_main_stop_thread((*mon)->tid);
	}
}

static void fcd_main_sigmask(sigset_t *mask, ...)
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

static void fcd_main_set_sig_handler(void)
{
	struct sigaction sa;

	sa.sa_handler = fcd_main_sig_handler;
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
	struct fcd_monitor **mon;
	pthread_t reaper_thread;
	int tty_fd, ret;

	fcd_main_parse_args(argc, argv);
	if (fcd_err_foreground) {
		fcd_main_enable_coredump();
	}
	else {
		openlog("freecusd", LOG_PID, LOG_DAEMON);
		if (daemon(0, 0) == -1)
			FCD_PABORT("daemon");
	}

	setlocale(LC_NUMERIC, "");

	fcd_main_sigmask(&worker_sigmask, SIGINT, SIGTERM, SIGCHLD, SIGUSR1, 0);
	fcd_main_sigmask(&main_sigmask, -SIGINT, -SIGTERM, SIGCHLD, SIGUSR1, 0);
	fcd_main_sigmask(&fcd_mon_ppoll_sigmask,
			 SIGINT, SIGTERM, SIGCHLD, -SIGUSR1, 0);
	fcd_main_sigmask(&fcd_proc_ppoll_sigmask,
			 SIGINT, SIGTERM, -SIGCHLD, -SIGUSR1, 0);

	ret = pthread_sigmask(SIG_SETMASK, &worker_sigmask, NULL);
	if (ret != 0)
		FCD_PT_ABRT("pthread_sigmask", ret);

	fcd_main_set_sig_handler();

	ret = pthread_create(&reaper_thread, NULL, fcd_proc_fn, NULL);
	if (ret != 0)
		FCD_PT_ABRT("pthread_create", ret);

	fcd_main_start_mon_threads();

	ret = pthread_sigmask(SIG_SETMASK, &main_sigmask, NULL);
	if (ret != 0)
		FCD_PT_ABRT("pthread_sigmask", ret);

	fcd_pic_setup_gpio();
	fcd_pic_reset();
	tty_fd = fcd_tty_open("/dev/ttyS0");
	fcd_alert_leds_open();

	while (!fcd_main_got_exit_signal) {

		for (mon = fcd_monitors; *mon != NULL; ++mon) {

			fcd_main_read_monitor(tty_fd, *mon);

			ret = nanosleep(&fcd_main_sleep, NULL);
			if (ret == -1 && errno != EINTR)
				FCD_PABORT("nanosleep");
			if (fcd_main_got_exit_signal)
				break;
		}
	}

	fcd_alert_leds_close();
	if (close(tty_fd) == -1)
		FCD_PERROR("close");

	fcd_main_stop_mon_threads();
	fcd_main_stop_thread(reaper_thread);

	FCD_INFO("Exiting\n");
	return 0;
}
