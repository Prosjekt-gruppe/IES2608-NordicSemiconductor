/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "accel.h"

#include <stdbool.h>
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
#define ACCEL_MOVEMENT_THRESHOLD_MG  120
#define ACCEL_SAMPLING_FREQUENCY_HZ  25
#define ACCEL_FULL_SCALE_G           2

#if DT_HAS_COMPAT_STATUS_OKAY(bosch_bmi270)
#define ACCEL_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(bosch_bmi270)
static const struct device *const accel_dev = DEVICE_DT_GET(ACCEL_NODE);
#else
static const struct device *const accel_dev = NULL;
#endif

K_THREAD_STACK_DEFINE(accel_thread_stack, ACCEL_THREAD_STACK_SIZE);
static struct k_thread accel_thread_data;
static bool accel_started;

static uint32_t accel_isqrt64(uint64_t value)
{
	uint64_t result = 0;
	uint64_t bit = UINT64_C(1) << 62;

	while (bit > value) {
		bit >>= 2;
	}

	while (bit != 0U) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}

		bit >>= 2;
	}

	return (uint32_t)result;
}

static uint32_t accel_vector_magnitude_mg(int32_t x_mg, int32_t y_mg, int32_t z_mg)
{
	uint64_t sum = ((int64_t)x_mg * x_mg) +
		       ((int64_t)y_mg * y_mg) +
		       ((int64_t)z_mg * z_mg);

	return accel_isqrt64(sum);
}

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

	int32_t previous_xyz_mg[3] = { 0 };
	bool have_previous_sample = false;

	while (true) {
		int32_t xyz_mg[3];
		int ret = accel_fetch_xyz_mg(accel_dev, xyz_mg);

		if (ret < 0) {
			LOG_WRN("Accelerometer read failed: %d", ret);
			k_sleep(K_MSEC(ACCEL_POLL_INTERVAL_MS));
			continue;
		}

		if (have_previous_sample) {
			int32_t dx_mg = xyz_mg[0] - previous_xyz_mg[0];
			int32_t dy_mg = xyz_mg[1] - previous_xyz_mg[1];
			int32_t dz_mg = xyz_mg[2] - previous_xyz_mg[2];
			uint32_t delta_mg = accel_vector_magnitude_mg(dx_mg, dy_mg, dz_mg);

			if (delta_mg >= ACCEL_MOVEMENT_THRESHOLD_MG) {
				uint32_t total_mg = accel_vector_magnitude_mg(xyz_mg[0],
										    xyz_mg[1],
										    xyz_mg[2]);

				LOG_INF("Movement detected: delta=%u mg, total=%u mg, xyz=(%d, %d, %d) mg",
					delta_mg, total_mg, xyz_mg[0], xyz_mg[1], xyz_mg[2]);
			}
		}

		previous_xyz_mg[0] = xyz_mg[0];
		previous_xyz_mg[1] = xyz_mg[1];
		previous_xyz_mg[2] = xyz_mg[2];
		have_previous_sample = true;

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

	if (!device_is_ready(accel_dev)) {
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
		accel_dev->name, ACCEL_MOVEMENT_THRESHOLD_MG, ACCEL_POLL_INTERVAL_MS);

	return 0;
}
