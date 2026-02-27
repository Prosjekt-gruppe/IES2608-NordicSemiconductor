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


enum test_states {
    STATE_IDLE,
    STATE_RUNNING
};


struct test_object {
    struct smf_ctx ctx;
    int counter; 
};


static const struct smf_state states[] = {
    [STATE_IDLE]    = SMF_CREATE_STATE(idle_entry,    idle_run,    NULL, NULL, NULL),
    [STATE_RUNNING] = SMF_CREATE_STATE(running_entry, running_run, NULL, NULL, NULL),
};


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
        smf_set_state(SMF_CTX(state), &states[STATE_RUNNING]);
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
        smf_set_state(SMF_CTX(state), &states[STATE_IDLE]);
    }

    return SMF_EVENT_HANDLED;

}


int main(void)
{

    static struct test_object obj;

    smf_set_initial(SMF_CTX(&obj), &states[STATE_IDLE]);


    while (1) {
        (void)smf_run_state(SMF_CTX(&obj));
    }

    return 0;
}