#include "location_service.h"
#include "cloud_service.h"
#include "app_events.h"

#if defined(CONFIG_APP_FIELD_LOG)
#include "field_log.h"
#endif

#include <modem/location.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(location_service, LOG_LEVEL_INF);

static bool location_initialized; 

static int publish_lte_loc_ok(const struct location_data *location)
{
    struct app_event ev = {
        .type = EVT_LTE_LOC_OK,
    };

    ev.pvt.latitude = location->latitude;
    ev.pvt.longitude = location->longitude; 
    ev.pvt.altitude = 0.0f;

    return app_event_put(&ev, K_NO_WAIT);
}

static int publish_lte_loc_fail(void)
{
    struct app_event ev = {
        .type = EVT_LTE_LOC_FAIL,
    };
    return app_event_put(&ev, K_NO_WAIT);
}

static int publish_lte_loc_timeout(void)
{
    struct app_event ev = {
        .type = EVT_LTE_LOC_TIMEOUT,
    };
    return app_event_put(&ev, K_NO_WAIT); 
}


static void location_event_handler(const struct location_event_data *event_data)
{
    int err;

    if (event_data == NULL) {
        LOG_ERR("location_event_handler: NULL event_data");
        return;
    }

    switch (event_data->id) {
    case LOCATION_EVT_LOCATION:
        LOG_INF("LTE location success: lat=%f lon=%f acc=%f m",
            (double)event_data->location.latitude,
            (double)event_data->location.longitude,
            (double)event_data->location.accuracy);
        err = publish_lte_loc_ok(&event_data->location); 
        LOG_INF("Published EVT_LTE_LOC_OK err=%d", err);

#if defined(CONFIG_APP_FIELD_LOG)
        field_log_note_location(FIELD_LOG_LOCATION_LTE,
                                event_data->location.latitude,
                                event_data->location.longitude,
                                event_data->location.accuracy);
#endif

        LOG_INF("location_event_handler entered, id=%d", event_data->id);
        break;

    case LOCATION_EVT_TIMEOUT:
        LOG_WRN("LTE location timeout");

        err = publish_lte_loc_timeout();
        LOG_INF("Published EVT_LTE_LOC_TIMEOUT err=%d", err);
        break;

    case LOCATION_EVT_ERROR:
        LOG_ERR("LTE location failed");

        err = publish_lte_loc_fail();
        LOG_INF("Published EVT_LTE_LOC_FAIL err=%d", err);
        break;

    default:
        LOG_WRN("Unhandled location event id=%d", event_data->id);
        break;
    }
}

int location_service_init(void)
{
    int err; 

    if (location_initialized){
        return 0;
    }

    err = location_init(location_event_handler); 
    if (err){
        LOG_ERR("location_init fialed, err=%d", err); 
        return err; 
    }

    location_initialized = true;

    LOG_INF("Location service initialized"); 
    return 0; 
}

int location_service_start_lte_location(void)
{
    int err;
    static struct location_config config;
    static enum location_method methods[] = { LOCATION_METHOD_CELLULAR };

    if (!cloud_service_is_connected()){
        LOG_ERR("Cannot start cloud cellular location: cloud not connected");
        return -ENOTCONN; 
    }

    memset(&config, 0, sizeof(config));

    LOG_INF("Preparing LTE location config");

    location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);

    config.methods[0].cellular.timeout = 40 * MSEC_PER_SEC;

    LOG_INF("Starting LTE location request");


    LOG_INF("About to call location_request");

    err = location_request(&config);

    LOG_INF("location_request returned err=%d", err);

    if (err) {
        LOG_ERR("location_request failed, err=%d", err);
        return err;
    }

    LOG_INF("LTE location request started successfully");

    return 0;
}

int location_service_stop(void)
{
    int err = location_request_cancel();

    if (err) {
        LOG_WRN("location_request_cancel failed, err=%d", err);
        return err;
    }

    LOG_INF("Location service stopped");
    return 0;
}
