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

#ifndef FREECUSD_H
#define FREECUSD_H

#include <pthread.h>
#include <inttypes.h>
#include <syslog.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/*
 * Command line flags
 */

extern int fcd_foreground;
extern int fcd_debug;
extern int fcd_thread_exit_flag;
extern pthread_mutex_t fcd_thread_exit_mutex;
extern pthread_cond_t fcd_thread_exit_cond;

/*
 * Error reporting stuff
 */

extern void fcd_err(int priority, const char *format, ...);

#define FCD_RAW_STRINGIFY(x)	#x
#define FCD_STRINGIFY(x)	FCD_RAW_STRINGIFY(x)

#define FCD_LOG(pri, ...)	fcd_err(pri, __FILE__":" \
					FCD_STRINGIFY(__LINE__) ": " \
					__VA_ARGS__)

#define FCD_ERR(...)		FCD_LOG(LOG_ERR, "ERROR: " __VA_ARGS__)
#define FCD_WARN(...)		FCD_LOG(LOG_WARNING, "WARNING: " __VA_ARGS__)
#define FCD_INFO(...)		FCD_LOG(LOG_INFO, "INFO: " __VA_ARGS__)
#define FCD_DEBUG(...)		FCD_LOG(LOG_DEBUG, "DEBUG: " __VA_ARGS__)

#define FCD_ABORT(...)		do { \
					FCD_LOG(LOG_ERR, \
						"FATAL: "__VA_ARGS__); \
					exit(EXIT_FAILURE); \
				} while (0)

/*
 * Array size macro slavishly copied from Linux kernel
 */

#define FCD_BUILD_BUG_ON_ZERO(e)	(sizeof(struct { int:-!!(e); }))

#define FCD_MUST_BE_ARRAY(a)		FCD_BUILD_BUG_ON_ZERO( \
						__builtin_types_compatible_p( \
							typeof(a), \
							typeof(&a[0])))

#define FCD_ARRAY_SIZE(arr)		(sizeof(arr) / sizeof((arr)[0]) + \
						FCD_MUST_BE_ARRAY(arr))

/* Used to communicate warning/failure alerts between threads */
enum fcd_alert {
	FCD_ALERT_CLR_ACK = 0,
	FCD_ALERT_SET_ACK,
	FCD_ALERT_CLR,
	FCD_ALERT_SET,
};

struct fcd_monitor {
	pthread_mutex_t mutex;
	char *name;
	void *(*monitor_fn)(void *);
	pthread_t tid;
	enum fcd_alert sys_warn;
	enum fcd_alert sys_fail;
	enum fcd_alert disk_alerts[5];
	uint8_t buf[66];
};

extern struct fcd_monitor fcd_loadavg_monitor;
extern struct fcd_monitor fcd_cputemp_monitor;
extern struct fcd_monitor fcd_sysfan_monitor;
extern struct fcd_monitor fcd_hddtemp_monitor;
extern struct fcd_monitor fcd_smart_monitor;

/* Called in monitor threads to update alert status; monitor mutex locked. */
extern void update_alert(enum fcd_alert new, enum fcd_alert *status);

/*
 * Serial port stuff
 */

extern int fcd_open_tty(const char *tty);
extern void fcd_write_msg(int fd, struct fcd_monitor *mon);

/*
 * LCD PIC stuff
 */

extern void fcd_setup_pic_gpio(void);
extern void fcd_reset_pic(void);

/*
 * Thread stuff
 */

extern FILE *fcd_cmd_spawn(pid_t *child, char **cmd);
extern int fcd_cmd_cleanup(FILE *fp, pid_t child);
extern int fcd_update_disk_presence(int *presence);
extern void fcd_copy_buf(const char *buf, struct fcd_monitor *mon);
extern int fcd_sleep_and_check_exit(time_t seconds);

__attribute__((noreturn))
extern void fcd_disable_monitor(struct fcd_monitor *mon);

#endif	/* FREECUSD_H */
