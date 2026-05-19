#include "cloud_service.h"
#include "gnss_service.h"
#include "app_events.h"

#include <net/nrf_cloud_agnss.h>
#include <net/nrf_cloud.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(cloud_service, LOG_LEVEL_INF);

static bool cloud_initialized; 
static bool cloud_connected;
static bool cloud_connecting; 

static int publish_cloud_ok(void)
{
    struct app_event ev = {
        .type = EVT_CLOUD_OK,
    };
    return app_event_put(&ev, K_NO_WAIT); 
}

static int publish_cloud_fail(void)
{
    struct app_event ev = {
        .type = EVT_CLOUD_FAIL,
    };
    return app_event_put(&ev, K_NO_WAIT); 
}

static int publish_cloud_disconnected(void)
{
    struct app_event ev = {
        .type = EVT_CLOUD_DISCONNECTED,
    };
    return app_event_put(&ev, K_NO_WAIT); 
}

static void cloud_event_handler(const struct nrf_cloud_evt *evt)
{

    if (evt == NULL){
        LOG_ERR("cloud_event_handler got NUll evt");
        return;
    }

    switch (evt->type){
        case NRF_CLOUD_EVT_TRANSPORT_CONNECTING: 
            LOG_INF("Cloud transport connecting");
            break;

        case NRF_CLOUD_EVT_TRANSPORT_CONNECTED: 
            LOG_INF("Cloud transport connected");
            break; 

        case NRF_CLOUD_EVT_USER_ASSOCIATED:
            LOG_INF("Cloud user associated");
            break;

        case NRF_CLOUD_EVT_READY: 
            cloud_connecting = false;
            cloud_connected = true; 
            LOG_INF("Cloud ready");
            (void)publish_cloud_ok();
            break; 

        case NRF_CLOUD_EVT_TRANSPORT_DISCONNECTED:
            cloud_connecting = false; 
            cloud_connected = false; 
            LOG_WRN("Cloud transport disconnected");
            (void)publish_cloud_disconnected();
            break; 

        case NRF_CLOUD_EVT_ERROR:
            cloud_connecting = false; 
            cloud_connected = false; 
            LOG_ERR("Cloud error");
            (void)publish_cloud_fail();
            break; 
        case NRF_CLOUD_EVT_RX_DATA_GENERAL:
            LOG_INF("Cloud RX data (general), len=%d", evt->data.len);

            if (evt->data.ptr && evt->data.len > 0) {

                int err = nrf_cloud_agnss_process(evt->data.ptr, evt->data.len);
                LOG_INF("nrf_cloud_agnss_process() -> %d", err); 


                if (!err) {
                    LOG_INF("A-GNSS data processed and injected");
                    gnss_notify_agnss_ready(); 
                    
                } else {
                    LOG_ERR("A-GNSS processing failed: %d", err);
                }
            }
            break;

        case NRF_CLOUD_EVT_RX_DATA_SHADOW:
            LOG_INF("Cloud shadow data received");
            break;

        default:
            LOG_WRN("Unhandled cloud event: %d", evt->type); 
            break;
    }
}

int cloud_service_init(void)
{
    int err;
    struct nrf_cloud_init_param init_param = {0};

    if (cloud_initialized){
        return 0;
    }

    cloud_connected = false; 
    cloud_connecting = false; 

    init_param.event_handler = cloud_event_handler; 


    /*
     * nRF Cloud uses the same callback for connection state and incoming
     * A-GNSS data, so the handler also notifies GNSS when assistance is ready.
     */
    err = nrf_cloud_init(&init_param);
    if (err){
        LOG_ERR("nrf_cloud_init failed %d", err);
        return err; 
    }

    cloud_initialized = true; 
    LOG_INF("Cloud service initialized"); 
    return 0;
}


int cloud_service_connect_async(void)
{
    int err; 

    if (!cloud_initialized){
        LOG_ERR("Cloud service not initalized");
        return -EINVAL;
    }

    if (cloud_connected){
        LOG_INF("Cloud already connected");
        return 0;
    }

    if (cloud_connecting){
        LOG_INF("Cloud connection already in progress");
        return 0; 
    }

    cloud_connecting = true;

    err = nrf_cloud_connect();
    if (err){
        cloud_connecting = false; 
        cloud_connected = false; 
        LOG_ERR("nrf_cloud_connect failed: %d", err);
        return err; 
    }
    LOG_INF("Cloud connection started");
    return 0;
}


int cloud_service_disconnect(void){

    int err; 

    if (!cloud_initialized) {
        return -EINVAL;
    }
    if (!cloud_connected && !cloud_connecting){
        LOG_INF("Cloud already disconnected");
        return 0;
    }

    err = nrf_cloud_disconnect();
    if (err){
        LOG_ERR("nrf_cloud_disconnect failed: %d", err); 
        return err;
    }

    cloud_connected = false;
    cloud_connecting = false; 

    LOG_INF("Cloud disconnected");
    return 0; 
}

bool cloud_service_is_connected(void)
{
    return cloud_connected; 
}
