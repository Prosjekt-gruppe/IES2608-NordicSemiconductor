/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include "app_sm.h"
#include "app_events.h"
#include "app_zbus.h"
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

LOG_MODULE_REGISTER(app_sm, LOG_LEVEL_INF);

ZBUS_MSG_SUBSCRIBER_DEFINE(app_fsm_sub); //Subscriber for app events

union app_sm_msg {
    struct app_event app_event;
    struct app_gnss_status gnss_status;
};

static void boot_entry(void *obj);
static enum smf_state_result boot_run(void *obj);

static void disconnected_entry(void *obj);

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

static void __maybe_unused handle_gnss_status(struct app_ctx *ctx,
                                              const struct app_gnss_status *status);

static void dispatch_app_event(struct app_ctx *ctx, const struct app_event *ev);
static void backoff_timer_handler(struct k_timer *timer);

static void ntn_timer_handler(struct k_timer *timer);
static void ltem_timer_handler(struct k_timer *timer);
static void handoff_timer_handler(struct k_timer *timer); 

static bool modem_profiles_ready(struct app_ctx *ctx);



static const struct smf_state states[] = {
    [STATE_BOOT] = SMF_CREATE_STATE(
        boot_entry,
        boot_run,
        NULL,
        NULL,
        NULL
    ),
    [STATE_RUNNING] = SMF_CREATE_STATE(
        NULL,
        NULL,
        NULL,
        NULL,
        NULL
    ),
    [STATE_CONNECTED] = SMF_CREATE_STATE(
        NULL,
        NULL,
        NULL,
        &states[STATE_RUNNING],
        NULL
    ),
    [STATE_DISCONNECTED] = SMF_CREATE_STATE(
        disconnected_entry,
        NULL,
        NULL,
        &states[STATE_RUNNING],
        NULL
    ),
    [STATE_LTEM_CONNECTING] = SMF_CREATE_STATE(
        ltem_connecting_entry, 
        ltem_connecting_run, 
        NULL,
        &states[STATE_DISCONNECTED],
        NULL
    ),
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
        &states[STATE_DISCONNECTED],
        NULL
    ),
    [STATE_BACKOFF] = SMF_CREATE_STATE(
        backoff_entry,
        backoff_run,
        backoff_exit,
        &states[STATE_DISCONNECTED],
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

static bool modem_profiles_ready(struct app_ctx *ctx)
{
    int err = modem_service_prepare_profiles(ctx);

    if (err) {
        LOG_ERR("modem_service_prepare_profiles failed: %d", err);
        return false;
    }

    return true;
}

static void dispatch_app_event(struct app_ctx *ctx, const struct app_event *ev)
{
    ctx->ev = *ev;
    LOG_INF("SMF thread got event %s", app_evt_name(ev->type));
    (void)smf_run_state(SMF_CTX(ctx));
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

static void disconnected_entry(void *obj)
{
    struct app_ctx *ctx = obj;

    (void)modem_profiles_ready(ctx);
}

static void ltem_connecting_entry(void *obj)
{
    struct app_ctx *ctx = obj;

    LOG_WRN("ENTER: STATE_LTEM_CONNECTING");

    if (!modem_profiles_ready(ctx)) {
        struct app_event ev = { .type = EVT_REG_FAIL };
        (void)app_event_put(&ev, K_NO_WAIT);
        return;
    }

    int err = lte_service_connect_async();
    if (err){
        LOG_ERR("lte_service_connect_async err=%d", err); 
        struct app_event ev = { .type = EVT_REG_FAIL};
        (void)app_event_put(&ev, K_NO_WAIT);
        return;
    }

    LOG_INF("LTEM connecting...");
}

static enum smf_state_result ltem_connecting_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
        case EVT_REG_OK: {
            ctx->active_rat = RAT_LTEM;
            ctx->lte_connected = true; 
            ctx->last_done = STEP_NONE;

            LOG_WRN("TRANSITION: STATE_LTEM_CONNECTING -> STATE_LTEM_CONNECTED");
            transition_to_state(ctx, STATE_LTEM_CONNECTED);
            return SMF_EVENT_HANDLED;
        }

        case EVT_REG_FAIL:
            ctx->next_rat = RAT_NTN;
            LOG_WRN("TRANSITION: STATE_LTEM_CONNECTING -> STATE_BACKOFF");
            transition_to_state(ctx, STATE_BACKOFF);
            return SMF_EVENT_HANDLED;

        default:
            return SMF_EVENT_HANDLED;
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
    LOG_INF("ltem timer start");
    k_timer_start(&ctx->lte_timer, K_MSEC(3000), K_NO_WAIT);
/*
#else
    /* just for test remove later */
    LOG_INF("lte timer force bad rsrp");
    /* allow cloud and gnss and those to setup ok */
    k_timer_start(&ctx->lte_timer, K_MSEC(60000), K_NO_WAIT);
*/
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
        LOG_INF("app_event_put(EVT_START_LTE_LOC) -> %d", pub_err); 
        return;
    }

    if(ctx->cloud_connected && ctx->last_done == STEP_CLOUD_DONE){
        struct app_event ev = {
            .type = EVT_START_LTE_LOC
        };

        LOG_INF("LTEM_CONNECTED: requesting LTE location");
        int pub_err = app_event_put(&ev, K_MSEC(10));
        LOG_INF("app_event_put(EVT_START_LTE_LOC) -> %d", pub_err);
        return;
    }

    if(ctx->cloud_connected && ctx->last_done == STEP_LTE_LOC_DONE) {
        struct app_event ev = {
            .type = EVT_START_GNSS
        };
        LOG_INF("LTEM_CONNECTED: requesting GNSS acquire");
        int pub_err = app_event_put(&ev, K_NO_WAIT);
        LOG_INF("app_event_put(EVT_START_LTE_LOC) -> %d", pub_err);
        return; 
    }
}

