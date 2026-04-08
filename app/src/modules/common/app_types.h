#pragma once

#include <stdbool.h>
#include <nrf_modem_gnss.h>
#include <zephyr/smf.h>
#include <zephyr/kernel.h>

enum rat {
    RAT_LTEM,
    RAT_NTN
};

enum app_state {
    STATE_BOOT,
    STATE_IDLE,

    STATE_LTEM_CONNECTING,
    STATE_LTEM_CONNECTED,
    STATE_LTE_LOCATION,

    STATE_GNSS_REFINE,

    STATE_NTN_CONNECTING,
    STATE_NTN_CONNECTED,

    STATE_BACKOFF,
};

enum app_evt_type {

    /* Boot */
    EVT_BOOT,

    /* Registration */
    EVT_REG_OK,
    EVT_REG_FAIL,

    /* PDN */
    EVT_PDN_UP,
    EVT_PDN_DOWN,

    /* LTE signal */
    EVT_RSRP_UPDATE,
    EVT_LTE_POOR,

    /* LTE location */
    EVT_LTE_LOC_OK,
    EVT_LTE_LOC_FAIL,
    EVT_LTE_LOC_TIMEOUT,

    /* GNSS */
    EVT_GNSS_FIX,
    EVT_GNSS_TIMEOUT,

    /* A-GNSS */
    EVT_AGNSS_REQUEST,
    EVT_AGNSS_READY,
    EVT_AGNSS_FAIL,

    /* NTN */
    EVT_NTN_PREPARE_DONE,
    EVT_NTN_TIMEOUT,
    EVT_NTN_REG_FAIL,

    /* Generic timeout */
    EVT_TIMEOUT,

    /* Backoff */
    EVT_BACKOFF_TIMEOUT,
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

    /* RAT overview */
    enum rat active_rat;
    enum rat next_rat;

    /* Timers */
    struct k_timer backoff_timer;

    /* PDN */
    bool pdn_up;

    /* LTE / signal */
    bool lte_connected;
    int rsrp_dbm;
    int backoff_ms;

    /* LTE location */
    bool lte_loc_requested;
    bool lte_fix;
    struct nrf_modem_gnss_pvt_data_frame lte_pvt;

    /* GNSS refine */
    bool agnss_requested;
    bool gnss_fix;
    struct nrf_modem_gnss_pvt_data_frame gnss_pvt;

    /* NTN / final selected position */
    bool ntn_initialized;
    bool final_fix;
    struct nrf_modem_gnss_pvt_data_frame final_pvt;

    /* Current event */
    struct app_event ev;
};