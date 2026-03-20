/*
* RSRP Serivce.c : 
*/

#include "rsrp_service.h"
#include "app_events.h"

#include <errno.h>
#include <nrf_modem_at.h>
#include <stdio.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>


LOG_MODULE_REGISTER(rsrp_service, LOG_LEVEL_INF); 


/*----- Defines ---------*/
#define RSRP_HISTORY_LEN 6


/*------ Variables -------*/
static struct k_work_delayable rsrp_work;
static bool initialized;
static bool monitor_enabled;
static bool fallback_requested;
static int last_rsrp_dbm = INT32_MIN;

static int rsrp_hist[RSRP_HISTORY_LEN];
static uint8_t rsrp_hist_idx;
static uint8_t rsrp_hist_count;


/*---- Functions -------*/

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

static void rsrp_work_handler(struct k_work *work)
{
    int err;
    int rsrp_dbm;

    ARG_UNUSED(work);

    if (!monitor_enabled) {
        return;
    }

    err = rsrp_service_get(&rsrp_dbm);
    if (err) {
        LOG_WRN("rsrp_service_get failed: %d", err);
        (void)k_work_reschedule(&rsrp_work,
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

    (void)k_work_reschedule(&rsrp_work,
                            K_SECONDS(CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC));
}

int rsrp_service_get(int *rsrp_dbm)
{
    int err;
    char buf[64];   // bigger buffer = safer
    int rxlev, ber, rscp, ecno, rsrq, rsrp_raw;

    if (rsrp_dbm == NULL) {
        return -EINVAL;
    }

    err = nrf_modem_at_cmd(buf, sizeof(buf), "AT+CESQ");
    if (err) {
        LOG_ERR("AT+CESQ failed: %d", err);
        return err;
    }

    LOG_DBG("CESQ response: %s", buf);

    int parsed = sscanf(buf,
                        "+CESQ: %d,%d,%d,%d,%d,%d",
                        &rxlev, &ber, &rscp, &ecno, &rsrq, &rsrp_raw);

    if (parsed != 6) {
        LOG_WRN("Failed to parse CESQ response");
        return -EIO;
    }

    if (rsrp_raw == 255) {
        LOG_WRN("RSRP not known");
        return -ENOENT;
    }

    if (rsrp_raw == 0) {
        LOG_WRN("RSRP < -140 dBm");
        *rsrp_dbm = -141;   // or clamp to -140 depending on your policy
        return 0;
    }

    *rsrp_dbm = rsrp_raw - 141;

    return 0;
}

int rsrp_service_sample_and_publish(void)
{
    int err;
    int rsrp_dbm;

    err = rsrp_service_get(&rsrp_dbm); 
    if (err)
    {
        return err; 
    }

    LOG_INF("LTE RSRP: %d dBm", rsrp_dbm); 
    return publish_rsrp_event(EVT_RSRP_UPDATE, rsrp_dbm);
}


int rsrp_service_init(void){
    if (initialized){
        return 0;
    }

    k_work_init_delayable(&rsrp_work, rsrp_work_handler);
    reset_signal_tracking();

    initialized = true;
    return 0; 
}

int rsrp_service_start(void)
{
    if (!initialized) {
        return -EINVAL;
    }

    monitor_enabled = true;
    reset_signal_tracking();

    return k_work_reschedule(&rsrp_work,
        K_SECONDS(CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC));
}

int rsrp_service_stop(void)
{
    monitor_enabled = false;
    reset_signal_tracking();
    return k_work_cancel_delayable(&rsrp_work);
}