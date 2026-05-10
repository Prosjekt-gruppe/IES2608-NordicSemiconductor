/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include "app_sm.h"
#include "app_events.h"
#include "rsrp_service.h"


#include "gnss_service.h"
#include "ntn_service.h"
#include "modem_service.h"
#include "lte_service.h"
#include "location_service.h"
#include "cloud_service.h"

#if defined(CONFIG_APP_FIELD_LOG)
#include "field_log.h"
#endif


#include <limits.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>
#include <modem/lte_lc.h>

LOG_MODULE_REGISTER(app_sm, LOG_LEVEL_INF);

ZBUS_MSG_SUBSCRIBER_DEFINE(app_fsm_sub); //Subscriber for app events

union app_sm_msg {
    struct app_event app_event;
};

static void boot_entry(void *obj);
static enum smf_state_result boot_run(void *obj);

static void disconnected_entry(void *obj);
static enum smf_state_result disconnected_run(void *obj);
static void disconnected_exit(void *obj);

static void connected_entry(void *obj);
static enum smf_state_result connected_run(void *obj);
static void connected_exit(void *obj);

static void ltem_connecting_entry(void *obj); 
static enum smf_state_result ltem_connecting_run(void *obj);

static void ltem_connected_entry(void *obj); 
static enum smf_state_result ltem_connected_run(void *obj);
static void ltem_connected_exit(void *obj); 

static void cloud_connecting_entry(void *obj); 
static enum smf_state_result cloud_connecting_run(void *obj); 
static void cloud_connecting_exit(void *obj);

static void lte_location_entry(void *obj); 
static enum smf_state_result lte_location_run(void *obj); 
static void lte_location_exit(void *obj);

static void backoff_entry(void *obj);
static enum smf_state_result backoff_run(void *obj);
static void backoff_exit(void *obj);

static void gnss_acquire_entry(void *obj);
static enum smf_state_result gnss_acquire_run(void *obj);
static void gnss_acquire_exit(void *obj);

static void ntn_connecting_entry(void *obj);
static enum smf_state_result ntn_connecting_run(void *obj);
static void ntn_connecting_exit(void *obj);

static void ntn_connected_entry(void *obj);
static enum smf_state_result ntn_connected_run(void *obj);
static void ntn_connected_exit(void *obj);

static void lte_probe_entry(void *obj);
static enum smf_state_result lte_probe_run(void *obj);

static void running_entry(void *obj);
static enum smf_state_result running_run(void *obj);
static void running_exit(void *obj);


static void dispatch_app_event(struct app_ctx *ctx, const struct app_event *ev);
static void backoff_timer_handler(struct k_timer *timer);

static void ntn_timer_handler(struct k_timer *timer);
static void ltem_timer_handler(struct k_timer *timer);
static void handoff_timer_handler(struct k_timer *timer); 
static const char *rat_name(enum rat rat);



static const struct smf_state states[] = {
    [STATE_BOOT] = SMF_CREATE_STATE(
        boot_entry,
        boot_run,
        NULL,
        NULL,
        NULL
    ),
    /* parent state */
    [STATE_RUNNING] = SMF_CREATE_STATE(
        running_entry,
        running_run,
        running_exit,
        NULL,
        &states[STATE_DISCONNECTED]
    ),
    /* disconnected parent state */
    [STATE_DISCONNECTED] = SMF_CREATE_STATE(
        disconnected_entry,
        disconnected_run,
        disconnected_exit,
        &states[STATE_RUNNING],
        &states[STATE_LTEM_CONNECTING]
    ),
        /* disconnected child states */
        [STATE_BACKOFF] = SMF_CREATE_STATE(
            backoff_entry,
            backoff_run,
            backoff_exit,
            &states[STATE_DISCONNECTED],
            NULL
        ),
        [STATE_LTEM_CONNECTING] = SMF_CREATE_STATE(
            ltem_connecting_entry,
            ltem_connecting_run,
            NULL,
            &states[STATE_DISCONNECTED],
            NULL
        ),
        [STATE_NTN_CONNECTING] = SMF_CREATE_STATE(
            ntn_connecting_entry,
            ntn_connecting_run,
            ntn_connecting_exit,
            &states[STATE_DISCONNECTED],
            NULL
        ),
    /* connecte parent state */
    [STATE_CONNECTED] = SMF_CREATE_STATE(
        connected_entry,
        connected_run,
        connected_exit,
        &states[STATE_RUNNING],
        &states[STATE_LTEM_CONNECTED]
    ),
        /* connected child states */
        [STATE_LTEM_CONNECTED] = SMF_CREATE_STATE(
            ltem_connected_entry,
            ltem_connected_run,
            ltem_connected_exit,
            &states[STATE_CONNECTED],
            NULL
        ),
        [STATE_CLOUD_CONNECTING] = SMF_CREATE_STATE(
            cloud_connecting_entry,
            cloud_connecting_run,
            cloud_connecting_exit,
            &states[STATE_CONNECTED],
            NULL
        ),

        [STATE_LTE_LOCATION] = SMF_CREATE_STATE(
            lte_location_entry,
            lte_location_run,
            lte_location_exit,
            &states[STATE_CONNECTED],
            NULL
        ),
        [STATE_GNSS_ACQUIRE] = SMF_CREATE_STATE(
            gnss_acquire_entry,
            gnss_acquire_run,
            gnss_acquire_exit,
            &states[STATE_CONNECTED],
            NULL
        ),
        [STATE_NTN_CONNECTED] = SMF_CREATE_STATE(
            ntn_connected_entry,
            ntn_connected_run,
            ntn_connected_exit,
            &states[STATE_CONNECTED],
            NULL
        ),
        [STATE_LTE_PROBE] = SMF_CREATE_STATE(
            lte_probe_entry,
            lte_probe_run,
            NULL,
            &states[STATE_CONNECTED],
            NULL
        ),
        [STATE_IDLE] = SMF_CREATE_STATE(
            NULL,
            NULL,
            NULL,
            &states[STATE_CONNECTED],
            NULL
        ),
};

