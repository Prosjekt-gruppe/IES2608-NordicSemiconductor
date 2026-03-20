/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "modem_signal_monitor.h"

#include "app_events.h"
#include "lte_service.h"

#include <limits.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(modem_signal_monitor, LOG_LEVEL_INF);

static struct k_work_delayable ltem_signal_work;
static bool initialized;
static bool monitor_enabled;
static bool fallback_requested;
static int last_rsrp_dbm = INT32_MIN;

#define RSRP_HISTORY_LEN 6

static int rsrp_hist[RSRP_HISTORY_LEN];
static uint8_t rsrp_hist_idx;
static uint8_t rsrp_hist_count;

static void reset_signal_tracking(void)
{
    last_rsrp_dbm = INT32_MIN;
    rsrp_hist_idx = 0U;
    rsrp_hist_count = 0U;
    fallback_requested = false;
}

static int publish_rsrp_event(enum app_evt_type type, int rsrp_dbm)
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

static bool should_publish_lte_poor(int rsrp_dbm)
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

    if (!monitor_enabled) {
        return;
    }

    err = lte_service_get_rsrp(&rsrp_dbm);
    if (err) {
        LOG_WRN("lte_service_get_rsrp failed: %d", err);
        (void)k_work_reschedule(&ltem_signal_work,
                                K_SECONDS(CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC));
        return;
    }

    LOG_INF("LTE-M RSRP: %d dBm", rsrp_dbm);
    (void)publish_rsrp_event(EVT_RSRP_UPDATE, rsrp_dbm);

    if (!fallback_requested && should_publish_lte_poor(rsrp_dbm)) {
        fallback_requested = true;
        monitor_enabled = false;

        LOG_WRN("LTE-M signal degradation detected, publishing LTE poor event");
        (void)publish_rsrp_event(EVT_LTE_POOR, rsrp_dbm);
        return;
    }

    (void)k_work_reschedule(&ltem_signal_work,
                            K_SECONDS(CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC));
}

int modem_signal_monitor_start(void)
{
    if (!initialized) {
        k_work_init_delayable(&ltem_signal_work, ltem_signal_work_handler);
        initialized = true;
    }

    reset_signal_tracking();
    monitor_enabled = true;

    return k_work_reschedule(&ltem_signal_work, K_NO_WAIT);
}

int modem_signal_monitor_stop(void)
{
    monitor_enabled = false;

    if (!initialized) {
        return 0;
    }

    reset_signal_tracking();

    return k_work_cancel_delayable(&ltem_signal_work);
}
