/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#include "app_events.h"

#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include "modem_service.h"
#include <nrf_modem_at.h>



#include <zephyr/net/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <zephyr/net/net_ip.h>

#include <zephyr/logging/log.h>
//#include <zephyr/kernel.h>

/* for udp test */
#define SERVER_PORT 57967 // port 
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

static int modem_at_ok(const char *cmd)
{
    int err = nrf_modem_at_printf("%s", cmd);
    if (err) {
        LOG_ERR("AT failed: %s (err=%d)", cmd, err);
        return err;
    }

    LOG_INF("AT ok: %s", cmd);
    return 0;
}



static int modem_service_switch(enum modem_access_mode mode)
{
    int err;

    /* preserve PDN context */
    err = modem_at_ok("AT+CFUN=45");
    if (err) return err;

    switch (mode) {
    /* set LTE-M system mode */
    case MODEM_ACCESS_TN:
        err = modem_at_ok("AT%XSYSTEMMODE=1,0,0,0,0");
        break;

    /* set NTN system mode */
    case MODEM_ACCESS_NTN:
        err = modem_at_ok("AT%XSYSTEMMODE=0,0,0,0,1");
        break;

    default:
        return -EINVAL;
    }

    if (err) return err;

    /* start modem */
    err = modem_at_ok("AT+CFUN=1");
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
