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

#include <string.h>
#include <regex.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>

#include "freecusd.h"

#define FCD_RAID_BUF_CHUNK	2000
#define FCD_RAID_MAX_CHUNKS	32

#define FCD_RAID_DEVNAME_SIZE	8

/* Max # of captures in any regex + 1 (for the complete match) */
#define FCD_RAID_MATCHES_SIZE	7

static const char *const fcd_raid_regexes[3] = {

	/*
	 * Matches the initial portion of an array entry in /proc/mdstat
	 */
	"^([^[:space:]]+) : "				// 1 - RAID device
	"(active|inactive) "				// 2 - [in]active
	"(\\(read-only\\) |\\(auto-read-only\\) )?"	// 3 - [auto-]read-only
	"(faulty |linear |multipath |raid0 |raid1 |"	// 4 - personality
		"raid4 |raid5 |raid6 |raid10 )?",

	/*
	 * Matches a single RAID array member
	 */
	"^([[:alnum:]-]+)"		// 1 - block device
	"\\[([[:digit:]]+)\\]"		// 2 - role number
	"(\\([WFSR]\\))?",		// 3 - status

	/*
	 * Matches the end of the second line of an array entry
	 */
	"([[:digit:]]+ near-copies )?"		// 1 - # near-copies
	"([[:digit:]]+ (far|offset)-copies )?"	// 2 - # far/offset-copies
	"\\[([[:digit:]]+)/"			// 4 - "ideal" devices in array
	"([[:digit:]]+)\\]"			// 5 - current devices in array
	" \\[([U_]+)\\]$",			// 6 - device status summary
};

enum fcd_raid_type {
	FCD_RAID_TYPE_FAULTY,
	FCD_RAID_TYPE_LINEAR,
	FCD_RAID_TYPE_MULTIPATH,
	FCD_RAID_TYPE_RAID0,
	FCD_RAID_TYPE_RAID1,
	FCD_RAID_TYPE_RAID4,
	FCD_RAID_TYPE_RAID5,
	FCD_RAID_TYPE_RAID6,
	FCD_RAID_TYPE_RAID10,
};

struct fcd_raid_type_match {
	enum fcd_raid_type type;
	const char *match;
};

const struct fcd_raid_type_match fcd_raid_type_matches[] = {
	{ FCD_RAID_TYPE_FAULTY,		"faulty " },
	{ FCD_RAID_TYPE_LINEAR,		"linear " },
	{ FCD_RAID_TYPE_MULTIPATH,	"multipath " },
	{ FCD_RAID_TYPE_RAID0,		"raid0 " },
	{ FCD_RAID_TYPE_RAID1,		"raid1 " },
	{ FCD_RAID_TYPE_RAID4,		"raid4 " },
	{ FCD_RAID_TYPE_RAID5,		"raid5 " },
	{ FCD_RAID_TYPE_RAID6,		"raid6 " },
	{ FCD_RAID_TYPE_RAID10,		"raid10 " },
};

enum fcd_raid_arr_stat {
	FCD_RAID_ARRAY_STOPPED = 0,	// not listed in /proc/mdstat
	FCD_RAID_ARRAY_INACTIVE,
	FCD_RAID_ARRAY_ACTIVE,		// ... but not one of the statuses below
	FCD_RAID_ARRAY_READONLY,
	FCD_RAID_ARRAY_DEGRADED,
	FCD_RAID_ARRAY_FAILED,
};

enum fcd_raid_dev_stat {
	FCD_RAID_DEV_MISSING = 0,
	FCD_RAID_DEV_ACTIVE,
	FCD_RAID_DEV_FAILED,
	FCD_RAID_DEV_SPARE,
	FCD_RAID_DEV_WRITEMOSTLY,
	FCD_RAID_DEV_REPLACEMENT,
};

