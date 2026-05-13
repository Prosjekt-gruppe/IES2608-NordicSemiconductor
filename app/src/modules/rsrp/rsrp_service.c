/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "rsrp_service.h"
#include "app_events.h"
#include "rsrp_monitor.h"
#include "rsrp_parse.h"

#include <errno.h>
#include <iso646.h>
#include <stdint.h>

#include <nrf_modem_at.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifndef CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC
#define CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC \
	CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC
#endif

#ifndef CONFIG_APP_MODEM_RSRP_RECOVERY_DBM
#define CONFIG_APP_MODEM_RSRP_RECOVERY_DBM \
	CONFIG_APP_MODEM_RSRP_FALLBACK_DBM
#endif

LOG_MODULE_REGISTER(rsrp_service, LOG_LEVEL_INF);

#define PROBE_POLL_MSEC 500

enum rsrp_mode {
	RSRP_MODE_IDLE,		
	RSRP_MODE_MONITOR,
	RSRP_MODE_PROBE,
};

static enum rsrp_mode current_mode;
static uint8_t probe_sample_target;

static struct k_work_delayable rsrp_work;
static bool service_initialized;
static bool lte_poor_event_sent;
static bool has_motion_hint;
static bool motion_hint_is_moving;
static uint32_t motion_speed_mm_s;
static uint32_t motion_linear_accel_mg;
static uint32_t monitor_poll_interval_sec = CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC;

static struct rsrp_monitor_state rsrp_monitor;

static const struct rsrp_monitor_config rsrp_monitor_config = {
	.fallback_dbm = CONFIG_APP_MODEM_RSRP_FALLBACK_DBM,
	.drop_db = CONFIG_APP_MODEM_RSRP_DROP_DB,
	.weak_sample_limit = CONFIG_APP_MODEM_RSRP_WEAK_SAMPLE_COUNT,
	.unavailable_sample_limit = CONFIG_APP_MODEM_RSRP_LOSS_SAMPLE_COUNT,
};

static void reset_signal_tracking(void)
{
	rsrp_monitor_reset(&rsrp_monitor);
	lte_poor_event_sent = false;
}

static int publish_rsrp_event(enum app_evt_type type, int rsrp_dbm)
{
	struct app_event ev = {
		.type = type,
	};

	ev.meas.rsrp_dbm = rsrp_dbm;

	return app_event_put(&ev, K_NO_WAIT);
}

static const char *rsrp_motion_state_str(void)
{
	if (not has_motion_hint) {
		return "unknown";
	}

	return motion_hint_is_moving ? "moving" : "still";
}

static uint32_t rsrp_target_poll_interval_sec(void)
{
	if (not has_motion_hint) {
		return CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC;
	}

	if (motion_hint_is_moving) {
		return CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC;
	}

	return CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC;
}

static bool should_publish_lte_poor(int rsrp_dbm)
{
	return rsrp_monitor_record_signal_sample(&rsrp_monitor,
						 &rsrp_monitor_config,
						 rsrp_dbm);
}

static void record_rsrp_available(void)
{
	rsrp_monitor_record_available(&rsrp_monitor);
}

static uint8_t record_rsrp_unavailable(void)
{
	return rsrp_monitor_record_unavailable(&rsrp_monitor);
}

static int rsrp_event_value_for_unavailable(void)
{
	return rsrp_monitor_unavailable_event_value(&rsrp_monitor,
						    &rsrp_monitor_config);
}

static bool rsrp_unavailable_limit_reached(void)
{
	return rsrp_monitor_unavailable_limit_reached(&rsrp_monitor,
						      &rsrp_monitor_config);
}

