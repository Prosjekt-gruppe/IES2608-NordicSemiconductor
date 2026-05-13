/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stdbool.h>

#define NTN_LOGIC_MODEM_FW_TOKEN "mfw_nrf9151-ntn"

void ntn_logic_strip_at_response_line_end(char *response);
bool ntn_logic_modem_fw_supports_ntn(const char *fw_version);
int ntn_logic_location_precheck(bool have_fix);