static enum smf_state_result ltem_connected_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_RSRP_UPDATE:
        ctx->rsrp_dbm = ctx->ev.meas.rsrp_dbm;
        LOG_INF("Updated LTE RSRP: %d dBm", ctx->rsrp_dbm);
        return SMF_EVENT_HANDLED;

    case EVT_LTE_POOR:
        LOG_WRN("LTE poor, consider switching RAT");
        ctx->next_rat = RAT_NTN;

        /* bad patch make better solution later if this works */
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
        return SMF_EVENT_HANDLED;
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
            return SMF_EVENT_HANDLED;
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
            ctx->gnss_timeout_sec = 30; 
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
            return SMF_EVENT_HANDLED;
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
        timeout_sec = 15;
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
    case EVT_GNSS_FIX:
        ctx->last_pvt = ctx->ev.pvt;
        ctx->have_fix = true;
        ctx->last_done = STEP_GNSS_DONE;
        ctx->gnss_goal = GNSS_GOAL_NONE;
        ctx->gnss_timeout_sec = 0;
        ctx->gnss_extend_once = false;

        LOG_INF("GNSS FIX OK: lat=%f, lon=%f",
                (double)ctx->last_pvt.latitude,
                (double)ctx->last_pvt.longitude);
        LOG_WRN("TRANSITION: STATE_GNSS_ACQUIRE -> STATE_LTEM_CONNECTED");

        transition_to_state(ctx, STATE_LTEM_CONNECTED);
        return SMF_EVENT_HANDLED;
    

    case EVT_TIMEOUT:
        LOG_INF("GNSS_ACQUIRE: gnss timeout");

        if (ctx->gnss_goal == GNSS_GOAL_REQUIRED_FOR_NTN &&
            ctx->gnss_extend_once) {
            ctx->gnss_extend_once = false;

            LOG_WRN("GNSS required for NTN: extending search once by 30 sec");
            (void)gnss_service_start_timeout(30);
            return SMF_EVENT_HANDLED;
        }

        ctx->last_done = STEP_GNSS_DONE;
        ctx->gnss_goal = GNSS_GOAL_NONE;
        ctx->gnss_timeout_sec = 0;
        ctx->gnss_extend_once = false;
        
        LOG_WRN("No GNSS fix obtained");
        LOG_WRN("TRANSITION: STATE_GNSS_ACQUIRE -> STATE_LTEM_CONNECTED");

        transition_to_state(ctx, STATE_LTEM_CONNECTED);
        return SMF_EVENT_HANDLED;

    default:
        LOG_INF("default smf handled");
        return SMF_EVENT_HANDLED;
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

    if (!modem_profiles_ready(ctx)) {
        struct app_event ev = { .type = EVT_REG_FAIL };
        (void)app_event_put(&ev, K_NO_WAIT);
        return;
    }

    int err = ntn_service_connect(ctx);

    if (err) {
        LOG_INF("ntn initialization failed (%d)", err);
        struct app_event ev = { .type = EVT_REG_FAIL };
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
        LOG_INF("ntn registered ok");

        /* connection success -> reset retry attempt-counter */
        retry_reset(ctx, RAT_NTN);
        
        ctx->active_rat = RAT_NTN;
        transition_to_state(ctx, STATE_NTN_CONNECTED);
        return SMF_EVENT_HANDLED;
    
    case EVT_PDN_UP:
        LOG_WRN("PDN_UP in NTN_CONNECTING (unexpected order)");
        retry_reset(ctx, RAT_NTN);
        ctx->pdn_up = true;
        ctx->active_rat = RAT_NTN;
        transition_to_state(ctx, STATE_NTN_CONNECTED);
        return SMF_EVENT_HANDLED;

    case EVT_REG_FAIL:
    case EVT_PDN_DOWN:
    case EVT_TIMEOUT:
        LOG_INF("ntn connect failed/timeout");
        ctx->pdn_up=false;

        /* increment NTN connect attempts */
        uint8_t attempts = retry_inc(ctx, RAT_NTN);

        if (attempts >= CONFIG_APP_MAX_NTN_RETRIES) {
            LOG_WRN("NTN retries exhausted -> trying LTE-M connection...");
            retry_reset(ctx, RAT_NTN);
            ctx->next_rat = RAT_LTEM;
        }
        
        transition_to_state(ctx, STATE_BACKOFF);
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_HANDLED;
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
    
    case EVT_REG_FAIL:
    case EVT_PDN_DOWN:
        ctx->pdn_up = false;
        ctx->next_rat = RAT_LTEM;
        LOG_INF("NTN connection lost/failed");
        transition_to_state(ctx, STATE_BACKOFF);
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_HANDLED;
    }
}