static void retry_reset(struct app_ctx *ctx, enum rat rat)
{
    if (rat == RAT_LTEM) {
        ctx->retry.ltem_attempts = 0;
    } else if (rat == RAT_NTN) {
        ctx->retry.ntn_attempts = 0;
    }
}

static uint8_t retry_inc(struct app_ctx *ctx, enum rat rat)
{
    if (rat == RAT_LTEM) {
        return ++ctx->retry.ltem_attempts;
    } else if (rat == RAT_NTN) {
        return ++ctx->retry.ntn_attempts;
    }

    return 0;
}

static void transition_to_state(struct app_ctx *ctx, enum app_state next_state)
{
#if defined(CONFIG_APP_FIELD_LOG)
    field_log_note_state_change(ctx->state, next_state, ctx->ev.type, ctx);
#endif
    ctx->state = next_state;
    smf_set_state(SMF_CTX(ctx), &states[next_state]);
}

static void dispatch_app_event(struct app_ctx *ctx, const struct app_event *ev)
{
    ctx->ev = *ev;
    LOG_INF("SMF thread got event %s", app_evt_name(ev->type));
    (void)smf_run_state(SMF_CTX(ctx));
}

static const char *rat_name(enum rat rat)
{
    switch (rat) {
    case RAT_LTEM:
        return "LTE-M";
    case RAT_NTN:
        return "NTN";
    default:
        return "UNKNOWN";
    }
}


static void boot_entry(void *obj)
{
    struct app_ctx *ctx = obj;
    
    int err = modem_service_init(); 
    if  (err){
        LOG_ERR("modem_service_init err=%d", err);
        return;
    }
    
    err = lte_service_init();
    if (err){
        LOG_ERR("lte_service_init err=%d", err); 
        return; 
    }
    
    err = cloud_service_init();
    if (err){
        LOG_ERR("cloud_service_init err=%d", err);
        return;
    }

    err = location_service_init();
    if (err){
        LOG_ERR("location_service_init err=%d", err);
        return;
    }

    err = rsrp_service_init();
    if (err){
        LOG_ERR("rsrp_service_init err=%d", err); 
        return; 
    }

    
    err = gnss_service_init();
    if (err) {
        LOG_ERR("gnss_service_init err=%d", err);
        return;
    }


    err = ntn_service_init();
    if (err){
        LOG_ERR("ntn_service_init err=%d", err); 
        return; 
    }


    /* init timers */
    k_timer_init(&ctx->backoff_timer, backoff_timer_handler, NULL);
    k_timer_init(&ctx->ntn_timer, ntn_timer_handler, NULL);
    k_timer_init(&ctx->lte_timer, ltem_timer_handler, NULL);
    k_timer_init(&ctx->handoff_timer, handoff_timer_handler, NULL);
    

    /* gnss timerout */
    ctx->gnss_goal = GNSS_GOAL_NONE; 
    ctx->gnss_timeout_sec = 0; 
    ctx->gnss_extend_once = false; 
    
    LOG_INF("BOOT complete");
}

static enum smf_state_result boot_run(void *obj)
{
    struct app_ctx *ctx = obj;
    
    if (ctx->ev.type == EVT_BOOT) {
        if (IS_ENABLED(CONFIG_APP_DEBUG_BOOT)) {
            LOG_INF("DEBUG: Halting in STATE_BOOT after initialization");
        } else {
            transition_to_state(ctx, STATE_LTEM_CONNECTING);
        }
    }
    return SMF_EVENT_HANDLED;
}

static void running_entry(void *obj)
{
    ARG_UNUSED(obj);
    LOG_WRN("ENTER: STATE_RUNNING");
}

static enum smf_state_result running_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_CLOUD_DISCONNECTED:
        ctx->cloud_connected = false;
        LOG_WRN("Cloud disconnected in RUNNING parent");
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_HANDLED;
    }
}

static void running_exit(void *obj)
{
    ARG_UNUSED(obj);
    LOG_WRN("EXIT: STATE_RUNNING");
}

/* disconnected parent state*/
static void disconnected_entry(void *obj)
{
    struct app_ctx *ctx = obj;

    LOG_WRN("ENTER: STATE_DISCONNECTED");

    ctx->pdn_up = false;
    ctx->lte_connected = false;
    ctx->cloud_connected = false;

    /*
     * make sure to dont force modem disconnect here unless every child expects it
     */
}

static enum smf_state_result disconnected_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_CLOUD_DISCONNECTED:
        ctx->cloud_connected = false;
        return SMF_EVENT_HANDLED;

    case EVT_PDN_DOWN:
        ctx->pdn_up = false;
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_HANDLED;
    }
}

static void disconnected_exit(void *obj)
{
    ARG_UNUSED(obj);
    LOG_WRN("EXIT: STATE_DISCONNECTED");
}


