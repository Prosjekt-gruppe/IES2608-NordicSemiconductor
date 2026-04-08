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
// #include "ntn_service.h"
#include "modem_service.h"
#include "lte_service.h"
#include "location_service.h"

#include <modem/nrf_modem_lib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app_sm, LOG_LEVEL_INF);

ZBUS_MSG_SUBSCRIBER_DEFINE(app_fsm_sub); // Subscriber for app events and GNSS status updates

union app_sm_msg
{
    struct app_event app_event;
    struct app_gnss_status gnss_status;
};

static void boot_entry(void *obj);
static enum smf_state_result boot_run(void *obj);

static void ltem_connecting_entry(void *obj);
static enum smf_state_result ltem_connecting_run(void *obj);

static void ltem_connected_entry(void *obj);
static enum smf_state_result ltem_connected_run(void *obj);
static void ltem_connected_exit(void *obj);

static void lte_location_entry(void *obj);
static enum smf_state_result lte_location_run(void *obj);
static void lte_location_exit(void *obj);

static void gnss_refine_entry(void *obj);
static enum smf_state_result gnss_refine_run(void *obj);
static void gnss_refine_exit(void *obj);

static void backoff_entry(void *obj);
static enum smf_state_result backoff_run(void *obj);

static void dispatch_app_event(struct app_ctx *ctx, const struct app_event *ev);
static void backoff_timer_handler(struct k_timer *timer);

/*
static void ntn_connecting_entry(void *obj);
static enum smf_state_result ntn_connecting_run(void *obj);
static void ntn_connecting_exit(void *obj);

static void ntn_connected_entry(void *obj);
static enum smf_state_result ntn_connected_run(void *obj);
static void ntn_connected_exit(void *obj);
*/

//static void handle_gnss_status(struct app_ctx *ctx, const struct app_gnss_status *status);

static const struct smf_state states[] = {
    [STATE_BOOT] = SMF_CREATE_STATE(
        boot_entry,
        boot_run,
        NULL,
        NULL,
        NULL),
    [STATE_LTEM_CONNECTING] = SMF_CREATE_STATE(
        ltem_connecting_entry,
        ltem_connecting_run,
        NULL,
        NULL,
        NULL),
    [STATE_LTEM_CONNECTED] = SMF_CREATE_STATE(
        ltem_connected_entry,
        ltem_connected_run,
        ltem_connected_exit,
        NULL,
        NULL),

    [STATE_LTE_LOCATION] = SMF_CREATE_STATE(
        lte_location_entry,
        lte_location_run,
        lte_location_exit,
        NULL,
        NULL),

    [STATE_GNSS_REFINE] = SMF_CREATE_STATE(
        gnss_refine_entry,
        gnss_refine_run,
        gnss_refine_exit,
        NULL,
        NULL),

    /*
    [STATE_NTN_CONNECTING] = SMF_CREATE_STATE(
        ntn_connecting_entry,
        ntn_connecting_run,
        ntn_connecting_exit,
        NULL,
        NULL
    ),
    [STATE_NTN_CONNECTED] = SMF_CREATE_STATE(
        ntn_connected_entry,
        ntn_connected_run,
        ntn_connected_exit,
        NULL,
        NULL
    ),
    */

    [STATE_IDLE] = SMF_CREATE_STATE(
        NULL,
        NULL,
        NULL,
        NULL,
        NULL),
    [STATE_BACKOFF] = SMF_CREATE_STATE(
        backoff_entry,
        backoff_run,
        NULL,
        NULL,
        NULL),
};

