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
static bool probe_pending;

static const char *lte_mode_name(enum lte_lc_lte_mode mode)
{
    switch (mode) {
    case LTE_LC_LTE_MODE_NONE:
        return "NONE";
    case LTE_LC_LTE_MODE_LTEM:
        return "LTE-M";
    case LTE_LC_LTE_MODE_NBIOT:
        return "NB-IoT";
    default:
        return "UNKNOWN";
    }
}

/*
* TODO: filter modem events based on active rat
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

            if (probe_pending) {
                probe_pending = false;
                struct app_event ev = {
                    .type = EVT_TN_READY_FOR_PROBE,
                    .source_rat = RAT_LTEM,
                };
                (void)app_event_put(&ev, K_NO_WAIT);
            } else {
                struct app_event ev = {
                    .type = EVT_REG_OK,
                    .source_rat = RAT_LTEM,
                };
                (void)app_event_put(&ev, K_NO_WAIT);
            }

            LOG_INF("LTE registered on network");
            break;

        case LTE_LC_NW_REG_NOT_REGISTERED:
        case LTE_LC_NW_REG_REGISTRATION_DENIED:
        case LTE_LC_NW_REG_UNKNOWN:
        case LTE_LC_NW_REG_UICC_FAIL:
            lte_connected=false;
            LOG_WRN("LTE registration failed/status=%d", evt->nw_reg_status);
            struct app_event ev = {
                .type = EVT_REG_FAIL,
                .source_rat = RAT_LTEM,
            };
            (void)app_event_put(&ev, K_NO_WAIT);
            break;

        default:
            break;
        }
        break;

    case LTE_LC_EVT_CELLULAR_PROFILE_ACTIVE:
        LOG_INF("modem activate cellular profile (RAT starting)");
        break;

    case LTE_LC_EVT_LTE_MODE_UPDATE:
        LOG_INF("LTE mode update: %s (%d)",
                lte_mode_name(evt->lte_mode),
                evt->lte_mode);
        break;
    default:
        LOG_DBG("Unhandled LTE evnt type: %d", evt->type);
        break;
    }
}

int lte_service_init(void)
{
    lte_connected = false;
    probe_pending = false;
    return 0; 
}

void lte_service_set_probe_pending(bool enable)
{
    probe_pending = enable;
}

int lte_service_connect_async(void)
{
    int err;

    lte_connected = false;

    LOG_INF("Preparing modem for LTE-M/GNSS");

    err = lte_lc_power_off();
    if (err) {
        LOG_ERR("LTE modem power-off before connect failed: %d", err);
        return err;
    }

    err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_GPS,
                                 LTE_LC_SYSTEM_MODE_PREFER_LTEM);
    if (err) {
        LOG_ERR("LTE-M/GNSS system mode set failed: %d", err);
        return err;
    }

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
