/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once

#include "app_types.h"

/*
 * NTN connect needs a recent GNSS fix. The NTN modem library asks for device
 * location during attach, and attach fails if we cannot provide one.
 */
int ntn_service_connect(struct app_ctx *ctx);
int ntn_service_stop(void);
int ntn_service_switch_to_tn(void);
int modem_service_switch_to_ntn(void);
int ntn_service_init(void);
