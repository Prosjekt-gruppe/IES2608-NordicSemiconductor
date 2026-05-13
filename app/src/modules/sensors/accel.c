/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "accel.h"
#include "accel_logic.h"
#include "app_zbus.h"
#include "rsrp_service.h"

#include <iso646.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(accel, LOG_LEVEL_INF);

#define ACCEL_THREAD_STACK_SIZE      1024
#define ACCEL_THREAD_PRIORITY        7
#define ACCEL_POLL_INTERVAL_MS       200
#define ACCEL_SAMPLING_FREQUENCY_HZ  25
#define ACCEL_FULL_SCALE_G           2
#define ACCEL_MOVEMENT_LOG_INTERVAL_MS 1000
#define ACCEL_ZBUS_PUBLISH_INTERVAL_MS 1000

#if DT_HAS_COMPAT_STATUS_OKAY(bosch_bmi270)
#define ACCEL_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(bosch_bmi270)
static const struct device *const accel_dev = DEVICE_DT_GET(ACCEL_NODE);
#else
static const struct device *const accel_dev = NULL;
#endif

K_THREAD_STACK_DEFINE(accel_thread_stack, ACCEL_THREAD_STACK_SIZE);
static struct k_thread accel_thread_data;
static bool accel_started;

static int accel_fetch_xyz_mg(const struct device *dev, int32_t xyz_mg[3])
{
	struct sensor_value accel[3];
	int ret;

	ret = sensor_sample_fetch(dev);
	if (ret < 0) {
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_ACCEL_XYZ, accel);
	if (ret < 0) {
		return ret;
	}

	xyz_mg[0] = sensor_ms2_to_mg(&accel[0]);
	xyz_mg[1] = sensor_ms2_to_mg(&accel[1]);
	xyz_mg[2] = sensor_ms2_to_mg(&accel[2]);

	return 0;
}

static int accel_configure(const struct device *dev)
{
	struct sensor_value full_scale = {
		.val1 = ACCEL_FULL_SCALE_G,
		.val2 = 0,
	};
	struct sensor_value sampling_frequency = {
		.val1 = ACCEL_SAMPLING_FREQUENCY_HZ,
		.val2 = 0,
	};
	int ret;

	ret = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE, &full_scale);
	if (ret < 0) {
		LOG_WRN("Unable to set accelerometer full scale: %d", ret);
	}

	ret = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ,
			      SENSOR_ATTR_SAMPLING_FREQUENCY,
			      &sampling_frequency);
	if (ret < 0) {
		LOG_WRN("Unable to set accelerometer sampling frequency: %d", ret);
	}

	return 0;
}

