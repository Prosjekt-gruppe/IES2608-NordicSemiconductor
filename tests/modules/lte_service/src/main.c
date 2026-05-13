/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <modem/lte_lc.h>
#include <zephyr/ztest.h>

#include "lte_logic.h"

ZTEST(lte_logic, test_registered_home_publishes_reg_ok)
{
	struct lte_logic_reg_result result =
		lte_logic_handle_nw_reg_status(LTE_LC_NW_REG_REGISTERED_HOME, false);

	zassert_true(result.handled);
	zassert_true(result.connected);
	zassert_false(result.probe_pending);
	zassert_equal(LTE_LOGIC_EVENT_REG_OK, result.event);
}

ZTEST(lte_logic, test_registered_roaming_while_probe_pending_publishes_probe_ready)
{
	struct lte_logic_reg_result result =
		lte_logic_handle_nw_reg_status(LTE_LC_NW_REG_REGISTERED_ROAMING, true);

	zassert_true(result.handled);
	zassert_true(result.connected);
	zassert_false(result.probe_pending);
	zassert_equal(LTE_LOGIC_EVENT_TN_READY_FOR_PROBE, result.event);
}

ZTEST(lte_logic, test_registration_failure_publishes_reg_fail)
{
	struct lte_logic_reg_result result =
		lte_logic_handle_nw_reg_status(LTE_LC_NW_REG_REGISTRATION_DENIED, true);

	zassert_true(result.handled);
	zassert_false(result.connected);
	zassert_true(result.probe_pending);
	zassert_equal(LTE_LOGIC_EVENT_REG_FAIL, result.event);
}

ZTEST(lte_logic, test_transient_registration_status_is_ignored)
{
	struct lte_logic_reg_result result =
		lte_logic_handle_nw_reg_status(LTE_LC_NW_REG_SEARCHING, true);

	zassert_false(result.handled);
	zassert_false(result.connected);
	zassert_true(result.probe_pending);
	zassert_equal(LTE_LOGIC_EVENT_NONE, result.event);
}

ZTEST(lte_logic, test_mode_names_cover_known_modes)
{
	zassert_mem_equal("NONE", lte_logic_mode_name(LTE_LC_LTE_MODE_NONE), sizeof("NONE"));
	zassert_mem_equal("LTE-M", lte_logic_mode_name(LTE_LC_LTE_MODE_LTEM), sizeof("LTE-M"));
	zassert_mem_equal("NB-IoT", lte_logic_mode_name(LTE_LC_LTE_MODE_NBIOT),
			  sizeof("NB-IoT"));
	zassert_mem_equal("UNKNOWN", lte_logic_mode_name(-1), sizeof("UNKNOWN"));
}

ZTEST_SUITE(lte_logic, NULL, NULL, NULL, NULL, NULL);
