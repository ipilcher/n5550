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
#include <limits.h>
#include <errno.h>

/*
 * Configuration file name
 */
const char *fcd_conf_file_name = NULL;

/*
 * RAID disk configuration
 */
unsigned fcd_conf_disk_count;

struct fcd_raid_disk fcd_conf_disks[FCD_MAX_DISK_COUNT] = {

	/*
	 * Used by fcd_conf_disk_idx() to determine whether the [freecusd]
	 * section has been processed yet.  (Will be set to default/provided
	 * value of hdd_temp_warn.)
	 */
	[0] = { .temp_warn = INT_MIN }
};

/*
 * Post-parse callback for monitor enable/disable booleans
 */
int fcd_conf_mon_enable_cb(cip_err_ctx *ctx __attribute__((unused)),
			   const cip_ini_value *value,
			   const cip_ini_sect *sect __attribute__((unused)),
			   const cip_ini_file *file __attribute__((unused)),
			   void *post_parse_data)
{
	struct fcd_monitor *mon;
	const bool *b;

	mon = (struct fcd_monitor *)post_parse_data;
	b = (const bool *)(value->value);

	mon->enabled = *b;
	if (!mon->enabled) {
		FCD_INFO("%s monitor disabled by configuration setting\n",
			 mon->name);
	}

	return 0;
}

/*
 * Finds the RAID disk index corresponding to a [raid_disk:X] instance.  Returns
 * -1 on error (invalid disk number) or 1 if the [freecusd] section has not yet
 * been processed.  If the disk number (X) is valid, and the [freecusd] section
 * has been processed, 0 is returned and either the index or UINT_MAX is written
 * to *index; UINT_MAX indicates that no disk was detected in position X.
 *
 * Thus callers must check both the return value of this function and the value
 * returned in *index.
 */
static int fcd_conf_disk_idx(cip_err_ctx *ctx, unsigned *index,
			     const cip_ini_sect *sect)
{
	static const cip_ini_sect *current_sect = NULL;
	static unsigned current_index;

	unsigned i;
	char disk;

	/*
	 * Don't process any [raid_disk:*] settings until the [freecusd]
	 * section has been processed.
	 */
	if (fcd_conf_disks[0].temp_warn == INT_MIN)
		return 1;

	/*
	 * Options in an instance of a multi-instance section are called
	 * sequentially, so it makes sense to "cache" this.  It also prevents
	 * issuing multiple warnings for a missing disk's [raid_disk:X] section.
	 */
	if (sect == current_sect) {
		*index = current_index;
		return 0;
	}

	disk = sect->node.name[0];
	if (disk < '1' || disk > '5' || sect->node.name[1] != 0) {
		cip_err(ctx, "Invalid disk number: %s (must be 1-5)",
			sect->node.name);
		return -1;
	}

	for (i = 0; i < fcd_conf_disk_count; ++i) {

		/* Disk 1 is on port 2, etc.  (Port 1 is the DOM.) */
		if ((unsigned)(disk - '0') == fcd_conf_disks[i].port_no - 1)
			break;
	}

	if (i == fcd_conf_disk_count) {
		cip_err(ctx, "Ignoring section: [raid_disk:%s]: No such disk",
			sect->node.name);
		i = UINT_MAX;
	}

	current_sect = sect;
	current_index = i;
	*index = i;
	return 0;
}

/*
 * Post-parse callback for disk-specific booleans
 */
int fcd_conf_disk_bool_cb(cip_err_ctx *ctx __attribute__((unused)),
			  const cip_ini_value *value, const cip_ini_sect *sect,
			  const cip_ini_file *file __attribute__((unused)),
			  void *post_parse_data)
{
	bool b, *p;
	unsigned i;
	int ret;

	ret = fcd_conf_disk_idx(ctx, &i, sect);
	if (ret != 0 || i == UINT_MAX)
		return ret;

	p = (bool *)(value->value);
	b = *p;

	p = fcd_conf_disk_member(post_parse_data, i);
	*p = b;

	return 0;
}

/*
 * Post-parse callback helper for disk-specific integers
 */
int fcd_conf_disk_int_cb_help(cip_err_ctx *ctx, const cip_ini_value *value,
			      const cip_ini_sect *sect,
			      const cip_ini_file *file __attribute__((unused)),
			      void *post_parse_data, int *result)
{
	unsigned u;
	int i, *p;

	i = fcd_conf_disk_idx(ctx, &u, sect);
	if (i != 0 || u == UINT_MAX)
		return i;

	p = (int *)(value->value);
	i = *p;

	p = fcd_conf_disk_member(post_parse_data, u);
	*p = i;

	if (result != NULL)
		*result = i;

	return 0;
}

