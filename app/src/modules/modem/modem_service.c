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


static int modem_at_ok(const char *cmd)
{
    int err = nrf_modem_at_printf("%s", cmd);

    if (err == 0) {
        LOG_INF("AT ok: %s", cmd);
        return 0;
    }

    if (err > 0) {
        LOG_ERR("AT failed: %s (raw=%d, at_err=%d)", cmd, err, nrf_modem_at_err(err));
        return err;
    }

    LOG_ERR("AT failed: %s (lib err=%d)", cmd, err);
    return err;
}

static int modem_service_switch(enum modem_access_mode mode)
{
    int err;

    LOG_INF("switch: sending CFUN=45");
    err = modem_at_ok("AT+CFUN=45");
    LOG_INF("switch: CFUN=45 ret=%d", err);
    if (err) {
        return err;
    }

    switch (mode) {
    case MODEM_ACCESS_TN:
        LOG_INF("switch: sending XSYSTEMMODE TN");
        err = modem_at_ok("AT%XSYSTEMMODE=1,0,0,0,0");
        LOG_INF("switch: XSYSTEMMODE TN ret=%d", err);
        break;

    case MODEM_ACCESS_NTN:
        LOG_INF("switch: sending XSYSTEMMODE NTN");
        err = modem_at_ok("AT%XSYSTEMMODE=0,0,0,0,1");
        LOG_INF("switch: XSYSTEMMODE NTN ret=%d", err);
        break;

    default:
        LOG_INF("modem service switch default case");
        return -EINVAL;
    }

    if (err) {
        return err;
    }

    LOG_INF("switch: sending CFUN=1");
    err = modem_at_ok("AT+CFUN=1");
    LOG_INF("switch: CFUN=1 ret=%d", err);
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




int modem_service_prepare_profiles(struct app_ctx *ctx)
{
    int err;

    if (!initialized || ctx == NULL) {
        return -EINVAL;
    }

    if (ctx->ntn_initialized) {
        return 0;
    }

    struct lte_lc_cellular_profile ntn_profile = {
        .id = 0,
        .act = LTE_LC_ACT_NTN,
        .uicc = LTE_LC_UICC_PHYSICAL,
    };

    struct lte_lc_cellular_profile tn_profile = {
        .id = 1,
        .act = LTE_LC_ACT_LTEM,
        .uicc = LTE_LC_UICC_PHYSICAL,
    };

    err = lte_lc_power_off();
    if (err) {
        LOG_ERR("lte_lc_power_off failed: %d", err);
        return err;
    }

    err = lte_lc_cellular_profile_configure(&ntn_profile);
    if (err) {
        LOG_ERR("NTN profile config failed: %d", err);
        return err;
    }

    err = lte_lc_cellular_profile_configure(&tn_profile);
    if (err) {
        LOG_ERR("TN profile config failed: %d", err);
        return err;
    }

    ctx->ntn_initialized = true;
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
