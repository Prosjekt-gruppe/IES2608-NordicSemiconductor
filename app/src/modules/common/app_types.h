/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <nrf_modem_gnss.h>
#include <zephyr/smf.h>

enum rat {
    RAT_LTEM = 0,
    RAT_NTN = 1,
    RAT_UNKNOWN = 255
};

struct retry_state {
    uint8_t ltem_attempts;
    uint8_t ntn_attempts;
};

/*
 * These values are laid out like the Zephyr SMF tree. The indented states are
 * children of the parent state above them, and child states can pass events up
 * to the parent with SMF_EVENT_PROPAGATE.
 */
enum app_state {
    STATE_BOOT,
    STATE_RUNNING,

        STATE_DISCONNECTED,
            STATE_BACKOFF,
            STATE_LTEM_CONNECTING,
            STATE_NTN_CONNECTING,

        STATE_CONNECTED,
            STATE_LTEM_CONNECTED,
            STATE_CLOUD_CONNECTING,
            STATE_LTE_LOCATION,
            STATE_GNSS_ACQUIRE,
            STATE_NTN_CONNECTED,
            STATE_LTE_PROBE,
            STATE_IDLE,
};


enum app_evt_type {
    EVT_BOOT,
    EVT_REG_OK,
    EVT_REG_FAIL,
    EVT_START_CLOUD,
    EVT_START_LTE_LOC,
    EVT_START_GNSS,
    EVT_GNSS_FIX,
    EVT_GNSS_TIMEOUT,
    EVT_TIMEOUT,
    EVT_RSRP_UPDATE,
    EVT_LTE_POOR,
    EVT_LTE_GOOD,
    EVT_LTE_LOC_OK,
    EVT_LTE_LOC_FAIL,
    EVT_LTE_LOC_TIMEOUT,
    EVT_CLOUD_OK,
    EVT_CLOUD_FAIL,
    EVT_CLOUD_DISCONNECTED,
    EVT_BACKOFF_TIMEOUT,
    EVT_PDN_UP,
    EVT_PDN_DOWN,
    EVT_MODEM_SWITCH_FAIL,
    EVT_MODEM_SWITCH_CMD_OK,
    EVT_TN_READY_FOR_PROBE,
};

/*
 * LTE-M can run a small chain of optional steps. This enum prevents the state
 * machine from immediately repeating the same step when it returns to LTE-M.
 */
enum app_step_done {
    STEP_NONE = 0,
    STEP_CLOUD_DONE,
    STEP_LTE_LOC_DONE,
    STEP_GNSS_DONE,
};

enum gnss_goal { 
    GNSS_GOAL_NONE = 0,
    GNSS_GOAL_REFINE_LTE_FIX,
    GNSS_GOAL_REQUIRED_FOR_NTN,
};


struct app_event {
    enum app_evt_type type;
    enum rat source_rat;
    union {
        struct nrf_modem_gnss_pvt_data_frame pvt;
        struct { enum rat rat; } reg;
        struct { int rsrp_dbm; } meas;
    };
};


struct app_ctx {
    struct smf_ctx ctx;
    enum app_state state;
    
    /* Current RAT and the RAT we will try after the next backoff. */
    enum rat active_rat;
    enum rat next_rat;

    struct retry_state retry;
    
    /* Latest LTE-M signal value used by fallback decisions. */
    int rsrp_dbm;
    int backoff_ms;
    
    /* Cloud state mirrored here so SMF can decide what step comes next. */
    bool cloud_connected; 

    /* Last GNSS fix. NTN needs this before the modem can attach. */
    bool have_fix;
    int64_t last_fix_uptime_ms;
    struct nrf_modem_gnss_pvt_data_frame last_pvt;
    enum gnss_goal gnss_goal; 
    int32_t gnss_timeout_sec; 
    bool gnss_extend_once; 
    
    /* NTN setup state. */
    bool ntn_initialized;
    
    /* Copy of the zbus event currently being handled by SMF. */
    struct app_event ev;

    /* LTE service state mirrored in the state machine thread. */
   	bool lte_connected;



    /* Last completed step in the LTE-M cloud/location/GNSS chain. */
    enum app_step_done last_done; 

    /*
     * Timers publish EVT_TIMEOUT or delayed events back to zbus instead of
     * calling SMF directly.
     */
    struct k_timer backoff_timer;
    struct k_timer ntn_timer;
    struct k_timer ntn_connect_timer;
    struct k_timer lte_timer;

    struct k_timer handoff_timer;
    enum app_evt_type delayed_event; 

    /* Packet data network state from LTE/NTN callbacks. */
    bool pdn_up;
    
};
