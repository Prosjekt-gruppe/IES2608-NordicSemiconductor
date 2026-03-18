#include "app_zbus.h"
#include "gnss.h"
#include "ltem.h"

#include <dk_buttons_and_leds.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

LOG_MODULE_REGISTER(app_main, LOG_LEVEL_INF);

ZBUS_SUBSCRIBER_DEFINE(app_fsm_sub, 8);

enum app_state {
	APP_STATE_INIT = 0,
	APP_STATE_WAIT_LTEM,
	APP_STATE_START_GNSS,
	APP_STATE_WAIT_GNSS_FIX,
	APP_STATE_RUNNING,
	APP_STATE_ERROR,
};

static enum app_state current_state = APP_STATE_INIT;

static const char *app_state_name(enum app_state state)
{
	switch (state) {
	case APP_STATE_INIT:
		return "INIT";
	case APP_STATE_WAIT_LTEM:
		return "WAIT_LTEM";
	case APP_STATE_START_GNSS:
		return "START_GNSS";
	case APP_STATE_WAIT_GNSS_FIX:
		return "WAIT_GNSS_FIX";
	case APP_STATE_RUNNING:
		return "RUNNING";
	case APP_STATE_ERROR:
		return "ERROR";
	default:
		return "UNKNOWN";
	}
}

static void set_state(enum app_state next_state)
{
	if (current_state == next_state) {
		return;
	}

	LOG_INF("FSM transition: %s -> %s", 
		app_state_name(current_state),
		app_state_name(next_state));
	current_state = next_state;
}

static void enter_error_state(const char *reason, int err)
{
	LOG_ERR("%s, err: %d", 
		reason, 
		err);
	set_state(APP_STATE_ERROR);
}

static void handle_ltem_status(const struct app_ltem_status *status)
{
	int err;

	switch (status->state) {
	case APP_LTEM_STATE_CONNECTED:
		if (current_state != APP_STATE_WAIT_LTEM) {
			break;
		}

		set_state(APP_STATE_START_GNSS);
		err = gnss_start();
		if (err) {
			enter_error_state("Failed to start GNSS", err);
			return;
		}

		set_state(APP_STATE_WAIT_GNSS_FIX);
		break;

	case APP_LTEM_STATE_ERROR:
		enter_error_state("LTE-M module reported an error", status->err);
		break;

	default:
		LOG_INF("LTE-M status update: %d", 
			status->state);
		break;
	}
}

static void handle_gnss_status(const struct app_gnss_status *status)
{
	switch (status->state) {
	case APP_GNSS_STATE_FIX:
		if (current_state == APP_STATE_WAIT_GNSS_FIX) {
			set_state(APP_STATE_RUNNING);
		}

		LOG_INF("GNSS fix ready: lat=%.06f lon=%.06f satellites=%u",
			status->latitude, 
			status->longitude, 
			status->tracked_satellites);

		if (status->time_to_first_fix_ms >= 0) {
			LOG_INF("First fix acquired in %lld ms",
				(long long)status->time_to_first_fix_ms);
		}
		break;

	case APP_GNSS_STATE_ERROR:
		enter_error_state("GNSS module reported an error", status->err);
		break;

	default:
		LOG_INF("GNSS status update: %d", 
			status->state);
		break;
	}
}

int main(void)
{
	int err;
	const struct zbus_channel *chan;

	if (dk_leds_init() != 0) {
		LOG_WRN("Failed to initialize the LED library");
	}

	set_state(APP_STATE_INIT);

	err = ltem_init();
	if (err) {
		enter_error_state("Failed to initialize LTE-M module", err);
	}

	if (current_state != APP_STATE_ERROR) {
		err = gnss_init();
		if (err) {
			enter_error_state("Failed to initialize GNSS module", err);
		}
	}

	if (current_state != APP_STATE_ERROR) {
		set_state(APP_STATE_WAIT_LTEM);
		err = ltem_connect();
		if (err) {
			enter_error_state("Failed to start LTE-M connection", err);
		}
	}

	while (1) {
		if (current_state == APP_STATE_ERROR) {
			k_sleep(K_FOREVER);
		}

		err = zbus_sub_wait(&app_fsm_sub, &chan, K_FOREVER);
		if (err) {
			LOG_WRN("zbus_sub_wait failed, err: %d", 
				err);
			continue;
		}

		if (chan == &ltem_status_chan) {
			struct app_ltem_status status;

			err = zbus_chan_read(chan, &status, K_MSEC(50));
			if (err) {
				LOG_WRN("Failed to read LTE-M status, err: %d", 
					err);
				continue;
			}

			handle_ltem_status(&status);
			continue;
		}

		if (chan == &gnss_status_chan) {
			struct app_gnss_status status;

			err = zbus_chan_read(chan, &status, K_MSEC(50));
			if (err) {
				LOG_WRN("Failed to read GNSS status, err: %d", 
					err);
				continue;
			}

			handle_gnss_status(&status);
			continue;
		}

		LOG_WRN("Received notification from unexpected channel: %s",
			zbus_chan_name(chan));
	}
}