struct fcd_raid_array {
	char name[FCD_RAID_DEVNAME_SIZE];
	int ideal_devs;
	int current_devs;
	enum fcd_raid_type type;
	enum fcd_raid_arr_stat array_status;
	enum fcd_raid_dev_stat dev_status[5];
};

static struct fcd_raid_array fcd_raid_arrays[20] = {
	{ .name = "md1" },	{ .name = "md2" },
	{ .name = "md3" },	{ .name = "md4" },
	{ .name = "md5" },	{ .name = "md6" },
	{ .name = "md7" },	{ .name = "md8" },
	{ .name = "md9" },	{ .name = "md10" },
	{ .name = "md11" },	{ .name = "md12" },
	{ .name = "md13" },	{ .name = "md14" },
	{ .name = "md15" },	{ .name = "md16" },
	{ .name = "md17" },	{ .name = "md18" },
	{ .name = "md19" },	{ .name = "md20" },
};

static enum fcd_raid_type fcd_raid_parse_type(const char *buf,
					      const regmatch_t *match)
{
	const char *m;
	size_t len;
	unsigned i;

	m = buf + match->rm_so;
	len = match->rm_eo - match->rm_so;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_raid_type_matches); ++i)
	{
		if (strncmp(fcd_raid_type_matches[i].match, m, len) == 0)
			return fcd_raid_type_matches[i].type;
	}

	FCD_ABORT("Unknown personality: %.20s\n", match);
}

static struct fcd_raid_array *fcd_raid_find_array(const char *buf,
						  const regmatch_t *match)
{
	const char *m;
	size_t len;
	unsigned i;

	m = buf + match->rm_so;
	len = match->rm_eo - match->rm_so;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_raid_arrays); ++i)
	{
		if (strncmp(fcd_raid_arrays[i].name, m, len) == 0)
			return &fcd_raid_arrays[i];
	}

	FCD_INFO("Ignoring unknown RAID array %.*s\n", (int)len, m);
	return NULL;
}

static regoff_t fcd_raid_parse_dev(const char *c, const regex_t regexes[],
				   regmatch_t matches[],
				   struct fcd_raid_array *array)
{
	int i;

	if (regexec(&regexes[1], c, FCD_RAID_MATCHES_SIZE, matches, 0) != 0)
		return 0;

	/* Assume that device (match 1) is sd[b-f]XX */

	i = (c + matches[1].rm_so)[2] - 'b';
	if (i < 0 || i > 4) {
		FCD_WARN("Unexpected RAID array member: %.*s\n",
			 (int)(matches[1].rm_eo - matches[1].rm_so),
			 c + matches[1].rm_so);
		return -1;
	}

	if (matches[3].rm_so == -1) {
		array->dev_status[i] = FCD_RAID_DEV_ACTIVE;
	}
	else {
		switch (c[matches[3].rm_so + 1])
		{
			case 'W':
				array->dev_status[i] = FCD_RAID_DEV_WRITEMOSTLY;
				break;

			case 'F':
				array->dev_status[i] = FCD_RAID_DEV_FAILED;
				break;

			case 'S':
				array->dev_status[i] = FCD_RAID_DEV_SPARE;
				break;

			case 'R':
				array->dev_status[i] = FCD_RAID_DEV_REPLACEMENT;
				break;
		}
	}

	return matches[0].rm_eo;
}

static int fcd_raid_parse_devs(const char *c, const regex_t regexes[],
			       regmatch_t matches[],
			       struct fcd_raid_array *array)
{
	regoff_t ret;

	/* FCD_RAID_DEV_MISSING == 0 */
	memset(array->dev_status, 0, sizeof array->dev_status);

	while (1)
	{
		ret = fcd_raid_parse_dev(c, regexes, matches, array);
		if (ret < 1)
			return ret;

		c += ret;
		if (*c == ' ')
			++c;
	}
}