static void ntn_connected_exit(void *obj)
{
    LOG_INF("ntn connected exit");
#if defined(CONFIG_APP_CORE_SM_PROBE_TEST)
    struct app_ctx *ctx = obj;

    k_timer_stop(&ctx->ntn_timer);
#else
    ARG_UNUSED(obj);
#endif
}

static void __maybe_unused handle_gnss_status(struct app_ctx *ctx,
                                              const struct app_gnss_status *status)
{
    struct app_event ev = {0};
    
    switch (status->state) {
        case APP_GNSS_STATE_FIX:
        ev.type = EVT_GNSS_FIX;
        ev.pvt.latitude = status->latitude;
        ev.pvt.longitude = status->longitude;
        ev.pvt.altitude = status->altitude;
        dispatch_app_event(ctx, &ev);
        return;
        
        case APP_GNSS_STATE_TIMEOUT:
        ev.type = EVT_TIMEOUT;
        dispatch_app_event(ctx, &ev);
        return;
        
        case APP_GNSS_STATE_ERROR:
        LOG_WRN("GNSS reported error %d, treating as timeout", status->err);
        ev.type = EVT_TIMEOUT;
        dispatch_app_event(ctx, &ev);
        return;
        
        default:
        LOG_INF("GNSS status update: state=%d satellites=%u ttff_ms=%lld",
            status->state,
            status->tracked_satellites,
            (long long)status->time_to_first_fix_ms);
            return;
        }
    }
    //#endif
    
static void lte_probe_entry(void *obj)
{
    int err;
    struct app_ctx *ctx = obj;
    
    /* enable lte-service probe event on rsrp */
    lte_service_set_probe_pending(true);
    
    /* switch to tn network */
    err = modem_service_switch_to_tn();
    if (err) {
        LOG_INF("switch to tn lte probe fault");
        lte_service_set_probe_pending(false);
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
        LOG_INF("LTE probe: TN still bad -> staying on NTN");

        /* return to NTN */
        modem_service_switch_to_ntn();

        transition_to_state(ctx, STATE_NTN_CONNECTED);
        return SMF_EVENT_HANDLED;

    case EVT_LTE_GOOD:
        LOG_INF("LTE probe: TN good -> UDP test");
        
        /* send udp test */
        err = modem_service_udp_send_test();
        
        if (err) {
            LOG_ERR("UDP test failed: %d", err);
            
            modem_service_switch_to_ntn();
            transition_to_state(ctx, STATE_NTN_CONNECTED);
            return SMF_EVENT_HANDLED;
        }
    
        LOG_INF("UDP TEST OK");
    
    
        /* should probably go to LTEM_CONNECTING/CONNECTED but for this test its ok */
        ctx->next_rat = RAT_LTEM;

        /*
         * TODO: Might have to do a check here to see if context 
         *       was preserved successfully, If not it might have
         *       to reconnect to STATE_LTEM_CONNECTING again.
         */

        transition_to_state(ctx, STATE_LTEM_CONNECTED);
        return SMF_EVENT_HANDLED;

    case EVT_TN_READY_FOR_PROBE:
        LOG_INF("something");

        /* start rsrp probe with n attempts */
        err = rsrp_service_start_probe(3);
        if (err < 0) {
            LOG_ERR("rsrp_service_start_probe failed: %d", err);
            modem_service_switch_to_ntn();
            transition_to_state(ctx, STATE_NTN_CONNECTED);
        }

        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_HANDLED;
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
            ctx->gnss_timeout_sec = 60; 
            ctx->gnss_extend_once = true;

            LOG_WRN("TRANSITION: STATE_BACKOFF -> STATE_GNSS_ACQUIRE");
            transition_to_state(ctx, STATE_GNSS_ACQUIRE);
            return SMF_EVENT_HANDLED;
        }

        transition_to_state(ctx, STATE_NTN_CONNECTING);
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_HANDLED;
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

        /*
        if (chan == &gnss_status_chan) {
            handle_gnss_status(ctx, &msg.gnss_status);
            continue;
        }
        */

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
