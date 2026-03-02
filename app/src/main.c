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


static void idle_entry(void *obj);
static enum smf_state_result idle_run(void *obj);

static void running_entry(void *obj);
static enum smf_state_result running_run(void *obj);


K_SEM_DEFINE(state_change_sem, 0, 1);


enum test_states {
    STATE_IDLE,
    STATE_RUNNING
};


struct test_object {
    struct smf_ctx ctx;

    /* shared data */
    int counter;
    int running_ms;
    enum test_states current_state;
    
    /* sync */
    struct k_mutex lock;
};

struct state_event {
    enum test_states new_state;
    int counter;
    int running_ms;
};

K_MSGQ_DEFINE(state_evt_q, sizeof(struct state_event), 8, 4);


static const struct smf_state states[] = {
    [STATE_IDLE]    = SMF_CREATE_STATE(idle_entry,    idle_run,    NULL, NULL, NULL),
    [STATE_RUNNING] = SMF_CREATE_STATE(running_entry, running_run, NULL, NULL, NULL),
};

static void set_state(struct test_object *o, enum test_states s) 
{
    o->current_state = s;
    smf_set_state(SMF_CTX(o), &states[s]);

    /* signal monitor thread */
    //k_sem_give(&state_change_sem);

    struct state_event ev = {
        .new_state = s,
        .counter = o->counter,
        .running_ms = o-> running_ms,
    };

    /* add state event object to queue */
    int err = k_msgq_put(&state_evt_q, &ev, K_NO_WAIT);
    if (err != 0) {
        LOG_WRN("state_evt_q full, dropping event");
    }
}


static void idle_entry(void *obj) 
{
    ARG_UNUSED(obj);
    LOG_INF("IDLE STATE");
}


static enum smf_state_result idle_run(void *obj)
{
    struct test_object *state = obj;

    LOG_INF("IDLE: counter=%d", state->counter);

    if (state->counter++ > 3) {
        state->counter = 0;
        set_state(obj, STATE_RUNNING);
    }

    return SMF_EVENT_HANDLED;
}


static void running_entry(void *obj) 
{
    ARG_UNUSED(obj);
    LOG_INF("RUNNING STATE");
}


static enum smf_state_result running_run(void *obj)
{
    struct test_object *state = obj;

    LOG_INF("RUNNING: counter=%d", state->counter);

    k_sleep(K_SECONDS(1));

    if (state->counter++ >= 9) {
        state->counter = 0;
        set_state(obj, STATE_IDLE);
    }

    return SMF_EVENT_HANDLED;

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
    struct test_object *o = p1;

    k_mutex_lock(&o->lock, K_FOREVER);
    o->current_state = STATE_IDLE;
    smf_set_initial(SMF_CTX(o), &states[STATE_IDLE]);
    k_mutex_unlock(&o->lock);

    while (1) {
        k_mutex_lock(&o->lock, K_FOREVER);
        (void)smf_run_state(SMF_CTX(o));
        k_mutex_unlock(&o->lock);

        k_sleep((K_MSEC(100)));
    }
}


struct mon_snapshot {
    enum test_states state;
    int counter;
    int running_ms;
};


static const char *state_name(enum test_states s)
{
    switch (s) {
        case STATE_IDLE: return "IDLE";
        case STATE_RUNNING: return "RUNNING";
        default: return "?";
    }
}

static void monitor_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    //struct test_object *o = p1;

    while (1) {
        //struct mon_snapshot snap;

        struct state_event ev;

        //k_sem_take(&state_change_sem, K_FOREVER);



        //k_mutex_lock(&o->lock, K_FOREVER);
        //snap.state = o->current_state;
        //snap.counter = o->counter;
        //snap.running_ms = o-> running_ms;
        //k_mutex_unlock(&o->lock);

        k_msgq_get(&state_evt_q, &ev, K_FOREVER);


        LOG_INF("MON: STATE_CHANGE=%s, counter=%d, running_ms=%d",
            state_name(ev.new_state), ev.counter, ev.running_ms);

        //k_sleep(K_MSEC(500));
    }
}


int main(void)
{

    static struct test_object obj;

    k_mutex_init(&obj.lock);

    k_thread_create(&smf_thread_data, smf_stack, SMF_STACK_SIZE,
        smf_thread, &obj, NULL, NULL, SMF_PRIORITY, 0, K_NO_WAIT);

    k_thread_create(&mon_thread_data, mon_stack, MON_STACK_SIZE,
        monitor_thread, &obj, NULL, NULL, MON_PRIORITY, 0, K_NO_WAIT);

    //smf_set_initial(SMF_CTX(&obj), &states[STATE_IDLE]);


    while (1) {
        k_sleep(K_SECONDS(60));
    }

    return 0;
}