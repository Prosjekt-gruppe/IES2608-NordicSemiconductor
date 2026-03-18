/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#include "app_sm.h"
#include "app_events.h"
#include "gnss_service.h"
#include "modem_service.h"
#include "ntn_service.h"

#include <limits.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

LOG_MODULE_REGISTER(app_sm, LOG_LEVEL_INF);

ZBUS_MSG_SUBSCRIBER_DEFINE(app_fsm_sub);

static void boot_entry(void *obj);
static enum smf_state_result boot_run(void *obj);

static void gnss_acquire_entry(void *obj);
static enum smf_state_result gnss_acquire_run(void *obj);
static void gnss_acquire_exit(void *obj);

static void ltem_connecting_entry(void *obj);
static enum smf_state_result ltem_connecting_run(void *obj);

static void ltem_connected_entry(void *obj);
static enum smf_state_result ltem_connected_run(void *obj);
static void ltem_connected_exit(void *obj);

static void ntn_connecting_entry(void *obj);
static enum smf_state_result ntn_connecting_run(void *obj);
static void ntn_connecting_exit(void *obj);

static const struct smf_state states[] = {
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
    [STATE_LTEM_CONNECTING] = SMF_CREATE_STATE(
        ltem_connecting_entry,
        ltem_connecting_run,
        NULL,
        NULL,
        NULL
    ),
    [STATE_LTEM_CONNECTED] = SMF_CREATE_STATE(
        ltem_connected_entry,
        ltem_connected_run,
        ltem_connected_exit,
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
};

static void boot_entry(void *obj)
{
    struct app_ctx *ctx = obj;

    ctx->active_rat = RAT_LTEM;
    ctx->next_rat = RAT_LTEM;
    ctx->rsrp_dbm = INT_MIN;
    ctx->have_fix = false;
    ctx->ntn_initialized = false;
    ctx->lte_connected = false;

    int err = modem_service_init();
    if (err) {
        LOG_ERR("modem_service_init err=%d", err);
        return;
    }

    err = modem_service_prepare_ltem();
    if (err) {
        LOG_ERR("modem_service_prepare_ltem err=%d", err);
        return;
    }

    err = gnss_service_init();
    if (err) {
        LOG_ERR("gnss_service_init err=%d", err);
        return;
    }

    LOG_INF("(%s) BOOT: modem services initialized", __func__);
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
        smf_set_state(SMF_CTX(ctx), &states[STATE_LTEM_CONNECTING]);
        return SMF_EVENT_HANDLED;

    case EVT_GNSS_TIMEOUT:
        LOG_INF("GNSS_ACQUIRE: gnss timeout");
        (void)gnss_service_stop();

        /* ONLY FOR TESTING */
        ctx->last_pvt.latitude = 63.4210;
        ctx->last_pvt.longitude = 10.4370;
        ctx->last_pvt.altitude = 160;

        ctx->have_fix = true;

        LOG_INF("Using fallback GNSS position (Trondheim)");

        smf_set_state(SMF_CTX(ctx), &states[STATE_LTEM_CONNECTING]);
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

static void ltem_connecting_entry(void *obj)
{
    struct app_ctx *ctx = obj;
    int err;

    ctx->next_rat = RAT_LTEM;

    err = modem_service_prepare_ltem();
    if (!err) {
        err = modem_service_connect_async();
    }

    if (err) {
        LOG_INF("LTE-M initialization failed (%d)", err);
        struct app_event ev = { .type = EVT_REG_FAIL };
        (void)app_event_put(&ev, K_NO_WAIT);
        return;
    }

    LOG_INF("(%s) LTE-M connect started", __func__);
}

static enum smf_state_result ltem_connecting_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_REG_OK:
        ctx->active_rat = RAT_LTEM;
        ctx->lte_connected = true;
        LOG_INF("LTE-M registered ok");
        smf_set_state(SMF_CTX(ctx), &states[STATE_LTEM_CONNECTED]);
        return SMF_EVENT_HANDLED;

    case EVT_REG_FAIL:
        ctx->lte_connected = false;
        LOG_INF("LTE-M connect failed, falling back to NTN");
        smf_set_state(SMF_CTX(ctx), &states[STATE_NTN_CONNECTING]);
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_HANDLED;
    }
}

static void ltem_connected_entry(void *obj)
{
    struct app_ctx *ctx = obj;
    int err = modem_service_start_ltem_monitor();

    ctx->next_rat = RAT_LTEM;

    if (err) {
        LOG_WRN("Failed to start LTE-M signal monitor: %d", err);
        return;
    }

    LOG_INF("LTE-M signal monitoring started");
}

static enum smf_state_result ltem_connected_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_RSRP_UPDATE:
        ctx->rsrp_dbm = ctx->ev.meas.rsrp_dbm;
        LOG_INF("LTE-M RSRP update: %d dBm", ctx->rsrp_dbm);
        return SMF_EVENT_HANDLED;

    case EVT_NTN_FALLBACK_REQUEST:
        ctx->rsrp_dbm = ctx->ev.meas.rsrp_dbm;
        ctx->next_rat = RAT_NTN;
        LOG_WRN("Switching to NTN, LTE-M RSRP=%d dBm", ctx->rsrp_dbm);
        smf_set_state(SMF_CTX(ctx), &states[STATE_NTN_CONNECTING]);
        return SMF_EVENT_HANDLED;

    case EVT_REG_FAIL:
        ctx->next_rat = RAT_NTN;
        LOG_WRN("LTE-M registration lost, switching to NTN");
        smf_set_state(SMF_CTX(ctx), &states[STATE_NTN_CONNECTING]);
        return SMF_EVENT_HANDLED;

    default:
        return SMF_EVENT_HANDLED;
    }
}

static void ltem_connected_exit(void *obj)
{
    struct app_ctx *ctx = obj;

    ctx->lte_connected = false;
    (void)modem_service_stop_ltem_monitor();
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

    LOG_INF("(%s) ntn started", __func__);
}

static enum smf_state_result ntn_connecting_run(void *obj)
{
    struct app_ctx *ctx = obj;

    ctx->next_rat = RAT_NTN;

    switch (ctx->ev.type) {
    case EVT_REG_OK:
        ctx->active_rat = RAT_NTN;
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
}

#define SMF_STACK_SIZE 2048
#define SMF_PRIORITY 5

K_THREAD_STACK_DEFINE(smf_stack, SMF_STACK_SIZE);

static struct k_thread smf_thread_data;

static void smf_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    struct app_ctx *ctx = p1;

    smf_set_initial(SMF_CTX(ctx), &states[STATE_BOOT]);

    while (1) {
        const struct zbus_channel *chan;
        struct app_event ev;
        int err = zbus_sub_wait_msg(&app_fsm_sub, &chan, &ev, K_FOREVER);

        if (err) {
            LOG_WRN("zbus_sub_wait_msg failed, err=%d", err);
            continue;
        }

        if (chan != &app_evt_chan) {
            LOG_WRN("Received message from unexpected channel: %s", zbus_chan_name(chan));
            continue;
        }

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
