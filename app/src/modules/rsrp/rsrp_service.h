/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once
#include <stdint.h>

int rsrp_service_get(int *rsrp_dbm);
int rsrp_service_sample_and_publish(void); 
int rsrp_service_start_monitor(void);
int rsrp_service_stop(void);
int rsrp_service_init(void);
int rsrp_service_start_probe(uint8_t samples);