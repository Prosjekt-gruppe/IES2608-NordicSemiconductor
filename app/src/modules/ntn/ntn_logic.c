/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "ntn_logic.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

void ntn_logic_strip_at_response_line_end(char *response)
{
	if (response == NULL) {
		return;
	}

	for (char *p = response; *p != '\0'; p++) {
		if ((*p == '\r') || (*p == '\n')) {
			*p = '\0';
			return;
		}
	}
}

bool ntn_logic_modem_fw_supports_ntn(const char *fw_version)
{
	if (fw_version == NULL) {
		return false;
	}

	return strstr(fw_version, NTN_LOGIC_MODEM_FW_TOKEN) != NULL;
}

int ntn_logic_location_precheck(bool have_fix)
{
	return have_fix ? 0 : -ENODATA;
}
