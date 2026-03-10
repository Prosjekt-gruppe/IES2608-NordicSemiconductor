/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 


#include <modem/nrf_modem_lib.h>
#include <nrf_modem_gnss.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
//#include <zephyr/sys/printk.h>
#include <modem/lte_lc.h>
#include <modem/ntn.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);


static void boot_entry(void *obj);
static enum smf_state_result boot_run(void *obj);

/*
static void ltem_connecting_entry(void *obj);
static enum smf_state_result ltem_connecting_run(void *obj);

static void ltem_connected_entry(void *obj);
static enum smf_state_result ltem_connected_run(void *obj);
*/

static void boot_entry(void *obj);
static enum smf_state_result boot_run(void *obj);

static void gnss_acquire_entry(void *obj);
static enum smf_state_result gnss_acquire_run(void *obj);
static void gnss_acquire_exit(void *obj);

static void ntn_connecting_entry(void *obj);
static enum smf_state_result ntn_connecting_run(void *obj);
static void ntn_connecting_exit(void *obj);

//static enum smf_state_result idle_run(void *obj);

//K_SEM_DEFINE(state_change_sem, 0, 1);


enum rat {
    RAT_LTEM,
    RAT_NTN
};


enum app_state {
    STATE_BOOT,
    STATE_IDLE,
    STATE_GNSS_ACQUIRE,
    STATE_NTN_CONNECTING,
    STATE_NTN_CONNECTED,
    STATE_LTEM_CONNECTING,
    STATE_LTEM_CONNECTED,
    STATE_BACKOFF,
};


enum app_evt_type {
    EVT_BOOT,
    EVT_REG_OK,
    EVT_REG_FAIL,
    EVT_GNSS_FIX,
    EVT_GNSS_TIMEOUT,
    EVT_NTN_REG_FAIL,
    EVT_NTN_TIMEOUT,
    EVT_TIMEOUT,
    EVT_RSRP_UPDATE,
};


struct app_event {
    enum app_evt_type type;
    union {
        struct nrf_modem_gnss_pvt_data_frame pvt;
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
    

    /* ltem signal strength*/
    int rsrp_dbm;
    int backoff_ms;

    /* gnss */
    bool have_fix;
    struct nrf_modem_gnss_pvt_data_frame last_pvt;
    
    /* ntn */
    bool ntn_initialized;

    /* events */
    struct app_event ev;

    //struct k_mutex lock;
};


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
};

/* register event queue */
K_MSGQ_DEFINE(app_evt_q, sizeof(struct app_event), 16, 4);

//K_MSGQ_DEFINE(monitor_q, sizeof(struct monitor_event), 16, 4);

static struct k_work gnss_pvt_work;
static struct k_work_delayable gnss_timeout_work;

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt) 
{
    struct app_event app_ev = {0};

    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
        switch (evt->nw_reg_status) {
        case LTE_LC_NW_REG_REGISTERED_HOME:
        case LTE_LC_NW_REG_REGISTERED_ROAMING:
            app_ev.type = EVT_REG_OK;
            (void)k_msgq_put(&app_evt_q, &app_ev, K_NO_WAIT);
            break;
        case LTE_LC_NW_REG_NOT_REGISTERED:
        case LTE_LC_NW_REG_REGISTRATION_DENIED:
        case LTE_LC_NW_REG_UNKNOWN:
        case LTE_LC_NW_REG_UICC_FAIL:
            app_ev.type = EVT_REG_FAIL;
            (void)k_msgq_put(&app_evt_q, &app_ev, K_NO_WAIT);
            break;
        
        default:
            break;
        }
        break;
        
    case LTE_LC_EVT_CELLULAR_PROFILE_ACTIVE:
        LOG_INF("modem activate cellular profile (RAT starting)");
        break;

    case LTE_LC_EVT_LTE_MODE_UPDATE:
        LOG_INF("LTE mode update %d", evt->lte_mode);
        break;

    default:
        break;
    }
}


