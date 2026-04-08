#include "location_service.h"
#include "app_events.h"

#include <modem/location.h>
#include <net/nrf_cloud_coap.h>

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(location_service, LOG_LEVEL_INF);

static bool location_initialized;
static struct k_work agnss_work;


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

static int publish_agnss_ready(void)
{
    struct app_event ev = {
        .type = EVT_AGNSS_READY,
    };

    return app_event_put(&ev, K_NO_WAIT);
}

static int publish_agnss_fail(void)
{
    struct app_event ev = {
        .type = EVT_AGNSS_FAIL,
    };

    return app_event_put(&ev, K_NO_WAIT);
}

static void location_event_handler(const struct location_event_data *event_data)
{
    int err;

    switch (event_data->id)
    {
    case LOCATION_EVT_LOCATION:
        LOG_INF("LTE location success: lat=%f lon=%f",
                (double)event_data->location.latitude,
                (double)event_data->location.longitude);

        err = publish_lte_loc_ok(&event_data->location);
        LOG_INF("Published EVT_LTE_LOC_OK err=%d", err);
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

    case LOCATION_EVT_GNSS_ASSISTANCE_REQUEST:
        LOG_INF("Location library reported GNSS assistance request");
        break;

    default:
        LOG_WRN("Unhandled location event id=%d", event_data->id);
        break;
    }
}

/*
 * TODO:
 * 1. Download A-GNSS data over LTE / nRF Cloud
 * 2. Inject assistance into modem
 * 3. Set err if something fails
 */
#if defined(CONFIG_NRF_CLOUD_AGNSS) && CONFIG_NRF_CLOUD_AGNSS
static void agnss_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    int err;
    uint8_t buf[4096];

    struct nrf_cloud_rest_agnss_request req = {0};
    struct nrf_cloud_rest_agnss_result result = {
        .buf = buf,
        .buf_sz = sizeof(buf),
    };

    LOG_INF("Requesting A-GNSS data from nRF Cloud over CoAP");

    err = nrf_cloud_coap_agnss_data_get(&req, &result);
    if (err) {
        LOG_ERR("nrf_cloud_coap_agnss_data_get failed, err=%d", err);
        (void)publish_agnss_fail();
        return;
    }

    err = location_agnss_data_process(result.buf, result.buf_sz);
    if (err) {
        LOG_ERR("location_agnss_data_process failed, err=%d", err);
        (void)publish_agnss_fail();
        return;
    }

    LOG_INF("A-GNSS download + injection complete");
    (void)publish_agnss_ready();
}
#else
static void agnss_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    LOG_WRN("A-GNSS work called, but NRF_CLOUD_AGNSS is disabled");
    (void)publish_agnss_fail();
}
#endif

int location_service_init(void)
{
    int err;

    if (location_initialized)
    {
        return 0;
    }

    err = location_init(location_event_handler);
    if (err)
    {
        LOG_ERR("location_init failed, err=%d", err);
        return err;
    }

    k_work_init(&agnss_work, agnss_work_handler);

    location_initialized = true;

    LOG_INF("Location service initialized");
    return 0;
}

int location_service_start_lte_location(void)
{
    int err;
    struct location_config config;
    enum location_method methods[] = {LOCATION_METHOD_CELLULAR};

    location_config_defaults_set(&config, ARRAY_SIZE(methods), methods);

    config.methods[0].cellular.timeout = 40 * MSEC_PER_SEC;

    LOG_INF("Starting LTE location request");

    err = location_request(&config);
    if (err)
    {
        LOG_ERR("location_request failed, err=%d", err);
        return err;
    }

    return 0;
}

int location_service_start_agnss(void)
{
#if defined(CONFIG_NRF_CLOUD_AGNSS) && CONFIG_NRF_CLOUD_AGNSS
    LOG_INF("Scheduling A-GNSS assist work");
    k_work_submit(&agnss_work);
    return 0;
#else
    LOG_WRN("A-GNSS disabled in current build");
    return -ENOTSUP;
#endif
}

int location_service_stop(void)
{
    int err = location_request_cancel();

    if (err)
    {
        LOG_WRN("location_request_cancel failed, err=%d", err);
        return err;
    }

    LOG_INF("Location service stopped");
    return 0;
}