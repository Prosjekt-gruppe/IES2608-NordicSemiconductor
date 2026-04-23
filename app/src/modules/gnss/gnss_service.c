/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "gnss_service.h"
#include "app_zbus.h"
#include "app_events.h"
#include "cloud_service.h"

#if defined(CONFIG_APP_FIELD_LOG)
#include "field_log.h"
#endif

#include <stdbool.h>
#include <errno.h>
#include <nrf_modem_gnss.h>
#include <net/nrf_cloud_agnss.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(gnss_service, LOG_LEVEL_INF);

/* internal state */
static struct k_work gnss_pvt_work;
static struct k_work_delayable gnss_timeout_work;
static struct k_work agnss_request_work;

static struct nrf_modem_gnss_pvt_data_frame pvt_data;
static struct nrf_modem_gnss_agnss_data_frame agnss_req;

static bool agnss_req_received;
static int64_t gnss_start_time;
static bool first_fix;
static bool gnss_initialized;
static bool gnss_running;
static bool assisted_start_in_progress;
static bool agnss_ready;

/* forward declarations */
static int publish_timeout(void);
static int publish_fix(const struct nrf_modem_gnss_pvt_data_frame *pvt, int64_t ttff_ms);
static int publish_error(int err);
static int publish_error_as_timeout(int err);
static int handle_error(int err);

static uint8_t count_tracked_satellites(const struct nrf_modem_gnss_pvt_data_frame *pvt);
static void log_fix_data(const struct nrf_modem_gnss_pvt_data_frame *pvt);

static void gnss_timeout_work_handler(struct k_work *work);
static void gnss_pvt_work_handler(struct k_work *work);
static void gnss_event_handler(int event);
static void agnss_request_work_handler(struct k_work *work);

static int gnss_prepare_agnss(void);
static int gnss_start_search(void);
static void gnss_restart_with_agnss(void);

/* app event + zbus publish */

static int publish_timeout(void)
{
    struct app_event ev = {
        .type = EVT_TIMEOUT,
    };

    (void)app_zbus_publish_gnss_status(APP_GNSS_STATE_TIMEOUT,
                                       0,
                                       -1,
                                       0.0,
                                       0.0,
                                       0.0f,
                                       0);

    return app_event_put(&ev, K_NO_WAIT);
}

static int publish_error(int err)
{
    (void)app_zbus_publish_gnss_status(APP_GNSS_STATE_ERROR,
                                       err,
                                       -1,
                                       0.0,
                                       0.0,
                                       0.0f,
                                       0);
    return err;
}

static int publish_fix(const struct nrf_modem_gnss_pvt_data_frame *pvt, int64_t ttff_ms)
{
    struct app_event ev = {
        .type = EVT_GNSS_FIX,
    };

    ev.pvt.latitude = pvt->latitude;
    ev.pvt.longitude = pvt->longitude;
    ev.pvt.altitude = pvt->altitude;
    ev.pvt.accuracy = pvt->accuracy;

    (void)app_zbus_publish_gnss_status(APP_GNSS_STATE_FIX,
                                       0,
                                       ttff_ms,
                                       pvt->latitude,
                                       pvt->longitude,
                                       pvt->altitude,
                                       count_tracked_satellites(pvt));

    return app_event_put(&ev, K_NO_WAIT);
}

static int publish_error_as_timeout(int err)
{
    LOG_WRN("GNSS error %d, treating as timeout", err);
    return publish_timeout();
}

static int handle_error(int err)
{
    LOG_ERR("GNSS service error, err=%d", err);
    (void)publish_error(err);
    (void)publish_error_as_timeout(err);
    return err;
}

/* helpers */

static uint8_t count_tracked_satellites(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
    uint8_t count = 0U;

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
    LOG_INF("Accuracy: %.01f m", (double)pvt->accuracy);
}

/* timeout handling */

static void gnss_timeout_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    int err = publish_timeout();
    LOG_INF("gnss timeout fired, publish err=%d", err);
}

static void gnss_pvt_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    uint8_t satellites;
    int64_t ttff_ms = -1;
    int err = nrf_modem_gnss_read(&pvt_data,
                                  sizeof(pvt_data),
                                  NRF_MODEM_GNSS_DATA_PVT);

    if (err) {
        LOG_ERR("nrf_modem_gnss_read failed, err=%d", err);
        (void)handle_error(err);
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

#if defined(CONFIG_APP_FIELD_LOG)
    field_log_note_location(FIELD_LOG_LOCATION_GNSS,
                            pvt_data.latitude,
                            pvt_data.longitude,
                            pvt_data.accuracy);
#endif

    err = publish_fix(&pvt_data, ttff_ms);
    LOG_INF("published GNSS fix status err=%d", err);
}

/* GNSS modem events */

