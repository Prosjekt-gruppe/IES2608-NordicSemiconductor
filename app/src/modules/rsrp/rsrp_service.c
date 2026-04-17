/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#include "rsrp_service.h"
#include "app_events.h"


#include <errno.h>
#include <nrf_modem_at.h>
#include <stdio.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifndef CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC
#define CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC \
	CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC
#endif

#ifndef CONFIG_APP_MODEM_RSRP_RECOVERY_DBM
#define CONFIG_APP_MODEM_RSRP_RECOVERY_DBM \
	CONFIG_APP_MODEM_RSRP_FALLBACK_DBM
#endif

LOG_MODULE_REGISTER(rsrp_service, LOG_LEVEL_INF); 


/*----- Defines ---------*/
#define RSRP_HISTORY_LEN 6
#define PROBE_POLL_MSEC 500
#define RSRP_UNKNOWN_DBM CONFIG_APP_MODEM_RSRP_FALLBACK_DBM

/* service internal state control */
enum rsrp_mode {
    RSRP_MODE_IDLE,
    RSRP_MODE_MONITOR,
    RSRP_MODE_PROBE,
};

static enum rsrp_mode mode;
static uint8_t probe_target;


/*------ Variables -------*/
static struct k_work_delayable rsrp_work;
static bool initialized;
static bool fallback_requested;
static bool motion_hint_valid;
static bool motion_hint_moving;
static int last_rsrp_dbm = INT32_MIN;
static uint32_t motion_speed_mm_s;
static uint32_t motion_linear_accel_mg;
static uint32_t current_poll_interval_sec = CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC;

static int rsrp_hist[RSRP_HISTORY_LEN];
static uint8_t rsrp_hist_idx;
static uint8_t rsrp_hist_count;
static uint8_t rsrp_unavailable_count;


/*---- Functions -------*/
static void reset_signal_tracking(void)
{
    last_rsrp_dbm = INT32_MIN;
    rsrp_hist_idx = 0U;
    rsrp_hist_count = 0U;
    rsrp_unavailable_count = 0U;
    fallback_requested = false;
}

static int publish_rsrp_event(enum app_evt_type type, int rsrp_dbm)
{
    struct app_event ev = {
        .type = type,
    };

    /* add rsrp to event object */
    ev.meas.rsrp_dbm = rsrp_dbm;

    return app_event_put(&ev, K_NO_WAIT);
}

static const char *rsrp_motion_state_str(void)
{
    if (!motion_hint_valid) {
        return "unknown";
    }

    return motion_hint_moving ? "moving" : "still";
}

static uint32_t rsrp_target_poll_interval_sec(void)
{
    if (!motion_hint_valid) {
        return CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC;
    }

    if (motion_hint_moving) {
        return CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC;
    }

    return CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC;
}

/* consecutive worsening check */
static bool rsrp_trend_worsening(void)
{
    int i0;
    int i1;
    int i2;
    int total_drop;
    
    /* make sure the buffer contain at least three previous measurements */
    if (rsrp_hist_count < 3U) {
        return false;
    }
    
    /* get the three latest values from the ring buffer */
    i2 = (rsrp_hist_idx + RSRP_HISTORY_LEN - 1) % RSRP_HISTORY_LEN;
    i1 = (rsrp_hist_idx + RSRP_HISTORY_LEN - 2) % RSRP_HISTORY_LEN;
    i0 = (rsrp_hist_idx + RSRP_HISTORY_LEN - 3) % RSRP_HISTORY_LEN;

    /* make sure the trend is actually declining */
    if (!((rsrp_hist[i2] < rsrp_hist[i1]) && (rsrp_hist[i1] < rsrp_hist[i0]))) {
        return false;
    }

    total_drop = rsrp_hist[i2] - rsrp_hist[i0];
    
    /* see if total signal decline reaches a user defined threshold */
    if (total_drop > -CONFIG_APP_MODEM_RSRP_DROP_DB) {
        return false;
    }

    LOG_WRN("LTE-M RSRP worsening: %d -> %d -> %d dBm",
            rsrp_hist[i0], rsrp_hist[i1], rsrp_hist[i2]);

    return true;
}

