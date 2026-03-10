#include "gnss.h"

#include "app_zbus.h"

#include <dk_buttons_and_leds.h>
#include <modem/lte_lc.h>
#include <nrf_modem_gnss.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(gnss, LOG_LEVEL_INF);

static struct nrf_modem_gnss_pvt_data_frame pvt_data;
static int64_t gnss_start_time;
static bool first_fix;

static int publish_error(int err)
{
	(void)app_zbus_publish_gnss_status(APP_GNSS_STATE_ERROR, err, -1, 0.0, 0.0, 0);

	return err;
}

static uint8_t count_tracked_satellites(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	uint8_t count = 0;

	for (size_t i = 0; i < ARRAY_SIZE(pvt->sv); ++i) {
		if (pvt->sv[i].signal != 0U) {
			count++;
		}
	}

	return count;
}

static void print_fix_data(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	LOG_INF("Latitude: %.06f", 
		pvt->latitude);
	LOG_INF("Longitude: %.06f", 
		pvt->longitude);
	LOG_INF("Altitude: %.01f m", 
		(double)pvt->altitude);
}

static void gnss_event_handler(int event)
{
	int err;

	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT: {
		uint8_t satellites;
		int64_t ttff_ms = -1;

		err = nrf_modem_gnss_read(&pvt_data, sizeof(pvt_data), NRF_MODEM_GNSS_DATA_PVT);
		if (err) {
			LOG_ERR("nrf_modem_gnss_read failed, err: %d", 
				err);
			(void)publish_error(err);
			return;
		}

		satellites = count_tracked_satellites(&pvt_data);
		LOG_INF("GNSS search active, satellites tracked: %u", 
			satellites);

		if ((pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) == 0U) {
			return;
		}

		if (!first_fix) {
			ttff_ms = k_uptime_get() - gnss_start_time;
			first_fix = true;
			LOG_INF("Time to first fix: %lld s", 
				(long long)(ttff_ms / 1000));
		}

		print_fix_data(&pvt_data);
		(void)dk_set_led_on(DK_LED1);
		(void)app_zbus_publish_gnss_status(APP_GNSS_STATE_FIX, 0, ttff_ms,
						   pvt_data.latitude, pvt_data.longitude,
						   satellites);
		break;
	}

	case NRF_MODEM_GNSS_EVT_PERIODIC_WAKEUP:
		LOG_INF("GNSS woke up");
		break;

	case NRF_MODEM_GNSS_EVT_SLEEP_AFTER_FIX:
		LOG_INF("GNSS entered sleep after fix");
		break;

	default:
		break;
	}
}

int gnss_init(void)
{
	int err;

	first_fix = false;
	gnss_start_time = 0;

	err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
	if (err) {
		LOG_ERR("nrf_modem_gnss_event_handler_set failed, err: %d", 
			err);
		return publish_error(err);
	}

	err = nrf_modem_gnss_fix_interval_set(CONFIG_GNSS_PERIODIC_INTERVAL);
	if (err) {
		LOG_ERR("nrf_modem_gnss_fix_interval_set failed, err: %d", 
			err);
		return publish_error(err);
	}

	err = nrf_modem_gnss_fix_retry_set(CONFIG_GNSS_PERIODIC_TIMEOUT);
	if (err) {
		LOG_ERR("nrf_modem_gnss_fix_retry_set failed, err: %d", 
			err);
		return publish_error(err);
	}

	(void)app_zbus_publish_gnss_status(APP_GNSS_STATE_INITIALIZED, 0, -1, 0.0, 0.0, 0);
	return 0;
}

int gnss_start(void)
{
	int err;

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_NORMAL);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set failed, err: %d", 
			err);
		return publish_error(err);
	}

	LOG_INF("Starting GNSS");
	err = nrf_modem_gnss_start();
	if (err) {
		LOG_ERR("nrf_modem_gnss_start failed, err: %d", 
			err);
		return publish_error(err);
	}

	gnss_start_time = k_uptime_get();
	(void)app_zbus_publish_gnss_status(APP_GNSS_STATE_STARTED, 0, -1, 0.0, 0.0, 0);
	return 0;
}