/*
 * See http://en.wikipedia.org/wiki/Non-standard_RAID_levels#Linux_MD_RAID_10
 * for a description of the RAID-10 layout.  Also see drivers/md/raid10.c in the
 * Linux kernel sources.
 *
 * The logic of this function is inspired by the RAID-10 portion of the enough()
 * function in mdadm's util.c.
 */
static int fcd_raid_r10_failed(const char *c, const regmatch_t matches[],
			       struct fcd_raid_array *array)
{
	uint16_t all_disks_mask, active_disks_mask, chunk_disks_mask, mask;
	int near, far, copies, disks;

	if (matches[1].rm_so == -1)
		near = 1;
	else
		near = atoi(c + matches[1].rm_so);

	if (matches[2].rm_so == -1)
		far = 1;
	else
		far = atoi(c + matches[2].rm_so);

	copies = near * far;
	disks = array->ideal_devs;

	/* uint16_t can handle any 8-disk configuration */
	if (disks + near > (int)(sizeof mask * CHAR_BIT))
		FCD_ABORT("Bitmask data type too small for array\n");

	/* Special case when there is a copy of every chunk on every disk */
	if (copies == disks)
		return array->current_devs < 1;

	/* Special case with no redundancy */
	if (copies == 1)
		return 1;

	all_disks_mask = (1 << disks) - 1;
	chunk_disks_mask = (1 << copies) - 1;

	active_disks_mask = 0;

	for (mask = 1, c += matches[6].rm_so + 1; *c != ']'; mask <<= 1, ++c) {
		if (*c == 'U')
			active_disks_mask |= mask;
	}

	/* Mask for bits that get "rolled" back to bit 0 */
	mask = ((1 << near) - 1) << disks;

	do {
		/* Array is failed if no active disk contains a given chunk */
		if ((active_disks_mask & chunk_disks_mask) == 0)
			return 1;

		chunk_disks_mask <<= near;
		chunk_disks_mask |= (chunk_disks_mask & mask) >> disks;
		chunk_disks_mask &= all_disks_mask;

	/* Quit when the chunk disks mask returns to its starting value */
	} while (chunk_disks_mask != (1 << copies) - 1);

	return 0;
}

static int fcd_raid_array_failed(const char *c, const regmatch_t matches[],
				 struct fcd_raid_array *array)
{
	switch (array->type)
	{
		/* Used for testing; not really meaningful */
		case FCD_RAID_TYPE_FAULTY:
			return 0;

		/* No redundancy, so any missing devices == failed */
		case FCD_RAID_TYPE_LINEAR:
		case FCD_RAID_TYPE_RAID0:
			return 1;

		/* Only 1 device required */
		case FCD_RAID_TYPE_MULTIPATH:
		case FCD_RAID_TYPE_RAID1:
			return array->current_devs < 1;

		/* Can tolerate 1 failure */
		case FCD_RAID_TYPE_RAID4:
		case FCD_RAID_TYPE_RAID5:
			return array->ideal_devs - array->current_devs > 1;

		/* Can tolerate 2 failures */
		case FCD_RAID_TYPE_RAID6:
			return array->ideal_devs - array->current_devs > 2;

		/* It's complicated */
		case FCD_RAID_TYPE_RAID10:
			return fcd_raid_r10_failed(c, matches, array);
	}

	FCD_ABORT("Unreachable code\n");
}

