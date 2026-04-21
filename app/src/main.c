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

#if defined(CONFIG_APP_FIELD_LOG)
#include "field_log.h"
#endif


#if defined(CONFIG_APP_SENSOR_ACCEL_DEMO)
#include "accel.h"
#endif


LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);




int main(void)
{
    static struct app_ctx ctx;

#if defined(CONFIG_APP_FIELD_LOG)
    int err = field_log_start();

    if (err) {
        LOG_WRN("field_log_start failed: %d", err);
    }
#endif

    app_sm_start(&ctx);

    struct app_event boot = { .type = EVT_BOOT };

    app_event_put(&boot, K_NO_WAIT);

    LOG_INF("Firmware version: %s", APP_VERSION_STRING);

#if defined(CONFIG_APP_SENSOR_ACCEL_DEMO)
    accel_start();
#endif

    while (1) {
        k_sleep(K_SECONDS(60));
    }

    return 0;
}
