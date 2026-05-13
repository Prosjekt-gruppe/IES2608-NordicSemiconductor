/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "gnss_logic.h"

#include <stddef.h>

uint8_t gnss_logic_count_tracked_satellites(
	const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	uint8_t count = 0U;

	if (pvt == NULL) {
		return 0U;
	}

	for (size_t i = 0; i < (sizeof(pvt->sv) / sizeof(pvt->sv[0])); ++i) {
		if (pvt->sv[i].signal != 0U) {
			count++;
		}
	}

	return count;
}

bool gnss_logic_agnss_processed_has_data(
	const struct nrf_modem_gnss_agnss_data_frame *processed)
{
	if (processed == NULL) {
		return false;
	}

	if (processed->data_flags != 0) {
		return true;
	}

	for (uint8_t i = 0; i < processed->system_count; i++) {
		if ((processed->system[i].sv_mask_ephe != 0) ||
		    (processed->system[i].sv_mask_alm != 0)) {
			return true;
		}
	}

	return false;
}

enum gnss_logic_timeout_action gnss_logic_timeout_action(
	bool gnss_running,
	bool assisted_start_in_progress,
	bool agnss_request_sent,
	bool cloud_request_in_progress,
	bool agnss_pending_timeout_extended)
{
	if (gnss_running &&
	    assisted_start_in_progress &&
	    agnss_request_sent &&
	    cloud_request_in_progress &&
	    !agnss_pending_timeout_extended) {
		return GNSS_LOGIC_TIMEOUT_EXTEND_AGNSS_PENDING;
	}

	return GNSS_LOGIC_TIMEOUT_PUBLISH_TIMEOUT;
}