/*
 * Parse the configuration file
 */
static int fcd_conf_warn(const char *msg)
{
	FCD_WARN("%s\n", msg);
	return 0;
}

static int fcd_conf_per_mon(cip_err_ctx *ctx, struct fcd_monitor *mon,
			    cip_sect_schema *freecusd_schema,
			    cip_sect_schema *raiddisk_schema)
{
	int ret;

	if (mon->enabled_opt_name != NULL) {

		ret = cip_opt_schema_new1(ctx, freecusd_schema,
					  mon->enabled_opt_name,
					  CIP_OPT_TYPE_BOOL,
					  fcd_conf_mon_enable_cb, mon, 0, NULL);
		if (ret == -1)
			return -1;
	}

	if (mon->freecusd_opts != NULL) {

		ret = cip_opt_schema_new3(ctx, freecusd_schema,
					  mon->freecusd_opts);
		if (ret == -1)
			return -1;
	}

	if (mon->raiddisk_opts != NULL) {

		ret = cip_opt_schema_new3(ctx, raiddisk_schema,
					  mon->raiddisk_opts);
		if (ret == -1)
			return -1;
	}

	return 0;
}

#if 0
static void fcd_conf_dump_raid_disks(void)
{
	unsigned i;

	if (!fcd_err_foreground)
		return;

	for (i = 0; i < fcd_conf_disk_count; ++i) {
		printf("%s:\n", fcd_conf_disks[i].name);
		printf("\tPort number: %u\n", fcd_conf_disks[i].port_no);
		printf("\tS.M.A.R.T. monitor disabled: %s\n",
		       fcd_conf_disks[i].smart_ignore ? "true" : "false");
		printf("\tHDD temperature monitor disabled: %s\n",
		       fcd_conf_disks[i].temp_ignore ? "true" : "false");
		printf("\tWarning temperature: %d\n",
		       fcd_conf_disks[i].temp_warn);
		printf("\tCritical temperature: %d\n",
		       fcd_conf_disks[i].temp_crit);
	}

	exit(0);
}
#endif

void fcd_conf_parse(void)
{
	cip_sect_schema *freecusd_schema, *raiddisk_schema;
	cip_file_schema *file_schema;
	const char *cfg_file_name;
	struct fcd_monitor **mon;
	cip_ini_file *file;
	cip_err_ctx ctx;
	FILE *stream;
	int ret;

	ret = fcd_disk_detect();
	if (ret < 1) {
		FCD_WARN("Failed to auto-detect RAID disks\n");
		fcd_conf_disk_count = 0;
	}
	else {
		fcd_conf_disk_count = ret;
	}

	cip_err_ctx_init(&ctx);

	file_schema = cip_file_schema_new1(&ctx);
	if (file_schema == NULL)
		FCD_FATAL("%s\n", cip_last_err(&ctx));

	freecusd_schema = cip_sect_schema_new1(&ctx, file_schema, "freecusd",
					       CIP_SECT_CREATE);
	if (freecusd_schema == NULL)
		FCD_FATAL("%s\n", cip_last_err(&ctx));

	raiddisk_schema = cip_sect_schema_new1(&ctx, file_schema, "raid_disk",
					       CIP_SECT_MULTIPLE);
	if (raiddisk_schema == NULL)
		FCD_FATAL("%s\n", cip_last_err(&ctx));

	for (mon = fcd_monitors; *mon != NULL; ++mon) {

		ret = fcd_conf_per_mon(&ctx, *mon, freecusd_schema,
				       raiddisk_schema);
		if (ret == -1)
			FCD_FATAL("%s\n", cip_last_err(&ctx));
	}

	cfg_file_name = (fcd_conf_file_name != NULL) ? fcd_conf_file_name :
						"/etc/freecusd.conf";
	stream = fopen(cfg_file_name, "re");
	if (stream == NULL) {
		if (fcd_conf_file_name == NULL && errno == ENOENT)
			cfg_file_name = "(none)";
		else
			FCD_FATAL("Failed to open configuration file: %s: %m\n",
				  cfg_file_name);
	}

	file = cip_parse_stream(&ctx, stream, cfg_file_name, file_schema,
				fcd_conf_warn);
	if (file == NULL)
		FCD_FATAL("%s\n", cip_last_err(&ctx));

	if (stream != NULL && fclose(stream) == EOF)
		FCD_PERROR(cfg_file_name);

	cip_ini_file_free(file);
	cip_file_schema_free(file_schema);
	cip_err_ctx_fini(&ctx);
#if 0
	fcd_conf_dump_raid_disks();
#endif
}

