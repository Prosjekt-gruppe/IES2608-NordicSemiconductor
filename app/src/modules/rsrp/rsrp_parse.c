/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "rsrp_parse.h"

#include <errno.h>
#include <stdio.h>

int rsrp_parse_cesq_rsrp(const char *response, int *rsrp_dbm)
{
	int rxlev;
	int ber;
	int rscp;
	int ecno;
	int rsrq;
	int rsrp_raw;
	int parsed;

	if ((response == NULL) || (rsrp_dbm == NULL)) {
		return -EINVAL;
	}

	parsed = sscanf(response, "+CESQ: %d,%d,%d,%d,%d,%d",
			&rxlev, &ber, &rscp, &ecno, &rsrq, &rsrp_raw);
	if (parsed != 6) {
		return -EIO;
	}

	if (rsrp_raw == 255) {
		return -ENOENT;
	}

	if (rsrp_raw == 0) {
		*rsrp_dbm = -141;
		return 0;
	}

	*rsrp_dbm = rsrp_raw - 141;

	return 0;
}