/* two step signal worsening check */
static bool should_publish_lte_poor(int rsrp_dbm)
{
    bool weak_signal = rsrp_dbm <= CONFIG_APP_MODEM_RSRP_FALLBACK_DBM;
    bool sharp_drop = false;
    bool worsening;

    /* detect sharp decline in signal strength */
    if (last_rsrp_dbm != INT32_MIN) {
        int delta = rsrp_dbm - last_rsrp_dbm;

        if (delta <= -CONFIG_APP_MODEM_RSRP_DROP_DB) {
            LOG_WRN("LTE-M RSRP dropped %d dB (%d -> %d)",
                    -delta, last_rsrp_dbm, rsrp_dbm);
            sharp_drop = true;
        }
    }

    /* add new rsrp measurement to ring buffer */
    rsrp_hist[rsrp_hist_idx] = rsrp_dbm;
    rsrp_hist_idx = (rsrp_hist_idx + 1U) % RSRP_HISTORY_LEN;
    if (rsrp_hist_count < RSRP_HISTORY_LEN) {
        rsrp_hist_count++;
    }

    worsening = rsrp_trend_worsening();
    last_rsrp_dbm = rsrp_dbm;
    
    return weak_signal && (sharp_drop || worsening);
}

static void record_rsrp_available(void)
{
    rsrp_unavailable_count = 0U;
}

static uint8_t record_rsrp_unavailable(void)
{
    if (rsrp_unavailable_count < UINT8_MAX) {
        rsrp_unavailable_count++;
    }

    return rsrp_unavailable_count;
}

static int rsrp_event_value_for_unavailable(void)
{
    if ((last_rsrp_dbm != INT32_MIN) && (last_rsrp_dbm < RSRP_UNKNOWN_DBM)) {
        return last_rsrp_dbm;
    }

    return RSRP_UNKNOWN_DBM;
}

static bool rsrp_unavailable_limit_reached(void)
{
    return rsrp_unavailable_count >= CONFIG_APP_MODEM_RSRP_LOSS_SAMPLE_COUNT;
}


static void rsrp_work_handler(struct k_work *work)
{
    int err;
    int rsrp_dbm;

    ARG_UNUSED(work);

    switch (mode) {
    case RSRP_MODE_MONITOR:
        /* get rsrp info from modem */
        err = rsrp_service_get(&rsrp_dbm);
        if (err) {
            (void)record_rsrp_unavailable();
            current_poll_interval_sec = rsrp_target_poll_interval_sec();

            LOG_WRN("LTE-M RSRP unavailable: err=%d, count=%u/%u, next=%u s",
                    err,
                    (unsigned int)rsrp_unavailable_count,
                    (unsigned int)CONFIG_APP_MODEM_RSRP_LOSS_SAMPLE_COUNT,
                    current_poll_interval_sec);

            if (!fallback_requested && rsrp_unavailable_limit_reached()) {
                fallback_requested = true;
                mode = RSRP_MODE_IDLE;

                rsrp_dbm = rsrp_event_value_for_unavailable();

                LOG_WRN("LTE-M RSRP unavailable for %u samples, publishing LTE poor event",
                        (unsigned int)rsrp_unavailable_count);
                (void)publish_rsrp_event(EVT_LTE_POOR, rsrp_dbm);
                return;
            }

            (void)k_work_reschedule(&rsrp_work,
                                    K_SECONDS(current_poll_interval_sec));
            return;
        }
    
        record_rsrp_available();
        current_poll_interval_sec = rsrp_target_poll_interval_sec();
        LOG_INF("LTE-M RSRP: %d dBm, next=%u s, motion=%s, speed=%u mm/s, accel=%u mg",
                rsrp_dbm,
                current_poll_interval_sec,
                rsrp_motion_state_str(),
                motion_speed_mm_s,
                motion_linear_accel_mg);
        (void)publish_rsrp_event(EVT_RSRP_UPDATE, rsrp_dbm);

        /* signal main sm to exit lte-connected state */
        if (!fallback_requested && should_publish_lte_poor(rsrp_dbm)) {
            fallback_requested = true;
            mode = RSRP_MODE_IDLE;

            LOG_WRN("LTE-M signal degradation detected, publishing LTE poor event");
            (void)publish_rsrp_event(EVT_LTE_POOR, rsrp_dbm);
            return;
        }

        /* reschedule rsrp operation with the given interval */
        (void)k_work_reschedule(&rsrp_work,
                                K_SECONDS(current_poll_interval_sec));
        return;


    /* a bit patchy but it should be ok for now */
    case RSRP_MODE_PROBE:
    
        err = rsrp_service_get(&rsrp_dbm);
        if (err) {
            (void)record_rsrp_unavailable();

            LOG_WRN("LTE probe RSRP unavailable: err=%d, count=%u/%u",
                    err,
                    (unsigned int)rsrp_unavailable_count,
                    (unsigned int)CONFIG_APP_MODEM_RSRP_LOSS_SAMPLE_COUNT);

            if (rsrp_unavailable_limit_reached()) {
                rsrp_dbm = rsrp_event_value_for_unavailable();

                LOG_WRN("LTE probe failed because RSRP stayed unavailable");
                (void)publish_rsrp_event(EVT_LTE_POOR, rsrp_dbm);
                mode = RSRP_MODE_IDLE;
                return;
            }

            (void)k_work_reschedule(&rsrp_work, K_MSEC(PROBE_POLL_MSEC));
            return;
        }
    
        record_rsrp_available();

        /* store sample in ring buffer */
        rsrp_hist[rsrp_hist_idx] = rsrp_dbm;
        rsrp_hist_idx = (rsrp_hist_idx + 1U) % RSRP_HISTORY_LEN;
        if (rsrp_hist_count < RSRP_HISTORY_LEN) {
            rsrp_hist_count++;
        }

        LOG_INF("LTE probe RSRP sample %u/%u: %d dBm",
                rsrp_hist_count, probe_target, rsrp_dbm);
    
        if (rsrp_hist_count < probe_target) {
            //TODO: implement it into Kconfig 
            k_work_reschedule(&rsrp_work, K_MSEC(PROBE_POLL_MSEC));
            return;
        }

        /* 
         * TODO: make this more robust
         */

        int sum = 0;
        for (int i = 0; i < rsrp_hist_count; i++) {
            sum += rsrp_hist[i];
        }

        int avg = sum / rsrp_hist_count;

        if (avg >= CONFIG_APP_MODEM_RSRP_RECOVERY_DBM) {
            publish_rsrp_event(EVT_LTE_GOOD, avg);
        } else {
            publish_rsrp_event(EVT_LTE_POOR, avg);
        }

        LOG_INF("LTE probe complete: avg=%d dBm over %u samples", avg, probe_target);
        mode = RSRP_MODE_IDLE;

        return;

    case RSRP_MODE_IDLE:
    default:
        return;

    }
}


