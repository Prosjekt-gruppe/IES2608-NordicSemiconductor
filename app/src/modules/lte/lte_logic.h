/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stdbool.h>

enum lte_logic_event {
	LTE_LOGIC_EVENT_NONE,
	LTE_LOGIC_EVENT_REG_OK,
	LTE_LOGIC_EVENT_REG_FAIL,
	LTE_LOGIC_EVENT_TN_READY_FOR_PROBE,
};

struct lte_logic_reg_result {
	bool handled;
	bool connected;
	bool probe_pending;
	enum lte_logic_event event;
};

const char *lte_logic_mode_name(int mode);
struct lte_logic_reg_result lte_logic_handle_nw_reg_status(int status,
							   bool probe_pending);