/* connected parent state */
static void connected_entry(void *obj)
{
    struct app_ctx *ctx = obj;

    LOG_WRN("ENTER: STATE_CONNECTED");

    /*
     * child states decide RAT_LTEM vs RAT_NTN
     */
}


static enum smf_state_result connected_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {

    case EVT_RSRP_UPDATE:
        ctx->rsrp_dbm = ctx->ev.meas.rsrp_dbm;

        LOG_INF("CONNECTED: network quality sample, RSRP=%d dBm",
                ctx->rsrp_dbm);

        return SMF_EVENT_HANDLED;

    case EVT_CLOUD_DISCONNECTED:
        ctx->cloud_connected = false;
        LOG_WRN("CONNECTED: cloud disconnected");
        return SMF_EVENT_HANDLED;

    case EVT_PDN_DOWN:
    case EVT_REG_FAIL:
        LOG_WRN("CONNECTED: network lost -> BACKOFF");

        ctx->pdn_up = false;
        ctx->cloud_connected = false;

        if (ctx->active_rat == RAT_LTEM) {
            ctx->lte_connected = false;
            ctx->next_rat = RAT_NTN;
        } else if (ctx->active_rat == RAT_NTN) {
            ctx->pdn_up = false;
            ctx->next_rat = RAT_LTEM;
        } else {
            ctx->next_rat = RAT_LTEM;
        }

        transition_to_state(ctx, STATE_BACKOFF);
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_PROPAGATE;
    }
}

static void connected_exit(void *obj)
{
    struct app_ctx *ctx = obj;

    LOG_WRN("EXIT: STATE_CONNECTED");

    k_timer_stop(&ctx->ntn_timer);
    k_timer_stop(&ctx->lte_timer);

    (void)rsrp_service_stop();
}


static void ltem_connecting_entry(void *obj)
{
    ARG_UNUSED(obj);

    LOG_WRN("ENTER: STATE_LTEM_CONNECTING");

    int err = lte_service_connect_async();
    if (err) {
        LOG_ERR("lte_service_connect_async err=%d", err); 
        struct app_event ev = {
            .type = EVT_REG_FAIL,
            .source_rat = RAT_LTEM,
        };
        (void)app_event_put(&ev, K_NO_WAIT);
        return;
    }

    LOG_INF("LTEM connecting...");
}

static enum smf_state_result ltem_connecting_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_REG_OK:
        if (ctx->ev.source_rat == RAT_NTN) {
            LOG_WRN("Ignoring %s from %s during LTE-M connect",
                    app_evt_name(ctx->ev.type),
                    rat_name(ctx->ev.source_rat));
            return SMF_EVENT_HANDLED;
        }
        ctx->active_rat = RAT_LTEM;
        ctx->lte_connected = true;
        ctx->pdn_up = true;
        ctx->last_done = STEP_NONE;

        transition_to_state(ctx, STATE_LTEM_CONNECTED);
        return SMF_EVENT_HANDLED;

    case EVT_REG_FAIL:
        if (ctx->ev.source_rat == RAT_NTN) {
            LOG_WRN("Ignoring %s from %s during LTE-M connect",
                    app_evt_name(ctx->ev.type),
                    rat_name(ctx->ev.source_rat));
            return SMF_EVENT_HANDLED;
        }
        ctx->next_rat = RAT_NTN;
        LOG_WRN("TRANSITION: STATE_LTEM_CONNECTING -> STATE_BACKOFF");
        transition_to_state(ctx, STATE_BACKOFF);
        return SMF_EVENT_HANDLED;

        default:
            return SMF_EVENT_PROPAGATE;
    }
}

static void ltem_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    struct app_event ev = {
        .type = EVT_TIMEOUT
    };

    (void)app_event_put(&ev, K_NO_WAIT);
}


