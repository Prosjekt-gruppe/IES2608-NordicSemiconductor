/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 
#include "gnss_service.h"
#include "app_zbus.h"

#include <stdbool.h>
#include <nrf_modem_gnss.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(gnss_service, LOG_LEVEL_INF);

static struct k_work gnss_pvt_work;
static struct k_work_delayable gnss_timeout_work;
static struct nrf_modem_gnss_pvt_data_frame pvt_data;
static int64_t gnss_start_time;
static bool first_fix;

static int publish_error(int err)
{
    (void)app_zbus_publish_gnss_status(APP_GNSS_STATE_ERROR, err, -1, 0.0, 0.0, 0.0f, 0);

    return err;
}

static uint8_t count_tracked_satellites(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
    uint8_t count = 0;

    for (size_t i = 0; i < ARRAY_SIZE(pvt->sv); ++i) {
        if (pvt->sv[i].signal != 0U) {
            count++;
        }
    }

    return count;
}

static void log_fix_data(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
    LOG_INF("Latitude: %.06f", pvt->latitude);
    LOG_INF("Longitude: %.06f", pvt->longitude);
    LOG_INF("Altitude: %.01f m", (double)pvt->altitude);
}

static void gnss_timeout_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    int err = app_zbus_publish_gnss_status(APP_GNSS_STATE_TIMEOUT, 0, -1, 0.0, 0.0, 0.0f, 0);

    LOG_INF("gnss timeout fired, publish err=%d", err);
}

static void gnss_pvt_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    uint8_t satellites;
    int64_t ttff_ms = -1;
    int err = nrf_modem_gnss_read(&pvt_data, sizeof(pvt_data), NRF_MODEM_GNSS_DATA_PVT);

    if (err) {
        LOG_ERR("nrf_modem_gnss_read failed, err=%d", err);
        (void)publish_error(err);
        return;
    }

    satellites = count_tracked_satellites(&pvt_data);
    LOG_INF("GNSS search active, satellites tracked: %u", satellites);

    if ((pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) == 0U) {
        return;
    }

    if (!first_fix) {
        ttff_ms = k_uptime_get() - gnss_start_time;
        first_fix = true;
        LOG_INF("Time to first fix: %lld s", (long long)(ttff_ms / 1000));
    }

    log_fix_data(&pvt_data);
    err = app_zbus_publish_gnss_status(APP_GNSS_STATE_FIX, 0, ttff_ms,
                                       pvt_data.latitude, pvt_data.longitude,
                                       pvt_data.altitude, satellites);
    LOG_INF("published GNSS fix status err=%d", err);
}

static void gnss_event_handler(int event)
{
    if (event == NRF_MODEM_GNSS_EVT_PVT) {
        k_work_submit(&gnss_pvt_work);
    }
}

int gnss_service_init(void)
{
    int err;

    first_fix = false;
    gnss_start_time = 0;

    k_work_init(&gnss_pvt_work, gnss_pvt_work_handler);
    k_work_init_delayable(&gnss_timeout_work, gnss_timeout_work_handler);

    err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
    if (err) {
        LOG_ERR("nrf_modem_gnss_event_handler_set failed, err=%d", err);
        return publish_error(err);
    }

    (void)app_zbus_publish_gnss_status(APP_GNSS_STATE_INITIALIZED, 0, -1,
                                       0.0, 0.0, 0.0f, 0);
    return 0;
}

int gnss_service_start(void)
{
    int err = nrf_modem_gnss_start();

    if (err) {
        LOG_ERR("nrf_modem_gnss_start failed, err=%d", err);
        return publish_error(err);
    }

    first_fix = false;
    gnss_start_time = k_uptime_get();
    (void)app_zbus_publish_gnss_status(APP_GNSS_STATE_STARTED, 0, -1,
                                       0.0, 0.0, 0.0f, 0);
    return 0;
}

int gnss_service_stop(void)
{
    int err = nrf_modem_gnss_stop();

    if (!err) {
        (void)app_zbus_publish_gnss_status(APP_GNSS_STATE_IDLE, 0, -1,
                                           0.0, 0.0, 0.0f, 0);
    }

    return err;
}

int gnss_service_start_timeout(int32_t timeout_sec)
{
    return k_work_reschedule(&gnss_timeout_work, K_SECONDS(timeout_sec));
}

int gnss_service_cancel_timeout(void)
{
    return k_work_cancel_delayable(&gnss_timeout_work);
}