static void gnss_event_handler(int event)
{
    int err;

    switch (event) {
    case NRF_MODEM_GNSS_EVT_PVT:
        k_work_submit(&gnss_pvt_work);
        break;

    case NRF_MODEM_GNSS_EVT_AGNSS_REQ:
        LOG_INF("GNSS requested A-GNSS data");

        err = nrf_modem_gnss_read(&agnss_req,
                                  sizeof(agnss_req),
                                  NRF_MODEM_GNSS_DATA_AGNSS_REQ);
        if (err) {
            LOG_ERR("Failed to read AGNSS request: %d", err);
            return;
        }

        agnss_req_received = true;
        LOG_INF("agnss_req_received set true");

        LOG_INF("Submitting A-GNSS request work");
        k_work_submit(&agnss_request_work);
        break;

    default:
        break;
    }
}

/* public init */

int gnss_service_init(void)
{
    int err;

    first_fix = false;
    gnss_start_time = 0;
    gnss_running = false;
    assisted_start_in_progress = false;
    agnss_ready = false;
    agnss_req_received = false;

    k_work_init(&gnss_pvt_work, gnss_pvt_work_handler);
    k_work_init(&agnss_request_work, agnss_request_work_handler);
    k_work_init_delayable(&gnss_timeout_work, gnss_timeout_work_handler);

    err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
    if (err) {
        LOG_ERR("nrf_modem_gnss_event_handler_set failed, err=%d", err);
        return handle_error(err);
    }

    (void)app_zbus_publish_gnss_status(APP_GNSS_STATE_INITIALIZED,
                                       0,
                                       -1,
                                       0.0,
                                       0.0,
                                       0.0f,
                                       0);

    gnss_initialized = true;
    LOG_INF("GNSS service initialized");

    return 0;
}

/* raw GNSS start helper */

static int gnss_start_search(void)
{
    int err = nrf_modem_gnss_start();

    if (err) {
        LOG_ERR("nrf_modem_gnss_start failed, err=%d", err);
        return err;
    }

    gnss_running = true;
    first_fix = false;
    gnss_start_time = k_uptime_get();

    LOG_INF("GNSS started");
    return 0;
}

/* A-GNSS preparation */

static int gnss_prepare_agnss(void)
{
    int err;

    LOG_INF("gnss_prepare_agnss: entry");

    if (!cloud_service_is_connected()) {
        LOG_ERR("Cloud not connected");
        return -ENOTCONN;
    }

    if (!agnss_req_received) {
        LOG_WRN("No AGNSS request from modem yet");
        return -EAGAIN;
    }

    LOG_INF("Requesting A-GNSS from cloud");

    err = nrf_cloud_agnss_request(&agnss_req);
    LOG_INF("nrf_cloud_agnss_request() -> %d", err);

    if (err) {
        LOG_ERR("A-GNSS request failed: %d", err);
        return err;
    }

    LOG_INF("A-GNSS request sent successfully");
    return 0;
}

/* public assisted start API */

int gnss_service_start_assisted(int32_t timeout_sec)
{
    int err;

    if (!gnss_initialized) {
        LOG_ERR("GNSS not initialized");
        return -EINVAL;
    }

    if (gnss_running) {
        LOG_WRN("GNSS already running");
        return -EALREADY;
    }

    agnss_ready = false;
    agnss_req_received = false;
    assisted_start_in_progress = true;

    LOG_INF("Starting GNSS (preparing for AGNSS)");

    /* Start GNSS first to trigger AGNSS request */
    err = gnss_start_search();
    if (err) {
        return handle_error(err);
    }

    /* Start timeout */
    k_work_reschedule(&gnss_timeout_work, K_SECONDS(timeout_sec));

    return 0;
}

/* work item */

static void agnss_request_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    int err = gnss_prepare_agnss();
    if (err) {
        LOG_ERR("gnss_prepare_agnss failed: %d", err);
    }
}

void gnss_notify_agnss_ready(void)
{
    LOG_INF("gnss_notify_agnss_ready: entry");

    if (!gnss_running) {
        LOG_WRN("Ignoring A-GNSS ready notification: GNSS not running");
        return;
    }

    if (agnss_ready) {
        LOG_INF("A-GNSS already ready, ignoring duplicate notification");
        return;
    }

    LOG_INF("A-GNSS data received and injected into modem");

    agnss_ready = true;
    assisted_start_in_progress = false;

    (void)gnss_service_cancel_timeout();

    LOG_INF("Restarting GNSS after A-GNSS");
    gnss_restart_with_agnss();
}

int gnss_service_start(void)
{
    int err;

    if (!gnss_initialized) {
        LOG_ERR("GNSS service not initialized");
        return -EINVAL;
    }

    if (gnss_running) {
        LOG_WRN("GNSS already running");
        return -EALREADY;
    }

    err = gnss_start_search();
    if (err) {
        return handle_error(err);
    }

    return 0;
}

static void gnss_restart_with_agnss(void)
{
    int err;

    LOG_INF("Restarting GNSS with A-GNSS");

    (void)gnss_service_stop();

    err = gnss_start_search();
    if (err) {
        LOG_ERR("Failed to restart GNSS: %d", err);
    }
}

int gnss_service_stop(void)
{
    int err = nrf_modem_gnss_stop();

    if (!err) {
        gnss_running = false;
        agnss_ready = false;
        assisted_start_in_progress = false;
        LOG_INF("GNSS stopped");
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