/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once

#include "app_types.h"
#include <modem/lte_lc.h>

struct udp_test_cfg {
    size_t payload_len;
    int interval_ms;
    int count;
};


int modem_service_init(void);

/* RAT switch calls are asynchronous because the modem needs time between AT steps. */
int modem_service_switch_to_tn(void);
int modem_service_switch_to_ntn(void);

/* UDP helpers are only used to check that the active network context can send data. */
int modem_service_udp_send_test(void);
int modem_service_udp_send_burst(const struct udp_test_cfg *cfg);
int modem_service_conn_eval_get(struct lte_lc_conn_eval_params *params);

