#include <inttypes.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>

#include <atasmart.h>

#include "status.h"

#define ZERO_C_MKELVIN		273150

int main(int argc __attribute__((unused)), char *argv[])
{
	uint64_t mkelvin;
	int status, temp;
	SkSmartOverall overall;
	SkDisk *disk;

	if (sk_disk_open(argv[1], &disk) < 0) {
		perror(argv[1]);
		exit(EXIT_FAILURE);
	}

	if (sk_disk_smart_read_data(disk) < 0) {
		perror(argv[1]);
		exit(EXIT_FAILURE);
	}

	if (sk_disk_smart_get_overall(disk, &overall) < 0) {
		perror(argv[1]);
		exit(EXIT_FAILURE);
	}

	if (sk_disk_smart_get_temperature(disk, &mkelvin) < 0) {
		perror(argv[1]);
		exit(EXIT_FAILURE);
	}

	sk_disk_free(disk);

	switch (overall) {

		case SK_SMART_OVERALL_GOOD:
		case SK_SMART_OVERALL_BAD_ATTRIBUTE_IN_THE_PAST:
			status = FCD_SMART_OK;
			break;

		case SK_SMART_OVERALL_BAD_SECTOR:
		case SK_SMART_OVERALL_BAD_ATTRIBUTE_NOW:
			status = FCD_SMART_WARN;
			break;

		case SK_SMART_OVERALL_BAD_SECTOR_MANY:
		case SK_SMART_OVERALL_BAD_STATUS:
			status = FCD_SMART_FAIL;
			break;

		default:
			fprintf(stderr, "Unknown SMART status: %d\n", overall);
			exit(EXIT_FAILURE);
	}

	if (mkelvin > (uint64_t)INT_MAX) {
		fprintf(stderr,
			"Temperature (%" PRIu64 " mK) out of range\n",
			mkelvin);
		exit(EXIT_FAILURE);
	}

	temp = mkelvin;
	temp -= ZERO_C_MKELVIN;
	temp /= 1000;

	printf("%d\n%d\n", status, temp);

	exit(EXIT_SUCCESS);
}