int rsrp_service_get(int *rsrp_dbm)
{
    int err;
    char buf[64];   // bigger buffer = safer
    int rxlev, ber, rscp, ecno, rsrq, rsrp_raw;

    if (rsrp_dbm == NULL) {
        return -EINVAL;
    }

    /* ask modem for rsrp information */
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

    /* update rsrp dbm variable of rsrp-service */
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

    mode = RSRP_MODE_IDLE;

    k_work_init_delayable(&rsrp_work, rsrp_work_handler);
    reset_signal_tracking();
    current_poll_interval_sec = rsrp_target_poll_interval_sec();

    initialized = true;
    return 0;
}

int rsrp_service_start_monitor(void)
{
    if (!initialized) {
        return -EINVAL;
    }

    mode = RSRP_MODE_MONITOR;
    reset_signal_tracking();
    current_poll_interval_sec = rsrp_target_poll_interval_sec();

    LOG_INF("Starting LTE-M RSRP monitor: interval=%u s, motion=%s",
            current_poll_interval_sec, rsrp_motion_state_str());

    return k_work_reschedule(&rsrp_work,
        K_SECONDS(current_poll_interval_sec));
}

/* start gathering of lte-probe samples */
int rsrp_service_start_probe(uint8_t samples)
{
    if (!initialized) {
        return -EINVAL;
    }
    
    
    reset_signal_tracking();
    
    
    mode = RSRP_MODE_PROBE;
    probe_target = samples;
    
    return k_work_reschedule(&rsrp_work, K_NO_WAIT);
}


int rsrp_service_stop(void)
{
    mode = RSRP_MODE_IDLE;
    reset_signal_tracking();
    return k_work_cancel_delayable(&rsrp_work);
}

void rsrp_service_set_motion_hint(bool moving, uint32_t speed_mm_s,
                                  uint32_t linear_accel_mg)
{
    bool state_changed;
    uint32_t next_interval_sec;

    state_changed = (!motion_hint_valid || (motion_hint_moving != moving));

    motion_hint_valid = true;
    motion_hint_moving = moving;
    motion_speed_mm_s = speed_mm_s;
    motion_linear_accel_mg = linear_accel_mg;

    next_interval_sec = rsrp_target_poll_interval_sec();

    if (!state_changed && (next_interval_sec == current_poll_interval_sec)) {
        return;
    }

    current_poll_interval_sec = next_interval_sec;

    LOG_INF("RSRP poll interval %u s (%s, speed=%u mm/s, accel=%u mg)",
            current_poll_interval_sec,
            moving ? "moving" : "still",
            motion_speed_mm_s,
            motion_linear_accel_mg);

    if (initialized && (mode == RSRP_MODE_MONITOR)) {
        (void)k_work_reschedule(&rsrp_work, K_SECONDS(current_poll_interval_sec));
    }
}
