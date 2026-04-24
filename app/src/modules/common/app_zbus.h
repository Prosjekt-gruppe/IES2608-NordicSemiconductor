/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/zbus/zbus.h>
#include "app_gnss_types.h"



struct app_accel_sample {
	int64_t timestamp_ms;
	int32_t x_mg;
	int32_t y_mg;
	int32_t z_mg;
	int32_t linear_x_mg;
	int32_t linear_y_mg;
	int32_t linear_z_mg;
	uint32_t linear_accel_mg;
	uint32_t speed_mm_s;
	uint32_t quiet_time_ms;
	bool moving;
};

struct app_battery_sample {
	int64_t timestamp_ms;
	int64_t voltage_mv;
	int64_t current_ma;
	int64_t temp_mdegc;
	int32_t charger_status;
	int32_t charger_error;
	bool vbus_present;
};

struct app_environment_sample {
	int64_t timestamp_ms;
	int64_t temp_mdegc;
	int64_t pressure_pa;
	int64_t humidity_milli_pct;
	int64_t gas_ohm;
};

ZBUS_CHAN_DECLARE(gnss_status_chan);
ZBUS_CHAN_DECLARE(accel_sample_chan);
ZBUS_CHAN_DECLARE(battery_sample_chan);
ZBUS_CHAN_DECLARE(environment_sample_chan);

int app_zbus_publish_gnss_status(enum app_gnss_state state, int err,
				 int64_t time_to_first_fix_ms, double latitude,
				 double longitude, float altitude,
				 uint8_t tracked_satellites);

int app_zbus_publish_gnss_error(int err);

int app_zbus_publish_accel_sample(bool moving, uint32_t speed_mm_s,
				  uint32_t linear_accel_mg,
				  uint32_t quiet_time_ms,
				  const int32_t xyz_mg[3],
				  const int32_t linear_xyz_mg[3]);

int app_zbus_publish_battery_sample(int64_t voltage_mv, int64_t current_ma,
				    int64_t temp_mdegc, int32_t charger_status,
				    int32_t charger_error, bool vbus_present);

int app_zbus_publish_environment_sample(int64_t temp_mdegc, int64_t pressure_pa,
					int64_t humidity_milli_pct,
					int64_t gas_ohm);


