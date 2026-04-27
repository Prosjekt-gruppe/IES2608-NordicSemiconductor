/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#include "app_events.h"

#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include "modem_service.h"
#include <zephyr/net/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <zephyr/net/net_ip.h>

#include <zephyr/logging/log.h>
//#include <zephyr/kernel.h>

/* for udp test */
#define SERVER_PORT 41313 // port 
#define SERVER_ADDR "46.226.106.127" // tcpbin.net

static struct k_work switch_work;
static enum modem_switch_state switch_state;
static bool initialized;

enum modem_switch_state {
    MODEM_SWITCH_IDLE,
    MODEM_SWITCH_TO_TN,
    MODEM_SWITCH_TO_NTN,
};


enum modem_access_mode {
    MODEM_ACCESS_TN,
    MODEM_ACCESS_NTN,
};

LOG_MODULE_REGISTER(modem_service, LOG_LEVEL_INF);

static int modem_system_mode_set(enum modem_access_mode mode)
{
    switch (mode) {
    case MODEM_ACCESS_TN:
        LOG_INF("switch: setting terrestrial system mode");
        return lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS,
                                      LTE_LC_SYSTEM_MODE_PREFER_LTEM_PLMN_PRIO);

    case MODEM_ACCESS_NTN:
        LOG_INF("switch: setting NTN NB-IoT system mode");
        return lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT,
                                      LTE_LC_SYSTEM_MODE_PREFER_AUTO);

    default:
        return -EINVAL;
    }
}

static int modem_service_switch(enum modem_access_mode mode)
{
    int err;

    LOG_INF("switch: powering modem off before system mode change");
    err = lte_lc_power_off();
    if (err) {
        LOG_ERR("switch: lte_lc_power_off failed: %d", err);
        return err;
    }

    err = modem_system_mode_set(mode);
    if (err) {
        LOG_ERR("switch: system mode set failed: %d", err);
        return err;
    }

    LOG_INF("switch: bringing modem back to normal mode");
    err = lte_lc_normal();
    if (err) {
        LOG_ERR("switch: lte_lc_normal failed: %d", err);
    }

    return err;
}


static void modem_switch_work_handler(struct k_work *work)
{
    int err;

    ARG_UNUSED(work);

    switch (switch_state) {
    case MODEM_SWITCH_TO_TN:
        err = modem_service_switch(MODEM_ACCESS_TN);
        if (err) {
            LOG_WRN("Switch to TN failed: %d", err);
            (void)app_event_publish_type(EVT_MODEM_SWITCH_FAIL);
        } else {
            (void)app_event_publish_type(EVT_MODEM_SWITCH_CMD_OK);
        }
        break;

    case MODEM_SWITCH_TO_NTN:
        err = modem_service_switch(MODEM_ACCESS_NTN);
        if (err) {
            LOG_WRN("Switch to NTN failed: %d", err);
            (void)app_event_publish_type(EVT_MODEM_SWITCH_FAIL);
        } else {
            (void)app_event_publish_type(EVT_MODEM_SWITCH_CMD_OK);
        }
        break;

    case MODEM_SWITCH_IDLE:
        LOG_INF("modem switch idle case triggered");
        break;
    default:
        break;
    }

    switch_state = MODEM_SWITCH_IDLE;
}

int modem_service_switch_to_tn(void)
{
    if (!initialized) {
        return -EINVAL;
    }

    if (switch_state != MODEM_SWITCH_IDLE) {
        return -EBUSY;
    }

    switch_state = MODEM_SWITCH_TO_TN;
    k_work_submit(&switch_work);
    return 0;
}


int modem_service_switch_to_ntn(void)
{
    if (!initialized) {
        return -EINVAL;
    }

    if (switch_state != MODEM_SWITCH_IDLE) {
        return -EBUSY;
    }

    switch_state = MODEM_SWITCH_TO_NTN;
    k_work_submit(&switch_work);
    return 0;
}




int modem_service_udp_send_test(void)
{
    int sock;
    struct sockaddr_in addr = {0};
    int err;

    sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        LOG_ERR("socket failed: %d", errno);
        return -errno;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    zsock_inet_pton(AF_INET, SERVER_ADDR, &addr.sin_addr);

    /* send 'a' to tcpbin */
    err = zsock_sendto(sock, "a", 1, 0,
                    (struct sockaddr *)&addr,
                    sizeof(addr));

    if (err < 0) {
        err = -errno;
        LOG_ERR("sendto failed: errno=%d", -err);
        
        zsock_close(sock);
        return err;
    }


    zsock_close(sock);
    return 0;
}

int modem_service_udp_send_burst(const struct udp_test_cfg *cfg)
{
    if (cfg == NULL) {
        return -EINVAL;
    }

    const struct udp_test_cfg *c = cfg;
    
    int sock;
    struct sockaddr_in addr = {0};
    
    char buf[256];

    if (c->payload_len > sizeof(buf)) {
        return -EINVAL;
    }

    /* generate dummy data */
    memset(buf, 0xAA, c->payload_len);

    sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -errno;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    zsock_inet_pton(AF_INET, SERVER_ADDR, &addr.sin_addr);

    for (int i = 0; i < c->count; i++) {
        int err = zsock_sendto(sock, buf, c->payload_len, 0,
                               (struct sockaddr *)&addr,
                               sizeof(addr));

        if (err < 0) {
            zsock_close(sock);
            return -errno;
        }

        k_msleep(c->interval_ms);
    }

    zsock_close(sock);
    return 0;
}


int modem_service_init(void){ 
    int err; 

    if (initialized) {
        return 0;
    }


    err = nrf_modem_lib_init(); 
    if (err){
        LOG_ERR("nrf_modem_lib_init failed: %u", err); 
        return err; 
    }

    k_work_init(&switch_work, modem_switch_work_handler);
    switch_state = MODEM_SWITCH_IDLE;
    initialized = true;

    return 0;
}
