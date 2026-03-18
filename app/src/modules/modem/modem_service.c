/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#include "app_events.h"

#include <errno.h>
#include <limits.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include "modem_service.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(modem_service, LOG_LEVEL_INF);

static struct k_work_delayable ltem_signal_work;
static bool lte_connected;
static bool modem_info_ready;
static bool ltem_monitor_enabled;
static bool fallback_requested;
static int last_rsrp_dbm = INT32_MIN;

#define RSRP_HISTORY_LEN 6

static int rsrp_hist[RSRP_HISTORY_LEN];
static uint8_t rsrp_hist_idx;
static uint8_t rsrp_hist_count;

static int schedule_signal_work(k_timeout_t delay)
{
    return k_work_reschedule(&ltem_signal_work, delay);
}

static void reset_signal_tracking(void)
{
    last_rsrp_dbm = INT32_MIN;
    rsrp_hist_idx = 0U;
    rsrp_hist_count = 0U;
    fallback_requested = false;
}

static int publish_rsrp_evt(enum app_evt_type type, int rsrp_dbm)
{
    struct app_event ev = {
        .type = type,
    };

    ev.meas.rsrp_dbm = rsrp_dbm;

    return app_event_put(&ev, K_NO_WAIT);
}

static bool rsrp_trend_worsening(void)
{
    int i0;
    int i1;
    int i2;
    int total_drop;

    if (rsrp_hist_count < 3U) {
        return false;
    }

    i2 = (rsrp_hist_idx + RSRP_HISTORY_LEN - 1) % RSRP_HISTORY_LEN;
    i1 = (rsrp_hist_idx + RSRP_HISTORY_LEN - 2) % RSRP_HISTORY_LEN;
    i0 = (rsrp_hist_idx + RSRP_HISTORY_LEN - 3) % RSRP_HISTORY_LEN;

    if (!((rsrp_hist[i2] < rsrp_hist[i1]) && (rsrp_hist[i1] < rsrp_hist[i0]))) {
        return false;
    }

    total_drop = rsrp_hist[i2] - rsrp_hist[i0];

    if (total_drop > -CONFIG_APP_MODEM_RSRP_DROP_DB) {
        return false;
    }

    LOG_WRN("LTE-M RSRP worsening: %d -> %d -> %d dBm",
            rsrp_hist[i0], rsrp_hist[i1], rsrp_hist[i2]);

    return true;
}

static bool should_request_ntn_fallback(int rsrp_dbm)
{
    bool weak_signal = rsrp_dbm <= CONFIG_APP_MODEM_RSRP_FALLBACK_DBM;
    bool sharp_drop = false;
    bool worsening;

    if (last_rsrp_dbm != INT32_MIN) {
        int delta = rsrp_dbm - last_rsrp_dbm;

        if (delta <= -CONFIG_APP_MODEM_RSRP_DROP_DB) {
            LOG_WRN("LTE-M RSRP dropped %d dB (%d -> %d)",
                    -delta, last_rsrp_dbm, rsrp_dbm);
            sharp_drop = true;
        }
    }

    rsrp_hist[rsrp_hist_idx] = rsrp_dbm;
    rsrp_hist_idx = (rsrp_hist_idx + 1U) % RSRP_HISTORY_LEN;
    if (rsrp_hist_count < RSRP_HISTORY_LEN) {
        rsrp_hist_count++;
    }

    worsening = rsrp_trend_worsening();
    last_rsrp_dbm = rsrp_dbm;

    return weak_signal && (sharp_drop || worsening);
}

static void ltem_signal_work_handler(struct k_work *work)
{
    int err;
    int rsrp_dbm;

    ARG_UNUSED(work);

    if (!ltem_monitor_enabled) {
        return;
    }

    err = modem_info_get_rsrp(&rsrp_dbm);
    if (err) {
        LOG_WRN("modem_info_get_rsrp failed: %d", err);
        (void)schedule_signal_work(K_SECONDS(CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC));
        return;
    }

    LOG_INF("LTE-M RSRP: %d dBm", rsrp_dbm);
    (void)publish_rsrp_evt(EVT_RSRP_UPDATE, rsrp_dbm);

    if (!fallback_requested && should_request_ntn_fallback(rsrp_dbm)) {
        fallback_requested = true;
        ltem_monitor_enabled = false;

        LOG_WRN("LTE-M signal degradation detected, requesting NTN fallback");
        (void)publish_rsrp_evt(EVT_NTN_FALLBACK_REQUEST, rsrp_dbm);
        return;
    }

    (void)schedule_signal_work(K_SECONDS(CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC));
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
    struct app_event app_ev = {0};

    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
        switch (evt->nw_reg_status) {
        case LTE_LC_NW_REG_REGISTERED_HOME:
        case LTE_LC_NW_REG_REGISTERED_ROAMING:
            lte_connected = true;
            app_ev.type = EVT_REG_OK;
            (void)app_event_put(&app_ev, K_NO_WAIT);
            break;

        case LTE_LC_NW_REG_NOT_REGISTERED:
        case LTE_LC_NW_REG_REGISTRATION_DENIED:
        case LTE_LC_NW_REG_UNKNOWN:
        case LTE_LC_NW_REG_UICC_FAIL:
            lte_connected = false;
            ltem_monitor_enabled = false;
            (void)k_work_cancel_delayable(&ltem_signal_work);
            app_ev.type = EVT_REG_FAIL;
            (void)app_event_put(&app_ev, K_NO_WAIT);
            break;

        default:
            break;
        }
        break;

    case LTE_LC_EVT_CELLULAR_PROFILE_ACTIVE:
        LOG_INF("modem activate cellular profile (RAT starting)");
        break;

    case LTE_LC_EVT_LTE_MODE_UPDATE:
        LOG_INF("LTE mode update %d", evt->lte_mode);
        break;

    default:
        break;
    }
}

int modem_service_init(void)
{
    int err;

    k_work_init_delayable(&ltem_signal_work, ltem_signal_work_handler);
    lte_connected = false;
    ltem_monitor_enabled = false;
    modem_info_ready = false;
    reset_signal_tracking();

    err = nrf_modem_lib_init();
    if (err) {
        LOG_ERR("nrf_modem_lib_init failed: %u", err);
        return err;
    }

    err = modem_info_init();
    if (err) {
        LOG_ERR("modem_info_init failed: %d", err);
        return err;
    }

    modem_info_ready = true;

    return 0;
}

int modem_service_prepare_ltem(void)
{
    int err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_GPS,
                                     LTE_LC_SYSTEM_MODE_PREFER_AUTO);

    if (err) {
        LOG_ERR("lte_lc_system_mode_set(LTE-M) failed: %d", err);
    }

    return err;
}

int modem_service_connect_async(void)
{
    return lte_lc_connect_async(lte_lc_evt_handler);
}

int modem_service_start_ltem_monitor(void)
{
    if (!modem_info_ready || !lte_connected) {
        return -EAGAIN;
    }

    reset_signal_tracking();
    ltem_monitor_enabled = true;

    return schedule_signal_work(K_NO_WAIT);
}

int modem_service_stop_ltem_monitor(void)
{
    ltem_monitor_enabled = false;
    reset_signal_tracking();

    return k_work_cancel_delayable(&ltem_signal_work);
}
