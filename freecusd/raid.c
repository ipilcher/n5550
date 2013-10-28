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

#include <limits.h>
#include <string.h>
#include <regex.h>
#include <errno.h>
#include <fcntl.h>

/* Max size of a RAID array kernel name - 11 chars + terminating 0 */
#define FCD_RAID_DEVNAME_SIZE		12

/* /sys/devices/virtual/block/<DEV>/md/array_state; <DEV> is 11 chars max */
#define FCD_RAID_SYSFS_FILE_SIZE	54

/* Buffer size required for a UUID - aaaaaaaa:bbbbbbbb:cccccccc:dddddddd */
#define FCD_RAID_UUID_BUF_SIZE		36

/* Max size of buffer used to read /etc/mdadm.conf and /proc/mdstat */
#define FCD_RAID_FILE_BUF_SIZE		20000

/*
 * Regex to match/parse the initial portion of an array in /proc/mdstat
 */
static const char fcd_raid_mdstat_array_pattern_1[] =
	"^([^[:space:]]+) : "				// 1 - RAID device
	"(active|inactive) "				// 2 - [in]active
	"(\\(read-only\\) |\\(auto-read-only\\) )?"	// 3 - [auto-]read-only
	"(faulty |linear |multipath |raid0 |raid1 |"	// 4 - personality
		"raid4 |raid5 |raid6 |raid10 )?";

static regmatch_t fcd_raid_mdstat_array_matches_1[5];

/*
 * Regex to match/parse a single RAID array member in /proc/mdstat
 */
static const char fcd_raid_mdstat_dev_pattern[] =
	"^([[:alnum:]-]+)"			// 1 - device name
	"\\[([[:digit:]]+)\\]"			// 2 - device number
	"(\\([WFSR]\\))?";			// 3 - device status

static regmatch_t fcd_raid_mdstat_dev_matches[4];

/*
 * Regex to match/parse the end of the second line of an array in /proc/mdstat
 */
static const char fcd_raid_mdstat_array_pattern_2[] =
	"([[:digit:]]+ near-copies )?"		// 1 - # near-copies
	"([[:digit:]]+ (far|offset)-copies )?"	// 2 - # far/offset-copies
	"\\[([[:digit:]]+)/"			// 4 - "ideal" devices
	"([[:digit:]]+)\\]"			// 5 - current devices
	" \\[([U_]+)\\]$";			// 6 - device status summary

static regmatch_t fcd_raid_mdstat_array_matches_2[7];

/*
 * Regex to match/parse an array that is identified by UUID in /etc/mdadm.conf
 */
static const char fcd_raid_conf_array_pattern[] =
	"^ARRAY\\s+(<ignore>\\s+)?[^#]*"		// 1 - <ignore>
	"\\bUUID=(([0-9a-f]{8}:){3}[0-9a-f]{8})\\b";	// 2 - UUID

static regmatch_t fcd_raid_conf_array_matches[3];

/*
 * Regex to match/parse the MD_UUID in the output of mdadm --detail --export
 */
static const char fcd_raid_detail_pattern[] =
	"^MD_UUID=(([0-9a-f]{8}:){3}[0-9a-f]{8})$";	// 1 - UUID

static regmatch_t fcd_raid_detail_matches[2];

struct fcd_raid_regex {
	int cflags;
	const char *pattern;
	regex_t regex;
	regmatch_t *matches;
	size_t nmatch;
};