static void rsrp_work_handler(struct k_work *work)
{
	int err;
	int rsrp_dbm;
	int probe_average_dbm;

	ARG_UNUSED(work);

	switch (current_mode) {
	case RSRP_MODE_MONITOR:
		err = rsrp_service_get(&rsrp_dbm);
		if (err) {
			uint8_t unavailable_count = record_rsrp_unavailable();

			monitor_poll_interval_sec = rsrp_target_poll_interval_sec();

			LOG_WRN("LTE-M RSRP unavailable: err=%d, count=%u/%u, next=%u s",
				err,
				(unsigned int)unavailable_count,
				(unsigned int)CONFIG_APP_MODEM_RSRP_LOSS_SAMPLE_COUNT,
				monitor_poll_interval_sec);

			if ((not lte_poor_event_sent) and rsrp_unavailable_limit_reached()) {
				lte_poor_event_sent = true;
				current_mode = RSRP_MODE_IDLE;
				rsrp_dbm = rsrp_event_value_for_unavailable();

				LOG_WRN("LTE-M RSRP unavailable for %u samples, publishing LTE poor event",
					(unsigned int)unavailable_count);
				(void)publish_rsrp_event(EVT_LTE_POOR, rsrp_dbm);
				return;
			}

			(void)k_work_reschedule(&rsrp_work, K_SECONDS(monitor_poll_interval_sec));
			return;
		}

		record_rsrp_available();
		monitor_poll_interval_sec = rsrp_target_poll_interval_sec();

		LOG_INF("LTE-M RSRP: %d dBm, next=%u s, motion=%s, speed=%u mm/s, accel=%u mg",
			rsrp_dbm,
			monitor_poll_interval_sec,
			rsrp_motion_state_str(),
			motion_speed_mm_s,
			motion_linear_accel_mg);
		(void)publish_rsrp_event(EVT_RSRP_UPDATE, rsrp_dbm);

		if ((not lte_poor_event_sent) and should_publish_lte_poor(rsrp_dbm)) {
			lte_poor_event_sent = true;
			current_mode = RSRP_MODE_IDLE;

			LOG_WRN("LTE-M signal degradation detected, publishing LTE poor event");
			(void)publish_rsrp_event(EVT_LTE_POOR, rsrp_dbm);
			return;
		}

		(void)k_work_reschedule(&rsrp_work, K_SECONDS(monitor_poll_interval_sec));
		return;

	case RSRP_MODE_PROBE:
		err = rsrp_service_get(&rsrp_dbm);
		if (err) {
			uint8_t unavailable_count = record_rsrp_unavailable();

			LOG_WRN("LTE probe RSRP unavailable: err=%d, count=%u/%u",
				err,
				(unsigned int)unavailable_count,
				(unsigned int)CONFIG_APP_MODEM_RSRP_LOSS_SAMPLE_COUNT);

			if (rsrp_unavailable_limit_reached()) {
				rsrp_dbm = rsrp_event_value_for_unavailable();

				LOG_WRN("LTE probe failed because RSRP stayed unavailable");
				(void)publish_rsrp_event(EVT_LTE_POOR, rsrp_dbm);
				current_mode = RSRP_MODE_IDLE;
				return;
			}

			(void)k_work_reschedule(&rsrp_work, K_MSEC(PROBE_POLL_MSEC));
			return;
		}

		record_rsrp_available();

		rsrp_monitor_history_add(&rsrp_monitor, rsrp_dbm);

		LOG_INF("LTE probe RSRP sample %u/%u: %d dBm",
			rsrp_monitor_history_count(&rsrp_monitor),
			probe_sample_target, rsrp_dbm);

		if (rsrp_monitor_history_count(&rsrp_monitor) < probe_sample_target) {
			k_work_reschedule(&rsrp_work, K_MSEC(PROBE_POLL_MSEC));
			return;
		}

		probe_average_dbm = rsrp_monitor_history_average(&rsrp_monitor);

		if (probe_average_dbm >= CONFIG_APP_MODEM_RSRP_RECOVERY_DBM) {
			publish_rsrp_event(EVT_LTE_GOOD, probe_average_dbm);
		} else {
			publish_rsrp_event(EVT_LTE_POOR, probe_average_dbm);
		}

		LOG_INF("LTE probe complete: avg=%d dBm over %u samples",
			probe_average_dbm, probe_sample_target);
		current_mode = RSRP_MODE_IDLE;
		return;

	case RSRP_MODE_IDLE:
	default:
		return;
	}
}


