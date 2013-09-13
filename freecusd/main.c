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

#include <signal.h>
#include <string.h>
#include <locale.h>
#include <errno.h>

#include "freecusd.h"

int fcd_foreground = 0;
int fcd_debug = 0;

int fcd_thread_exit_flag = 0;
pthread_mutex_t fcd_thread_exit_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t fcd_thread_exit_cond = PTHREAD_COND_INITIALIZER;

static void fcd_sig_handler(int sig);

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
	NULL
};

static volatile sig_atomic_t fcd_got_exit_signal = 0;

static struct sigaction fcd_sigaction = {
	.sa_handler 	= fcd_sig_handler,
};

static const struct timespec fcd_main_sleep = {
	.tv_sec		= 5,
	.tv_nsec	= 0,
};

static void fcd_sig_handler(int sig __attribute__((unused)))
{
	fcd_got_exit_signal = 1;
}

static void fcd_parse_args(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; ++i)
	{
		if (strcmp("-f", argv[i]) == 0)
			fcd_foreground = 1;
		else if (strcmp("-d", argv[i]) == 0)
			fcd_debug = 1;
		else
			FCD_WARN("Unknown option: '%s'\n", argv[i]);
	}
}

static void fcd_start_monitor_threads()
{
	struct fcd_monitor *mon;
	int i, ret;

	for (i = 0; mon = fcd_monitors[i], mon != NULL; ++i)
	{
		if (mon->monitor_fn != 0)
		{
			ret = pthread_create(&mon->tid, NULL,
					     mon->monitor_fn, mon);
			if (ret != 0) {
				FCD_ABORT("pthread_create: %s\n",
					  strerror(ret));
			}
		}
	}
}

static void fcd_join_monitor_threads()
{
	struct fcd_monitor *mon;
	int i, ret;

	for (i = 0; mon = fcd_monitors[i], mon != NULL; ++i)
	{
		if (mon->monitor_fn != 0)
		{
			ret = pthread_join(mon->tid, NULL);
			if (ret != 0)
				FCD_ABORT("pthread_join: %s\n", strerror(ret));
		}
	}
}

static void fcd_sigset_init(sigset_t *set)
{
	if (sigemptyset(set) == -1)
		FCD_ABORT("sigemptyset");
	if (sigaddset(set, SIGINT) == -1)
		FCD_ABORT("sigaddset(SIGINT)");
	if (sigaddset(set, SIGTERM) == -1)
		FCD_ABORT("sigaddset(SIGTERM)");
}

int main(int argc, char *argv[])
{
	struct fcd_monitor *mon;
	int i, tty_fd, ret;
	sigset_t signals;

	openlog("freecusd", LOG_PID, LOG_DAEMON);
	fcd_parse_args(argc, argv);
	setlocale(LC_NUMERIC, "");
	fcd_sigset_init(&signals);

	ret = pthread_sigmask(SIG_BLOCK, &signals, NULL);
	if (ret != 0)
		FCD_ABORT("pthread_sigmask: %s\n", strerror(ret));

	if (sigaction(SIGINT, &fcd_sigaction, NULL) == -1)
		FCD_ABORT("sigaction(SIGINT)");
	if (sigaction(SIGTERM, &fcd_sigaction, NULL) == -1)
		FCD_ABORT("sigaction(SIGTERM)");

	fcd_start_monitor_threads();

	ret = pthread_sigmask(SIG_UNBLOCK, &signals, NULL);
	if (ret != 0)
		FCD_ABORT("pthread_sigmask: %s\n", strerror(ret));

	fcd_setup_pic_gpio();
	fcd_reset_pic();
	tty_fd = fcd_open_tty("/dev/ttyS0");

	while (!fcd_got_exit_signal)
	{
		for (i = 0; mon = fcd_monitors[i], mon != NULL; ++i) {
			fcd_write_msg(tty_fd, mon);
			ret = nanosleep(&fcd_main_sleep, NULL);
			if (ret == -1 && errno != EINTR)
				FCD_ABORT("nanosleep: %m\n");
			if (fcd_got_exit_signal)
				break;
		}
	}

	if (close(tty_fd) == -1)
		FCD_ERR("close: %m\n");

	ret = pthread_mutex_lock(&fcd_thread_exit_mutex);
	if (ret != 0)
		FCD_ABORT("pthread_mutex_lock: %s\n", strerror(ret));

	fcd_thread_exit_flag = 1;

	ret = pthread_cond_broadcast(&fcd_thread_exit_cond);
	if (ret != 0)
		FCD_ABORT("pthread_cond_broadcast: %s\n", strerror(ret));

	ret = pthread_mutex_unlock(&fcd_thread_exit_mutex);
	if (ret != 0)
		FCD_ABORT("pthread_mutex_unlock: %s\n", strerror(ret));

	fcd_join_monitor_threads();

	FCD_INFO("Exiting\n");
	return 0;
}
