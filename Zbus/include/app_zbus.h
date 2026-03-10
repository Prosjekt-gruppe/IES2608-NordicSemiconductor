/*
 * Shared Zbus channels and message types used by the LTE-M and GNSS modules.
 */

#ifndef APP_ZBUS_H_
#define APP_ZBUS_H_

#include <stdint.h>

#include <zephyr/zbus/zbus.h>

enum app_ltem_state {
	APP_LTEM_STATE_OFF = 0,
	APP_LTEM_STATE_INITIALIZED,
	APP_LTEM_STATE_CONNECTING,
	APP_LTEM_STATE_CONNECTED,
	APP_LTEM_STATE_ERROR,
};

struct app_ltem_status {
	enum app_ltem_state state;
	int err;
};

enum app_gnss_state {
	APP_GNSS_STATE_IDLE = 0,
	APP_GNSS_STATE_INITIALIZED,
	APP_GNSS_STATE_STARTED,
	APP_GNSS_STATE_FIX,
	APP_GNSS_STATE_ERROR,
};

struct app_gnss_status {
	enum app_gnss_state state;
	int err;
	int64_t time_to_first_fix_ms;
	double latitude;
	double longitude;
	uint8_t tracked_satellites;
};

ZBUS_CHAN_DECLARE(ltem_status_chan);
ZBUS_CHAN_DECLARE(gnss_status_chan);

int app_zbus_publish_ltem_status(enum app_ltem_state state, int err);
int app_zbus_publish_gnss_status(enum app_gnss_state state, int err,
				 int64_t time_to_first_fix_ms, double latitude,
				 double longitude, uint8_t tracked_satellites);

#endif /* APP_ZBUS_H_ */