static struct fcd_raid_regex fcd_raid_regexes[] = {
	{
		.cflags 	= REG_EXTENDED | REG_NEWLINE,
		.pattern	= fcd_raid_mdstat_array_pattern_1,
		.matches	= fcd_raid_mdstat_array_matches_1,
		.nmatch		= FCD_ARRAY_SIZE(
					fcd_raid_mdstat_array_matches_1),
	},
	{
		.cflags		= REG_EXTENDED | REG_NEWLINE,
		.pattern	= fcd_raid_mdstat_dev_pattern,
		.matches	= fcd_raid_mdstat_dev_matches,
		.nmatch		= FCD_ARRAY_SIZE(fcd_raid_mdstat_dev_matches),
	},
	{
		.cflags		= REG_EXTENDED | REG_NEWLINE,
		.pattern	= fcd_raid_mdstat_array_pattern_2,
		.matches	= fcd_raid_mdstat_array_matches_2,
		.nmatch		= FCD_ARRAY_SIZE(
					fcd_raid_mdstat_array_matches_2),
	},
	{
		.cflags		= REG_EXTENDED | REG_NEWLINE | REG_ICASE,
		.pattern	= fcd_raid_conf_array_pattern,
		.matches	= fcd_raid_conf_array_matches,
		.nmatch		= FCD_ARRAY_SIZE(fcd_raid_conf_array_matches),
	},
	{
		.cflags		= REG_EXTENDED | REG_NEWLINE,
		.pattern	= fcd_raid_detail_pattern,
		.matches	= fcd_raid_detail_matches,
		.nmatch		= FCD_ARRAY_SIZE(fcd_raid_detail_matches),
	},
};

static char fcd_raid_mdadm_dev[FCD_RAID_DEVNAME_SIZE + 5] = "/dev/";

static char *fcd_raid_mdadm_cmd[] = {
	"/sbin/mdadm",
	"mdadm",
	"--detail",
	"--export",
	fcd_raid_mdadm_dev,
	NULL
};

/* Buffer used by fcd_raid_get_uuid for mdadm output */
static char *fcd_raid_uuid_buf = NULL;
static size_t fcd_raid_uuid_buf_size = 0;

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
	FCD_RAID_ARRAY_STOPPED = 0,	/* not listed in /proc/mdstat */
	FCD_RAID_ARRAY_INACTIVE,
	FCD_RAID_ARRAY_ACTIVE,		/* but not one of the statuses below */
	FCD_RAID_ARRAY_READONLY,
	FCD_RAID_ARRAY_DEGRADED,
	FCD_RAID_ARRAY_FAILED,
};

enum fcd_raid_dev_stat {
	FCD_RAID_DEV_EXPECTED = -1,	/* only used in fcd_raid_parse_devs */
	FCD_RAID_DEV_UNKNOWN = 0,
	FCD_RAID_DEV_MISSING,		/* should be a member of the array */
	FCD_RAID_DEV_ACTIVE,
	FCD_RAID_DEV_FAILED,
	FCD_RAID_DEV_SPARE,
	FCD_RAID_DEV_WRITEMOSTLY,
	FCD_RAID_DEV_REPLACEMENT,
};

struct fcd_raid_array {
	uint32_t uuid[4];
	struct fcd_raid_array *next;
	char name[FCD_RAID_DEVNAME_SIZE];
	int sysfs_fd;
	int transient;
	int ideal_devs;
	int current_devs;
	enum fcd_raid_type type;
	enum fcd_raid_arr_stat array_status;
	enum fcd_raid_dev_stat dev_status[5];
};

static struct fcd_raid_array *fcd_raid_list = NULL;
static struct fcd_raid_array **fcd_raid_list_end = &fcd_raid_list;

static void fcd_raid_list_append(struct fcd_raid_array *array)
{
	array->next = NULL;
	*fcd_raid_list_end = array;
	fcd_raid_list_end = &array->next;
}

static struct fcd_raid_array *fcd_raid_find_by_substr(const char *s, size_t len)
{
	struct fcd_raid_array *array;

	for (array = fcd_raid_list; array != NULL; array = array->next) {

		if (strlen(array->name) == len &&
				memcmp(s, array->name, len) == 0)
			return array;
	}

	return NULL;
}
#if 0
static struct fcd_raid_array *fcd_raid_find_by_name(const char *name)
{
	return fcd_raid_find_by_substr(name, strlen(name));
}
#endif
static struct fcd_raid_array *fcd_raid_find_by_uuid(const uint32_t *uuid)
{
	struct fcd_raid_array *array;

	for (array = fcd_raid_list; array != NULL; array = array->next) {

		if (memcmp(array->uuid, uuid, sizeof *uuid) == 0)
			return array;
	}

	return NULL;
}

static int fcd_raid_close_array_fd(struct fcd_raid_array *array)
{
	if (close(array->sysfs_fd) == -1) {
		FCD_PERROR("close");
		return -1;
	}

	array->sysfs_fd = -1;

	memset(array->name, 0, sizeof array->name);

	return 0;
}

