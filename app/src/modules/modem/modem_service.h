/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once

#include "app_types.h"


struct udp_test_cfg {
    size_t payload_len;
    int interval_ms;
    int count;
};


int modem_service_init(void);
int modem_service_prepare_profiles(struct app_ctx *ctx);
int modem_service_switch_to_tn(void);
int modem_service_switch_to_ntn(void);
int modem_service_udp_send_test(void);
int modem_service_udp_send_burst(const struct udp_test_cfg *cfg);

