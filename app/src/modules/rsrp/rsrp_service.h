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

/*
 * Monitor mode watches LTE-M for fallback. Probe mode is used from NTN to see
 * if LTE-M has recovered enough to switch back.
 */
int rsrp_service_start_monitor(void);
int rsrp_service_start_ntn_monitor(void);
int rsrp_service_start_probe(uint8_t samples);
int rsrp_service_stop(void);
int rsrp_service_init(void);
void rsrp_service_set_motion_hint(bool moving, uint32_t speed_mm_s,
				  uint32_t linear_accel_mg);