static int fcd_raid_parse_array(const char *c, const regex_t regexes[],
				regmatch_t matches[])
{
	struct fcd_raid_array *array;

	if (regexec(&regexes[0], c, FCD_RAID_MATCHES_SIZE, matches, 0) != 0)
		return 0;

	array = fcd_raid_find_array(c, &matches[1]);
	if (array == NULL)
		return 0;

	/* Match is either "active" or "inactive" */
	if (c[matches[2].rm_so] == 'i') {
		array->array_status = FCD_RAID_ARRAY_INACTIVE;
	}
	else {
		/* Match may be "(read-only) " or "(auto-read-only) " */
		if (matches[3].rm_so != -1 && c[matches[3].rm_so + 1] == 'r')
			array->array_status = FCD_RAID_ARRAY_READONLY;
		else
			array->array_status = FCD_RAID_ARRAY_ACTIVE;

		array->type = fcd_raid_parse_type(c, &matches[4]);
	}

	c += matches[0].rm_eo;

	if (fcd_raid_parse_devs(c, regexes, matches, array) == -1)
		return -1;

	if (array->array_status == FCD_RAID_ARRAY_INACTIVE)
		return 1;

	c = strchr(c, '\n');
	if (c == NULL) {
		FCD_WARN("Error parsing /proc/mdstat\n");
		return -1;
	}

	if (regexec(&regexes[2], ++c, FCD_RAID_MATCHES_SIZE, matches, 0) != 0) {
		FCD_WARN("Error parsing /proc/mdstat\n");
		return -1;
	}

	array->ideal_devs = atoi(c + matches[4].rm_so);
	array->current_devs = atoi(c + matches[5].rm_so);

	if (array->current_devs < array->ideal_devs) {
		if (fcd_raid_array_failed(c, matches, array))
			array->array_status = FCD_RAID_ARRAY_FAILED;
		else
			array->array_status = FCD_RAID_ARRAY_DEGRADED;
	}

	return 1;
}

static int fcd_raid_parse_mdstat(const char *c, const regex_t regexes[])
{
	regmatch_t matches[FCD_RAID_MATCHES_SIZE];
	unsigned i;
	int ret;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_raid_arrays); ++i)
		fcd_raid_arrays[i].array_status = FCD_RAID_ARRAY_STOPPED;

	while (1)
	{
		ret = fcd_raid_parse_array(c, regexes, matches);
		if (ret == -1)
			return -1;

		/* If we just parsed an array, skip 2 lines */
		for (ret *= 2; ret >= 0; --ret)
		{
			c = strchr(c, '\n');
			if (c == NULL)
				return 0;
			++c;
		}
	}
}

static int fcd_raid_regcomp(regex_t regexes[])
{
	size_t errbuf_size;
	char *errbuf;
	int ret, i;

	for (i = 0; i < (int)FCD_ARRAY_SIZE(fcd_raid_regexes); ++i)
	{
		ret = regcomp(&regexes[i], fcd_raid_regexes[i],
			      REG_EXTENDED | REG_NEWLINE);
		if (ret != 0)
			goto regcomp_error;
	}

	return 0;

regcomp_error:

	errbuf_size = regerror(ret, &regexes[i], 0, 0);

	errbuf = malloc(errbuf_size);
	if (errbuf == NULL) {
		FCD_ERR("malloc: %m\n");
		FCD_WARN("Cannot format regcomp error message: code %d\n", ret);
	}
	else {
		regerror(ret, &regexes[i], errbuf, errbuf_size);
		FCD_ERR("regcomp: %s\n", errbuf);
		free(errbuf);
	}

	for (--i; i >= 0; --i)
		regfree(&regexes[i]);

	return -1;
}

static void fcd_raid_regfree(regex_t regexes[])
{
	unsigned i;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_raid_regexes); ++i)
		regfree(&regexes[i]);
}

static int fcd_raid_grow_buf(char **buf, size_t *buf_size)
{
	size_t new_size;
	char *new_buf;

	if (*buf_size == FCD_RAID_BUF_CHUNK * FCD_RAID_MAX_CHUNKS) {
		FCD_WARN("Cannot read more than %d bytes from /proc/mdstat\n",
			 FCD_RAID_BUF_CHUNK * FCD_RAID_MAX_CHUNKS);
		return -1;
	}

	if (*buf == NULL || *buf_size == 0)
		new_size = FCD_RAID_BUF_CHUNK;
	else
		new_size = *buf_size + FCD_RAID_BUF_CHUNK;

	new_buf = realloc(*buf, new_size);
	if (new_buf == NULL) {
		FCD_ERR("realloc: %m\n");
		return -1;
	}

	*buf = new_buf;
	*buf_size = new_size;

	return 0;
}

