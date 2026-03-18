/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#include "app_events.h"

ZBUS_OBS_DECLARE(app_fsm_sub);

ZBUS_CHAN_DEFINE(app_evt_chan,
                 struct app_event,
                 NULL,
                 NULL,
                 ZBUS_OBSERVERS(app_fsm_sub),
                 ZBUS_MSG_INIT(.type = EVT_BOOT));

int app_event_put(const struct app_event *ev, k_timeout_t timeout)
{
    return zbus_chan_pub(&app_evt_chan, ev, timeout);
}
