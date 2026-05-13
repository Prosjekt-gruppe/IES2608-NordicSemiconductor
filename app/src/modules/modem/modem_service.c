/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#include "app_events.h"

#include <modem/nrf_modem_lib.h>
//#include <modem/lte_lc.h>
#include "modem_service.h"
#include "modem_logic.h"
#include <nrf_modem_at.h>



#include <zephyr/net/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <zephyr/net/net_ip.h>

#include <zephyr/logging/log.h>
//#include <zephyr/kernel.h>

/* for udp test */
#define SERVER_PORT 41313 // port 
#define SERVER_ADDR "46.226.106.127" // tcpbin.net

enum modem_switch_state {
    MODEM_SWITCH_IDLE,
    MODEM_SWITCH_TO_TN,
    MODEM_SWITCH_TO_NTN,
};

static struct k_work switch_work;
static enum modem_switch_state switch_state;
static bool initialized;


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

static int modem_service_switch(enum modem_logic_access_mode mode)
{
    int err;
    const char *system_mode_cmd;

    LOG_INF("switch: sending CFUN=45");
    err = modem_at_ok("AT+CFUN=45");
    LOG_INF("switch: CFUN=45 ret=%d", err);
    if (err) {
        return err;
    }

    system_mode_cmd = modem_logic_system_mode_cmd(mode);
    if (system_mode_cmd == NULL) {
        LOG_INF("modem service switch default case");
        return -EINVAL;
    }

    LOG_INF("switch: sending XSYSTEMMODE");
    err = modem_at_ok(system_mode_cmd);
    LOG_INF("switch: XSYSTEMMODE ret=%d", err);

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
        err = modem_service_switch(MODEM_LOGIC_ACCESS_TN);
        if (err) {
            LOG_WRN("Switch to TN failed: %d", err);
            (void)app_event_publish_type(EVT_MODEM_SWITCH_FAIL);
        } else {
            (void)app_event_publish_type(EVT_MODEM_SWITCH_CMD_OK);
        }
        break;

    case MODEM_SWITCH_TO_NTN:
        err = modem_service_switch(MODEM_LOGIC_ACCESS_NTN);
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
    
    char buf[MODEM_LOGIC_UDP_MAX_PAYLOAD_LEN];

    if (!modem_logic_udp_payload_len_valid(c->payload_len)) {
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

int modem_service_conn_eval_get(struct lte_lc_conn_eval_params *params)
{
    int err;

    if (params == NULL) {
        return -EINVAL;
    }

    err = lte_lc_conn_eval_params_get(params);
    if (err) {
        LOG_WRN("conn eval get failed: %d", err);
        return err;
    }

    LOG_INF("conn eval: rrc=%d energy=%d tau=%d ce=%d",
            params->rrc_state, params->energy_estimate,
            params->tau_trig, params->ce_level);
    LOG_INF("conn eval: rsrp=%d rsrq=%d snr=%d dl_pl=%d tx_pwr=%d tx_rep=%d rx_rep=%d",
            params->rsrp, params->rsrq, params->snr, params->dl_pathloss,
            params->tx_power, params->tx_rep, params->rx_rep);
    LOG_INF("conn eval: earfcn=%d band=%d phy_cid=%d cell_id=%u mcc=%d mnc=%d",
            params->earfcn, params->band, params->phy_cid, params->cell_id,
            params->mcc, params->mnc);

    return err;
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