static int fcd_raid_array_unchanged(struct fcd_raid_array *array)
{
	ssize_t ret;
	char c;

	if (lseek(array->sysfs_fd, SEEK_SET, 0) == -1) {
		FCD_PERROR("lseek");
		return -1;
	}

	ret = read(array->sysfs_fd, &c, 1);

	switch (ret) {

		case 1:		return 1;

		case 0:		FCD_ERR("Unexpected EOF\n");
				return -1;

		case -1:	if (errno == ENODEV)
					return fcd_raid_close_array_fd(array);
				FCD_PERROR("read");
				return -1;

		default:	FCD_ABORT("read returned %zd\n", ret);
	}
}

static void fcd_raid_parse_uuid(uint32_t *uuid, const char *s)
{
	int i;

	/*
	 * Called from fcd_raid_get_uuid and fcd_raid_read_mdadm_conf, both of
	 * which use regular expressions that only match valid UUIDs.
	 */

	for (i = 3; i >= 0; --i, s += 9)
		uuid[i] = (uint32_t)strtoul(s, NULL, 16);
}

static int fcd_raid_get_uuid(uint32_t *uuid, const char *c,
			     const regmatch_t *match, const int *pipe_fds)
{
	static const struct fcd_raid_regex *const regex = &fcd_raid_regexes[4];
	static regmatch_t *const matches = fcd_raid_detail_matches;
	struct timespec timeout;
	size_t name_len;
	int ret, status;

	/*
	 * fcd_raid_get_uuid is only called from fcd_raid_find_array which
	 * checks the length of the device name
	 */

	name_len = match->rm_eo - match->rm_so;
	memcpy(fcd_raid_mdadm_dev + 5, c + match->rm_so, name_len);
	(fcd_raid_mdadm_dev + 5)[name_len] = 0;

	timeout.tv_sec = 2;
	timeout.tv_nsec = 0;

	ret = fcd_cmd_output(&status, fcd_raid_mdadm_cmd, &fcd_raid_uuid_buf,
			     &fcd_raid_uuid_buf_size, 1000, &timeout, pipe_fds);
	if (ret < 0) {
		if (ret == -2)
			FCD_WARN("mdadm command timed out\n");
		return ret;
	}

	if (status != 0) {
		FCD_WARN("Non-zero mdadm exit status: %d\n");
		return -1;
	}

	ret = regexec(&regex->regex, fcd_raid_uuid_buf,
		      regex->nmatch, matches, 0);
	if (ret != 0) {
		FCD_WARN("Error parsing mdadm output\n");
		return -1;
	}

	fcd_raid_parse_uuid(uuid, fcd_raid_uuid_buf + matches[1].rm_so);

	return 0;
}

static struct fcd_raid_array *fcd_raid_array_alloc(void)
{
	static const struct fcd_raid_array template = {
		.sysfs_fd	= -1,
	};

	struct fcd_raid_array *array;

	array = malloc(sizeof *array);
	if (array == NULL)
		FCD_PERROR("malloc");
	else
		*array = template;

	return array;
}

static int fcd_raid_find_array_error(int fd, int ret)
{
	if (close(fd) == -1) {
		FCD_PERROR("close");
		return -1;
	}

	return ret;
}

/*
 * Returns 0 if array name/UUID mapping is known to have been stable since the
 * previous pass, 1 if mapping may have changed (-1 = error, -2 = timeout, -3 =
 * exit signal received, -4 = mdadm output buffer size exceeded)
 */