/* TIMERS */
/*
static void timeout_timer_cb(struct k_timer *t)
{
    ARG_UNUSED(t);
    struct app_event ev = { .type = EVT_GNSS_TIMEOUT };
    int err = k_msgq_put(&app_evt_q, &ev, K_NO_WAIT);
    LOG_INF("GNSS TIMEOUT CALLBACK fired, q_put err=%d", err);

}
K_TIMER_DEFINE(timeout_timer, timeout_timer_cb, NULL);
*/


static void gnss_timeout_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    struct app_event ev = { .type = EVT_GNSS_TIMEOUT };
    
    int err = k_msgq_put(&app_evt_q, &ev, K_NO_WAIT);

    
    LOG_INF("gnss_timeout_work_handler fired, q_put err=%d", err);
}

/*static void ntn_timeout_cb(struct k_timer *t)
{
    ARG_UNUSED(t);
    struct app_event ev = { .type = EVT_NTN_TIMEOUT };
    (void)k_msgq_put(&app_evt_q, &ev, K_NO_WAIT);
}

K_TIMER_DEFINE(ntn_timeout, ntn_timeout_cb, NULL);
*/

/* define kernel work task */
static void gnss_pvt_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    struct nrf_modem_gnss_pvt_data_frame pvt;

    int err = nrf_modem_gnss_read(&pvt, sizeof(pvt), NRF_MODEM_GNSS_DATA_PVT);

    if (err) {
        return;
    }

    if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
        struct app_event ev = { .type = EVT_GNSS_FIX, .pvt = pvt };
        int err = k_msgq_put(&app_evt_q, &ev, K_NO_WAIT);
        LOG_INF("put event gnss fix err=%d", err);
    }

}    

/* listen to gnss events from modem */
static void gnss_event_handler(int event)
{
    if (event == NRF_MODEM_GNSS_EVT_PVT) {
        k_work_submit(&gnss_pvt_work);
    }
}


static void boot_entry(void *obj)
{
    ARG_UNUSED(obj);

    int err = nrf_modem_lib_init();
    if (err) {
        LOG_ERR("nrf_modem_lib_init err=%d", err);
        return;
    }

    k_work_init(&gnss_pvt_work, gnss_pvt_work_handler);
    k_work_init_delayable(&gnss_timeout_work, gnss_timeout_work_handler);    
    /* register modem callback */
    nrf_modem_gnss_event_handler_set(gnss_event_handler);

    LOG_INF("(%s) BOOT: modem lib init ok", __func__);
}


static enum smf_state_result boot_run(void *obj)
{
    struct app_ctx *ctx = obj;

    if (ctx->ev.type == EVT_BOOT) {
        smf_set_state(SMF_CTX(ctx), &states[STATE_GNSS_ACQUIRE]);
        return SMF_EVENT_HANDLED;
    }

    return SMF_EVENT_HANDLED;
}


static void gnss_acquire_entry(void *obj)
{
    struct app_ctx *ctx = obj;
    ARG_UNUSED(ctx);

    int ret = k_work_reschedule(&gnss_timeout_work, K_SECONDS(10));

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
            (double)ctx->last_pvt.longitude
        );

        (void)nrf_modem_gnss_stop();
        smf_set_state(SMF_CTX(ctx), &states[STATE_NTN_CONNECTING]);
        return SMF_EVENT_HANDLED;


    case EVT_GNSS_TIMEOUT:
        LOG_INF("GNSS_ACQUIRE: gnss timeout");
        (void)nrf_modem_gnss_stop();
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

    k_work_cancel_delayable(&gnss_timeout_work);
    LOG_INF("gnss acquire exit");    
    //(void)nrf_modem_gnss_stop();
}

