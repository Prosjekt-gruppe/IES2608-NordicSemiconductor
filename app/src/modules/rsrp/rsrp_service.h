/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once

int rsrp_service_get(int *rsrp_dbm); 
int rsrp_service_sample_and_publish(void); 
int rsrp_service_start(void);
int rsrp_service_stop(void);
int rsrp_service_init(void);