static int fcd_raid_find_array(struct fcd_raid_array **array, const char *buf,
			       const regmatch_t *match, const int *pipe_fds)
{
	static char sysfs_file[FCD_RAID_SYSFS_FILE_SIZE];
	int ret, sysfs_fd;
	uint32_t uuid[4];
	size_t name_len;

	name_len = match->rm_eo - match->rm_so;
	if (name_len >= FCD_RAID_DEVNAME_SIZE - 1) {
		FCD_WARN("RAID device name '%.*s' too long\n", (int)name_len,
			 buf + match->rm_so);
#ifdef __OPTIMIZE_SIZE__
		/* See https://bugzilla.redhat.com/show_bug.cgi?id=1018422 */
		*array = NULL;
#endif
		return -1;
	}

	*array = fcd_raid_find_by_substr(buf + match->rm_so, name_len);
	if (*array != NULL) {

		ret = fcd_raid_array_unchanged(*array);
		if (ret == -1)
			return -1;
		if (ret == 1)
			return 0;
	}

	sprintf(sysfs_file, "/sys/devices/virtual/block/%.*s/md/array_state",
		(int)name_len, buf + match->rm_so);
	sysfs_fd = open(sysfs_file, O_RDONLY | O_CLOEXEC);
	if (sysfs_fd == -1) {
		if (errno == ENOENT)
			return 1;
		FCD_PERROR(sysfs_file);
		return -1;
	}

	ret = fcd_raid_get_uuid(uuid, buf, match, pipe_fds);
	if (ret < 0)
		return fcd_raid_find_array_error(sysfs_fd, ret);

	*array = fcd_raid_find_by_uuid(uuid);
	if (*array == NULL) {
		*array = fcd_raid_array_alloc();
		if (*array == NULL)
			return fcd_raid_find_array_error(sysfs_fd, -1);
		memcpy((*array)->uuid, uuid, sizeof uuid);
		(*array)->transient = 1;
		fcd_raid_list_append(*array);
	}
	else if ((*array)->sysfs_fd != -1 &&
				fcd_raid_close_array_fd(*array) == -1) {
		return fcd_raid_find_array_error(sysfs_fd, -1);
	}

	memcpy((*array)->name, buf + match->rm_so, name_len);
	(*array)->name[name_len] = 0;
	(*array)->sysfs_fd = sysfs_fd;

	return 1;
}

static enum fcd_raid_type fcd_raid_parse_type(const char *buf,
					      const regmatch_t *match)
{
	const char *m;
	size_t len;
	unsigned i;

	m = buf + match->rm_so;
	len = match->rm_eo - match->rm_so;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_raid_type_matches); ++i) {

		if (strncmp(fcd_raid_type_matches[i].match, m, len) == 0)
			return fcd_raid_type_matches[i].type;
	}

	/* Regex should prevent this from ever happening */
	FCD_ABORT("Unknown personality: %.20s\n", match);
}

/*
 * Returns # of characters matched (0 = no match, -1 = error)
 */