static void ltem_connected_entry(void *obj)
{
    struct app_ctx *ctx = obj;
    int err;
    int rsrp_dbm;

    LOG_WRN("ENTER: STATE_LTEM_CONNECTED");

    ctx->active_rat = RAT_LTEM;
    ctx->lte_connected = true;
    

    err = rsrp_service_get(&rsrp_dbm);
    if (!err) {
        ctx->rsrp_dbm = rsrp_dbm;
        LOG_INF("LTE RSRP on entry: %d dBm", rsrp_dbm);
    } else {
        LOG_WRN("Could not read LTE RSRP: %d", err);
    }

    err = rsrp_service_start_monitor();
        if (err < 0) {
            LOG_WRN("Failed to start LTE signal monitor: %d", err);
        }

    struct lte_lc_conn_eval_params conn_eval = {0};

    err = modem_service_conn_eval_get(&conn_eval);
    if (err) {
        LOG_WRN("DEBUG CONEVAL test failed: %d", err);
    }

#if defined(CONFIG_APP_DEBUG_CORE_UDP_BURST_TEST)
    struct udp_test_cfg test_cfg = {
        .payload_len = 32,
        .interval_ms = 500,
        .count = 5,
    };

    LOG_INF("Starting UDP burst test");
    err = modem_service_udp_send_burst(&test_cfg);
    if (err) {
        LOG_ERR("UDP burst test failed: %d", err);
    } else {
        LOG_INF("UDP burst test OK");
    }
#endif
 
#if defined(CONFIG_APP_CORE_SM_PROBE_TEST)
    LOG_INF("forcing NTN fallback test in 10 sec");
    k_timer_start(&ctx->lte_timer,
                  K_SECONDS(10),
                  K_NO_WAIT);
#endif
    
    // debug
    if (IS_ENABLED(CONFIG_APP_DEBUG_CLOUD_CONNECTING) && 
        ctx->last_done == STEP_CLOUD_DONE){
            LOG_INF("DEBUG: Halting after cloud conenct");
            return; 
        }
    if (IS_ENABLED(CONFIG_APP_DEBUG_LTE_LOCATION) &&
        ctx->last_done == STEP_LTE_LOC_DONE) {
            LOG_INF("DEBUG: Halting after LTE location");
            return;
        }
    if (IS_ENABLED(CONFIG_APP_DEBUG_GNSS_ACQUIRE) && 
        ctx->last_done == STEP_GNSS_DONE){
        LOG_INF("DEBUG: Halting after GNSS acquire");
        return; 
    }

    LOG_INF("ltem_connected_entry ok");

    // orchestration logic 
    if (!ctx->cloud_connected){
        struct app_event ev = {
            .type = EVT_START_CLOUD
        };

        LOG_INF("LTEM_CONNECTED: requesting cloud connect");
        int pub_err = app_event_put(&ev, K_NO_WAIT);
        //LOG_INF("app_event_put(EVT_START_LTE_LOC) -> %d", pub_err); 
        return;
    }

    if(ctx->cloud_connected && ctx->last_done == STEP_CLOUD_DONE){
        struct app_event ev = {
            .type = EVT_START_LTE_LOC
        };

        LOG_INF("LTEM_CONNECTED: requesting LTE location");
        int pub_err = app_event_put(&ev, K_MSEC(10));
        //LOG_INF("app_event_put(EVT_START_LTE_LOC) -> %d", pub_err);
        return;
    }

    if(ctx->cloud_connected && ctx->last_done == STEP_LTE_LOC_DONE) {
        struct app_event ev = {
            .type = EVT_START_GNSS
        };
        LOG_INF("LTEM_CONNECTED: requesting GNSS acquire");
        int pub_err = app_event_put(&ev, K_NO_WAIT);
        //LOG_INF("app_event_put(EVT_START_LTE_LOC) -> %d", pub_err);
        return; 
    }
}

static enum smf_state_result ltem_connected_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {

    case EVT_LTE_POOR:
        LOG_WRN("LTE poor, consider switching RAT");
        ctx->next_rat = RAT_NTN;

        if (lte_service_is_connected()) {
            
            /* try disconnecting all services before NTN */
            int err = location_service_stop();
            if (err) {
                LOG_ERR("Failed to stop location service: err=%d", err);
            }
            
            err = cloud_service_disconnect();
            if (err) {
                LOG_ERR("Failed to disconnect cloud: err=%d", err);
            }

            err = lte_service_disconnect();
            if (err) {
                LOG_ERR("Failed to disconnect LTE: err=%d", err);
            }
        }

        /* go to backoff */
        LOG_WRN("TRANSITION: STATE_LTEM_CONNECTED -> STATE_BACKOFF");
        transition_to_state(ctx, STATE_BACKOFF);
        return SMF_EVENT_HANDLED;

    case EVT_START_CLOUD: 
        LOG_INF("LTEM_CONNECTED: starting cloud connect");
        transition_to_state(ctx, STATE_CLOUD_CONNECTING); 
        return SMF_EVENT_HANDLED;
    
    case EVT_START_LTE_LOC:
        LOG_INF("LTEM_CONNECTED: starting LTE location");
        transition_to_state(ctx, STATE_LTE_LOCATION);
        return SMF_EVENT_HANDLED;
        
    case EVT_START_GNSS:
        LOG_INF("LTEM_CONNECTED: starting GNSS acquire");
        transition_to_state(ctx, STATE_GNSS_ACQUIRE);
        return SMF_EVENT_HANDLED; 
        
/* force EVT_LTE_POOR (usually should be disabled) */
#if defined(CONFIG_APP_CORE_SM_PROBE_TEST)
    case EVT_TIMEOUT:
        ctx->next_rat = RAT_NTN;
        LOG_INF("manual force test going to STATE_BACKOFF");
        transition_to_state(ctx, STATE_BACKOFF);
        return SMF_EVENT_HANDLED;
#endif

    default:
        /* send unkown events to parent state */
        //LOG_INF("unkown event arrived should propagate to parent node");
        return SMF_EVENT_PROPAGATE;
    }
}

static void ltem_connected_exit(void *obj)
{
    ARG_UNUSED(obj);
    int err;

#if defined(CONFIG_APP_CORE_SM_PROBE_TEST)
    struct app_ctx *ctx = obj;
    LOG_INF("ltem timer stop");
    k_timer_stop(&ctx->lte_timer);
#endif

    err = rsrp_service_stop();
    if (err) {
        LOG_ERR("rsrp_service_stop failed: err=%d", err);
    }

    LOG_WRN("EXIT: STATE_LTEM_CONNECTED");
}


static void cloud_connecting_entry(void *obj)
{
    struct app_ctx *ctx = obj;
    int err; 

    LOG_WRN("ENTER: STATE_CLOUD_CONNECTING");

    ctx->cloud_connected = false; 

    err = cloud_service_connect_async();
    if (err){
        LOG_ERR("cloud_service_connect_async err=%d", err);
        (void)app_event_publish_type(EVT_CLOUD_FAIL);
        return;
    }

    LOG_INF("Cloud connecting...");
}

