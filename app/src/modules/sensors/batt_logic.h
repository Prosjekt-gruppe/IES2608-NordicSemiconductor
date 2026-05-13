/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stdint.h>

#define BATT_LOGIC_STATUS_COMPLETE_MASK (1 << 1)
#define BATT_LOGIC_STATUS_TRICKLE_MASK (1 << 2)
#define BATT_LOGIC_STATUS_CC_MASK (1 << 3)
#define BATT_LOGIC_STATUS_CV_MASK (1 << 4)

const char *batt_logic_level_string(int64_t voltage_mv);
const char *batt_logic_charge_state_string(int32_t status);
