/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_events.h"
#include "modem_service.h"

#include <modem/lte_lc.h>

static void modem_connect_evt_handler(const struct lte_lc_evt *const evt)
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

    default:
        break;
    }
}

int modem_service_connect_async(void)
{
    return lte_lc_connect_async(modem_connect_evt_handler);
}
