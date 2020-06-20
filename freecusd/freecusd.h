/*
 * Copyright 2013-2014, 2016-2017, 2020 Ian Pilcher <arequipeno@gmail.com>
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
#include <stdbool.h>
#include <pthread.h>
#include <syslog.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>

#include <libcip.h>


/*
 * Error reporting stuff
 */

/* "Private" functions and macros */

extern void fcd_err_msg(int priority, const char *format, ...);
extern void fcd_err_perror(const char *msg, const char *file, int line,
			   int sev);
extern void fcd_err_pt_err(const char *msg, int err, const char *file,
			   int line, int sev);
__attribute__((noreturn))
extern void fcd_err_child_pabort(const char *msg, const char *file, int line);


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

#define FCD_CHILD_PABORT(msg)	fcd_err_child_pabort((msg), __FILE__, __LINE__)

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
 * Data types & constants
 */

/* Used to set the size of various structures & buffers */
#define FCD_DISK_NAME_SIZE             (sizeof "/dev/sd_")
#define FCD_DISK_DEV_SIZE	       (sizeof "sd_")
#define FCD_MAX_DISK_COUNT             5

/* Monitor PWM flags */
#define FCD_FAN_HI_HYST		0x01	/* above fan high hysteresis threshold */
#define FCD_FAN_HI_ON		0x02	/* at or above fan high on threshold */
#define FCD_FAN_MAX_HYST	0x04	/* above fan max hysteresis threshold */
#define FCD_FAN_MAX_ON		0x08	/* at or above fan max on threshold */

/* Fan PWM states */
enum fcd_pwm_state {
	FCD_PWM_NORMAL = 0,
	FCD_PWM_HIGH,
	FCD_PWM_MAX
};

/* String representations of the PWM states */
extern const char *const fcd_pwm_state_names[FCD_PWM_MAX + 1];

/* Parsed PWM value */
struct fcd_pwm_value {
	size_t	len;		/* strlen(s) */
	int	value;		/* 0 - 255 */
	char	s[4];		/* value as a string */
};

/* Used to communicate warning/failure alerts between threads */
enum fcd_alert_msg {
	FCD_ALERT_CLR_ACK = 0,
	FCD_ALERT_SET_ACK,
	FCD_ALERT_CLR_REQ,
	FCD_ALERT_SET_REQ,
};

/*
 * Data about a "monitor" - which monitors, displays, and/or controls some
 * aspect of the NAS.  Most monitors run as a separate thread, but a single
 * thread can manage multiple monitors.  (For example, the HDD temperature
 * monitor runs in the SMART monitor thread.)  A monitor may also lack a
 * dedicated thread if it is completely static (the logo "monitor") or
 * reactive (the PWM monitor).
 *
 * The SYNCHRONIZED members of the structure are updated by the monitor threads
 * and processed by the "main" thread, which updates the NAS's front-panel LCD
 * display and alert LEDs and controls the fan speed.  All access to the
 * SYNCHRONIZED members requires locking the monitor's mutex.  (This is true
 * even of the static "logo monitor".
 */
struct fcd_monitor {
	pthread_mutex_t mutex;
	const char *name;
	char *enabled_opt_name;
	const cip_opt_info *freecusd_opts;
	const cip_opt_info *raiddisk_opts;
	void *(*monitor_fn)(void *);
	pthread_t tid;
	bool enabled;
	enum fcd_alert_msg sys_warn;				/* SYNCHRONIZED */
	enum fcd_alert_msg sys_fail;				/* SYNCHRONIZED */
	enum fcd_alert_msg disk_alerts[FCD_MAX_DISK_COUNT];	/* SYNCHRONIZED */
	uint8_t buf[66];					/* SYNCHRONIZED */
};

/* Config info about a RAID disk */
struct fcd_raid_disk {
	unsigned port_no;
	int temp_warn;
	int temp_crit;
	bool temp_ignore;
	bool smart_ignore;
	char name[FCD_DISK_NAME_SIZE];
};

/*
 * Global variables
 */

/* Configuration file name */
extern const char *fcd_conf_file_name;

/* Detach from terminal?  Log to syslog or stderr? */
extern _Bool fcd_err_foreground;

/* Log/print debugging messages? */
extern _Bool fcd_err_debug;

/* File descriptor used to log errors in fork()ed child (before exec) */
extern int fcd_err_child_errfd;

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

/* Number and names of disks to monitor */
extern unsigned fcd_conf_disk_count;
extern struct fcd_raid_disk fcd_conf_disks[FCD_MAX_DISK_COUNT];

/*
 * Given a pointer to a member of fcd_conf_disks[0], returns a pointer to the
 * corresponding member of fcd_conf_disks[idx].
 */
__attribute__((always_inline))
static inline void *fcd_conf_disk_member(unsigned char *member, unsigned idx)
{
	return member + (idx * sizeof(struct fcd_raid_disk));
}

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
extern void fcd_lib_disable_slave(struct fcd_monitor *mon);
__attribute__((noreturn))
extern void fcd_lib_disable_cmd_mon(struct fcd_monitor *mon,
				    const int *pipe_fds, char *buf);
extern int fcd_lib_disk_index(char c);
extern void fcd_lib_disk_mutex_lock(void);
extern void fcd_lib_disk_mutex_unlock(void);

/* Config file parsing - conf.c */
extern void fcd_conf_parse(void);
extern int fcd_conf_disk_bool_cb(cip_err_ctx *ctx, const cip_ini_value *value,
				 const cip_ini_sect *sect,
				 const cip_ini_file *file,
				 void *post_parse_data);
extern int fcd_conf_disk_int_cb_help(cip_err_ctx *ctx,
				     const cip_ini_value *value,
				     const cip_ini_sect *sect,
				     const cip_ini_file *file,
				     void *post_parse_data, int *result);

/* RAID disk auto-detection - disk.c */
extern int fcd_disk_detect(void);


#endif	/* FREECUSD_H */
