/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 
#include "app_sm.h"
#include "app_events.h"
#include "gnss_service.h"
#include "ntn_service.h"

#include <modem/nrf_modem_lib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>

LOG_MODULE_REGISTER(app_sm, LOG_LEVEL_INF);

static void boot_entry(void *obj);
static enum smf_state_result boot_run(void *obj);

static void gnss_acquire_entry(void *obj);
static enum smf_state_result gnss_acquire_run(void *obj);
static void gnss_acquire_exit(void *obj);

static void ntn_connecting_entry(void *obj);
static enum smf_state_result ntn_connecting_run(void *obj);
static void ntn_connecting_exit(void *obj);


/* define state machine framework */
static const struct smf_state states[] = {
    //[STATE_LTEM_CONNECTING] = SMF_CREATE_STATE(ltem_connecting_entry, ltem_connecting_run, NULL, NULL, NULL),
    //[STATE_LTEM_CONNECTED] = SMF_CREATE_STATE(ltem_connected_entry, ltem_connected_run, NULL, NULL, NULL),
    [STATE_BOOT] = SMF_CREATE_STATE(
        boot_entry, 
        boot_run, 
        NULL, 
        NULL,
        NULL
    ),
    [STATE_GNSS_ACQUIRE] = SMF_CREATE_STATE(
        gnss_acquire_entry, 
        gnss_acquire_run,
        gnss_acquire_exit, 
        NULL, 
        NULL
    ),
    [STATE_NTN_CONNECTING] = SMF_CREATE_STATE(
        ntn_connecting_entry, 
        ntn_connecting_run,
        ntn_connecting_exit, 
        NULL, 
        NULL
    ),
    [STATE_IDLE] = SMF_CREATE_STATE(
        NULL,
        NULL, 
        NULL, 
        NULL, 
        NULL
    ),

    // TODO: add ltem states

};

static void boot_entry(void *obj)
{
    ARG_UNUSED(obj);

    int err = nrf_modem_lib_init();
    if (err) {
        LOG_ERR("nrf_modem_lib_init err=%d", err);
        return;
    }

    err = gnss_service_init();
    if (err) {
        LOG_ERR("gnss_service_init err=%d", err);
        return;
    }

    LOG_INF("(%s) BOOT: modem lib init ok", __func__);
}

static enum smf_state_result boot_run(void *obj)
{
    struct app_ctx *ctx = obj;

    if (ctx->ev.type == EVT_BOOT) {
        smf_set_state(SMF_CTX(ctx), &states[STATE_GNSS_ACQUIRE]);
    }

    return SMF_EVENT_HANDLED;
}

static void gnss_acquire_entry(void *obj)
{
    ARG_UNUSED(obj);

    (void)gnss_service_start_timeout(CONFIG_APP_GNSS_TIMEOUT_SEC);
    (void)gnss_service_start();

    LOG_INF("(%s) GNSS_ACQUIRE entry done", __func__);
}

static enum smf_state_result gnss_acquire_run(void *obj)
{
    struct app_ctx *ctx = obj;

    LOG_INF("entering (%s)", __func__);

    switch (ctx->ev.type) {
    case EVT_GNSS_FIX:
        ctx->last_pvt = ctx->ev.pvt;
        ctx->have_fix = true;

        LOG_INF("GNSS FIX OK: lat=%f, lon=%f",
                (double)ctx->last_pvt.latitude,
                (double)ctx->last_pvt.longitude);

        (void)gnss_service_stop();
        smf_set_state(SMF_CTX(ctx), &states[STATE_NTN_CONNECTING]);
        return SMF_EVENT_HANDLED;

    case EVT_GNSS_TIMEOUT:
        LOG_INF("GNSS_ACQUIRE: gnss timeout");
        (void)gnss_service_stop();

        /* ONLY FOR TESTING */
        ctx->last_pvt.latitude  = 634305000;   /* 63.4305° */
        ctx->last_pvt.longitude = 103951000;   /* 10.3951° */
        ctx->last_pvt.altitude  = 10;

        ctx->have_fix = true;

        LOG_INF("Using fallback GNSS position (Trondheim)");

        smf_set_state(SMF_CTX(ctx), &states[STATE_NTN_CONNECTING]);
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
    LOG_INF("gnss acquire exit");
}

static void ntn_connecting_entry(void *obj)
{
    struct app_ctx *ctx = obj;
    int err = ntn_service_connect(ctx);

    if (err) {
        LOG_INF("ntn initialization failed (%d)", err);
        struct app_event ev = { .type = EVT_NTN_REG_FAIL };
        (void)app_event_put(&ev, K_NO_WAIT);
        return;
    }

    //k_timer_start(&ntn_timeout, K_SECONDS(180), K_NO_WAIT);
    LOG_INF("(%s) ntn started", __func__);
}

static enum smf_state_result ntn_connecting_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_REG_OK:
        LOG_INF("ntn registered ok");
        smf_set_state(SMF_CTX(ctx), &states[STATE_IDLE]);
        return SMF_EVENT_HANDLED;

    case EVT_REG_FAIL:
    case EVT_NTN_REG_FAIL:
    case EVT_NTN_TIMEOUT:
        LOG_INF("ntn connect failed/timeout");
        smf_set_state(SMF_CTX(ctx), &states[STATE_IDLE]);
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_HANDLED;
    }
}

static void ntn_connecting_exit(void *obj)
{
    ARG_UNUSED(obj);
    //k_timer_stop(&ntn_timeout);
}


/* thread parameters */
#define SMF_STACK_SIZE 2048
//#define MON_STACK_SIZE 1024

#define SMF_PRIORITY 5
//#define MON_PRIORITY 7

K_THREAD_STACK_DEFINE(smf_stack, SMF_STACK_SIZE);
//K_THREAD_STACK_DEFINE(mon_stack, MON_STACK_SIZE);

static struct k_thread smf_thread_data;
//static struct k_thread mon_thread_data;

static void smf_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    struct app_ctx *ctx = p1;

    smf_set_initial(SMF_CTX(ctx), &states[STATE_BOOT]);
 
    //k_mutex_lock(&o->lock, K_FOREVER);
    //o->current_state = STATE_IDLE;
    //smf_set_initial(SMF_CTX(o), &states[STATE_IDLE]);
    //k_mutex_unlock(&o->lock);

    //k_timer_start(&timeout_timer, K_MSEC(100), K_MSEC(100));

    while (1) {
        struct app_event ev;
        //k_mutex_lock(&o->lock, K_FOREVER);
        //(void)smf_run_state(SMF_CTX(o));
        //k_mutex_unlock(&o->lock);




        //k_sleep((K_MSEC(100)));

        /* block thread until next message */
        app_event_get(&ev, K_FOREVER); // k_msgq_get
        LOG_INF("SMF thread got event %d", ev.type);
        ctx->ev = ev;
        (void)smf_run_state(SMF_CTX(ctx));
    }
}

int app_sm_start(struct app_ctx *ctx)
{
    k_thread_create(&smf_thread_data, smf_stack, SMF_STACK_SIZE,
                    smf_thread, ctx, NULL, NULL,
                    SMF_PRIORITY, 0, K_NO_WAIT);
    return 0;
}