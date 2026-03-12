/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 
#include "ntn_service.h"
#include "app_events.h"
#include "modem_service.h"

#include <modem/lte_lc.h>
#include <modem/ntn.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ntn_service, LOG_LEVEL_INF);



static int ntn_service_prepare(struct app_ctx *ctx)
{
    int err;

    struct lte_lc_cellular_profile ntn_profile = {
        .id = 0,
        .act = LTE_LC_ACT_NTN,
        .uicc = LTE_LC_UICC_PHYSICAL,
    };

    if (!ctx->ntn_initialized) {
        err = lte_lc_power_off();
        if (err) {
            return err;
        }

        err = lte_lc_cellular_profile_configure(&ntn_profile);
        if (err) {
            return err;
        }

        ctx->ntn_initialized = true;
    }

    if (ctx->have_fix) {
        err = ntn_location_set(ctx->last_pvt.latitude,
                               ctx->last_pvt.longitude,
        err = ntn_location_set((double)ctx->last_pvt.latitude / 1e7,
                               (double)ctx->last_pvt.longitude / 1e7,
                               (float)ctx->last_pvt.altitude,
                               0);
        if (err) {
            return err;
        }
    }

    err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT,
                                 LTE_LC_SYSTEM_MODE_PREFER_AUTO);
    if (err) {
        return err;
    }

    return 0;
}

/* simple connect attempt no udp */
int ntn_service_connect(struct app_ctx *ctx)
{
    int err;

    err = ntn_service_prepare(ctx);
    if (err) {
        return err;
    }

    return modem_service_connect_async();
}
