/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "batt.h"
#include "app_zbus.h"

#include <errno.h>
#include <iso646.h>
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

struct batt_sample {
	int64_t voltage_mv;
	int64_t current_ma;
	int64_t temp_mdegc;
	int32_t charger_status;
	int32_t charger_error;
	bool vbus_present;
};

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
	if ((status & BATT_STATUS_COMPLETE_MASK) != 0) {
		return "complete";
	}

	if ((status & BATT_STATUS_TRICKLE_MASK) != 0) {
		return "trickle";
	}

	if ((status & BATT_STATUS_CC_MASK) != 0) {
		return "charging";
	}

	if ((status & BATT_STATUS_CV_MASK) != 0) {
		return "topping-off";
	}

	return "idle";
}

static int batt_read_sample(struct batt_sample *sample)
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
	sample->voltage_mv = sensor_value_to_milli(&value);

	ret = sensor_channel_get(batt_dev, SENSOR_CHAN_GAUGE_AVG_CURRENT, &value);
	if (ret < 0) {
		return ret;
	}
	sample->current_ma = sensor_value_to_milli(&value);

	ret = sensor_channel_get(batt_dev, SENSOR_CHAN_GAUGE_TEMP, &value);
	if (ret < 0) {
		return ret;
	}
	sample->temp_mdegc = sensor_value_to_milli(&value);

	ret = sensor_channel_get(batt_dev,
				 (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_STATUS,
				 &value);
	if (ret < 0) {
		return ret;
	}
	sample->charger_status = value.val1;

	ret = sensor_channel_get(batt_dev,
				 (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_ERROR,
				 &value);
	if (ret < 0) {
		return ret;
	}
	sample->charger_error = value.val1;

	ret = sensor_attr_get(batt_dev,
			      (enum sensor_channel)SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS,
			      (enum sensor_attribute)SENSOR_ATTR_NPM13XX_CHARGER_VBUS_PRESENT,
			      &value);
	if (ret < 0) {
		return ret;
	}
	sample->vbus_present = (value.val1 != 0);

	return 0;
}

static void batt_publish_sample(const struct batt_sample *sample)
{
	(void)app_zbus_publish_battery_sample(sample->voltage_mv,
					      sample->current_ma,
					      sample->temp_mdegc,
					      sample->charger_status,
					      sample->charger_error,
					      sample->vbus_present);
}

static void batt_log_sample(const struct batt_sample *sample)
{
	LOG_INF("Battery: %lld mV (%s), %s, %s, current=%lld mA, temp=%lld.%03lld C, err=%d",
		sample->voltage_mv,
		batt_level_string(sample->voltage_mv),
		batt_charge_state_string(sample->charger_status),
		sample->vbus_present ? "USB present" : "USB not present",
		sample->current_ma,
		sample->temp_mdegc / 1000,
		sample->temp_mdegc < 0 ? -(sample->temp_mdegc % 1000) :
					 (sample->temp_mdegc % 1000),
		sample->charger_error);
}

static void batt_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		struct batt_sample sample;
		int ret = batt_read_sample(&sample);

		if (ret < 0) {
			LOG_WRN("Battery read failed: %d", ret);
		} else {
			batt_publish_sample(&sample);
			batt_log_sample(&sample);
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

	if (not device_is_ready(batt_dev)) {
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
