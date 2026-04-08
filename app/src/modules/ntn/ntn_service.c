/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include "ntn_service.h"
#include "app_events.h"

#include <modem/lte_lc.h>
#include <modem/ntn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ntn_service, LOG_LEVEL_INF);

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

static void ntn_lc_evt_handler(const struct lte_lc_evt *const evt)
{
    // struct app_event app_ev = {0};

    switch (evt->type)
    {
    case LTE_LC_EVT_NW_REG_STATUS:
        LOG_INF("NTN NW registration status: %d", evt->nw_reg_status);

        switch (evt->nw_reg_status)
        {
        case LTE_LC_NW_REG_REGISTERED_HOME:
        case LTE_LC_NW_REG_REGISTERED_ROAMING:
            LOG_INF("NTN registered on network");
            (void)app_event_publish_type(EVT_REG_OK);
            break;

        case LTE_LC_NW_REG_NOT_REGISTERED:
        case LTE_LC_NW_REG_REGISTRATION_DENIED:
        case LTE_LC_NW_REG_UNKNOWN:
        case LTE_LC_NW_REG_UICC_FAIL:
            LOG_WRN("NTN registration failed/status=%d", evt->nw_reg_status);
            (void)app_event_publish_type(EVT_REG_FAIL);
            break;

        default:
            break;
        }
        break;

    case LTE_LC_EVT_PDN:
        switch (evt->pdn.type)
        {

        case LTE_LC_EVT_PDN_ACTIVATED:
            LOG_INF("NTN: PDN activated");
            // app_ev.type = EVT_PDN_UP;
            (void)app_event_publish_type(EVT_PDN_UP);
            break;

        case LTE_LC_EVT_PDN_DEACTIVATED:
        case LTE_LC_EVT_PDN_NETWORK_DETACH:
            LOG_INF("NTN: PDN down");
            // app_ev.type = EVT_PDN_DOWN;
            (void)app_event_publish_type(EVT_PDN_DOWN);
            break;

        default:
            break;
        }
        break;

    case LTE_LC_EVT_CELLULAR_PROFILE_ACTIVE:
        LOG_INF("NTN cellular profile active");
        break;

    case LTE_LC_EVT_LTE_MODE_UPDATE:
        LOG_INF("NTN mode update %d", evt->lte_mode);
        break;

    default:
        LOG_DBG("Unhandled NTN event type: %d", evt->type);
        break;
    }
}

static int ntn_service_prepare(struct app_ctx *ctx)
{
    int err;

    struct lte_lc_cellular_profile ntn_profile = {
        .id = 0,
        .act = LTE_LC_ACT_NTN,
        .uicc = LTE_LC_UICC_PHYSICAL,
    };

    if (!ctx->ntn_initialized)
    {
        err = lte_lc_power_off();
        if (err)
        {
            return err;
        }

        err = lte_lc_cellular_profile_configure(&ntn_profile);
        if (err)
        {
            return err;
        }

        ctx->ntn_initialized = true;
    }

    if (ctx->final_fix)
    {
        err = ntn_location_set((double)ctx->final_pvt.latitude,
                               (double)ctx->final_pvt.longitude,
                               (float)ctx->final_pvt.altitude,
                               0);
        if (err)
        {
            LOG_WRN("ntn_location_set failed: %d", err);
        }
    }

    err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT,
                                 LTE_LC_SYSTEM_MODE_PREFER_AUTO);
    if (err)
    {
        return err;
    }

    return 0;
}

/* simple connect attempt */
int ntn_service_connect(struct app_ctx *ctx)
{
    int err;

    err = ntn_service_prepare(ctx);
    if (err)
    {
        return err;
    }

    return lte_lc_connect_async(ntn_lc_evt_handler);
}
