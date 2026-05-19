/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#include "ntn_service.h"
#include "app_events.h"

#include <errno.h>
#include <string.h>
#include <nrf_modem_at.h>
#include <modem/lte_lc.h>
#include <modem/ntn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ntn_service, LOG_LEVEL_INF);

#define NTN_MODEM_FW_TOKEN "mfw_nrf9151-ntn"

static bool initialized;
static struct app_ctx *active_ctx;
static void ntn_location_work_handler(struct k_work *work);

K_WORK_DEFINE(ntn_location_work, ntn_location_work_handler);

static void strip_at_response_line_end(char *response)
{
    for (char *p = response; *p != '\0'; p++) {
        if (*p == '\r' || *p == '\n') {
            *p = '\0';
            return;
        }
    }
}

static int ntn_service_check_modem_fw(void)
{
    char fw_version[64] = {0};
    int err;

    err = nrf_modem_at_cmd(fw_version, sizeof(fw_version), "AT+CGMR");
    if (err) {
        if (err > 0) {
            LOG_ERR("Could not read modem firmware version: raw=%d type=%d at_err=%d",
                    err, nrf_modem_at_err_type(err), nrf_modem_at_err(err));
        } else {
            LOG_ERR("Could not read modem firmware version: lib err=%d", err);
        }
        return -EFAULT;
    }

    strip_at_response_line_end(fw_version);
    LOG_INF("Modem firmware for NTN: %s", fw_version);

    if (strstr(fw_version, NTN_MODEM_FW_TOKEN) == NULL) {
        LOG_ERR("NTN NB-IoT requires modem firmware %s; current firmware is %s",
                NTN_MODEM_FW_TOKEN, fw_version);
        return -ENOTSUP;
    }

    return 0;
}

static int ntn_service_set_system_mode(void)
{
    int err;
    int raw_err;

    err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT,
                                 LTE_LC_SYSTEM_MODE_PREFER_AUTO);
    if (!err) {
        return 0;
    }

    raw_err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,0,0,1");
    if (raw_err > 0) {
        LOG_ERR("Raw NTN XSYSTEMMODE failed: raw=%d type=%d at_err=%d",
                raw_err, nrf_modem_at_err_type(raw_err),
                nrf_modem_at_err(raw_err));
    } else if (raw_err < 0) {
        LOG_ERR("Raw NTN XSYSTEMMODE failed: lib err=%d", raw_err);
    }

    LOG_ERR("NTN system mode set failed: %d", err);
    return err;
}

static int ntn_service_set_location(const struct app_ctx *ctx, uint32_t validity)
{
    int err;

    if (!ctx->have_fix) {
        LOG_WRN("No GNSS fix available for NTN location update");
        return -ENODATA;
    }

    LOG_INF("Setting NTN location: lat=%f lon=%f alt=%f",
            (double)ctx->last_pvt.latitude,
            (double)ctx->last_pvt.longitude,
            (double)ctx->last_pvt.altitude);

    err = ntn_location_set((double)ctx->last_pvt.latitude,
                           (double)ctx->last_pvt.longitude,
                           (float)ctx->last_pvt.altitude,
                           validity);
    if (err) {
        LOG_ERR("ntn_location_set failed: %d", err);
    }

    return err;
}

static void ntn_location_work_handler(struct k_work *work)
{
    int err;

    ARG_UNUSED(work);

    if (active_ctx != NULL) {
        err = ntn_service_set_location(active_ctx, 0);
        if (err) {
            struct app_event ev = {
                .type = EVT_MODEM_SWITCH_FAIL,
                .source_rat = RAT_NTN,
            };

            (void)app_event_put(&ev, K_NO_WAIT);
        }
    }
}

static void ntn_evt_handler(const struct ntn_evt *evt)
{
    switch (evt->type) {
    case NTN_EVT_LOCATION_REQUEST:
        LOG_INF("NTN location request: requested=%d accuracy=%u m",
                evt->location_request.requested,
                (unsigned int)evt->location_request.accuracy);

        if (evt->location_request.requested && active_ctx != NULL) {
            (void)k_work_submit(&ntn_location_work);
        }
        break;

    default:
        LOG_DBG("Unhandled NTN library event: %d", evt->type);
        break;
    }
}