static int fcd_raid_read_mdstat(int fd, char **buf, size_t *buf_size)
{
	size_t total;
	ssize_t ret;

	if (lseek(fd, 0, SEEK_SET) == -1) {
		FCD_ERR("lseek: %m\n");
		return -1;
	}

	total = 0;

	do {
		if (total == *buf_size)
			fcd_raid_grow_buf(buf, buf_size);

		ret = read(fd, *buf + total, *buf_size - total);
		if (ret == -1) {
			if (errno == EINTR)
				continue;
			FCD_ERR("read: %m\n");
			return -1;
		}

		total += ret;

	} while (ret != 0);

	if (total == 0) {
		FCD_WARN("Read 0 bytes from /proc/mdstat\n");
		return -1;
	}

	(*buf)[total - 1] = 0;

	return 0;
}

__attribute__((noreturn))
static void fcd_raid_disable(char *mdstat_buf, int fd, regex_t regexes[],
			     struct fcd_monitor *mon)
{
	free(mdstat_buf);
	if (close(fd) == -1)
		FCD_ERR("close: %m\n");
	fcd_raid_regfree(regexes);
	fcd_disable_monitor(mon);
}

__attribute__((noreturn))
static void *fcd_raid_fn(void *arg)
{
	regex_t regexes[FCD_ARRAY_SIZE(fcd_raid_regexes)];
	struct fcd_monitor *mon = arg;
	char buf[21], *mdstat_buf = NULL;
	size_t mdstat_size = 0;
	int ret, fd, ok, warn, fail;
	unsigned i;

	if (fcd_raid_regcomp(regexes) == -1)
		fcd_disable_monitor(mon);

	fd = open("/proc/mdstat", O_RDONLY);
	if (fd == -1) {
		FCD_ERR("open: %m\n");
		fcd_raid_regfree(regexes);
		fcd_disable_monitor(mon);
	}

	do {
		memset(buf, ' ', sizeof buf);

		if (fcd_raid_read_mdstat(fd, &mdstat_buf, &mdstat_size) == -1)
			fcd_raid_disable(mdstat_buf, fd, regexes, mon);

		if (fcd_raid_parse_mdstat(mdstat_buf, regexes) == -1)
			fcd_raid_disable(mdstat_buf, fd, regexes, mon);

		ok = warn = fail = 0;

		for (i = 0; i < FCD_ARRAY_SIZE(fcd_raid_arrays); ++i)
		{
			if (fcd_raid_arrays[i].array_status ==
					FCD_RAID_ARRAY_ACTIVE) {
				++ok;
			}
			else if (fcd_raid_arrays[i].array_status ==
					FCD_RAID_ARRAY_DEGRADED) {
				++warn;
			}
			else {
				++fail;
			}
		}

		ret = snprintf(buf, sizeof buf, "OK:%d WARN:%d FAIL:%d",
			       ok, warn, fail);
		if (ret < 0) {
			FCD_ERR("snprintf: %m\n");
			fcd_raid_disable(mdstat_buf, fd, regexes, mon);
		}

		if (ret < (int)sizeof buf)
			buf[ret] = ' ';

		fcd_copy_buf(buf, mon);

	} while (fcd_sleep_and_check_exit(30) == 0);

	free(mdstat_buf);
	if (close(fd) == -1)
		FCD_ERR("close: %m\n");
	fcd_raid_regfree(regexes);

	pthread_exit(NULL);
}

struct fcd_monitor fcd_raid_monitor = {
	.mutex		= PTHREAD_MUTEX_INITIALIZER,
	.name		= "RAID status",
	.monitor_fn	= fcd_raid_fn,
	.buf		= "....."
			  "RAID STATUS         "
			  "                    ",
};

