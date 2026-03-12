/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 


/*
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_gnss.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/smf.h>
#include <zephyr/sys/printk.h>
#include <modem/lte_lc.h>
#include <modem/ntn.h>
*/

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/app_version.h>

#include "app_types.h"
#include "app_events.h"
#include "app_sm.h"


LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);




int main(void)
{

    static struct app_ctx ctx;

    //k_mutex_init(&ctx.lock);

    app_sm_start(&ctx);

    struct app_event boot = { .type = EVT_BOOT };

    app_event_put(&boot, K_NO_WAIT);


    LOG_INF("Firmware version: %s", APP_VERSION_STRING);
    
    
    //k_msgq_put(&app_evt_q, &boot, K_NO_WAIT);

    //k_thread_create(&mon_thread_data, mon_stack, MON_STACK_SIZE,
    //    monitor_thread, NULL, NULL, NULL, MON_PRIORITY, 0, K_NO_WAIT);

    //smf_set_initial(SMF_CTX(&ctx), &states[STATE_IDLE]);


    while (1) {
        //int32_t rem = k_timer_remaining_get(&timeout_timer);
        //uint32_t st = k_timer_status_get(&timeout_timer);

        //LOG_INF("timer remaining=%d ms, status=%u", rem, st);
        k_sleep(K_SECONDS(60));
    }

    return 0;
}