#include "ltem.h"

#include "app_zbus.h"

#include <dk_buttons_and_leds.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ltem, LOG_LEVEL_INF);

static int publish_error(int err)
{
	(void)app_zbus_publish_ltem_status(APP_LTEM_STATE_ERROR, err);

	return err;
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		    (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			break;
		}

		LOG_INF("LTE-M connected to %s network",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
				"home" :
				"roaming");
		(void)dk_set_led_on(DK_LED2);
		(void)app_zbus_publish_ltem_status(APP_LTEM_STATE_CONNECTED, 0);
		break;

	case LTE_LC_EVT_RRC_UPDATE:
		LOG_INF("RRC mode: %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "connected" : "idle");
		break;

	default:
		break;
	}
}

int ltem_init(void)
{
	int err;

	LOG_INF("Initializing modem library");
	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init failed, err: %d", 
			err);
		return publish_error(err);
	}

	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_GPS,
				     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("lte_lc_system_mode_set failed, err: %d", 
			err);
		return publish_error(err);
	}

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_NORMAL);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set failed, err: %d", 
			err);
		return publish_error(err);
	}

	(void)app_zbus_publish_ltem_status(APP_LTEM_STATE_INITIALIZED, 0);
	return 0;
}

int ltem_connect(void)
{
	int err;

	LOG_INF("Starting LTE-M connection");
	(void)app_zbus_publish_ltem_status(APP_LTEM_STATE_CONNECTING, 0);

	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async failed, err: %d", 
			err);
		return publish_error(err);
	}

	return 0;
}