static void boot_entry(void *obj)
{
    struct app_ctx *ctx = obj;

    int err = modem_service_init();
    if (err)
    {
        LOG_ERR("modem_service_init err=%d", err);
        return;
    }

    err = lte_service_init();
    if (err)
    {
        LOG_ERR("lte_service_init err=%d", err);
        return;
    }

    err = rsrp_service_init();
    if (err)
    {
        LOG_ERR("rsrp_service_init err=%d", err);
        return;
    }

    err = location_service_init();
    if (err)
    {
        LOG_ERR("location_service_init err=%d", err);
        return;
    }

    err = gnss_service_init();
    if (err)
    {
        LOG_ERR("gnss_service_init err=%d", err);
        return;
    }

    /* init timers */
    k_timer_init(&ctx->backoff_timer, backoff_timer_handler, NULL);

    LOG_INF("BOOT complete");
}

static enum smf_state_result boot_run(void *obj)
{
    struct app_ctx *ctx = obj;

    if (ctx->ev.type == EVT_BOOT)
    {

        // Kconfig Debugging Option
        if (IS_ENABLED(CONFIG_APP_DEBUG_BOOT))
        {
            LOG_INF("DEBUG: Halting in STATE_BOOT after initalization");
        }
        else
        {
            LOG_INF("Transition: STATE_BOOT --> STATE_LTEM_CONNECTING");
            smf_set_state(SMF_CTX(ctx), &states[STATE_LTEM_CONNECTING]);
        }

        return SMF_EVENT_HANDLED;
    }
    return SMF_EVENT_HANDLED;
}


    static void ltem_connecting_entry(void *obj)
    {
        ARG_UNUSED(obj);

        int err = lte_service_connect_async();
        if (err)
        {
            LOG_ERR("lte_service_connect_async err=%d", err);
            struct app_event ev = {.type = EVT_REG_FAIL};
            (void)app_event_put(&ev, K_NO_WAIT);
            return;
        }

        LOG_INF("LTEM connecting...");
    }

    static enum smf_state_result ltem_connecting_run(void *obj)
    {
        struct app_ctx *ctx = obj;

        switch (ctx->ev.type)
        {
        case EVT_REG_OK:
            ctx->active_rat = RAT_LTEM;

        if (IS_ENABLED(CONFIG_APP_DEBUG_LTE_CONNECTING)){
            LOG_INF("Debug: Halting after LTE registration");
            smf_set_state(SMF_CTX(ctx), &states[STATE_IDLE]);
        } else{
            smf_set_state(SMF_CTX(ctx), &states[STATE_LTEM_CONNECTED]);
        }
        return SMF_EVENT_HANDLED; 

        case EVT_REG_FAIL:
            smf_set_state(SMF_CTX(ctx), &states[STATE_BACKOFF]);
            return SMF_EVENT_HANDLED;

        default:
            return SMF_EVENT_HANDLED;
        }
    }




    static void ltem_connected_entry(void *obj)
    {
        struct app_ctx *ctx = obj;
        int err;
        int rsrp_dbm;

        ctx->active_rat = RAT_LTEM;
        ctx->lte_connected = true;

        err = rsrp_service_get(&rsrp_dbm);
        if (!err)
        {
            ctx->rsrp_dbm = rsrp_dbm;
            LOG_INF("LTE RSRP on entry: %d dBm", rsrp_dbm);
        }
        else
        {
            LOG_WRN("Could not read LTE RSRP: %d", err);
        }

        err = rsrp_service_start();
        if (err < 0)
        {
            LOG_WRN("Failed to start LTE signal monitor: %d", err);
        }

        LOG_INF("ltem_connected_entry ok");

        smf_set_state(SMF_CTX(ctx), &states[STATE_LTE_LOCATION]);
    }

    static enum smf_state_result ltem_connected_run(void *obj)
    {
        struct app_ctx *ctx = obj;

        switch (ctx->ev.type)
        {
        case EVT_RSRP_UPDATE:
            ctx->rsrp_dbm = ctx->ev.meas.rsrp_dbm;
            LOG_INF("Updated LTE RSRP: %d dBm", ctx->rsrp_dbm);
            return SMF_EVENT_HANDLED;

        case EVT_LTE_POOR:
            LOG_WRN("LTE poor");
            smf_set_state(SMF_CTX(ctx), &states[STATE_BACKOFF]);
            return SMF_EVENT_HANDLED;

        default:
            return SMF_EVENT_HANDLED;
        }
    }

    static void ltem_connected_exit(void *obj)
    {
        ARG_UNUSED(obj);
    }

    static void lte_location_entry(void *obj)
    {
        int err;
        struct app_ctx *ctx = obj;

        ctx->lte_loc_requested = true;
        ctx->lte_fix = false;

        err = location_service_start_lte_location();
        if (err)
        {
            LOG_ERR("location_service_start_lte_location err=%d", err);
            app_event_publish_type(EVT_LTE_LOC_FAIL);
            return;
        }

        LOG_INF("LTE location started");
    }

    static enum smf_state_result lte_location_run(void *obj)
    {
        struct app_ctx *ctx = obj;

        switch (ctx->ev.type)
        {
        case EVT_LTE_LOC_OK:
            ctx->lte_fix = true;
            ctx->lte_pvt = ctx->ev.pvt;
            LOG_INF("LTE location fix: lat=%f lon=%f",
                    (double)ctx->lte_pvt.latitude,
                    (double)ctx->lte_pvt.longitude);
            smf_set_state(SMF_CTX(ctx), &states[STATE_GNSS_REFINE]);
            return SMF_EVENT_HANDLED;

        case EVT_LTE_LOC_FAIL:
        case EVT_LTE_LOC_TIMEOUT:
            LOG_WRN("LTE location failed or timed out; continue to GNSS refine");
            smf_set_state(SMF_CTX(ctx), &states[STATE_GNSS_REFINE]);
            return SMF_EVENT_HANDLED;

        default:
            return SMF_EVENT_HANDLED;
        }
    }

    static void lte_location_exit(void *obj)
    {
        ARG_UNUSED(obj);
    }

    static void dispatch_app_event(struct app_ctx * ctx, const struct app_event *ev)
    {
        ctx->ev = *ev;
        LOG_INF("SMF thread got event %s", app_evt_name(ev->type));
        (void)smf_run_state(SMF_CTX(ctx));
    }

    static void gnss_refine_entry(void *obj)
    {
        struct app_ctx *ctx = obj;

        ctx->agnss_requested = false;
        ctx->gnss_fix = false;

        (void)gnss_service_start_timeout(CONFIG_APP_GNSS_TIMEOUT_SEC);

        int err = gnss_service_start();
        if (err)
        {
            LOG_ERR("gnss_service_start err=%d", err);
            app_event_publish_type(EVT_GNSS_TIMEOUT);
            return;
        }

        LOG_INF("GNSS refine started");
    }

    static enum smf_state_result gnss_refine_run(void *obj)
    {
        struct app_ctx *ctx = obj;

        switch (ctx->ev.type)
        {
        case EVT_AGNSS_REQUEST:
        {
            int err;

            ctx->agnss_requested = true;
            LOG_INF("Received EVT_AGNSS_REQUEST, starting CoAP A-GNSS");

            err = location_service_start_agnss();
            if (err)
            {
                LOG_ERR("location_service_start_agnss err=%d", err);
                app_event_publish_type(EVT_AGNSS_FAIL);
            }
            return SMF_EVENT_HANDLED;
        }

        case EVT_AGNSS_READY:
            LOG_INF("A-GNSS injection complete");
            return SMF_EVENT_HANDLED;

        case EVT_AGNSS_FAIL:
            LOG_WRN("A-GNSS failed; continue GNSS without assistance");
            return SMF_EVENT_HANDLED;

        case EVT_GNSS_FIX:
            ctx->gnss_fix = true;
            ctx->gnss_pvt = ctx->ev.pvt;
            ctx->final_fix = true;
            ctx->final_pvt = ctx->ev.pvt;

            LOG_INF("GNSS fix OK: lat=%f lon=%f alt=%f",
                    (double)ctx->gnss_pvt.latitude,
                    (double)ctx->gnss_pvt.longitude,
                    (double)ctx->gnss_pvt.altitude);

            (void)gnss_service_stop();
            smf_set_state(SMF_CTX(ctx), &states[STATE_IDLE]);
            return SMF_EVENT_HANDLED;

        case EVT_GNSS_TIMEOUT:
            LOG_WRN("GNSS timeout");
            (void)gnss_service_stop();
            smf_set_state(SMF_CTX(ctx), &states[STATE_IDLE]);
            return SMF_EVENT_HANDLED;

        default:
            return SMF_EVENT_HANDLED;
        }
    }

    static void gnss_refine_exit(void *obj)
    {
        ARG_UNUSED(obj);
        (void)gnss_service_cancel_timeout();
        LOG_INF("GNSS refine exit");
    }

