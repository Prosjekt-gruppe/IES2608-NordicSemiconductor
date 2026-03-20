/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#include "app_events.h"

#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include "modem_service.h"

#include <zephyr/logging/log.h>
//#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(modem_service, LOG_LEVEL_INF);

int modem_service_init(void){ 
    int err; 

    err = nrf_modem_lib_init(); 
    if (err){
        LOG_ERR("nrf_modem_lib_init failed: %u", err); 
        return err; 
    }

    return 0;
}