static regoff_t fcd_raid_parse_dev(const char *c, struct fcd_raid_array *array)
{
	static const struct fcd_raid_regex *const regex = &fcd_raid_regexes[1];
	static regmatch_t *const matches = fcd_raid_mdstat_dev_matches;
	int i;

	if (regexec(&regex->regex, c, regex->nmatch, matches, 0) != 0)
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

static int fcd_raid_parse_devs(const char *c, struct fcd_raid_array *array)
{
	regoff_t ret;
	size_t i;

	for (i = 0; i < FCD_ARRAY_SIZE(array->dev_status); ++i) {

		if (array->dev_status[i] != FCD_RAID_DEV_UNKNOWN)
			array->dev_status[i] = FCD_RAID_DEV_EXPECTED;
	}

	while (1) {

		ret = fcd_raid_parse_dev(c, array);
		if (ret < 1)
			return ret;

		c += ret;
		if (*c == ' ')
			++c;
	}

	for (i = 0; i < FCD_ARRAY_SIZE(array->dev_status); ++i) {

		if (array->dev_status[i] == FCD_RAID_DEV_EXPECTED)
			array->dev_status[i] = FCD_RAID_DEV_MISSING;
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
	chunk_disks_mask = (1 << copies) - 1;	/* Starting value; see below */

	active_disks_mask = 0;

	for (mask = 1, c += matches[6].rm_so; *c != ']'; mask <<= 1, ++c) {
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
	switch (array->type) {

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

	FCD_ABORT("Invalid enum value\n");
}

/*
 * Returns 1 if array was successfully parsed (0 = no match, -1 = error, -2 =
 * timeout, -3 exit signal received, -4 mdadm output buffer size exceeded)
 */
static int fcd_raid_parse_array(int *names_changed, const char *c,
				const int *pipe_fds)
{
	const struct fcd_raid_regex *regex;
	struct fcd_raid_array *array;
	regmatch_t *matches;
	int ret;

	regex = &fcd_raid_regexes[0]; matches = regex->matches;

	if (regexec(&regex->regex, c, regex->nmatch, matches, 0) != 0)
		return 0;

	ret = fcd_raid_find_array(&array, c, &matches[1], pipe_fds);
	if (ret < 0)
		return ret;

	*names_changed += ret;
	if (*names_changed)
		return 1;

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

	if (fcd_raid_parse_devs(c, array) == -1)
		return -1;

	if (array->array_status == FCD_RAID_ARRAY_INACTIVE)
		return 1;

	c = strchr(c, '\n');
	if (c == NULL) {
		FCD_WARN("Error parsing /proc/mdstat\n");
		return -1;
	}

	regex = &fcd_raid_regexes[2]; matches = regex->matches;

	if (regexec(&regex->regex, ++c, regex->nmatch, matches, 0) != 0) {
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

static int fcd_raid_parse_mdstat(const char *buf, const int *pipe_fds)
{
	struct fcd_raid_array *array;
	int names_changed, ret;
	const char *c;

	for (array = fcd_raid_list; array != NULL; array = array->next)
		array->array_status = FCD_RAID_ARRAY_STOPPED;

	/*
	 * See http://article.gmane.org/gmane.linux.raid/44417 for an
	 * explanation of what this loop does (along with fcd_raid_parse_array
	 * and fcd_raid_find_array)
	 */

	do {
		names_changed = 0;
		c = buf;

		do {
			ret = fcd_raid_parse_array(&names_changed, c, pipe_fds);
			if (ret == -3)
				return -3;
			if (ret < 0)
				return -1;

			/*
			* If we just parsed an array, skip the next two lines
			* (i.e. move past 3 newlines).  Otherwise, move to the
			* next line (past 1 newline).
			*/

			for (ret *= 2; ret >= 0; --ret)	{

				c = strchr(c, '\n');
				if (c == NULL)
					break;
				++c;
			}

		} while (c != NULL);

	} while (names_changed != 0);

	return 0;
}

static int fcd_raid_regcomp(void)
{
	struct fcd_raid_regex *regex;
	size_t errbuf_size;
	char *errbuf;
	int ret, i;

	for (i = 0; i < (int)FCD_ARRAY_SIZE(fcd_raid_regexes); ++i) {

		regex = &fcd_raid_regexes[i];

		ret = regcomp(&regex->regex, regex->pattern, regex->cflags);
		if (ret != 0)
			goto regcomp_error;
	}

	return 0;

regcomp_error:

	errbuf_size = regerror(ret, &regex->regex, 0, 0);

	errbuf = malloc(errbuf_size);
	if (errbuf == NULL) {
		FCD_PERROR("malloc");
		FCD_WARN("Cannot format regcomp error message: code %d\n", ret);
	}
	else {
		regerror(ret, &regex->regex, errbuf, errbuf_size);
		FCD_ERR("regcomp: %s\n", errbuf);
		free(errbuf);
	}

	for (--i; i >= 0; --i)
		regfree(&fcd_raid_regexes[i].regex);

	return -1;
}
#if 0
static const char *fcd_raid_format_uuid(const uint32_t *uuid, char *buf)
{
	sprintf(buf, "%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32 ":%08" PRIx32,
		uuid[3], uuid[2], uuid[1], uuid[0]);

	return buf;
}
#endif
static ssize_t fcd_raid_read_file(int fd, char **buf, size_t *buf_size)
{
	struct timespec timeout;
	ssize_t ret;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	ret = fcd_read_all(fd, buf, buf_size, FCD_RAID_FILE_BUF_SIZE, &timeout);
	if (ret == -2) {
		FCD_WARN("Read from regular file timed out\n");
		return -1;
	}

	return ret;
}

static int fcd_raid_read_mdadm_conf(char **buf, size_t *buf_size)
{
	static const struct fcd_raid_regex *const regex = &fcd_raid_regexes[3];
	static regmatch_t *const matches = fcd_raid_conf_array_matches;
	static const char path[] = "/etc/mdadm.conf";
	struct fcd_raid_array *array;
	int ret, fd;
	char *c;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1) {
		if (errno == ENOENT)
			return 0;
		FCD_PERROR(path);
		return -1;
	}

	ret = fcd_raid_read_file(fd, buf, buf_size);
	if (ret < 0) {
		if (close(fd) == -1)
			FCD_PERROR("close");
		return ret;
	}

	if (close(fd) == -1) {
		FCD_PERROR("close");
		return -1;
	}

	for (c = *buf; *c != 0; ++c)
	{
		if (regexec(&regex->regex, c, regex->nmatch, matches, 0) == 0 &&
				matches[1].rm_so == -1)	  /* not <inactive> */
		{
			array = fcd_raid_array_alloc();
			if (array == NULL)
				return -1;

			fcd_raid_parse_uuid(array->uuid, c + matches[2].rm_so);
			fcd_raid_list_append(array);

			c += matches[0].rm_eo;
		}

		c = strchr(c, '\n');
		if (c == NULL)
			break;
	}

	return 0;
}

static void fcd_raid_cleanup(char *mdstat_buf, int mdstat_fd, int *pipe_fds)
{
	struct fcd_raid_array *array, *next;
	size_t i;

	for (i = 0; i < FCD_ARRAY_SIZE(fcd_raid_regexes); ++i)
		regfree(&fcd_raid_regexes[i].regex);

	for (array = fcd_raid_list; array != NULL; array = next) {

		if (array->sysfs_fd != -1 && close(array->sysfs_fd) == -1)
			FCD_PERROR("close");

		next = array->next;
		free(array);
	}

	if (mdstat_fd != -1 && close(mdstat_fd) == -1)
		FCD_PERROR("close");
	if (pipe_fds[0] != -1 && close(pipe_fds[0]) == -1)
		FCD_PERROR("close");
	if (pipe_fds[1] != -1 && close(pipe_fds[1]) == -1)
		FCD_PERROR("close");

	free(fcd_raid_uuid_buf);
	free(mdstat_buf);
}

__attribute__((noreturn))
static void fcd_raid_disable(char *mdstat_buf, int mdstat_fd, int *pipe_fds,
			     struct fcd_monitor *mon)
{
	fcd_raid_cleanup(mdstat_buf, mdstat_fd, pipe_fds);
	fcd_disable_monitor(mon);
}

static int fcd_raid_setup(int *pipe_fds, int *mdstat_fd, char **mdstat_buf,
			  size_t *mdstat_size)
{
	static const char path[] = "/proc/mdstat";
	int ret;

	*mdstat_buf = NULL;
	*mdstat_size = 0;
	*mdstat_fd = -1;
	pipe_fds[0] = -1;
	pipe_fds[1] = -1;

	if (fcd_raid_regcomp() == -1)
		return -1;

	if (pipe2(pipe_fds, O_CLOEXEC) == -1)
		return -1;

	ret = fcd_raid_read_mdadm_conf(mdstat_buf, mdstat_size);
	if (ret < 0)
		return ret;

	*mdstat_fd = open(path, O_RDONLY | O_CLOEXEC);
	if (*mdstat_fd == -1) {
		FCD_PERROR(path);
		return -1;
	}

	return 0;
}

static void fcd_raid_result(int *ok, int *warn, int *fail, int *disks,
			    const struct fcd_raid_array *array)
{
	enum fcd_raid_dev_stat status;
	size_t i;

	switch (array->array_status) {

		case FCD_RAID_ARRAY_ACTIVE:	++(*ok);
						return;

		case FCD_RAID_ARRAY_DEGRADED:	++(*warn);
						break;

		case FCD_RAID_ARRAY_STOPPED:
		case FCD_RAID_ARRAY_INACTIVE:	if (array->transient)
							return;
						/* else fall through */
		case FCD_RAID_ARRAY_READONLY:
		case FCD_RAID_ARRAY_FAILED:	++(*fail);
						break;

		default:
			FCD_ABORT("Invalid enum value\n");
	}

	if (array->array_status == FCD_RAID_ARRAY_STOPPED)
		return;

	for (i = 0; i < FCD_ARRAY_SIZE(array->dev_status); ++i) {

		status = array->dev_status[i];

		if (status == FCD_RAID_DEV_FAILED ||
				status == FCD_RAID_DEV_MISSING ||
				(status == FCD_RAID_DEV_UNKNOWN &&
						array->ideal_devs == 5)) {
			++disks[i];
		}
	}
}

__attribute__((noreturn))
static void *fcd_raid_fn(void *arg)
{
	struct fcd_monitor *mon = arg;
	int ret, fd, ok, warn, fail, disks[5], pipe_fds[2];
	const struct fcd_raid_array *array;
	char buf[21], *mdstat_buf;
	size_t mdstat_size;

	if (fcd_raid_setup(pipe_fds, &fd, &mdstat_buf, &mdstat_size) != 0)
		fcd_raid_disable(mdstat_buf, fd, pipe_fds, mon);

	do {
		memset(buf, ' ', sizeof buf);

		if (lseek(fd, SEEK_SET, 0) == -1) {
			FCD_PERROR("lseek");
			fcd_raid_disable(mdstat_buf, fd, pipe_fds, mon);
		}

		ret = fcd_raid_read_file(fd, &mdstat_buf, &mdstat_size);
		if (ret == -3)
			continue;
		if (ret < 0)
			fcd_raid_disable(mdstat_buf, fd, pipe_fds, mon);

		ret = fcd_raid_parse_mdstat(mdstat_buf, pipe_fds);
		if (ret == -3)
			continue;
		if (ret < 0)
			fcd_raid_disable(mdstat_buf, fd, pipe_fds, mon);

		ok = warn = fail = 0;
		memset(disks, 0, sizeof disks);

		for (array = fcd_raid_list; array != NULL;
					    array = array->next) {

			fcd_raid_result(&ok, &warn, &fail, disks, array);
		}

		ret = snprintf(buf, sizeof buf, "OK:%d WARN:%d FAIL:%d",
			       ok, warn, fail);
		if (ret < 0) {
			FCD_PERROR("snprintf");
			fcd_raid_disable(mdstat_buf, fd, pipe_fds, mon);
		}

		if (ret < (int)sizeof buf)
			buf[ret] = ' ';

		fcd_copy_buf_and_alerts(mon, buf, warn, fail, disks);

	} while (fcd_lib_monitor_sleep(30) == 0);

	fcd_raid_cleanup(mdstat_buf, fd, pipe_fds);
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

void fcd_raid_test(void)
{
	char uuid_buf[FCD_RAID_UUID_BUF_SIZE];
	struct timespec timeout = { 0, 0 };
	struct fcd_raid_array *array;
	int fd, ret, pipe_fds[2];
	size_t buf_size = 0;
	char *buf = NULL;

	fcd_foreground = 1;

	if (pipe2(pipe_fds, O_CLOEXEC) == -1)
		FCD_ABORT("pipe2: %m\n");

	ret = fcd_raid_regcomp();
	if (ret != 0)
		FCD_ABORT("fcd_raid_regcomp returned %d\n", ret);

	ret = fcd_raid_read_mdadm_conf();
	if (ret != 0)
		FCD_ABORT("fcd_raid_read_mdadm_conf returned %d\n", ret);

	for (array = fcd_raid_list; array != NULL; array = array->next)
		puts(fcd_raid_format_uuid(array->uuid, uuid_buf));

	while (1) {

		fd = open("/proc/mdstat", O_RDONLY | O_CLOEXEC);
		if (fd == -1)
			FCD_ABORT("/proc/mdstat: %m\n");

		ret = fcd_read_all(fd, &buf, &buf_size, 32000, &timeout);
		if (ret <= 0)
			FCD_ABORT("fcd_read_all returned %d\n", ret);

		close(fd);

		ret = fcd_raid_parse_mdstat(buf, pipe_fds);
		if (ret != 0)
			FCD_ABORT("fcd_raid_parse_mdstat returned %d\n", ret);

		for (array = fcd_raid_list; array != NULL;
					    array = array->next) {
			printf("%s%s:\t%s\t%s\t%d/%d\t%s\n", array->name,
			       array->transient ? "(T)" : "",
			       fcd_raid_format_uuid(array->uuid, uuid_buf),
			       format_raid_status(array->array_status),
			       array->current_devs, array->ideal_devs,
			       format_raid_type(array->type));
		}

		puts("Hit Enter to continue");
		getchar();

	}
}
#endif
