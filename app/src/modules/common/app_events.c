/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 




#include "app_events.h"

K_MSGQ_DEFINE(app_evt_q, sizeof(struct app_event), 16, 4);


/* common app event helper function */
int app_event_put(const struct app_event *ev, k_timeout_t timeout)
{
    return k_msgq_put(&app_evt_q, ev, timeout);
}

int app_event_get(struct app_event *ev, k_timeout_t timeout)
{
    return k_msgq_get(&app_evt_q, ev, timeout);
}