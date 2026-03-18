/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once

#include <stdint.h>

#include <zephyr/zbus/zbus.h>

enum app_gnss_state {
    APP_GNSS_STATE_IDLE = 0,
    APP_GNSS_STATE_INITIALIZED,
    APP_GNSS_STATE_STARTED,
    APP_GNSS_STATE_FIX,
    APP_GNSS_STATE_TIMEOUT,
    APP_GNSS_STATE_ERROR,
};

struct app_gnss_status {
    enum app_gnss_state state;
    int err;
    int64_t time_to_first_fix_ms;
    double latitude;
    double longitude;
    float altitude;
    uint8_t tracked_satellites;
};

ZBUS_CHAN_DECLARE(gnss_status_chan);

int app_zbus_publish_gnss_status(enum app_gnss_state state, int err,
                                 int64_t time_to_first_fix_ms, double latitude,
                                 double longitude, float altitude,
                                 uint8_t tracked_satellites);
