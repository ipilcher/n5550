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

#define _GNU_SOURCE	/* for ppoll, pipe2, vsyslog, etc. */

#include <inttypes.h>
#include <pthread.h>
#include <syslog.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>

/*
 * Error reporting stuff
 */

/* "Private" functions and macros */

extern void fcd_err_msg(int priority, const char *format, ...);
extern void fcd_err_perror(const char *msg, const char *file, int line,
			   int sev);
extern void fcd_err_pt_err(const char *msg, int err, const char *file,
			   int line, int sev);

#define FCD_RAW_STRINGIFY(x)	#x
#define FCD_STRINGIFY(x)	FCD_RAW_STRINGIFY(x)

/* "Public" macros begin here */

#define FCD_ERR(...)		fcd_err_msg(LOG_ERR, "ERROR: " __FILE__ ":" \
					FCD_STRINGIFY(__LINE__) ": " \
					__VA_ARGS__)

#define FCD_WARN(...)		fcd_err_msg(LOG_WARNING, "WARNING: " __FILE__ \
					":" FCD_STRINGIFY(__LINE__) ": " \
					__VA_ARGS__)

#define FCD_INFO(...)		fcd_err_msg(LOG_INFO, "INFO: " __FILE__ ":" \
					FCD_STRINGIFY(__LINE__) ": " \
					__VA_ARGS__)

#define FCD_DEBUG(...)		fcd_err_msg(LOG_DEBUG, "DEBUG: " __FILE__ ":" \
					FCD_STRINGIFY(__LINE__) ": " \
					__VA_ARGS__)

#define FCD_FATAL(...)		do { \
					fcd_err_msg(LOG_ERR, "FATAL: " \
						__FILE__ ":" \
						FCD_STRINGIFY(__LINE__) ": " \
						__VA_ARGS__); \
					exit(1); \
				} while (0)

#define FCD_ABORT(...)		do { \
					fcd_err_msg(LOG_ERR, "FATAL: " \
						__FILE__ ":" \
						FCD_STRINGIFY(__LINE__) ": " \
						__VA_ARGS__); \
					abort(); \
				} while (0)

#define FCD_PERROR(msg)		fcd_err_perror((msg), __FILE__, __LINE__, 0)

#define FCD_PFATAL(msg)		do { \
					fcd_err_perror((msg), __FILE__, \
						__LINE__, 1); \
					exit(1); \
				} while (0)

#define FCD_PABORT(msg)		do { \
					fcd_err_perror((msg), __FILE__, \
						__LINE__, 2); \
					abort(); \
				} while (0)

#define FCD_PT_ERR(msg, err)	fcd_err_pt_err((msg), (err), __FILE__, \
					__LINE__, 0)

#define FCD_PT_FTL(msg, err)	do { \
					fcd_err_pt_err((msg), (err), __FILE__, \
						__LINE__, 1); \
					exit(1); \
				} while (0)

#define FCD_PT_ABRT(msg, err)	do { \
					fcd_err_pt_err((msg), (err), __FILE__, \
						__LINE__, 2); \
					abort(); \
				} while (0)

/*
 * Array size macro shamelessly copied from the Linux kernel
 */

#define FCD_BUILD_BUG_ON_ZERO(e)	(sizeof(struct { int:-!!(e); }))

#define FCD_MUST_BE_ARRAY(a)		FCD_BUILD_BUG_ON_ZERO( \
						__builtin_types_compatible_p( \
							typeof(a), \
							typeof(&a[0])))

#define FCD_ARRAY_SIZE(arr)		(sizeof(arr) / sizeof((arr)[0]) + \
						FCD_MUST_BE_ARRAY(arr))

/*
 * Data types
 */

/* Used to communicate warning/failure alerts between threads */
enum fcd_alert_msg {
	FCD_ALERT_CLR_ACK = 0,
	FCD_ALERT_SET_ACK,
	FCD_ALERT_CLR_REQ,
	FCD_ALERT_SET_REQ,
};

/* Every thread that monitors some aspect of the NAS has one of these */
struct fcd_monitor {
	pthread_mutex_t mutex;
	const char *name;
	void *(*monitor_fn)(void *);
	pthread_t tid;
	enum fcd_alert_msg sys_warn;
	enum fcd_alert_msg sys_fail;
	enum fcd_alert_msg disk_alerts[5];
	uint8_t buf[66];
};

/*
 * Global variables
 */

/* Detach from terminal?  Log to syslog or stderr? */
extern int fcd_err_foreground;

/* Set by SIGUSR1 handler in monitor/worker threads */
extern __thread volatile sig_atomic_t fcd_thread_exit_flag;

/* Signal mask for monitor thread calls to ppoll */
extern sigset_t fcd_mon_ppoll_sigmask;

/* Signal mask for reaper thread calls to ppoll */
extern sigset_t fcd_proc_ppoll_sigmask;

/* The monitors */
extern struct fcd_monitor fcd_loadavg_monitor;
extern struct fcd_monitor fcd_cputemp_monitor;
extern struct fcd_monitor fcd_sysfan_monitor;
extern struct fcd_monitor fcd_hddtemp_monitor;
extern struct fcd_monitor fcd_smart_monitor;
extern struct fcd_monitor fcd_raid_monitor;
extern struct fcd_monitor *fcd_monitors[];

/*
 * Non-static functions
 */

/* Alert stuff - alert.c */
extern void fcd_alert_update(enum fcd_alert_msg new,
			     enum fcd_alert_msg *status);
extern void fcd_alert_read_monitor(struct fcd_monitor *mon);
extern void fcd_alert_leds_close(void);
extern void fcd_alert_leds_open(void);

/* Serial port stuff  - tty.c */
extern int fcd_tty_open(const char *tty);
extern void fcd_tty_write_msg(int fd, struct fcd_monitor *mon);

/* LCD PIC stuff - pic.c */
extern void fcd_pic_setup_gpio(void);
extern void fcd_pic_reset(void);

/* Child process stuff - proc.c */
extern pid_t fcd_proc_fork(const int *pipe_fds);
extern int fcd_proc_kill(pid_t pid, const int *pipe_fds);
extern int fcd_proc_wait(int *status, const int *pipe_fds,
			 struct timespec *timeout);
extern int fcd_proc_close_pipe(const int *pipe_fds);
__attribute__((noreturn)) extern void *fcd_proc_fn(void *arg);

/* Utility functions - lib.c */
extern int fcd_lib_disk_presence(int *presence);
extern void fcd_lib_set_mon_status(struct fcd_monitor *mon, const char *buf,
				   int warn, int fail, const int *disks);
extern int fcd_lib_monitor_sleep(time_t seconds);
extern ssize_t fcd_lib_read(int fd, void *buf, size_t count,
			    struct timespec *timeout);
extern ssize_t fcd_lib_read_all(int fd, char **buf, size_t *buf_size,
				size_t max_size, struct timespec *timeout);
extern ssize_t fcd_lib_cmd_output(int *status, char **cmd, char **buf,
				  size_t *buf_size, size_t max_size,
				  struct timespec *timeout,
				  const int *pipe_fds);
extern int fcd_lib_cmd_status(char **cmd, struct timespec *timeout,
			      const int *pipe_fds);
__attribute__((noreturn))
extern void fcd_lib_disable_monitor(struct fcd_monitor *mon);
__attribute__((noreturn))
extern void fcd_lib_disable_cmd_mon(struct fcd_monitor *mon,
				    const int *pipe_fds, char *buf);

#endif	/* FREECUSD_H */
