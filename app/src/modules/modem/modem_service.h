/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once

int modem_service_init(void);
int modem_service_prepare_ltem(void);
int modem_service_connect_async(void);
int modem_service_start_ltem_monitor(void);
int modem_service_stop_ltem_monitor(void);
