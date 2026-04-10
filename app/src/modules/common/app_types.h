/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once

#include <stdbool.h>
#include <nrf_modem_gnss.h>
#include <zephyr/smf.h>

enum rat {
    RAT_LTEM,
    RAT_NTN
};


enum app_state {
    STATE_BOOT,
    STATE_IDLE,
    STATE_GNSS_ACQUIRE,
    STATE_NTN_CONNECTING,
    STATE_NTN_CONNECTED,
    STATE_LTEM_CONNECTING,
    STATE_LTEM_CONNECTED,
<<<<<<< HEAD
    STATE_LTE_LOCATION,
=======
>>>>>>> origin/main
    STATE_LTE_PROBE,
    STATE_BACKOFF,
};


enum app_evt_type {
    EVT_BOOT,
    EVT_REG_OK,
    EVT_REG_FAIL,
    EVT_GNSS_FIX,
    EVT_GNSS_TIMEOUT,
    EVT_NTN_REG_FAIL,
    EVT_NTN_TIMEOUT,
    EVT_TIMEOUT,
    EVT_RSRP_UPDATE,
    EVT_LTE_POOR,
    EVT_LTE_GOOD,
<<<<<<< HEAD
    EVT_LTE_LOC_OK,
    EVT_LTE_LOC_FAIL,
    EVT_LTE_LOC_TIMEOUT,
=======
>>>>>>> origin/main
    EVT_BACKOFF_TIMEOUT,
    EVT_PDN_UP,
    EVT_PDN_DOWN,
};

struct app_event {
    enum app_evt_type type;
    union {
        struct nrf_modem_gnss_pvt_data_frame pvt;
        struct { enum rat rat; } reg;
        struct { int rsrp_dbm; } meas;
    };
};

struct app_ctx {
    struct smf_ctx ctx;
    
    /* rat overview */
    enum rat active_rat;
    enum rat next_rat;
    
    
    /* ltem signal strength*/
    int rsrp_dbm;
    int backoff_ms;
    
    /* gnss */
    bool have_fix;
    struct nrf_modem_gnss_pvt_data_frame last_pvt;
    
    /* ntn */
    bool ntn_initialized;
    
    /* events */
    struct app_event ev;

    /* lte */
   	bool lte_connected;

    /* timers */
    struct k_timer backoff_timer;
    struct k_timer ntn_timer;
    struct k_timer lte_timer;

    /* pdn */
    bool pdn_up;
    
};

/*
struct monitor_event {
    enum app_state state;
    struct app_event ev;
    int rsrp_dbm;
};
*/