static enum smf_state_result cloud_connecting_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch(ctx->ev.type){
        case EVT_CLOUD_OK:
            ctx->cloud_connected = true; 
            ctx->last_done = STEP_CLOUD_DONE; 

            LOG_INF("Cloud connected"); 
            LOG_WRN("TRANSITION: STATE_CLOUD -> STATE_LTEM_CONNECTED");
            transition_to_state(ctx, STATE_LTEM_CONNECTED); 
            return SMF_EVENT_HANDLED;

        case EVT_CLOUD_FAIL: 
            ctx->cloud_connected = false; 

            LOG_WRN("Cloud connection failed");
            LOG_WRN("TRANSITION: STATE_CLOUD_CONNECTING -> STATE_BACKOFF");
            transition_to_state(ctx, STATE_BACKOFF);
            return SMF_EVENT_HANDLED;

        case EVT_CLOUD_DISCONNECTED:
            ctx->cloud_connected = false;

            LOG_WRN("Cloud disconnected while connecting");
            LOG_WRN("TRANSITION: STATE_CLOUD -> STATE_BACKOFF");
            /* not sure if this backoff needs to handle this hmm */
            //transition_to_state(ctx, STATE_BACKOFF);
            transition_to_state(ctx, STATE_LTEM_CONNECTED);
            return SMF_EVENT_HANDLED;

        default:
            return SMF_EVENT_PROPAGATE;
    }
}

static void cloud_connecting_exit(void *obj)
{
    ARG_UNUSED(obj);
    LOG_WRN("EXIT: STATE_CLOUD_CONNECTING"); 
}


static void lte_location_entry(void *obj)
{
    ARG_UNUSED(obj);

    int err; 
    LOG_WRN("ENTER: STATE_LTE_LOCATION");
    
    err = location_service_start_lte_location();
    if (err){
        LOG_ERR("location_service_start_lte_location() -> %d", err);
        (void)app_event_publish_type(EVT_LTE_LOC_FAIL);
        return;
    }
    LOG_INF("LTE location started");
}

static enum smf_state_result lte_location_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type){
        case EVT_LTE_LOC_OK:
            ctx->last_pvt = ctx->ev.pvt;
            ctx->have_fix = true; 
            ctx->last_done = STEP_LTE_LOC_DONE;

            ctx->gnss_goal = GNSS_GOAL_REFINE_LTE_FIX;
            ctx->gnss_timeout_sec = CONFIG_APP_GNSS_TIMEOUT_SEC;
            ctx->gnss_extend_once = false; 

            LOG_INF("LTE location fix stored: lat=%f lon=%f",
                (double)ctx->last_pvt.latitude,
                (double)ctx->last_pvt.longitude);
            
            LOG_WRN("TRANSITION: STATE_LTE_LOCATION -> STATE_LTEM_CONNECTED");
            transition_to_state(ctx, STATE_LTEM_CONNECTED);
            return SMF_EVENT_HANDLED;

        case EVT_LTE_LOC_FAIL:
            LOG_WRN("LTE location failed");
            ctx->last_done=STEP_LTE_LOC_DONE;
            
            LOG_WRN("TRANSITION: STATE_LTE_LOCATION -> STATE_LTEM_CONNECTED");
            transition_to_state(ctx, STATE_LTEM_CONNECTED);
            return SMF_EVENT_HANDLED;

        case EVT_LTE_LOC_TIMEOUT:
            LOG_WRN("LTE location timeout");
            ctx->last_done=STEP_LTE_LOC_DONE;

            LOG_WRN("TRANSITION: STATE_LTE_LOCATION -> STATE_LTEM_CONNECTED"); 
            transition_to_state(ctx, STATE_LTEM_CONNECTED);
            return SMF_EVENT_HANDLED;

        default:
            return SMF_EVENT_PROPAGATE;
    }
}

static void lte_location_exit(void *obj)
{
    ARG_UNUSED(obj);

    int err = location_service_stop();
    if(err){
        LOG_WRN("location_service_stop failed: %d", err); 
    }

    LOG_WRN("EXIT: STATE_LTE_LOCATION");
}


static void gnss_acquire_entry(void *obj)
{
    struct app_ctx *ctx = obj;
    int err;
    int32_t timeout_sec = ctx->gnss_timeout_sec;

    
    LOG_WRN("ENTER: STATE_GNSS_ACQUIRE");

    if (timeout_sec <= 0) {
        timeout_sec = CONFIG_APP_GNSS_TIMEOUT_SEC;
    }

    LOG_INF("GNSS goal=%d timeout=%d sec",
            ctx->gnss_goal, timeout_sec);

    err = gnss_service_start_assisted(timeout_sec);
    LOG_INF("gnss_service_start_assisted() -> %d", err);

    if (err) {
        LOG_ERR("gnss_service_start_assisted failed: %d", err);

        struct app_event ev = {
            .type = EVT_TIMEOUT
        };

        (void)app_event_put(&ev, K_NO_WAIT);
        return;
    }

    LOG_INF("GNSS acquire started (A-GNSS handled internally)");
}

static enum smf_state_result gnss_acquire_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_GNSS_FIX: {
        bool required_for_ntn =
            (ctx->gnss_goal == GNSS_GOAL_REQUIRED_FOR_NTN);

