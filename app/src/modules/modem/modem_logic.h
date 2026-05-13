/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#define MODEM_LOGIC_UDP_MAX_PAYLOAD_LEN 256U

enum modem_logic_access_mode {
	MODEM_LOGIC_ACCESS_TN,
	MODEM_LOGIC_ACCESS_NTN,
};

const char *modem_logic_system_mode_cmd(enum modem_logic_access_mode mode);
bool modem_logic_udp_payload_len_valid(size_t payload_len);
