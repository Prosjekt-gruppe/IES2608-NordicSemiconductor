/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <nrf_modem_gnss.h>

enum gnss_logic_timeout_action {
	GNSS_LOGIC_TIMEOUT_PUBLISH_TIMEOUT,
	GNSS_LOGIC_TIMEOUT_EXTEND_AGNSS_PENDING,
};

uint8_t gnss_logic_count_tracked_satellites(
	const struct nrf_modem_gnss_pvt_data_frame *pvt);

bool gnss_logic_agnss_processed_has_data(
	const struct nrf_modem_gnss_agnss_data_frame *processed);

enum gnss_logic_timeout_action gnss_logic_timeout_action(
	bool gnss_running,
	bool assisted_start_in_progress,
	bool agnss_request_sent,
	bool cloud_request_in_progress,
	bool agnss_pending_timeout_extended);
