/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once

#include <stdint.h>

int gnss_service_init(void);
int gnss_service_start(void);
int gnss_service_stop(void);
int gnss_service_start_timeout(int32_t timeout_sec);
int gnss_service_cancel_timeout(void);