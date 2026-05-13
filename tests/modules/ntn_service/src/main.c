/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "ntn_logic.h"

ZTEST(ntn_logic, test_strip_at_response_line_end_removes_crlf)
{
	char response[] = "mfw_nrf9151-ntn_1.0.0\r\nOK";

	ntn_logic_strip_at_response_line_end(response);

	zassert_mem_equal("mfw_nrf9151-ntn_1.0.0", response,
			  sizeof("mfw_nrf9151-ntn_1.0.0"));
}

ZTEST(ntn_logic, test_strip_at_response_line_end_leaves_plain_string)
{
	char response[] = "mfw_nrf9151-ntn_1.0.0";

	ntn_logic_strip_at_response_line_end(response);

	zassert_mem_equal("mfw_nrf9151-ntn_1.0.0", response,
			  sizeof("mfw_nrf9151-ntn_1.0.0"));
}

ZTEST(ntn_logic, test_modem_firmware_token_controls_ntn_support)
{
	zassert_true(ntn_logic_modem_fw_supports_ntn("mfw_nrf9151-ntn_1.0.0"));
	zassert_false(ntn_logic_modem_fw_supports_ntn("mfw_nrf9151_2.0.0"));
	zassert_false(ntn_logic_modem_fw_supports_ntn(NULL));
}

ZTEST(ntn_logic, test_location_precheck_requires_gnss_fix)
{
	zassert_equal(0, ntn_logic_location_precheck(true));
	zassert_equal(-ENODATA, ntn_logic_location_precheck(false));
}

ZTEST_SUITE(ntn_logic, NULL, NULL, NULL, NULL, NULL);