        ctx->last_pvt = ctx->ev.pvt;
        ctx->have_fix = true;
        ctx->last_done = STEP_GNSS_DONE;

        ctx->gnss_goal = GNSS_GOAL_NONE;
        ctx->gnss_timeout_sec = 0;
        ctx->gnss_extend_once = false;

        LOG_INF("GNSS FIX OK: lat=%f, lon=%f",
                (double)ctx->last_pvt.latitude,
                (double)ctx->last_pvt.longitude);

        if (required_for_ntn) {
            LOG_WRN("TRANSITION: STATE_GNSS_ACQUIRE -> STATE_NTN_CONNECTING");
            transition_to_state(ctx, STATE_NTN_CONNECTING);
        } else {
            LOG_WRN("TRANSITION: STATE_GNSS_ACQUIRE -> STATE_LTEM_CONNECTED");
            transition_to_state(ctx, STATE_LTEM_CONNECTED);
        }

        return SMF_EVENT_HANDLED;
    }

    case EVT_TIMEOUT: {
        LOG_INF("GNSS_ACQUIRE: gnss timeout");

        if (ctx->gnss_goal == GNSS_GOAL_REQUIRED_FOR_NTN &&
            ctx->gnss_extend_once) {
            ctx->gnss_extend_once = false;

            LOG_WRN("GNSS required for NTN: extending search once by %d sec",
                    CONFIG_APP_GNSS_TIMEOUT_SEC);
            (void)gnss_service_start_timeout(CONFIG_APP_GNSS_TIMEOUT_SEC);
            return SMF_EVENT_HANDLED;
        }

        bool required_for_ntn =
            (ctx->gnss_goal == GNSS_GOAL_REQUIRED_FOR_NTN);

        ctx->last_done = STEP_GNSS_DONE;
        ctx->gnss_goal = GNSS_GOAL_NONE;
        ctx->gnss_timeout_sec = 0;
        ctx->gnss_extend_once = false;

        LOG_WRN("No GNSS fix obtained");

        if (required_for_ntn) {
            LOG_WRN("TRANSITION: STATE_GNSS_ACQUIRE -> STATE_BACKOFF");
            transition_to_state(ctx, STATE_BACKOFF);
        } else {
            LOG_WRN("TRANSITION: STATE_GNSS_ACQUIRE -> STATE_LTEM_CONNECTED");
            transition_to_state(ctx, STATE_LTEM_CONNECTED);
        }

        return SMF_EVENT_HANDLED;
    }

    default:
        LOG_INF("default smf handled");
        return SMF_EVENT_PROPAGATE;
    }
}

static void gnss_acquire_exit(void *obj)
{
    ARG_UNUSED(obj);
    (void)gnss_service_cancel_timeout();
    (void)gnss_service_stop();
    LOG_WRN("EXIT: STATE_GNSS_ACQUIRE");
}

static void ntn_connecting_entry(void *obj)
{
    struct app_ctx *ctx = obj;

    int err = ntn_service_connect(ctx);

    if (err) {
        LOG_INF("ntn initialization failed (%d)", err);
        struct app_event ev = {
            .type = EVT_MODEM_SWITCH_FAIL,
            .source_rat = RAT_NTN,
        };
        (void)app_event_put(&ev, K_NO_WAIT);
        return;
    }

    LOG_INF("(%s) ntn connect started", __func__);
}

static enum smf_state_result ntn_connecting_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_REG_OK:
        if (ctx->ev.source_rat != RAT_NTN) {
            LOG_WRN("Ignoring %s from %s during NTN connect",
                    app_evt_name(ctx->ev.type),
                    rat_name(ctx->ev.source_rat));
            return SMF_EVENT_HANDLED;
        }
        LOG_INF("ntn registered ok");

        /* connection success -> reset retry attempt-counter */
        retry_reset(ctx, RAT_NTN);
        
        ctx->active_rat = RAT_NTN;
        transition_to_state(ctx, STATE_NTN_CONNECTED);
        return SMF_EVENT_HANDLED;
    
    case EVT_PDN_UP:
        if (ctx->ev.source_rat != RAT_NTN) {
            LOG_WRN("Ignoring %s from %s during NTN connect",
                    app_evt_name(ctx->ev.type),
                    rat_name(ctx->ev.source_rat));
            return SMF_EVENT_HANDLED;
        }
        LOG_WRN("PDN_UP in NTN_CONNECTING (unexpected order)");
        retry_reset(ctx, RAT_NTN);
        ctx->pdn_up = true;
        ctx->active_rat = RAT_NTN;
        transition_to_state(ctx, STATE_NTN_CONNECTED);
        return SMF_EVENT_HANDLED;

    case EVT_MODEM_SWITCH_FAIL:
        LOG_WRN("NTN modem setup failed");
        __fallthrough;

    case EVT_REG_FAIL:
        if (ctx->ev.source_rat != RAT_NTN) {
            LOG_WRN("Ignoring %s from %s during NTN connect",
                    app_evt_name(ctx->ev.type),
                    rat_name(ctx->ev.source_rat));
            return SMF_EVENT_HANDLED;
        }

        LOG_INF("ntn connect failed");
        __fallthrough;

    case EVT_PDN_DOWN:
        if (ctx->ev.type == EVT_PDN_DOWN && ctx->ev.source_rat != RAT_NTN) {
            LOG_WRN("Ignoring %s from %s during NTN connect",
                    app_evt_name(ctx->ev.type),
                    rat_name(ctx->ev.source_rat));
            return SMF_EVENT_HANDLED;
        }
        __fallthrough;

    case EVT_TIMEOUT:
        LOG_INF("ntn connect failed/timeout");
        ctx->pdn_up=false;

        /* increment NTN connect attempts */
        uint8_t attempts = retry_inc(ctx, RAT_NTN);

        if (attempts >= CONFIG_APP_MAX_NTN_RETRIES) {
            LOG_WRN("NTN retries exhausted -> trying LTE-M connection...");
            retry_reset(ctx, RAT_NTN);
            ctx->next_rat = RAT_LTEM;
        } else {
            ctx->next_rat = RAT_NTN;
        }
        
        transition_to_state(ctx, STATE_BACKOFF);
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_PROPAGATE;
    }
}

