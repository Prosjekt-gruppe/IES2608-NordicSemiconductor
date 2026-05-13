/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "lte_logic.h"

#include <modem/lte_lc.h>

const char *lte_logic_mode_name(int mode)
{
	switch (mode) {
	case LTE_LC_LTE_MODE_NONE:
		return "NONE";
	case LTE_LC_LTE_MODE_LTEM:
		return "LTE-M";
	case LTE_LC_LTE_MODE_NBIOT:
		return "NB-IoT";
	default:
		return "UNKNOWN";
	}
}

struct lte_logic_reg_result lte_logic_handle_nw_reg_status(int status,
							   bool probe_pending)
{
	struct lte_logic_reg_result result = {
		.handled = true,
		.connected = false,
		.probe_pending = probe_pending,
		.event = LTE_LOGIC_EVENT_NONE,
	};

	switch (status) {
	case LTE_LC_NW_REG_REGISTERED_HOME:
	case LTE_LC_NW_REG_REGISTERED_ROAMING:
		result.connected = true;
		result.probe_pending = false;
		result.event = probe_pending ? LTE_LOGIC_EVENT_TN_READY_FOR_PROBE :
					       LTE_LOGIC_EVENT_REG_OK;
		break;

	case LTE_LC_NW_REG_NOT_REGISTERED:
	case LTE_LC_NW_REG_REGISTRATION_DENIED:
	case LTE_LC_NW_REG_UNKNOWN:
	case LTE_LC_NW_REG_UICC_FAIL:
		result.connected = false;
		result.probe_pending = probe_pending;
		result.event = LTE_LOGIC_EVENT_REG_FAIL;
		break;

	default:
		result.handled = false;
		break;
	}

	return result;
}
