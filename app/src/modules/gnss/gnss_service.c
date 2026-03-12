/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 
#include "gnss_service.h"
#include "app_types.h"
#include "app_events.h"

#include <nrf_modem_gnss.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gnss_service, LOG_LEVEL_INF);

static struct k_work gnss_pvt_work;
static struct k_work_delayable gnss_timeout_work;

static void gnss_timeout_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    struct app_event ev = { .type = EVT_GNSS_TIMEOUT };
    
    int err = app_event_put(&ev, K_NO_WAIT);

    LOG_INF("gnss_timeout_work_handler fired, q_put err=%d", err);
}


/* define kernel work task */
static void gnss_pvt_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    struct nrf_modem_gnss_pvt_data_frame pvt;
    int err = nrf_modem_gnss_read(&pvt, sizeof(pvt), NRF_MODEM_GNSS_DATA_PVT);

    if (err) {
        return;
    }

    if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
        struct app_event ev = { .type = EVT_GNSS_FIX, .pvt = pvt };
        err = app_event_put(&ev, K_NO_WAIT);
        LOG_INF("put event gnss fix err=%d", err);
    }
}

/* listen to gnss events from modem */
static void gnss_event_handler(int event)
{
    if (event == NRF_MODEM_GNSS_EVT_PVT) {
        k_work_submit(&gnss_pvt_work);
    }
}

/* boot entry helper function */
int gnss_service_init(void)
{
    k_work_init(&gnss_pvt_work, gnss_pvt_work_handler);
    k_work_init_delayable(&gnss_timeout_work, gnss_timeout_work_handler);
    nrf_modem_gnss_event_handler_set(gnss_event_handler);
    return 0;
}

int gnss_service_start(void)
{
    return nrf_modem_gnss_start();
}

int gnss_service_stop(void)
{
    return nrf_modem_gnss_stop();
}

int gnss_service_start_timeout(int32_t timeout_sec)
{
    return k_work_reschedule(&gnss_timeout_work, K_SECONDS(timeout_sec));
}

int gnss_service_cancel_timeout(void)
{
    return k_work_cancel_delayable(&gnss_timeout_work);
}