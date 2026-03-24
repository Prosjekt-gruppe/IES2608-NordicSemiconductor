/*
 * GNSS_SERVICE:
 * --> GNSS start/stop
 * --> timeout handling
 * --> PVT/fix handling
 * --> Detect A-GNSS request from the modem
 */

#include "gnss_service.h"
#include "app_events.h"

#include <stdbool.h>
#include <nrf_modem_gnss.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(gnss_service, LOG_LEVEL_INF);

static struct k_work gnss_pvt_work;
static struct k_work agnss_req_work;
static struct k_work_delayable gnss_timeout_work;

static struct nrf_modem_gnss_pvt_data_frame pvt_data;
static struct nrf_modem_gnss_agnss_data_frame agnss_req;

static int64_t gnss_start_time;
static bool first_fix;

static int publish_timeout(void)
{
    struct app_event ev = {
        .type = EVT_GNSS_TIMEOUT,
    };

    return app_event_put(&ev, K_NO_WAIT);
}

static int publish_fix(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
    struct app_event ev = {
        .type = EVT_GNSS_FIX,
    };

    ev.pvt.latitude = pvt->latitude;
    ev.pvt.longitude = pvt->longitude;
    ev.pvt.altitude = pvt->altitude;

    return app_event_put(&ev, K_NO_WAIT);
}

static int publish_agnss_request(void)
{
    struct app_event ev = {
        .type = EVT_AGNSS_REQUEST,
    };

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
    (void)publish_error_as_timeout(err);
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

static void log_agnss_request(const struct nrf_modem_gnss_agnss_data_frame *req)
{
    LOG_INF("A-GNSS request received");
    LOG_INF("A-GNSS sv_mask_ephe=0x%08x", req->sv_mask_ephe);
    LOG_INF("A-GNSS sv_mask_alm=0x%08x", req->sv_mask_alm);
    LOG_INF("A-GNSS data_flags=0x%08x", req->data_flags);
}

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
    int err = nrf_modem_gnss_read(&pvt_data, sizeof(pvt_data), NRF_MODEM_GNSS_DATA_PVT);

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

    err = publish_fix(&pvt_data);
    LOG_INF("published GNSS fix status err=%d", err);
}

static void agnss_req_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    int err = nrf_modem_gnss_read(&agnss_req,
                                  sizeof(agnss_req),
                                  NRF_MODEM_GNSS_DATA_AGNSS_REQ);
    if (err) {
        LOG_ERR("nrf_modem_gnss_read(AGNSS_REQ) failed, err=%d", err);
        (void)handle_error(err);
        return;
    }

    log_agnss_request(&agnss_req);

    err = publish_agnss_request();
    LOG_INF("Published A-GNSS request err=%d", err);
}

static void gnss_event_handler(int event)
{
    switch (event) {
    case NRF_MODEM_GNSS_EVT_PVT:
        k_work_submit(&gnss_pvt_work);
        break;

    case NRF_MODEM_GNSS_EVT_AGNSS_REQ:
        k_work_submit(&agnss_req_work);
        break;

    default:
        break;
    }
}

int gnss_service_init(void)
{
    int err;

    first_fix = false;
    gnss_start_time = 0;

    k_work_init(&gnss_pvt_work, gnss_pvt_work_handler);
    k_work_init(&agnss_req_work, agnss_req_work_handler);
    k_work_init_delayable(&gnss_timeout_work, gnss_timeout_work_handler);

    err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
    if (err) {
        LOG_ERR("nrf_modem_gnss_event_handler_set failed, err=%d", err);
        return handle_error(err);
    }

    LOG_INF("GNSS service initialized");
    return 0;
}

int gnss_service_start(void)
{
    int err = nrf_modem_gnss_start();

    if (err) {
        LOG_ERR("nrf_modem_gnss_start failed, err=%d", err);
        return handle_error(err);
    }

    first_fix = false;
    gnss_start_time = k_uptime_get();

    LOG_INF("GNSS started");
    return 0;
}

int gnss_service_stop(void)
{
    int err = nrf_modem_gnss_stop();

    if (!err) {
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