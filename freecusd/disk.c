/*
 * Copyright 2014 Ian Pilcher <arequipeno@gmail.com>
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

#include <string.h>
#include <glob.h>

static const char fcd_disk_glob[] =	"/sys/devices/pci0000:00/0000:00:1f.2/"
					"ata[0-9]/host[0-9]/target[0-9]:0:0/"
					"[0-9]:0:0:0/block/sd[a-z]";

static int fcd_disk_glob_errfn(const char *epath, int eerrno)
{
	FCD_WARN("%s: %s\n", epath, strerror(eerrno));
	return 0;
}

/*
 * SYSFS PATH "MAGIC NUMBERS"
 *
 * sysfs paths for disks attached to the ICH10R SATA controller look like:
 *
 * /sys/devices/pci0000:00/0000:00:1f.2/ata4/host3/target3:0:0/3:0:0:0/block/sdb
 * ^         ^         ^         ^         ^         ^         ^         ^   ^
 * |         |         |         |         |         |         |         |   |
 * 0         1         2         3         4         5         6         7   7
 * 0         0         0         0         0         0         0         0   4
 *
 * Port number is read from:
 *
 * /sys/devices/pci0000:00/0000:00:1f.2/ata4/ata_port/ata4/port_no
 * ^         ^         ^         ^      ^  ^ ^       ^^    ^   ^
 * |         |         |         |      |  | |       ||    |   |
 * 0         1         2         3      3  4 4       55    5   6
 * 0         0         0         0      7  0 2       01    6   0
 */

/*
 * Populates fcd_conf_disks (name and port_no) with disks connected to ports
 * 2-6 of the ICH10R SATA controller.  Returns number of disks detected, which
 * may be 0; -1 on error.
 */
int fcd_disk_detect(void)
{
	glob_t disk_glob;
	unsigned port_no;
	int count, ret;
	char *path;
	FILE *fp;
	size_t i;

	ret = glob(fcd_disk_glob, GLOB_NOSORT | GLOB_ONLYDIR,
		   fcd_disk_glob_errfn, &disk_glob);

	if (ret == GLOB_NOMATCH)
		return 0;

	if (ret != 0) {

		if (ret == GLOB_NOSPACE)
			FCD_ERR("Out of memory\n");
		else if (ret == GLOB_ABORTED)
			FCD_ERR("Read error\n");
		else
			FCD_ERR("Unknown glob() error: %d\n", ret);

		return -1;
	}

	for (count = 0, i = 0; i < disk_glob.gl_pathc; ++i) {

		path = disk_glob.gl_pathv[i];

		/* See SYSFS PATH "MAGIC NUMBERS" comment above */
		memcpy(path + 42, "ata_port/", sizeof "ata_port/" - 1);
		memcpy(path + 51, path + 37, 5);
		memcpy(path + 56, "port_no", sizeof "port_no");

		fp = fopen(path, "re");
		if (fp == NULL) {
			FCD_PERROR(path);
			goto error_free_glob;
		}

		ret = fscanf(fp, "%u\n", &port_no);

		if (ret == EOF) {
			if (ferror(fp))
				FCD_PERROR(path);
			else
				FCD_ERR("%s: Unexpected end of file\n",	path);
			goto error_close_fp;
		}

		if (ret != 1) {
			FCD_ERR("%s: Unexpected match count: %d\n", path, ret);
			goto error_close_fp;
		}

		if (fclose(fp) != 0) {
			FCD_PERROR(path);
			goto error_free_glob;
		}

		if (port_no < 2 || port_no > 6)
			continue;

		sprintf(fcd_conf_disks[count].name, "/dev/%s", path + 74);
		fcd_conf_disks[count].port_no = port_no;
		++count;
	}

	globfree(&disk_glob);
	return count;

error_close_fp:
	if (fclose(fp) != 0)
		FCD_PERROR(path);
error_free_glob:
	globfree(&disk_glob);
	return -1;
}
