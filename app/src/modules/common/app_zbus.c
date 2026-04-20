/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_zbus.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_zbus, LOG_LEVEL_INF);

ZBUS_OBS_DECLARE(app_fsm_sub);

#if defined(CONFIG_APP_FIELD_LOG)
ZBUS_OBS_DECLARE(field_log_batt_sub);
#define APP_BATTERY_OBSERVERS ZBUS_OBSERVERS(field_log_batt_sub)
#else
#define APP_BATTERY_OBSERVERS ZBUS_OBSERVERS_EMPTY
#endif

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
			       .altitude = 0.0f,
			       .tracked_satellites = 0));

ZBUS_CHAN_DEFINE(accel_sample_chan,
		 struct app_accel_sample,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.timestamp_ms = 0,
			       .x_mg = 0,
			       .y_mg = 0,
			       .z_mg = 0,
			       .linear_x_mg = 0,
			       .linear_y_mg = 0,
			       .linear_z_mg = 0,
			       .linear_accel_mg = 0,
			       .speed_mm_s = 0,
			       .quiet_time_ms = 0,
			       .moving = false));

ZBUS_CHAN_DEFINE(battery_sample_chan,
		 struct app_battery_sample,
		 NULL,
		 NULL,
		 APP_BATTERY_OBSERVERS,
		 ZBUS_MSG_INIT(.timestamp_ms = 0,
			       .voltage_mv = 0,
			       .current_ma = 0,
			       .temp_mdegc = 0,
			       .charger_status = 0,
			       .charger_error = 0,
			       .vbus_present = false));

ZBUS_CHAN_DEFINE(environment_sample_chan,
		 struct app_environment_sample,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.timestamp_ms = 0,
			       .temp_mdegc = 0,
			       .pressure_pa = 0,
			       .humidity_milli_pct = 0,
			       .gas_ohm = 0));

static int publish_status(const struct zbus_channel *chan, const void *msg)
{
	int err = zbus_chan_pub(chan, msg, K_NO_WAIT);

	if (err) {
		LOG_WRN("Failed to publish %s, err=%d", zbus_chan_name(chan), err);
	}

	return err;
}

int app_zbus_publish_gnss_status(enum app_gnss_state state, int err,
				 int64_t time_to_first_fix_ms, double latitude,
				 double longitude, float altitude,
				 uint8_t tracked_satellites)
{
	const struct app_gnss_status msg = {
		.state = state,
		.err = err,
		.time_to_first_fix_ms = time_to_first_fix_ms,
		.latitude = latitude,
		.longitude = longitude,
		.altitude = altitude,
		.tracked_satellites = tracked_satellites,
	};

	return publish_status(&gnss_status_chan, &msg);
}

int app_zbus_publish_accel_sample(bool moving, uint32_t speed_mm_s,
				  uint32_t linear_accel_mg,
				  uint32_t quiet_time_ms,
				  const int32_t xyz_mg[3],
				  const int32_t linear_xyz_mg[3])
{
	const struct app_accel_sample msg = {
		.timestamp_ms = k_uptime_get(),
		.x_mg = xyz_mg[0],
		.y_mg = xyz_mg[1],
		.z_mg = xyz_mg[2],
		.linear_x_mg = linear_xyz_mg[0],
		.linear_y_mg = linear_xyz_mg[1],
		.linear_z_mg = linear_xyz_mg[2],
		.linear_accel_mg = linear_accel_mg,
		.speed_mm_s = speed_mm_s,
		.quiet_time_ms = quiet_time_ms,
		.moving = moving,
	};

	return publish_status(&accel_sample_chan, &msg);
}

int app_zbus_publish_battery_sample(int64_t voltage_mv, int64_t current_ma,
				    int64_t temp_mdegc, int32_t charger_status,
				    int32_t charger_error, bool vbus_present)
{
	const struct app_battery_sample msg = {
		.timestamp_ms = k_uptime_get(),
		.voltage_mv = voltage_mv,
		.current_ma = current_ma,
		.temp_mdegc = temp_mdegc,
		.charger_status = charger_status,
		.charger_error = charger_error,
		.vbus_present = vbus_present,
	};

	return publish_status(&battery_sample_chan, &msg);
}

int app_zbus_publish_environment_sample(int64_t temp_mdegc, int64_t pressure_pa,
					int64_t humidity_milli_pct,
					int64_t gas_ohm)
{
	const struct app_environment_sample msg = {
		.timestamp_ms = k_uptime_get(),
		.temp_mdegc = temp_mdegc,
		.pressure_pa = pressure_pa,
		.humidity_milli_pct = humidity_milli_pct,
		.gas_ohm = gas_ohm,
	};

	return publish_status(&environment_sample_chan, &msg);
}
