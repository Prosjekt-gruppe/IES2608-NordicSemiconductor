/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "batt_logic.h"

const char *batt_logic_level_string(int64_t voltage_mv)
{
	if (voltage_mv >= 4100) {
		return "full";
	}

	if (voltage_mv >= 3850) {
		return "high";
	}

	if (voltage_mv >= 3600) {
		return "medium";
	}

	if (voltage_mv >= 3400) {
		return "low";
	}

	return "critical";
}

const char *batt_logic_charge_state_string(int32_t status)
{
	if ((status & BATT_LOGIC_STATUS_COMPLETE_MASK) != 0) {
		return "complete";
	}

	if ((status & BATT_LOGIC_STATUS_TRICKLE_MASK) != 0) {
		return "trickle";
	}

	if ((status & BATT_LOGIC_STATUS_CC_MASK) != 0) {
		return "charging";
	}

	if ((status & BATT_LOGIC_STATUS_CV_MASK) != 0) {
		return "topping-off";
	}

	return "idle";
}
