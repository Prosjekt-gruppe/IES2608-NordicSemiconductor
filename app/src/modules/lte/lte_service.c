/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 



 #include "lte_service.h"
 #include "app_events.h"

 #include <modem/lte_lc.h>
 #include <zephyr/kernel.h>
 #include <zephyr/logging/log.h>


LOG_MODULE_REGISTER(lte_service, LOG_LEVEL_INF);

static bool lte_connected; 

/*
static int publish_evt(enum app_evt_type type)
{
    struct app_event ev = {
        .type = type,
    };

    LOG_INF("Publishing %s", app_evt_name(type));
    return app_event_put(&ev, K_NO_WAIT); 
}
*/
static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
        LOG_INF("LTE NW registration status: %d", evt->nw_reg_status);


        switch (evt->nw_reg_status) {
        case LTE_LC_NW_REG_REGISTERED_HOME:
        case LTE_LC_NW_REG_REGISTERED_ROAMING:
            lte_connected=true;
            LOG_INF("LTE registered on network");
            (void)app_event_publish_type(EVT_REG_OK);
            break;

        case LTE_LC_NW_REG_NOT_REGISTERED:
        case LTE_LC_NW_REG_REGISTRATION_DENIED:
        case LTE_LC_NW_REG_UNKNOWN:
        case LTE_LC_NW_REG_UICC_FAIL:
            lte_connected=false;
            LOG_WRN("LTE registration failed/status=%d", evt->nw_reg_status);
            (void)app_event_publish_type(EVT_REG_FAIL);
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
        LOG_DBG("Unhandled LTE evnt type: %d", evt->type);
        break;
    }
}

int lte_service_init(void)
{
    lte_connected = false; 
    return 0; 
}

int lte_service_connect_async(void)
{
    lte_connected = false;
    return lte_lc_connect_async(lte_lc_evt_handler);   
}

int lte_service_disconnect(void)
{
    lte_connected = false; 
    return lte_lc_offline(); 
}

bool lte_service_is_connected(void)
{
    return lte_connected; 
}