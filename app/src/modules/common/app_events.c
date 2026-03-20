/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#include "app_events.h"

LOG_MODULE_REGISTER(app_events, LOG_LEVEL_INF); 

ZBUS_OBS_DECLARE(app_fsm_sub);

ZBUS_CHAN_DEFINE(app_evt_chan,
                 struct app_event,
                 NULL,
                 NULL,
                 ZBUS_OBSERVERS(app_fsm_sub),
                 ZBUS_MSG_INIT(.type = EVT_BOOT));



const char *app_evt_name(enum app_evt_type type)
{
    switch (type) {
    case EVT_BOOT: return "EVT_BOOT";
    case EVT_REG_OK: return "EVT_REG_OK";
    case EVT_REG_FAIL: return "EVT_REG_FAIL";
    case EVT_GNSS_FIX: return "EVT_GNSS_FIX";
    case EVT_GNSS_TIMEOUT: return "EVT_GNSS_TIMEOUT";
    case EVT_NTN_REG_FAIL: return "EVT_NTN_REG_FAIL";
    case EVT_NTN_TIMEOUT: return "EVT_NTN_TIMEOUT";
    case EVT_TIMEOUT: return "EVT_TIMEOUT";
    case EVT_RSRP_UPDATE: return "EVT_RSRP_UPDATE";
    case EVT_LTE_POOR: return "EVT_LTE_POOR";
    case EVT_BACKOFF_TIMEOUT: return "EVT_BACKOFF_TIMEOUT";
    default: return "EVT_UNKNOWN";
    }
}

int app_event_put(const struct app_event *ev, k_timeout_t timeout)
{
    LOG_INF("app_event_put: type=%s", app_evt_name(ev->type)); 
    return zbus_chan_pub(&app_evt_chan, ev, timeout);
}