#if 0
static void ntn_connecting_entry(void *obj)
{
    struct app_ctx *ctx = obj;

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
        
        ctx->active_rat = RAT_NTN;
        smf_set_state(SMF_CTX(ctx), &states[STATE_IDLE]);
        return SMF_EVENT_HANDLED;
    
    case EVT_PDN_UP:
        ctx->pdn_up = true;
        ctx->active_rat = RAT_NTN;
        smf_set_state(SMF_CTX(ctx), &states[STATE_NTN_CONNECTED]);
        return SMF_EVENT_HANDLED;

    case EVT_REG_FAIL:
        LOG_INF("ntn register failed");
        smf_set_state(SMF_CTX(ctx), &states[STATE_BACKOFF]);
        return SMF_EVENT_HANDLED;

    //case EVT_NTN_REG_FAIL:
    case EVT_NTN_TIMEOUT:
        LOG_INF("ntn connect failed/timeout");
        smf_set_state(SMF_CTX(ctx), &states[STATE_BACKOFF]);
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_HANDLED;
    }
}

static void ntn_connecting_exit(void *obj)
{
    ARG_UNUSED(obj);
}

static void ntn_connected_entry(void *obj)
{
    ARG_UNUSED(obj);
    int err = modem_service_udp_send_test();
    LOG_INF("UDP test send result: %d", err);
    LOG_INF("(%s) finished", __func__);
}

