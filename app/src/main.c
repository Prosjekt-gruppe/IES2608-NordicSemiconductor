/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 


#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/sys/printk.h>


LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);


static void ltem_connecting_entry(void *ctx);
static enum smf_state_result ltem_connecting_run(void *ctx);

static void ltem_connected_entry(void *ctx);
static enum smf_state_result ltem_connected_run(void *ctx);


//K_SEM_DEFINE(state_change_sem, 0, 1);


enum rat {
    RAT_LTEM,
    RAT_NTN
};


enum app_state {
    STATE_LTEM_CONNECTING,
    STATE_LTEM_CONNECTED
};


enum app_evt_type {
    EVT_BOOT,
    EVT_REG_OK,
    EVT_REG_FAIL,
    EVT_TIMEOUT_1S,
    EVT_RSRP_UPDATE
};


struct app_event {
    enum app_evt_type type;
    union {
        struct { enum rat rat; } reg;
        struct { int rsrp_dbm; } meas;
    };
};


struct monitor_event {
    enum app_state state;
    struct app_event ev;
    int rsrp_dbm;
};

struct app_ctx {
    struct smf_ctx ctx;

    /* rat overview */
    enum rat active_rat;
    enum rat next_rat;
        
    int rsrp_dbm;
    int backoff_ms;
    
    /* events */
    struct app_event ev;

    //struct k_mutex lock;
};

/* event queue */
K_MSGQ_DEFINE(app_evt_q, sizeof(struct app_event), 16, 4);

K_MSGQ_DEFINE(monitor_q, sizeof(struct monitor_event), 16, 4);


static const struct smf_state states[] = {
    [STATE_LTEM_CONNECTING] = SMF_CREATE_STATE(ltem_connecting_entry, ltem_connecting_run, NULL, NULL, NULL),
    [STATE_LTEM_CONNECTED] = SMF_CREATE_STATE(ltem_connected_entry, ltem_connected_run, NULL, NULL, NULL),
};

/*
static void tick_timer_cb(struct k_timer *t)
{
    ARG_UNUSED(t);
    struct app_event ev = { .type = EVT_TICK_100MS};
    (void)k_msgq_put(&app_evt_q, &ev, K_NO_WAIT);

}
*/

//K_TIMER_DEFINE(tick_timer, tick_timer_cb, NULL);


static void timeout_timer_cb(struct k_timer *t)
{
    ARG_UNUSED(t);
    struct app_event ev = { .type = EVT_TIMEOUT_1S };
    (void)k_msgq_put(&app_evt_q, &ev, K_NO_WAIT);

}

K_TIMER_DEFINE(timeout_timer, timeout_timer_cb, NULL);


static void ltem_connecting_entry(void *ctx) {
    ARG_UNUSED(ctx);
    LOG_INF("ENTERING: STATE_LTEM_CONNECTING");
}


static enum smf_state_result ltem_connecting_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {
    case EVT_BOOT: {
        /* dummy input */
        struct app_event meas = { .type = EVT_RSRP_UPDATE, .meas = { .rsrp_dbm = -95 } };
        (void)k_msgq_put(&app_evt_q, &meas, K_NO_WAIT);
        break;
    }

    case EVT_RSRP_UPDATE:
        ctx->rsrp_dbm = ctx->ev.meas.rsrp_dbm;
        LOG_INF("LTEM_CONNECTING: rsrp=%d", ctx->rsrp_dbm);

        if (ctx->rsrp_dbm > -100) {
            smf_set_state(SMF_CTX(ctx), &states[STATE_LTEM_CONNECTED]);
        }
        break;

    default:
        break;
    }

    return SMF_EVENT_HANDLED;
}


static void ltem_connected_entry(void *obj) 
{
    ARG_UNUSED(obj);
    //struct app_ctx *ctx = obj;
    LOG_INF("ENTERING STATE: LTEM_CONNECTED");

    k_timer_start(&timeout_timer, K_SECONDS(1), K_NO_WAIT);
}


static enum smf_state_result ltem_connected_run(void *obj)
{
    struct app_ctx *ctx = obj;

