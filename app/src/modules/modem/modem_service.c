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

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
    struct app_event app_ev = {0};

    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
        switch (evt->nw_reg_status) {
        case LTE_LC_NW_REG_REGISTERED_HOME:
        case LTE_LC_NW_REG_REGISTERED_ROAMING:
            app_ev.type = EVT_REG_OK;
            (void)app_event_put(&app_ev, K_NO_WAIT);
            break;

        case LTE_LC_NW_REG_NOT_REGISTERED:
        case LTE_LC_NW_REG_REGISTRATION_DENIED:
        case LTE_LC_NW_REG_UNKNOWN:
        case LTE_LC_NW_REG_UICC_FAIL:
            app_ev.type = EVT_REG_FAIL;
            (void)app_event_put(&app_ev, K_NO_WAIT);
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

int modem_service_init(void){ 
    int err; 

    err = nrf_modem_lib_init(); 
    if (err){
        LOG_ERR("nrf_modem_lib_init failed: %u", err); 
        return err; 
    }

    return 0;
}

int modem_service_connect_async(void)
{
    return lte_lc_connect_async(lte_lc_evt_handler);
}
