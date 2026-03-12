/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 
#include "app_sm.h"
#include "app_events.h"
#include "app_zbus.h"
#include "gnss_service.h"
#include "ntn_service.h"

#include <modem/nrf_modem_lib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/zbus/zbus.h>

LOG_MODULE_REGISTER(app_sm, LOG_LEVEL_INF);

ZBUS_MSG_SUBSCRIBER_DEFINE(app_fsm_sub); //Subscriber for app events and GNSS status updates

union app_sm_msg {
    struct app_event app_event;
    struct app_gnss_status gnss_status;
};

static void boot_entry(void *obj);
static enum smf_state_result boot_run(void *obj);

static void gnss_acquire_entry(void *obj);
static enum smf_state_result gnss_acquire_run(void *obj);
static void gnss_acquire_exit(void *obj);

static void ntn_connecting_entry(void *obj);
static enum smf_state_result ntn_connecting_run(void *obj);
static void ntn_connecting_exit(void *obj);

static void dispatch_app_event(struct app_ctx *ctx, const struct app_event *ev);
static void handle_gnss_status(struct app_ctx *ctx, const struct app_gnss_status *status);

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
        ctx->last_pvt.latitude = 63.4305;
        ctx->last_pvt.longitude = 10.3951;
        ctx->last_pvt.altitude = 10;

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
}

static void dispatch_app_event(struct app_ctx *ctx, const struct app_event *ev)
{
    ctx->ev = *ev;
    LOG_INF("SMF thread got event %d", ev->type);
    (void)smf_run_state(SMF_CTX(ctx));
}

static void handle_gnss_status(struct app_ctx *ctx, const struct app_gnss_status *status)
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
        union app_sm_msg msg = {0};
        int err = zbus_sub_wait_msg(&app_fsm_sub, &chan, &msg, K_FOREVER);

        if (err) {
            LOG_WRN("zbus_sub_wait_msg failed, err=%d", err);
            continue;
        }

        if (chan == &app_evt_chan) {
            dispatch_app_event(ctx, &msg.app_event);
            continue;
        }

        if (chan == &gnss_status_chan) {
            handle_gnss_status(ctx, &msg.gnss_status);
            continue;
        }

        LOG_WRN("Received message from unexpected channel: %s", zbus_chan_name(chan));
    }
}

int app_sm_start(struct app_ctx *ctx)
{
    k_thread_create(&smf_thread_data, smf_stack, SMF_STACK_SIZE,
                    smf_thread, ctx, NULL, NULL,
                    SMF_PRIORITY, 0, K_NO_WAIT);
    return 0;
}
