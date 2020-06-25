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
	[0].temps[FCD_CONF_TEMP_WARN] = INT_MIN
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
}