static int ntn_start_connect(struct app_ctx *ctx)
{
    int err;
    
    struct lte_lc_cellular_profile ntn_profile = {
        .id = 0,
        .act = LTE_LC_ACT_NTN,
        .uicc = LTE_LC_UICC_PHYSICAL,
    };
    
    if (!ctx->ntn_initialized) {
        LOG_INF("NTN first time initialize");
        
        err = lte_lc_power_off();
        if (err) {
            LOG_INF("failing lte lc power off (%d)", err);
            return err;
        }
        
        err = lte_lc_cellular_profile_configure(&ntn_profile);
        if (err) {
            LOG_INF("failing lte lc cellular profile setup (%d)", err);
            return err;
        }
        
        ctx->ntn_initialized=true;
    }

    if (ctx->have_fix) {
        err = ntn_location_set((double)ctx->last_pvt.latitude, 
                               (double)ctx->last_pvt.longitude, 
                               (float)ctx->last_pvt.altitude, 
                               0);
        if (err) {
            LOG_INF("failed to set location (%d)", err);
            return err;
        }
    }

    /* configure for ntn */
	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_AUTO);

    if (err) {
        LOG_INF("system mode set error (%d)", err);
        return err;
    }

    /* actual connect attempt */
	err = lte_lc_connect_async(lte_lc_evt_handler);
    if (err) {
        return err;
    }

    return 0;

}