static enum smf_state_result ntn_connected_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_REG_FAIL:
    case EVT_NTN_REG_FAIL:
    case EVT_NTN_TIMEOUT:
    case EVT_PDN_DOWN:
        ctx->pdn_up = false;
        LOG_INF("NTN connection lost/failed");
        smf_set_state(SMF_CTX(ctx), &states[STATE_IDLE]);
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_HANDLED;
    }
}

static void ntn_connected_exit(void *obj)
{
    ARG_UNUSED(obj);
    LOG_INF("ntn connected exit");
    //k_timer_stop(&ntn_timeout);
}
#endif

#if 0
    static void handle_gnss_status(struct app_ctx * ctx, const struct app_gnss_status *status)
    {
        struct app_event ev = {0};

        switch (status->state)
        {
        case APP_GNSS_STATE_FIX:
            ev.type = EVT_GNSS_FIX;
            ev.pvt.latitude = status->latitude;
            ev.pvt.longitude = status->longitude;
            ev.pvt.altitude = status->altitude;
            dispatch_app_event(ctx, &ev);
            return;

        case APP_GNSS_STATE_TIMEOUT:
            ev.type = EVT_GNSS_TIMEOUT;
            dispatch_app_event(ctx, &ev);
            return;

        case APP_GNSS_STATE_ERROR:
            LOG_WRN("GNSS reported error %d, treating as timeout", status->err);
            ev.type = EVT_GNSS_TIMEOUT;
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
#endif

    static void backoff_timer_handler(struct k_timer * timer)
    {
        ARG_UNUSED(timer);

        struct app_event ev = {
            .type = EVT_BACKOFF_TIMEOUT};

        (void)app_event_put(&ev, K_NO_WAIT);
    }

    static enum smf_state_result backoff_run(void *obj)
    {
        struct app_ctx *ctx = obj;

        switch (ctx->ev.type)
        {
        case EVT_BACKOFF_TIMEOUT:
            LOG_INF("Retry LTE connect");
            smf_set_state(SMF_CTX(ctx), &states[STATE_LTEM_CONNECTING]);
            return SMF_EVENT_HANDLED;

        default:
            return SMF_EVENT_HANDLED;
        }
    }

    void app_start_backoff_timer(struct app_ctx * ctx)
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

#define SMF_STACK_SIZE 2048
#define SMF_PRIORITY 5

    K_THREAD_STACK_DEFINE(smf_stack, SMF_STACK_SIZE);

    static struct k_thread smf_thread_data;

    static void smf_thread(void *p1, void *p2, void *p3)
    {
        // void app_sm_post_dispatch(struct app_ctx *ctx, const struct app_event *ev);

        ARG_UNUSED(p2);
        ARG_UNUSED(p3);
        struct app_ctx *ctx = p1;

        smf_set_initial(SMF_CTX(ctx), &states[STATE_BOOT]);

        while (1)
        {
            const struct zbus_channel *chan;
            union app_sm_msg msg = {0};
            int err = zbus_sub_wait_msg(&app_fsm_sub, &chan, &msg, K_FOREVER);

            if (err)
            {
                LOG_WRN("zbus_sub_wait_msg failed, err=%d", err);
                continue;
            }

            if (chan == &app_evt_chan)
            {
                dispatch_app_event(ctx, &msg.app_event);
                // app_sm_post_dispatch(ctx, &msg.app_event);
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

    int app_sm_start(struct app_ctx * ctx)
    {
        k_thread_create(&smf_thread_data, smf_stack, SMF_STACK_SIZE,
                        smf_thread, ctx, NULL, NULL,
                        SMF_PRIORITY, 0, K_NO_WAIT);
        return 0;
    }

    /*
    void app_sm_post_dispatch(struct app_ctx *ctx, const struct app_event *ev)
    {

        int err;

        // ltem connect ok
        if (ev->type == EVT_REG_OK && ctx->next_rat != RAT_NTN &&
            ctx->active_rat == RAT_LTEM) {
            err = rsrp_service_start();
            if (err < 0) {
                LOG_WRN("Failed to start LTE signal monitor: %d", err);
            }
            return;
        }

        // start ntn
        if (ctx->next_rat == RAT_NTN &&
            (ev->type == EVT_LTE_POOR ||
             (ev->type == EVT_REG_FAIL && !ctx->ntn_initialized))) {
            struct app_event ntn_ev = { .type = EVT_NTN_REG_FAIL };

            ctx->lte_connected = false;
            (void)rsrp_service_stop();

            err = ntn_service_connect(ctx);
            if (err) {
                LOG_INF("ntn initialization failed (%d)", err);
                (void)app_event_put(&ntn_ev, K_NO_WAIT);
                return;
            }

            LOG_INF("NTN handover started");
            return;
        }

        // set app context to ntn
        if (ctx->next_rat == RAT_NTN && ctx->active_rat != RAT_NTN &&
            ev->type == EVT_REG_OK) {
            ctx->active_rat = RAT_NTN;
            LOG_INF("ntn registered ok");
            return;
        }

        // ntn failed
        if (ctx->next_rat == RAT_NTN && ctx->active_rat != RAT_NTN &&
            (ev->type == EVT_REG_FAIL ||
             ev->type == EVT_NTN_REG_FAIL ||
             ev->type == EVT_NTN_TIMEOUT)) {
            LOG_INF("ntn connect failed/timeout");
        }
    }

    */