static void ntn_connecting_exit(void *obj)
{
    ARG_UNUSED(obj);
}

static void ntn_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    struct app_event ev = {
        .type = EVT_TIMEOUT
    };

    (void)app_event_put(&ev, K_NO_WAIT);
}

static void ntn_connected_entry(void *obj)
{
    struct app_ctx *ctx = obj;

/* force lte probe check after 5s for test change to 50 or something later */
//#if defined(CONFIG_APP_CORE_SM_PROBE_TEST)
    LOG_INF("Starting ntn probe timer");
    int32_t delay_ms = 5000;
    k_timer_start(&ctx->ntn_timer,
                  K_MSEC(delay_ms),
                  K_NO_WAIT);
//#endif

    LOG_INF("(%s) finished entering ntn connected", __func__);
}

static enum smf_state_result ntn_connected_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_TIMEOUT:
        LOG_INF("ntn timeout -> entering lte-probe");
        transition_to_state(ctx, STATE_LTE_PROBE);
        return SMF_EVENT_HANDLED;

    case EVT_PDN_UP:
        ctx->pdn_up = true;
        LOG_INF("PDN up");

#if defined(CONFIG_APP_CORE_SEND_UDP_DATA)
        int err = modem_service_udp_send_test();
        LOG_INF("UDP test send result: %d", err);
#endif

        return SMF_EVENT_HANDLED;
    

    default:
        return SMF_EVENT_PROPAGATE;
    }
}



static void ntn_connected_exit(void *obj)
{
    LOG_INF("ntn connected exit");
    struct app_ctx *ctx = obj;
    k_timer_stop(&ctx->ntn_timer);
    ARG_UNUSED(obj);
}

static void lte_probe_entry(void *obj)
{
    int err;
    struct app_ctx *ctx = obj;

    lte_service_set_probe_pending(true);

    err = modem_service_switch_to_tn();
    if (err) {
        LOG_WRN("TN switch failed, attempting NTN restore");

        lte_service_set_probe_pending(false);

        err = modem_service_switch_to_ntn();
        if (err) {
            ctx->next_rat = RAT_NTN;
            transition_to_state(ctx, STATE_NTN_CONNECTING);
            return;
        }

        ctx->active_rat = RAT_NTN;
        transition_to_state(ctx, STATE_NTN_CONNECTED);
        return;
    }

    LOG_INF("lte_probe_entry");
}

static enum smf_state_result lte_probe_run(void *obj)
{
    int err;
    struct app_ctx *ctx = obj;
    
    switch(ctx->ev.type) {
    case EVT_RSRP_UPDATE:
        LOG_INF("received rsrp update event");
        return SMF_EVENT_HANDLED;
        
    case EVT_LTE_POOR:
        LOG_INF("LTE probe: TN still bad -> returning to NTN");

        lte_service_set_probe_pending(false);

        err = modem_service_switch_to_ntn();
        if (err) {
            LOG_WRN("Could not resume NTN context -> full NTN reconnect");
            ctx->pdn_up = false;
            ctx->next_rat = RAT_NTN;
            transition_to_state(ctx, STATE_NTN_CONNECTING);
            return SMF_EVENT_HANDLED;
        }

        ctx->active_rat = RAT_NTN;
        transition_to_state(ctx, STATE_NTN_CONNECTED);
        return SMF_EVENT_HANDLED;

    case EVT_LTE_GOOD:
        LOG_INF("LTE probe: TN good -> UDP test");
        
        /* send udp test */
        err = modem_service_udp_send_test();
        
        if (err) {
            LOG_ERR("UDP test failed: %d, TN context not valid", err);

            ctx->pdn_up = false;
            ctx->lte_connected = false;
            ctx->next_rat = RAT_LTEM;

            lte_service_set_probe_pending(false);

            transition_to_state(ctx, STATE_LTEM_CONNECTING);
            return SMF_EVENT_HANDLED;
        }

        LOG_INF("UDP TEST OK: TN context appears valid");

        ctx->active_rat = RAT_LTEM;
        ctx->next_rat = RAT_LTEM;
        ctx->pdn_up = true;
        ctx->lte_connected = true;

        lte_service_set_probe_pending(false);

        transition_to_state(ctx, STATE_LTEM_CONNECTED);
        return SMF_EVENT_HANDLED;
    

    case EVT_TN_READY_FOR_PROBE: {
        LOG_INF("something");

        struct lte_lc_conn_eval_params conn_eval = {0};
        
        err = modem_service_conn_eval_get(&conn_eval);
        if (err) {
            LOG_WRN("LTE probe conn eval failed: %d", err);
            lte_service_set_probe_pending(false);

            err = modem_service_switch_to_ntn();
            if (err) {
                LOG_WRN("Could not resume NTN after conn eval fail -> full NTN reconnect");
                ctx->next_rat = RAT_NTN;
                transition_to_state(ctx, STATE_NTN_CONNECTING);
                return SMF_EVENT_HANDLED;
            }

            ctx->active_rat = RAT_NTN;
            transition_to_state(ctx, STATE_NTN_CONNECTED);
            return SMF_EVENT_HANDLED;
        }

        /* start rsrp probe with n attempts */
        err = rsrp_service_start_probe(3);
        if (err < 0) {
            LOG_ERR("rsrp_service_start_probe failed: %d", err);

            lte_service_set_probe_pending(false);

            err = modem_service_switch_to_ntn();
            if (err) {
                ctx->next_rat = RAT_NTN;
                transition_to_state(ctx, STATE_NTN_CONNECTING);
                return SMF_EVENT_HANDLED;
            }

            ctx->active_rat = RAT_NTN;
            transition_to_state(ctx, STATE_NTN_CONNECTED);
            return SMF_EVENT_HANDLED;
        }
        return SMF_EVENT_HANDLED;
    }
    case EVT_PDN_DOWN:
    case EVT_REG_FAIL:
        LOG_WRN("LTE probe lost PDN/registration -> reconnect LTE-M");
        lte_service_set_probe_pending(false);

        ctx->pdn_up = false;
        ctx->lte_connected = false;
        ctx->next_rat = RAT_LTEM;

        transition_to_state(ctx, STATE_LTEM_CONNECTING);
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_PROPAGATE;
    }

}

    
static void backoff_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    struct app_event ev = {
        .type = EVT_TIMEOUT
    };

    (void)app_event_put(&ev, K_NO_WAIT);
}