    switch (ctx->ev.type) {

    case EVT_TIMEOUT_1S: {
        smf_set_state(SMF_CTX(ctx), &states[STATE_LTEM_CONNECTING]);

        int dummy_rsrp = -105;

        LOG_INF("LTEM_CONNECTED: Previous RSRP=%d", ctx->rsrp_dbm);

        struct app_event meas_ev = {
            .type = EVT_RSRP_UPDATE,
            .meas = { .rsrp_dbm = dummy_rsrp }
        };
        (void)k_msgq_put(&app_evt_q, &meas_ev, K_NO_WAIT);

        ctx->next_rat = RAT_LTEM;

        struct monitor_event mon = {
            .state = STATE_LTEM_CONNECTED,
            .ev = {
                .type = EVT_REG_OK
            },
            .rsrp_dbm = ctx->rsrp_dbm,
        };

        (void)k_msgq_put(&monitor_q, &mon, K_NO_WAIT);

        return SMF_EVENT_HANDLED;
    }

    case EVT_RSRP_UPDATE:
        ctx->rsrp_dbm = ctx->ev.meas.rsrp_dbm;
        LOG_INF("LTEM_CONNECTED: updated rsrp=%d", ctx->rsrp_dbm);
        return SMF_EVENT_HANDLED;

    default:
        LOG_INF("NO EVENT DETECTED");
        return SMF_EVENT_HANDLED;
    }
}

/* thread parameters */
#define SMF_STACK_SIZE 1024
#define MON_STACK_SIZE 1024
#define SMF_PRIORITY 5
#define MON_PRIORITY 7

K_THREAD_STACK_DEFINE(smf_stack, SMF_STACK_SIZE);
K_THREAD_STACK_DEFINE(mon_stack, MON_STACK_SIZE);

static struct k_thread smf_thread_data;
static struct k_thread mon_thread_data;

static void smf_thread(void *p1, void *p2, void *p3) 
{
    ARG_UNUSED(p2); ARG_UNUSED(p3);
    struct app_ctx *ctx = p1;

    smf_set_initial(SMF_CTX(ctx), &states[STATE_LTEM_CONNECTING]);

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
        k_msgq_get(&app_evt_q, &ev, K_FOREVER);
        ctx->ev = ev;
        (void)smf_run_state(SMF_CTX(ctx));
    }
}


/*
struct mon_snapshot {
    enum test_states state;
    int counter;
    int running_ms;
};
*/


static const char *state_name(enum app_state s)
{
    switch (s) {
        case STATE_LTEM_CONNECTING: return "CONNECTING";
        case STATE_LTEM_CONNECTED: return "CONNECTED";
        default: return "?";
    }
}

static const char *event_name(enum app_evt_type e)
{
    switch (e) {
    case EVT_BOOT: return "BOOT";
    case EVT_REG_OK: return "REG_OK";
    case EVT_REG_FAIL: return "REG_FAIL";
    case EVT_TIMEOUT_1S: return "TIMEOUT_1S";
    case EVT_RSRP_UPDATE: return "RSRP_UPDATE";
    default: return "?";
    }
}

static void monitor_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    //struct app_ctx *o = p1;
    
    struct monitor_event ev;
    
    while (1) {
        //struct mon_snapshot snap;


        //k_sem_take(&state_change_sem, K_FOREVER);



        //k_mutex_lock(&o->lock, K_FOREVER);
        //snap.state = o->current_state;
        //snap.counter = o->counter;
        //snap.running_ms = o-> running_ms;
        //k_mutex_unlock(&o->lock);

        k_msgq_get(&monitor_q, &ev, K_FOREVER);


        LOG_INF("MON: STATE_BEFORE=%s, CURRENT_RSRP=%d, EVENT_NAME=%s",
            state_name(ev.state), ev.rsrp_dbm, event_name(ev.ev.type));

        //k_sleep(K_MSEC(500));
    }
}

int main(void)
{

    static struct app_ctx ctx;

    //k_mutex_init(&ctx.lock);

    k_thread_create(&smf_thread_data, smf_stack, SMF_STACK_SIZE,
        smf_thread, &ctx, NULL, NULL, SMF_PRIORITY, 0, K_NO_WAIT);

    struct app_event boot = { .type = EVT_BOOT };
    k_msgq_put(&app_evt_q, &boot, K_NO_WAIT);

    k_thread_create(&mon_thread_data, mon_stack, MON_STACK_SIZE,
        monitor_thread, NULL, NULL, NULL, MON_PRIORITY, 0, K_NO_WAIT);

    //smf_set_initial(SMF_CTX(&ctx), &states[STATE_IDLE]);


    while (1) {
        k_sleep(K_SECONDS(60));
    }

    return 0;
}