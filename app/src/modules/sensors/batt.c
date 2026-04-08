/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "batt.h"

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(batt, LOG_LEVEL_INF);

#define BATT_THREAD_STACK_SIZE   1024
#define BATT_THREAD_PRIORITY     7
#define BATT_POLL_INTERVAL_SEC   15

#define BATT_STATUS_COMPLETE_MASK BIT(1)
#define BATT_STATUS_TRICKLE_MASK  BIT(2)
#define BATT_STATUS_CC_MASK       BIT(3)
#define BATT_STATUS_CV_MASK       BIT(4)

#if DT_HAS_COMPAT_STATUS_OKAY(nordic_npm1300_charger)
#define BATT_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(nordic_npm1300_charger)
static const struct device *const batt_dev = DEVICE_DT_GET(BATT_NODE);
#else
static const struct device *const batt_dev = NULL;
#endif

K_THREAD_STACK_DEFINE(batt_thread_stack, BATT_THREAD_STACK_SIZE);
static struct k_thread batt_thread_data;
static bool batt_started;

static const char *batt_level_string(int64_t voltage_mv)
{
	if (voltage_mv >= 4100) {
		return "full";
	}

	if (voltage_mv >= 3850) {
		return "high";
	}

	if (voltage_mv >= 3600) {
		return "medium";
	}

	if (voltage_mv >= 3400) {
		return "low";
	}

	return "critical";
}

static const char *batt_charge_state_string(int32_t status)
{
	if (status & BATT_STATUS_COMPLETE_MASK) {
		return "complete";
	}

	if (status & BATT_STATUS_TRICKLE_MASK) {
		return "trickle";
	}

	if (status & BATT_STATUS_CC_MASK) {
		return "charging";
	}

	if (status & BATT_STATUS_CV_MASK) {
		return "topping-off";
	}

	return "idle";
}

static int batt_read_sensor_values(int64_t *voltage_mv, int64_t *current_ma,
				   int64_t *temp_mdegc, int32_t *status,
				   int32_t *error, bool *vbus_present)
{
	struct sensor_value value;
	int ret;

	ret = sensor_sample_fetch(batt_dev);
	if (ret < 0) {
		return ret;
	}

	ret = sensor_channel_get(batt_dev, SENSOR_CHAN_GAUGE_VOLTAGE, &value);
	if (ret < 0) {
		return ret;
	}
	*voltage_mv = sensor_value_to_milli(&value);

	ret = sensor_channel_get(batt_dev, SENSOR_CHAN_GAUGE_AVG_CURRENT, &value);
	if (ret < 0) {
		return ret;
	}
	*current_ma = sensor_value_to_milli(&value);

	ret = sensor_channel_get(batt_dev, SENSOR_CHAN_GAUGE_TEMP, &value);
	if (ret < 0) {
		return ret;
	}
	*temp_mdegc = sensor_value_to_milli(&value);

	ret = sensor_channel_get(batt_dev,
				 (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_STATUS,
				 &value);
	if (ret < 0) {
		return ret;
	}
	*status = value.val1;

	ret = sensor_channel_get(batt_dev,
				 (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_ERROR,
				 &value);
	if (ret < 0) {
		return ret;
	}
	*error = value.val1;

	ret = sensor_attr_get(batt_dev,
			      (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS,
			      (enum sensor_attribute)SENSOR_ATTR_NPM13XX_CHARGER_VBUS_PRESENT,
			      &value);
	if (ret < 0) {
		return ret;
	}
	*vbus_present = (value.val1 != 0);

	return 0;
}

static void batt_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		int64_t voltage_mv;
		int64_t current_ma;
		int64_t temp_mdegc;
		int32_t status;
		int32_t error;
		bool vbus_present;
		int ret = batt_read_sensor_values(&voltage_mv, &current_ma,
							 &temp_mdegc, &status,
							 &error, &vbus_present);

		if (ret < 0) {
			LOG_WRN("Battery read failed: %d", ret);
		} else {
			LOG_INF("Battery: %lld mV (%s), %s, %s, current=%lld mA, temp=%lld.%03lld C, err=%d",
				voltage_mv,
				batt_level_string(voltage_mv),
				batt_charge_state_string(status),
				vbus_present ? "USB present" : "USB not present",
				current_ma,
				temp_mdegc / 1000,
				temp_mdegc < 0 ? -(temp_mdegc % 1000) : (temp_mdegc % 1000),
				error);
		}

		k_sleep(K_SECONDS(BATT_POLL_INTERVAL_SEC));
	}
}

int batt_start(void)
{
	if (batt_started) {
		return 0;
	}

	if (batt_dev == NULL) {
		LOG_INF("Battery demo skipped: no nPM1300 charger sensor is enabled for this board");
		return -ENODEV;
	}

	if (!device_is_ready(batt_dev)) {
		LOG_WRN("Battery device %s is not ready", batt_dev->name);
		return -ENODEV;
	}

	k_thread_create(&batt_thread_data, batt_thread_stack,
			K_THREAD_STACK_SIZEOF(batt_thread_stack),
			batt_thread, NULL, NULL, NULL,
			BATT_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&batt_thread_data, "batt_demo");

	batt_started = true;

	LOG_INF("Battery demo started on %s", batt_dev->name);

	return 0;
}

static int batt_init(void)
{
	(void)batt_start();
	return 0;
}

SYS_INIT(batt_init, APPLICATION, 90);