void app_start_backoff_timer(struct app_ctx *ctx)
{
    int32_t delay_ms = 3000; // test: 3 sek

    LOG_INF("Starting backoff timer: %d ms", delay_ms);

    k_timer_start(&ctx->backoff_timer,
                  K_MSEC(delay_ms),
                  K_NO_WAIT);
}

static void backoff_entry(void *obj)
{
    struct app_ctx *ctx = obj;

    LOG_INF("Entering backoff");
    app_start_backoff_timer(ctx);
}

static enum smf_state_result backoff_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_TIMEOUT:
        if (ctx->next_rat != RAT_NTN) {
            LOG_INF("Retry LTE connect");
            transition_to_state(ctx, STATE_LTEM_CONNECTING);
            return SMF_EVENT_HANDLED;
        }

        LOG_INF("Trying to connect NTN");

        if (!ctx->have_fix) {
            LOG_INF("No GNSS fix -> trying to acquire fix");
            ctx->gnss_goal = GNSS_GOAL_REQUIRED_FOR_NTN;
            ctx->gnss_timeout_sec = CONFIG_APP_GNSS_TIMEOUT_SEC;
            ctx->gnss_extend_once = true;

            LOG_WRN("TRANSITION: STATE_BACKOFF -> STATE_GNSS_ACQUIRE");
            transition_to_state(ctx, STATE_GNSS_ACQUIRE);
            return SMF_EVENT_HANDLED;
        }

        transition_to_state(ctx, STATE_NTN_CONNECTING);
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_PROPAGATE;
    }
}

static void backoff_exit(void *obj)
{
    struct app_ctx *ctx = obj;

    LOG_INF("Exiting backoff");
    k_timer_stop(&ctx->backoff_timer);
}


static void handoff_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    struct app_event ev = {
        .type = EVT_START_GNSS
    };

    (void)app_event_put(&ev, K_NO_WAIT);
}

/* main smf thread setup */
#define SMF_STACK_SIZE 2048
#define SMF_PRIORITY 5

K_THREAD_STACK_DEFINE(smf_stack, SMF_STACK_SIZE);

static struct k_thread smf_thread_data;

static void smf_thread(void *p1, void *p2, void *p3)
{
    //void app_sm_post_dispatch(struct app_ctx *ctx, const struct app_event *ev);

    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    struct app_ctx *ctx = p1;

    smf_set_initial(SMF_CTX(ctx), &states[STATE_BOOT]);
    ctx->state = STATE_BOOT;

    while (1) {
        const struct zbus_channel *chan;
        union app_sm_msg msg = {0};
        int err = zbus_sub_wait_msg(&app_fsm_sub, &chan, &msg, K_FOREVER);

        if (err) {
            LOG_WRN("zbus_sub_wait_msg failed, err=%d", err);
            continue;
        }

        if (chan == &app_evt_chan) {
            dispatch_app_event(ctx, &msg.app_event);
            //app_sm_post_dispatch(ctx, &msg.app_event);
            continue;
        }

        LOG_WRN("Received message from unexpected channel: %s", zbus_chan_name(chan));
    }
}

int app_sm_start(struct app_ctx *ctx)
{
    ctx->state = STATE_BOOT;
    ctx->rsrp_dbm = INT32_MIN;
    k_thread_create(&smf_thread_data, smf_stack, SMF_STACK_SIZE,
                    smf_thread, ctx, NULL, NULL,
                    SMF_PRIORITY, 0, K_NO_WAIT);
    return 0;
}