static void ntn_connecting_entry(void *obj)
{
    struct app_ctx *ctx = obj;
    int err = ntn_start_connect(ctx);

    if (err) {
        LOG_INF("ntn initialization failed (%d)", err);
        struct app_event ev = { .type = EVT_NTN_REG_FAIL };
        (void)k_msgq_put(&app_evt_q, &ev, K_NO_WAIT);
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


/*
static void tick_timer_cb(struct k_timer *t)
{
    ARG_UNUSED(t);
    struct app_event ev = { .type = EVT_TICK_100MS};
    (void)k_msgq_put(&app_evt_q, &ev, K_NO_WAIT);

}
*/

//K_TIMER_DEFINE(tick_timer, tick_timer_cb, NULL);




/*
static void ltem_connecting_entry(void *ctx) {
    ARG_UNUSED(ctx);
    LOG_INF("ENTERING: STATE_LTEM_CONNECTING");
}
*/

//static enum smf_state_result ltem_connecting_run(void *obj)
//{
//    struct app_ctx *ctx = obj;
//
//    switch (ctx->ev.type) {
//    case EVT_BOOT: {
//        /* dummy input */
//        struct app_event meas = { .type = EVT_RSRP_UPDATE, .meas = { .rsrp_dbm = -95 } };
//        (void)k_msgq_put(&app_evt_q, &meas, K_NO_WAIT);
//        break;
//    }
//
//    case EVT_RSRP_UPDATE:
//        ctx->rsrp_dbm = ctx->ev.meas.rsrp_dbm;
//        LOG_INF("LTEM_CONNECTING: rsrp=%d", ctx->rsrp_dbm);
//
//        if (ctx->rsrp_dbm > -100) {
//            smf_set_state(SMF_CTX(ctx), &states[STATE_LTEM_CONNECTED]);
//        }
//        break;
//
//    default:
//        break;
//    }
//
//    return SMF_EVENT_HANDLED;
//}


//static void ltem_connected_entry(void *obj) 
//{
//    ARG_UNUSED(obj);
//    //struct app_ctx *ctx = obj;
//    LOG_INF("ENTERING STATE: LTEM_CONNECTED");
//
//    k_timer_start(&timeout_timer, K_SECONDS(1), K_NO_WAIT);
//}


//static enum smf_state_result ltem_connected_run(void *obj)
//{
//    struct app_ctx *ctx = obj;
//
//    switch (ctx->ev.type) {
//
//    case EVT_TIMEOUT: {
//        smf_set_state(SMF_CTX(ctx), &states[STATE_LTEM_CONNECTING]);
//
//        int dummy_rsrp = -105;
//
//        LOG_INF("LTEM_CONNECTED: Previous RSRP=%d", ctx->rsrp_dbm);
//
//        struct app_event meas_ev = {
//            .type = EVT_RSRP_UPDATE,
//            .meas = { .rsrp_dbm = dummy_rsrp }
//        };
//        (void)k_msgq_put(&app_evt_q, &meas_ev, K_NO_WAIT);
//
//        ctx->next_rat = RAT_LTEM;
//
//        struct monitor_event mon = {
//            .state = STATE_LTEM_CONNECTED,
//            .ev = {
//                .type = EVT_REG_OK
//            },
//            .rsrp_dbm = ctx->rsrp_dbm,
//        };
//
//        (void)k_msgq_put(&monitor_q, &mon, K_NO_WAIT);
//
//        return SMF_EVENT_HANDLED;
//    }
//
//    case EVT_RSRP_UPDATE:
//        ctx->rsrp_dbm = ctx->ev.meas.rsrp_dbm;
//        LOG_INF("LTEM_CONNECTED: updated rsrp=%d", ctx->rsrp_dbm);
//        return SMF_EVENT_HANDLED;
//
//    default:
//        LOG_INF("NO EVENT DETECTED");
//        return SMF_EVENT_HANDLED;
//    }
//}


/* thread parameters */
#define SMF_STACK_SIZE 2048
#define MON_STACK_SIZE 1024
#define SMF_PRIORITY 5
#define MON_PRIORITY 7

K_THREAD_STACK_DEFINE(smf_stack, SMF_STACK_SIZE);
//K_THREAD_STACK_DEFINE(mon_stack, MON_STACK_SIZE);

static struct k_thread smf_thread_data;
//static struct k_thread mon_thread_data;

static void smf_thread(void *p1, void *p2, void *p3) 
{
    ARG_UNUSED(p2); ARG_UNUSED(p3);
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
        k_msgq_get(&app_evt_q, &ev, K_FOREVER);
        LOG_INF("SMF thread got event %d", ev.type);
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


/*
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
        case EVT_TIMEOUT: return "TIMEOUT_1S";
        case EVT_RSRP_UPDATE: return "RSRP_UPDATE";
        default: return "?";
    }
}
*/

//static void monitor_thread(void *p1, void *p2, void *p3)
//{
//    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
//    //struct app_ctx *o = p1;
//    
//    struct monitor_event ev;
//    
//    while (1) {
//        //struct mon_snapshot snap;
//
//
//        //k_sem_take(&state_change_sem, K_FOREVER);
//
//
//
//        //k_mutex_lock(&o->lock, K_FOREVER);
//        //snap.state = o->current_state;
//        //snap.counter = o->counter;
//        //snap.running_ms = o-> running_ms;
//        //k_mutex_unlock(&o->lock);
//
//        k_msgq_get(&monitor_q, &ev, K_FOREVER);
//
//
//        LOG_INF("MON: STATE_BEFORE=%s, CURRENT_RSRP=%d, EVENT_NAME=%s",
//            state_name(ev.state), ev.rsrp_dbm, event_name(ev.ev.type));
//
//        //k_sleep(K_MSEC(500));
//    }
//}

int main(void)
{

    static struct app_ctx ctx;

    //k_mutex_init(&ctx.lock);

    k_thread_create(&smf_thread_data, smf_stack, SMF_STACK_SIZE,
        smf_thread, &ctx, NULL, NULL, SMF_PRIORITY, 0, K_NO_WAIT);

    struct app_event boot = { .type = EVT_BOOT };
    k_msgq_put(&app_evt_q, &boot, K_NO_WAIT);

    //k_thread_create(&mon_thread_data, mon_stack, MON_STACK_SIZE,
    //    monitor_thread, NULL, NULL, NULL, MON_PRIORITY, 0, K_NO_WAIT);

    //smf_set_initial(SMF_CTX(&ctx), &states[STATE_IDLE]);


    while (1) {
        //k_sleep(K_SECONDS(60));

        //int32_t rem = k_timer_remaining_get(&timeout_timer);
        //uint32_t st = k_timer_status_get(&timeout_timer);

        //LOG_INF("timer remaining=%d ms, status=%u", rem, st);
        k_sleep(K_SECONDS(60));
    }

    return 0;
}