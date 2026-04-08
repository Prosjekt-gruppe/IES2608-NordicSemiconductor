/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once

#include <stdbool.h>
#include <stdint.h>

int rsrp_service_get(int *rsrp_dbm);
int rsrp_service_sample_and_publish(void);
int rsrp_service_start(void);
int rsrp_service_stop(void);
int rsrp_service_init(void);
void rsrp_service_set_motion_hint(bool moving, uint32_t speed_mm_s,
				  uint32_t linear_accel_mg);
