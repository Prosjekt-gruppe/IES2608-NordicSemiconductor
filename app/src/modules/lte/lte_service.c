/*
 * LTE_Service : LTE-specific behavior on top of the modem
 */ 


 #include "lte_service.h"
 #include "app_events.h"

 #include <modem/lte_lc.h>
 #include <nrf_modem_at.h>
 #include <zephyr/kernel.h>
 #include <zephyr/logging/log.h>


LOG_MODULE_REGISTER(lte_service, LOG_LEVEL_INF);

static bool lte_connected; 

static int publish_evt(enum app_evt_type type)
{
    struct app_event ev = {
        .type = type,
    };

    LOG_INF("Publishing %s", app_evt_name(type));
    return app_event_put(&ev, K_NO_WAIT); 
}

static int publish_rsrp_evt(int rsrp_dbm)
{
    struct app_event ev = {
        .type = EVT_RSRP_UPDATE,
    };
    ev.meas.rsrp_dbm = rsrp_dbm; 
    return app_event_put(&ev, K_NO_WAIT); 
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
        LOG_INF("LTE NW registration status: %d", evt->nw_reg_status);


        switch (evt->nw_reg_status) {
        case LTE_LC_NW_REG_REGISTERED_HOME:
        case LTE_LC_NW_REG_REGISTERED_ROAMING:
            lte_connected=true;
            LOG_INF("LTE registered on network");
            (void)publish_evt(EVT_REG_OK);
            break;

        case LTE_LC_NW_REG_NOT_REGISTERED:
        case LTE_LC_NW_REG_REGISTRATION_DENIED:
        case LTE_LC_NW_REG_UNKNOWN:
        case LTE_LC_NW_REG_UICC_FAIL:
            lte_connected=false;
            LOG_WRN("LTE registration failed/status=%d", evt->nw_reg_status);
            (void)publish_evt(EVT_REG_FAIL);
            break;

        default:
            break;
        }
        break;

    case LTE_LC_EVT_CELLULAR_PROFILE_ACTIVE:
        LOG_INF("modem activate cellular profile (RAT starting)");
        break;

    case LTE_LC_EVT_LTE_MODE_UPDATE:
        LOG_INF("LTE mode update %d", evt->lte_mode);
        break;

    default:
        LOG_DBG("Unhandled LTE evnt type: %d", evt->type);
        break;
    }
}

int lte_service_init(void){
    lte_connected = false; 
    return 0; 
}

int lte_service_connect_async(void)
{
    return lte_lc_connect_async(lte_lc_evt_handler);   
}

int lte_service_disconnect(void){
    lte_connected = false; 
    return lte_lc_offline(); 
}

bool lte_service_is_connected(void)
{
    return lte_connected; 
}

int lte_service_get_rsrp(int *rsrp_dbm)
{
    int err;
    char buf[64];   // bigger buffer = safer
    int rxlev, ber, rscp, ecno, rsrq, rsrp_raw;

    if (rsrp_dbm == NULL) {
        return -EINVAL;
    }

    err = nrf_modem_at_cmd(buf, sizeof(buf), "AT+CESQ");
    if (err) {
        LOG_ERR("AT+CESQ failed: %d", err);
        return err;
    }

    LOG_DBG("CESQ response: %s", buf);

    int parsed = sscanf(buf,
                        "+CESQ: %d,%d,%d,%d,%d,%d",
                        &rxlev, &ber, &rscp, &ecno, &rsrq, &rsrp_raw);

    if (parsed != 6) {
        LOG_WRN("Failed to parse CESQ response");
        return -EIO;
    }

    if (rsrp_raw == 255) {
        LOG_WRN("RSRP not known");
        return -ENOENT;
    }

    if (rsrp_raw == 0) {
        LOG_WRN("RSRP < -140 dBm");
        *rsrp_dbm = -141;   // or clamp to -140 depending on your policy
        return 0;
    }

    *rsrp_dbm = rsrp_raw - 141;

    return 0;
}

int lte_service_sample_and_publish_rsrp(void)
{
    int err;
    int rsrp_dbm;

    err = lte_service_get_rsrp(&rsrp_dbm); 
    if (err)
    {
        return err; 
    }

    LOG_INF("LTE RSRP: %d dBm", rsrp_dbm); 
    return publish_rsrp_evt(rsrp_dbm);
}
