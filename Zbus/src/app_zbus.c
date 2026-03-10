#include "app_zbus.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_zbus, LOG_LEVEL_INF);

ZBUS_CHAN_DEFINE(ltem_status_chan,
		 struct app_ltem_status,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(app_fsm_sub),
		 ZBUS_MSG_INIT(.state = APP_LTEM_STATE_OFF, .err = 0));

ZBUS_CHAN_DEFINE(gnss_status_chan,
		 struct app_gnss_status,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(app_fsm_sub),
		 ZBUS_MSG_INIT(.state = APP_GNSS_STATE_IDLE,
			       .err = 0,
			       .time_to_first_fix_ms = -1,
			       .latitude = 0.0,
			       .longitude = 0.0,
			       .tracked_satellites = 0));

static int publish_status(const struct zbus_channel *chan, const void *msg)
{
	int err = zbus_chan_pub(chan, msg, K_NO_WAIT);

	if (err) {
		LOG_WRN("Failed to publish %s, err: %d", 
			zbus_chan_name(chan), 
			err);
	}

	return err;
}

int app_zbus_publish_ltem_status(enum app_ltem_state state, int err)
{
	const struct app_ltem_status msg = {
		.state = state,
		.err = err,
	};

	return publish_status(&ltem_status_chan, &msg);
}

int app_zbus_publish_gnss_status(enum app_gnss_state state, int err,
				 int64_t time_to_first_fix_ms, double latitude,
				 double longitude, uint8_t tracked_satellites)
{
	const struct app_gnss_status msg = {
		.state = state,
		.err = err,
		.time_to_first_fix_ms = time_to_first_fix_ms,
		.latitude = latitude,
		.longitude = longitude,
		.tracked_satellites = tracked_satellites,
	};

	return publish_status(&gnss_status_chan, &msg);
}