static void ntn_lc_evt_handler(const struct lte_lc_evt *const evt)
{    
    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
        if (active_ctx != NULL) {
            LOG_INF("NTN NW registration status: %d (state=%d)",
                    evt->nw_reg_status, active_ctx->state);
        } else {
            LOG_INF("NTN NW registration status: %d", evt->nw_reg_status);
        }

        switch (evt->nw_reg_status) {
        case LTE_LC_NW_REG_REGISTERED_HOME:
        case LTE_LC_NW_REG_REGISTERED_ROAMING:
            LOG_INF("NTN registered on network");
            {
                struct app_event ev = {
                    .type = EVT_REG_OK,
                    .source_rat = RAT_NTN,
                };
                (void)app_event_put(&ev, K_NO_WAIT);
            }
            break;

        case LTE_LC_NW_REG_NOT_REGISTERED:
        case LTE_LC_NW_REG_UNKNOWN:
            if (active_ctx != NULL && active_ctx->state == STATE_NTN_CONNECTING) {
                LOG_WRN("NTN attach in progress: ignoring reg status=%d",
                        evt->nw_reg_status);
                break;
            }
            LOG_WRN("NTN registration failed/status=%d", evt->nw_reg_status);
            {
                struct app_event ev = {
                    .type = EVT_REG_FAIL,
                    .source_rat = RAT_NTN,
                };
                (void)app_event_put(&ev, K_NO_WAIT);
            }
            break;

        case LTE_LC_NW_REG_REGISTRATION_DENIED:
        case LTE_LC_NW_REG_UICC_FAIL:
            LOG_WRN("NTN registration failed/status=%d", evt->nw_reg_status);
            {
                struct app_event ev = {
                    .type = EVT_REG_FAIL,
                    .source_rat = RAT_NTN,
                };
                (void)app_event_put(&ev, K_NO_WAIT);
            }
            break;

        default:
            break;
        }
        break;

    case LTE_LC_EVT_PDN:
        switch (evt->pdn.type) {

        case LTE_LC_EVT_PDN_ACTIVATED:
            LOG_INF("NTN: PDN activated");
            {
                struct app_event ev = {
                    .type = EVT_PDN_UP,
                    .source_rat = RAT_NTN,
                };
                (void)app_event_put(&ev, K_NO_WAIT);
            }
            break;

        case LTE_LC_EVT_PDN_DEACTIVATED:
        case LTE_LC_EVT_PDN_NETWORK_DETACH:
            LOG_INF("NTN: PDN down");
            {
                struct app_event ev = {
                    .type = EVT_PDN_DOWN,
                    .source_rat = RAT_NTN,
                };
                (void)app_event_put(&ev, K_NO_WAIT);
            }
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

    LOG_INF("Preparing modem for NTN NB-IoT");

    err = ntn_service_check_modem_fw();
    if (err) {
        return err;
    }

    err = lte_lc_power_off();
    if (err) {
        LOG_ERR("lte_lc_power_off before NTN setup failed: %d", err);
        return err;
    }

    if (!ctx->have_fix) {
        LOG_ERR("Cannot start NTN without a GNSS fix");
        return -ENODATA;
    }

    err = ntn_service_set_system_mode();
    if (err) {
        return err;
    }

    ctx->ntn_initialized = true;

    return 0;
}

int ntn_service_init(void)
{
    if (initialized) {
        return 0;
    }

    ntn_register_handler(ntn_evt_handler);

    initialized = true;
    return 0;
}

/* simple connect attempt */
int ntn_service_connect(struct app_ctx *ctx)
{
    int err;

    LOG_INF("Starting NTN connect");

    if (!initialized) {
        return -EINVAL;
    }

    active_ctx = ctx;

    err = ntn_service_prepare(ctx);
    if (err) {
        return err;
    }
    
/* verbose modem */
#ifdef CONFIG_APP_DEBUG_NTN
    err = nrf_modem_at_printf("AT+CEREG=5");
    if (err) {
        return err;
    }
#endif

    /* start modem */
    return lte_lc_connect_async(ntn_lc_evt_handler);
}

int ntn_service_stop(void)
{
    int err;

    (void)k_work_cancel(&ntn_location_work);

    if (active_ctx != NULL) {
        active_ctx->ntn_initialized = false;
    }

    active_ctx = NULL;

    err = lte_lc_offline();
    if (err) {
        LOG_WRN("lte_lc_offline failed while stopping NTN: %d", err);
    }

    LOG_INF("NTN service stopped");
    return err;
}


