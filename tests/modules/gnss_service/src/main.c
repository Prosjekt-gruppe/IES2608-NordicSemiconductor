/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include "gnss_logic.h"

ZTEST(gnss_logic, test_count_tracked_satellites_counts_nonzero_signal_entries)
{
	struct nrf_modem_gnss_pvt_data_frame pvt = {0};

	pvt.sv[0].signal = 18U;
	pvt.sv[3].signal = 0U;
	pvt.sv[7].signal = 42U;

	zassert_equal(2, gnss_logic_count_tracked_satellites(&pvt));
}

ZTEST(gnss_logic, test_count_tracked_satellites_handles_empty_or_null_pvt)
{
	struct nrf_modem_gnss_pvt_data_frame pvt = {0};

	zassert_equal(0, gnss_logic_count_tracked_satellites(&pvt));
	zassert_equal(0, gnss_logic_count_tracked_satellites(NULL));
}

ZTEST(gnss_logic, test_agnss_processed_has_data_detects_global_flags)
{
	struct nrf_modem_gnss_agnss_data_frame processed = {
		.data_flags = 1U,
	};

	zassert_true(gnss_logic_agnss_processed_has_data(&processed));
}

ZTEST(gnss_logic, test_agnss_processed_has_data_detects_satellite_masks)
{
	struct nrf_modem_gnss_agnss_data_frame processed = {
		.system_count = 2U,
	};

	processed.system[1].sv_mask_alm = 0x10U;

	zassert_true(gnss_logic_agnss_processed_has_data(&processed));
}

ZTEST(gnss_logic, test_agnss_processed_has_data_rejects_empty_or_null_data)
{
	struct nrf_modem_gnss_agnss_data_frame processed = {0};

	zassert_false(gnss_logic_agnss_processed_has_data(&processed));
	zassert_false(gnss_logic_agnss_processed_has_data(NULL));
}

ZTEST(gnss_logic, test_timeout_extends_once_while_assisted_request_is_pending)
{
	zassert_equal(GNSS_LOGIC_TIMEOUT_EXTEND_AGNSS_PENDING,
		      gnss_logic_timeout_action(true, true, true, true, false));

	zassert_equal(GNSS_LOGIC_TIMEOUT_PUBLISH_TIMEOUT,
		      gnss_logic_timeout_action(true, true, true, true, true));
}

ZTEST(gnss_logic, test_timeout_publishes_when_not_waiting_for_agnss)
{
	zassert_equal(GNSS_LOGIC_TIMEOUT_PUBLISH_TIMEOUT,
		      gnss_logic_timeout_action(false, true, true, true, false));
	zassert_equal(GNSS_LOGIC_TIMEOUT_PUBLISH_TIMEOUT,
		      gnss_logic_timeout_action(true, false, true, true, false));
	zassert_equal(GNSS_LOGIC_TIMEOUT_PUBLISH_TIMEOUT,
		      gnss_logic_timeout_action(true, true, false, true, false));
	zassert_equal(GNSS_LOGIC_TIMEOUT_PUBLISH_TIMEOUT,
		      gnss_logic_timeout_action(true, true, true, false, false));
}

ZTEST_SUITE(gnss_logic, NULL, NULL, NULL, NULL, NULL);