static void accel_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	int32_t baseline_xyz_mg[3] = { 0 };
	int32_t velocity_mm_s[3] = { 0 };
	int32_t previous_xyz_mg[3] = { 0 };
	int32_t baseline_sum_xyz_mg[3] = { 0 };
	uint8_t baseline_sample_count = 0;
	bool have_baseline = false;
	bool have_previous_sample = false;
	bool last_reported_moving = false;
	bool motion_state_was_reported = false;
	uint32_t quiet_time_ms = 0;
	uint32_t last_sample_ts_ms = 0;
	uint32_t last_movement_ts_ms = 0;
	uint32_t last_motion_log_ts_ms = 0;
	uint32_t last_zbus_publish_ts_ms = 0;

	while (true) {
		int32_t xyz_mg[3];
		int32_t linear_xyz_mg[3];
		int ret = accel_fetch_xyz_mg(accel_dev, xyz_mg);
		uint32_t now_ms = k_uptime_get_32();
		uint32_t dt_ms;
		uint32_t linear_accel_mg;
		uint32_t speed_mm_s;
		bool is_moving_now;
		bool motion_state_has_changed;
		bool movement_detected_now;
		bool movement_seen_recently;
		uint32_t sample_delta_mg;
		bool sample_is_quiet;

		if (ret < 0) {
			LOG_WRN("Accelerometer read failed: %d", ret);
			k_sleep(K_MSEC(ACCEL_POLL_INTERVAL_MS));
			continue;
		}

		if (not have_baseline) {
			const int32_t zero_linear_xyz_mg[3] = { 0 };

			if (not accel_logic_sample_is_gravity_like(xyz_mg)) {
				baseline_sample_count = 0;
				accel_logic_zero_xyz(baseline_sum_xyz_mg);
				LOG_WRN("Ignoring accelerometer startup sample with invalid gravity magnitude: %u mg",
					accel_logic_raw_magnitude_mg(xyz_mg));
				k_sleep(K_MSEC(ACCEL_POLL_INTERVAL_MS));
				continue;
			}

			for (size_t i = 0; i < 3; i++) {
				baseline_sum_xyz_mg[i] += xyz_mg[i];
			}
			baseline_sample_count++;

			if (baseline_sample_count < ACCEL_LOGIC_BASELINE_SAMPLES) {
				k_sleep(K_MSEC(ACCEL_POLL_INTERVAL_MS));
				continue;
			}

			for (size_t i = 0; i < 3; i++) {
				baseline_xyz_mg[i] = baseline_sum_xyz_mg[i] /
						     baseline_sample_count;
			}

			have_baseline = true;
			accel_logic_copy_xyz(previous_xyz_mg, xyz_mg);
			have_previous_sample = true;
			last_sample_ts_ms = now_ms;
			rsrp_service_set_motion_hint(false, 0, 0);
			(void)app_zbus_publish_accel_sample(false, 0, 0, 0,
							    xyz_mg, zero_linear_xyz_mg);
			last_zbus_publish_ts_ms = now_ms;
			motion_state_was_reported = true;
			LOG_INF("Accelerometer baseline calibrated: xyz=(%d, %d, %d) mg, gravity=%u mg",
				baseline_xyz_mg[0], baseline_xyz_mg[1], baseline_xyz_mg[2],
				accel_logic_raw_magnitude_mg(baseline_xyz_mg));
			k_sleep(K_MSEC(ACCEL_POLL_INTERVAL_MS));
			continue;
		}

		dt_ms = now_ms - last_sample_ts_ms;
		last_sample_ts_ms = now_ms;
		dt_ms = accel_logic_normalize_dt_ms(dt_ms, ACCEL_POLL_INTERVAL_MS);

		linear_xyz_mg[0] = xyz_mg[0] - baseline_xyz_mg[0];
		linear_xyz_mg[1] = xyz_mg[1] - baseline_xyz_mg[1];
		linear_xyz_mg[2] = xyz_mg[2] - baseline_xyz_mg[2];
		accel_logic_apply_deadband(linear_xyz_mg);

		linear_accel_mg = accel_logic_vector_magnitude_mg(linear_xyz_mg[0],
							       linear_xyz_mg[1],
							       linear_xyz_mg[2]);

		if (have_previous_sample) {
			sample_delta_mg = accel_logic_vector_magnitude_mg(
				xyz_mg[0] - previous_xyz_mg[0],
				xyz_mg[1] - previous_xyz_mg[1],
				xyz_mg[2] - previous_xyz_mg[2]);
		} else {
			sample_delta_mg = 0;
			have_previous_sample = true;
		}
		accel_logic_copy_xyz(previous_xyz_mg, xyz_mg);

		sample_is_quiet = (sample_delta_mg <= ACCEL_LOGIC_QUIET_THRESHOLD_MG) and
				  accel_logic_sample_is_gravity_like(xyz_mg);

		if (sample_is_quiet) {
			if (quiet_time_ms < ACCEL_LOGIC_ZERO_VELOCITY_MS) {
				uint32_t remaining_ms =
					ACCEL_LOGIC_ZERO_VELOCITY_MS - quiet_time_ms;

				quiet_time_ms += (dt_ms < remaining_ms) ? dt_ms : remaining_ms;
			}

			accel_logic_update_baseline(baseline_xyz_mg, xyz_mg);
		} else {
			quiet_time_ms = 0;
			accel_logic_integrate_velocity(velocity_mm_s, linear_xyz_mg, dt_ms);
		}

		if ((quiet_time_ms >= ACCEL_LOGIC_ZERO_VELOCITY_MS) and
		    not accel_logic_velocity_is_zero(velocity_mm_s)) {
			accel_logic_zero_xyz(velocity_mm_s);
			LOG_INF("Standstill recalibration: velocity reset after %u ms without acceleration",
				quiet_time_ms);
		}

		speed_mm_s = accel_logic_vector_magnitude_mg(velocity_mm_s[0],
							 velocity_mm_s[1],
							 velocity_mm_s[2]);

		if (quiet_time_ms >= ACCEL_LOGIC_ZERO_VELOCITY_MS) {
			speed_mm_s = 0;
		}

		movement_detected_now = ((not sample_is_quiet) and
					 (linear_accel_mg >=
					  ACCEL_LOGIC_MOVEMENT_THRESHOLD_MG)) or
					(speed_mm_s >= ACCEL_LOGIC_MOVING_SPEED_MM_S);

		if (movement_detected_now) {
			last_movement_ts_ms = now_ms;
		}

		movement_seen_recently = (last_movement_ts_ms != 0U) and
					 ((now_ms - last_movement_ts_ms) <=
					  CONFIG_APP_SENSOR_ACCEL_MOVING_HOLD_MS);
		is_moving_now = movement_detected_now or movement_seen_recently;

		if ((not sample_is_quiet) and
		    (linear_accel_mg >= ACCEL_LOGIC_MOVEMENT_THRESHOLD_MG) and
		    ((last_motion_log_ts_ms == 0U) or
		     ((now_ms - last_motion_log_ts_ms) >= ACCEL_MOVEMENT_LOG_INTERVAL_MS))) {
			last_motion_log_ts_ms = now_ms;
			LOG_INF("Movement detected: accel=%u mg, delta=%u mg, speed=%u mm/s, xyz=(%d, %d, %d) mg, linear=(%d, %d, %d) mg",
				linear_accel_mg, sample_delta_mg, speed_mm_s,
				xyz_mg[0], xyz_mg[1], xyz_mg[2],
				linear_xyz_mg[0], linear_xyz_mg[1], linear_xyz_mg[2]);
		}

		motion_state_has_changed =
			(not motion_state_was_reported) or (is_moving_now != last_reported_moving);

		if (motion_state_has_changed) {
			rsrp_service_set_motion_hint(is_moving_now, speed_mm_s, linear_accel_mg);

			if (is_moving_now) {
				LOG_INF("Motion state: moving (speed=%u mm/s, accel=%u mg)",
					speed_mm_s, linear_accel_mg);
			} else {
				LOG_INF("Motion state: still (speed=%u mm/s, quiet=%u ms)",
					speed_mm_s, quiet_time_ms);
			}

			last_reported_moving = is_moving_now;
			motion_state_was_reported = true;
		}

		if (motion_state_has_changed or (last_zbus_publish_ts_ms == 0U) or
		    ((now_ms - last_zbus_publish_ts_ms) >= ACCEL_ZBUS_PUBLISH_INTERVAL_MS)) {
			(void)app_zbus_publish_accel_sample(is_moving_now, speed_mm_s,
							    linear_accel_mg, quiet_time_ms,
							    xyz_mg, linear_xyz_mg);
			last_zbus_publish_ts_ms = now_ms;
		}

		k_sleep(K_MSEC(ACCEL_POLL_INTERVAL_MS));
	}
}

int accel_start(void)
{
	if (accel_started) {
		return 0;
	}

	if (accel_dev == NULL) {
		LOG_INF("Accelerometer demo skipped: no BMI270 sensor is enabled for this board");
		return -ENODEV;
	}

	if (not device_is_ready(accel_dev)) {
		LOG_WRN("Accelerometer device %s is not ready", accel_dev->name);
		return -ENODEV;
	}

	accel_configure(accel_dev);

	k_thread_create(&accel_thread_data, accel_thread_stack,
			K_THREAD_STACK_SIZEOF(accel_thread_stack),
			accel_thread, NULL, NULL, NULL,
			ACCEL_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&accel_thread_data, "accel_demo");

	accel_started = true;

	LOG_INF("Accelerometer demo started on %s (threshold %d mg, poll %d ms)",
		accel_dev->name, ACCEL_LOGIC_MOVEMENT_THRESHOLD_MG, ACCEL_POLL_INTERVAL_MS);

	return 0;
}