int rsrp_service_get(int *rsrp_dbm)
{
	char response[64];
	int err;

	if (rsrp_dbm == NULL) {
		return -EINVAL;
	}

	err = nrf_modem_at_cmd(response, sizeof(response), "AT+CESQ");
	if (err) {
		LOG_ERR("AT+CESQ failed: %d", err);
		return err;
	}

	LOG_DBG("CESQ response: %s", response);
	err = rsrp_parse_cesq_rsrp(response, rsrp_dbm);
	if (err == -EIO) {
		LOG_WRN("Failed to parse CESQ response");
	} else if (err == -ENOENT) {
		LOG_WRN("RSRP not known");
	} else if ((err == 0) && (*rsrp_dbm == -141)) {
		LOG_WRN("RSRP < -140 dBm");
	}

	return err;
}

int rsrp_service_sample_and_publish(void)
{
	int err;
	int rsrp_dbm;

	err = rsrp_service_get(&rsrp_dbm);
	if (err) {
		return err;
	}

	LOG_INF("LTE RSRP: %d dBm", rsrp_dbm);
	return publish_rsrp_event(EVT_RSRP_UPDATE, rsrp_dbm);
}


int rsrp_service_init(void)
{
	if (service_initialized) {
		return 0;
	}

	current_mode = RSRP_MODE_IDLE;

	k_work_init_delayable(&rsrp_work, rsrp_work_handler);
	reset_signal_tracking();
	monitor_poll_interval_sec = rsrp_target_poll_interval_sec();

	service_initialized = true;
	return 0;
}

int rsrp_service_start_monitor(void)
{
	if (not service_initialized) {
		return -EINVAL;
	}

	current_mode = RSRP_MODE_MONITOR;
	reset_signal_tracking();
	monitor_poll_interval_sec = rsrp_target_poll_interval_sec();

	LOG_INF("Starting LTE-M RSRP monitor: interval=%u s, motion=%s",
		monitor_poll_interval_sec, rsrp_motion_state_str());

	return k_work_reschedule(&rsrp_work, K_SECONDS(monitor_poll_interval_sec));
}

int rsrp_service_start_probe(uint8_t samples)
{
	if (not service_initialized) {
		return -EINVAL;
	}

	reset_signal_tracking();

	current_mode = RSRP_MODE_PROBE;
	probe_sample_target = samples;

	return k_work_reschedule(&rsrp_work, K_NO_WAIT);
}


int rsrp_service_stop(void)
{
	current_mode = RSRP_MODE_IDLE;
	reset_signal_tracking();
	return k_work_cancel_delayable(&rsrp_work);
}

void rsrp_service_set_motion_hint(bool moving, uint32_t speed_mm_s,
                                  uint32_t linear_accel_mg)
{
	bool state_changed;
	uint32_t next_interval_sec;

	state_changed = (not has_motion_hint) or (motion_hint_is_moving != moving);

	has_motion_hint = true;
	motion_hint_is_moving = moving;
	motion_speed_mm_s = speed_mm_s;
	motion_linear_accel_mg = linear_accel_mg;

	next_interval_sec = rsrp_target_poll_interval_sec();

	if ((not state_changed) and (next_interval_sec == monitor_poll_interval_sec)) {
		return;
	}

	monitor_poll_interval_sec = next_interval_sec;

	LOG_INF("RSRP poll interval %u s (%s, speed=%u mm/s, accel=%u mg)",
		monitor_poll_interval_sec,
		moving ? "moving" : "still",
		motion_speed_mm_s,
		motion_linear_accel_mg);

	if (service_initialized and (current_mode == RSRP_MODE_MONITOR)) {
		(void)k_work_reschedule(&rsrp_work, K_SECONDS(monitor_poll_interval_sec));
	}
}
