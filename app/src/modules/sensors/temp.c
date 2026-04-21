/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "temp.h"
#include "app_zbus.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(temp, LOG_LEVEL_INF);

#define TEMP_THREAD_STACK_SIZE 1024
#define TEMP_THREAD_PRIORITY   7
#define TEMP_POLL_INTERVAL_SEC 15

#if DT_HAS_COMPAT_STATUS_OKAY(bosch_bme680)
#define TEMP_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(bosch_bme680)
static const struct device *const temp_dev = DEVICE_DT_GET(TEMP_NODE);
#else
static const struct device *const temp_dev = NULL;
#endif

K_THREAD_STACK_DEFINE(temp_thread_stack, TEMP_THREAD_STACK_SIZE);
static struct k_thread temp_thread_data;
static bool temp_started;

static int temp_read_sensor_values(int64_t *temp_mdegc, int64_t *pressure_pa,
				   int64_t *humidity_milli_pct, int64_t *gas_ohm)
{
	struct sensor_value value;
	int ret;

	ret = sensor_sample_fetch(temp_dev);
	if (ret < 0) {
		return ret;
	}

	ret = sensor_channel_get(temp_dev, SENSOR_CHAN_AMBIENT_TEMP, &value);
	if (ret < 0) {
		return ret;
	}
	*temp_mdegc = sensor_value_to_milli(&value);

	ret = sensor_channel_get(temp_dev, SENSOR_CHAN_PRESS, &value);
	if (ret < 0) {
		return ret;
	}
	*pressure_pa = sensor_value_to_milli(&value);

	ret = sensor_channel_get(temp_dev, SENSOR_CHAN_HUMIDITY, &value);
	if (ret < 0) {
		return ret;
	}
	*humidity_milli_pct = sensor_value_to_milli(&value);

	ret = sensor_channel_get(temp_dev, SENSOR_CHAN_GAS_RES, &value);
	if (ret < 0) {
		return ret;
	}
	*gas_ohm = sensor_value_to_milli(&value) / 1000;

	return 0;
}

static void temp_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		int64_t temp_mdegc;
		int64_t pressure_pa;
		int64_t humidity_milli_pct;
		int64_t gas_ohm;
		int ret = temp_read_sensor_values(&temp_mdegc, &pressure_pa,
						   &humidity_milli_pct, &gas_ohm);

		if (ret < 0) {
			LOG_WRN("Temperature read failed: %d", ret);
		} else {
			(void)app_zbus_publish_environment_sample(temp_mdegc, pressure_pa,
								  humidity_milli_pct,
								  gas_ohm);
			LOG_INF("Environment: temp=%lld.%03lld C, pressure=%lld.%03lld kPa, humidity=%lld.%03lld %%, gas=%lld ohm",
				temp_mdegc / 1000,
				temp_mdegc < 0 ? -(temp_mdegc % 1000) : (temp_mdegc % 1000),
				pressure_pa / 1000,
				pressure_pa < 0 ? -(pressure_pa % 1000) : (pressure_pa % 1000),
				humidity_milli_pct / 1000,
				humidity_milli_pct < 0 ? -(humidity_milli_pct % 1000) :
							 (humidity_milli_pct % 1000),
				gas_ohm);
		}

		k_sleep(K_SECONDS(TEMP_POLL_INTERVAL_SEC));
	}
}

int temp_start(void)
{
	if (temp_started) {
		return 0;
	}

	if (temp_dev == NULL) {
		LOG_INF("Temperature demo skipped: no BME680 sensor is enabled for this board");
		return -ENODEV;
	}

	if (!device_is_ready(temp_dev)) {
		LOG_WRN("Temperature device %s is not ready", temp_dev->name);
		return -ENODEV;
	}

	k_thread_create(&temp_thread_data, temp_thread_stack,
			K_THREAD_STACK_SIZEOF(temp_thread_stack),
			temp_thread, NULL, NULL, NULL,
			TEMP_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&temp_thread_data, "temp_demo");

	temp_started = true;

	LOG_INF("Temperature demo started on %s", temp_dev->name);

	return 0;
}

static int temp_init(void)
{
	(void)temp_start();
	return 0;
}

SYS_INIT(temp_init, APPLICATION, 90);
