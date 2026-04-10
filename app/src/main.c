/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 



#include <modem/nrf_modem_lib.h>
#include <nrf_modem_gnss.h>


#include <zephyr/smf.h>
#include <zephyr/sys/printk.h>
#include <modem/lte_lc.h>
#include <modem/ntn.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/app_version.h>

#include "app_types.h"
#include "app_events.h"
#include "app_sm.h"
<<<<<<< HEAD
//#include "accel.h"
=======
#if defined(CONFIG_APP_SENSOR_ACCEL_DEMO)
#include "accel.h"
#endif
>>>>>>> origin/main


LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);




int main(void)
{
    static struct app_ctx ctx;

    app_sm_start(&ctx);

    struct app_event boot = { .type = EVT_BOOT };

    app_event_put(&boot, K_NO_WAIT);

<<<<<<< HEAD

    //accel_start();
    
    
    //k_msgq_put(&app_evt_q, &boot, K_NO_WAIT);

    //k_thread_create(&mon_thread_data, mon_stack, MON_STACK_SIZE,
    //    monitor_thread, NULL, NULL, NULL, MON_PRIORITY, 0, K_NO_WAIT);

    //smf_set_initial(SMF_CTX(&ctx), &states[STATE_IDLE]);
=======
    LOG_INF("Firmware version: %s", APP_VERSION_STRING);
>>>>>>> origin/main

#if defined(CONFIG_APP_SENSOR_ACCEL_DEMO)
    accel_start();
#endif

    while (1) {
        k_sleep(K_SECONDS(60));
    }

    return 0;
}