#if 0
#include <fcntl.h>

int fcd_foreground = 1;

static const char *format_raid_type(enum fcd_raid_type type)
{
	unsigned i;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_raid_type_matches); ++i) {
		if (fcd_raid_type_matches[i].type == type)
			return fcd_raid_type_matches[i].match;
	}

	FCD_ABORT("Unknown RAID type\n");
}

static const struct {
	enum fcd_raid_arr_stat status;
	const char *name;
} array_status_names[] = {
	{ FCD_RAID_ARRAY_STOPPED,	"stopped" },
	{ FCD_RAID_ARRAY_INACTIVE,	"inactive" },
	{ FCD_RAID_ARRAY_ACTIVE,	"active" },
	{ FCD_RAID_ARRAY_READONLY,	"read-only" },
	{ FCD_RAID_ARRAY_DEGRADED,	"degraded" },
	{ FCD_RAID_ARRAY_FAILED,	"failed" }
};

static const char *format_raid_status(enum fcd_raid_arr_stat status)
{
	unsigned i;

	for (i = 0; i < FCD_ARRAY_SIZE(array_status_names); ++i) {
		if (array_status_names[i].status == status)
			return array_status_names[i].name;
	}

	FCD_ABORT("Unknown RAID status\n");
}

static const struct {
	enum fcd_raid_dev_stat status;
	const char *name;
} dev_status_names[] = {
	{ FCD_RAID_DEV_MISSING,		"missing" },
	{ FCD_RAID_DEV_ACTIVE,		"active" },
	{ FCD_RAID_DEV_FAILED,		"failed" },
	{ FCD_RAID_DEV_SPARE,		"spare" },
	{ FCD_RAID_DEV_WRITEMOSTLY,	"write-mostly" },
	{ FCD_RAID_DEV_REPLACEMENT,	"replacement" }
};

static const char *format_dev_status(enum fcd_raid_dev_stat status)
{
	unsigned i;

	for (i = 0; i < FCD_ARRAY_SIZE(dev_status_names); ++i) {
		if (dev_status_names[i].status == status)
			return dev_status_names[i].name;
	}

	FCD_ABORT("Unknown device status\n");
}

int main(int argc __attribute__((unused)), char *argv[])
{
	regex_t regexes[FCD_ARRAY_SIZE(fcd_raid_regexes)];
	size_t buf_size;
	char *buf;
	int i, fd;

	if (fcd_raid_regcomp(regexes) == -1)
		exit(1);

	fd = open(argv[1], O_RDONLY);
	if (fd == -1) {
		perror(argv[1]);
		exit(1);
	}

	buf = NULL;
	buf_size = 0;

	if (fcd_raid_read_mdstat(fd, &buf, &buf_size) == -1)
		exit(1);

	if (close(fd) == -1) {
		perror("close");
		exit(1);
	}

	if (fcd_raid_parse_mdstat(buf, regexes) == -1)
		exit(1);

	for (i = 0; i < (int)FCD_ARRAY_SIZE(fcd_raid_arrays); ++i) {
		struct fcd_raid_array *array = &fcd_raid_arrays[i];
		puts(array->name);
		printf("\tideal_devs = %d\n", array->ideal_devs);
		printf("\tcurrent_devs = %d\n", array->current_devs);
		printf("\ttype = %s\n", format_raid_type(array->type));
		printf("\tstatus = %s\n",
		       format_raid_status(array->array_status));
		printf("\tdevice status = %s/%s/%s/%s/%s\n\n",
		       format_dev_status(array->dev_status[0]),
		       format_dev_status(array->dev_status[1]),
		       format_dev_status(array->dev_status[2]),
		       format_dev_status(array->dev_status[3]),
		       format_dev_status(array->dev_status[4]));
	}

	return 0;
}
#endif
