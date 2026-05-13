/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "modem_logic.h"

const char *modem_logic_system_mode_cmd(enum modem_logic_access_mode mode)
{
	switch (mode) {
	case MODEM_LOGIC_ACCESS_TN:
		return "AT%XSYSTEMMODE=1,0,0,0,0";
	case MODEM_LOGIC_ACCESS_NTN:
		return "AT%XSYSTEMMODE=0,0,0,0,1";
	default:
		return NULL;
	}
}

bool modem_logic_udp_payload_len_valid(size_t payload_len)
{
	return payload_len <= MODEM_LOGIC_UDP_MAX_PAYLOAD_LEN;
}
