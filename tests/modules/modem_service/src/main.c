/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>

#include "modem_logic.h"

ZTEST(modem_logic, test_system_mode_command_selects_tn_and_ntn)
{
	zassert_mem_equal("AT%XSYSTEMMODE=1,0,0,0,0",
			  modem_logic_system_mode_cmd(MODEM_LOGIC_ACCESS_TN),
			  sizeof("AT%XSYSTEMMODE=1,0,0,0,0"));
	zassert_mem_equal("AT%XSYSTEMMODE=0,0,0,0,1",
			  modem_logic_system_mode_cmd(MODEM_LOGIC_ACCESS_NTN),
			  sizeof("AT%XSYSTEMMODE=0,0,0,0,1"));
}

ZTEST(modem_logic, test_system_mode_command_rejects_invalid_mode)
{
	zassert_is_null(modem_logic_system_mode_cmd((enum modem_logic_access_mode)-1));
}

ZTEST(modem_logic, test_udp_payload_length_accepts_buffer_boundary)
{
	zassert_true(modem_logic_udp_payload_len_valid(0));
	zassert_true(modem_logic_udp_payload_len_valid(MODEM_LOGIC_UDP_MAX_PAYLOAD_LEN));
	zassert_false(modem_logic_udp_payload_len_valid(MODEM_LOGIC_UDP_MAX_PAYLOAD_LEN + 1U));
}

ZTEST_SUITE(modem_logic, NULL, NULL, NULL, NULL, NULL);
