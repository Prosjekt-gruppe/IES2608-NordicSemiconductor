/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "rsrp_service.h"
#include "app_events.h"

#if defined(CONFIG_APP_FIELD_LOG)
#include "field_log.h"
#endif

#include <errno.h>
#include <iso646.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <modem/lte_lc.h>
#include <nrf_modem_at.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#ifndef CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC
#define CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC \
	CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC
#endif

#ifndef CONFIG_APP_MODEM_RSRP_RECOVERY_DBM
#define CONFIG_APP_MODEM_RSRP_RECOVERY_DBM \
	CONFIG_APP_MODEM_RSRP_FALLBACK_DBM
#endif

LOG_MODULE_REGISTER(rsrp_service, LOG_LEVEL_INF);

#define RSRP_HISTORY_LEN 6
#define PROBE_POLL_MSEC 500
#define RSRP_UNKNOWN_DBM CONFIG_APP_MODEM_RSRP_FALLBACK_DBM

enum rsrp_mode {
	RSRP_MODE_IDLE,		
	RSRP_MODE_MONITOR,
	RSRP_MODE_PROBE,
	RSRP_MODE_NTN_MONITOR,
};

static enum rsrp_mode current_mode;
static uint8_t probe_sample_target;

static struct k_work_delayable rsrp_work;
static bool service_initialized;
static bool lte_poor_event_sent;
static bool has_motion_hint;
static bool motion_hint_is_moving;
static int last_rsrp_dbm = INT32_MIN;
static uint32_t motion_speed_mm_s;
static uint32_t motion_linear_accel_mg;
static uint32_t monitor_poll_interval_sec = CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_SEC;

static int rsrp_history_dbm[RSRP_HISTORY_LEN];
static uint8_t rsrp_history_next_idx;
static uint8_t rsrp_history_count;
static uint8_t weak_sample_count;
static uint8_t unavailable_sample_count;

struct ntn_monitor_sample {
	int reg_status;
	char plmn[8];
	char tac[8];
	int act;
	int band;
	char cell_id[16];
	int phys_cell_id;
	int earfcn;
	int rsrp;
	int snr;
};

static void reset_signal_tracking(void)
{
	last_rsrp_dbm = INT32_MIN;
	rsrp_history_next_idx = 0U;
	rsrp_history_count = 0U;
	weak_sample_count = 0U;
	unavailable_sample_count = 0U;
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

static void rsrp_history_add(int rsrp_dbm)
{
	rsrp_history_dbm[rsrp_history_next_idx] = rsrp_dbm;
	rsrp_history_next_idx = (rsrp_history_next_idx + 1U) % RSRP_HISTORY_LEN;

	if (rsrp_history_count < RSRP_HISTORY_LEN) {
		rsrp_history_count++;
	}
}

static int rsrp_history_average(void)
{
	int sum = 0;

	for (int i = 0; i < rsrp_history_count; i++) {
		sum += rsrp_history_dbm[i];
	}

	return sum / rsrp_history_count;
}

static bool rsrp_trend_worsening(void)
{
	int oldest_idx;
	int middle_idx;
	int newest_idx;
	int total_drop_db;

	if (rsrp_history_count < 3U) {
		return false;
	}

	newest_idx = (rsrp_history_next_idx + RSRP_HISTORY_LEN - 1) % RSRP_HISTORY_LEN;
	middle_idx = (rsrp_history_next_idx + RSRP_HISTORY_LEN - 2) % RSRP_HISTORY_LEN;
	oldest_idx = (rsrp_history_next_idx + RSRP_HISTORY_LEN - 3) % RSRP_HISTORY_LEN;

	if (not ((rsrp_history_dbm[newest_idx] < rsrp_history_dbm[middle_idx]) and
		 (rsrp_history_dbm[middle_idx] < rsrp_history_dbm[oldest_idx]))) {
		return false;
	}

	total_drop_db = rsrp_history_dbm[newest_idx] - rsrp_history_dbm[oldest_idx];
	if (total_drop_db > -CONFIG_APP_MODEM_RSRP_DROP_DB) {
		return false;
	}

	LOG_WRN("LTE-M RSRP worsening: %d -> %d -> %d dBm",
		rsrp_history_dbm[oldest_idx],
		rsrp_history_dbm[middle_idx],
		rsrp_history_dbm[newest_idx]);

	return true;
}

static bool should_publish_lte_poor(int rsrp_dbm)
{
	bool weak_signal = rsrp_dbm <= CONFIG_APP_MODEM_RSRP_FALLBACK_DBM;
	bool sharp_drop = false;
	bool worsening;
	bool weak_for_too_long = false;

	if (weak_signal) {
		if (weak_sample_count < UINT8_MAX) {
			weak_sample_count++;
		}

		if (weak_sample_count >= CONFIG_APP_MODEM_RSRP_WEAK_SAMPLE_COUNT) {
			LOG_WRN("LTE-M RSRP weak for %u/%u samples: %d dBm",
				(unsigned int)weak_sample_count,
				(unsigned int)CONFIG_APP_MODEM_RSRP_WEAK_SAMPLE_COUNT,
				rsrp_dbm);
			weak_for_too_long = true;
		}
	} else {
		weak_sample_count = 0U;
	}

	if (last_rsrp_dbm != INT32_MIN) {
		int delta_db = rsrp_dbm - last_rsrp_dbm;

		if (delta_db <= -CONFIG_APP_MODEM_RSRP_DROP_DB) {
			LOG_WRN("LTE-M RSRP dropped %d dB (%d -> %d)",
				-delta_db, last_rsrp_dbm, rsrp_dbm);
			sharp_drop = true;
		}
	}

	rsrp_history_add(rsrp_dbm);
	worsening = rsrp_trend_worsening();
	last_rsrp_dbm = rsrp_dbm;

	return weak_signal and (sharp_drop or worsening or weak_for_too_long);
}

static void record_rsrp_available(void)
{
	unavailable_sample_count = 0U;
}

static uint8_t record_rsrp_unavailable(void)
{
	weak_sample_count = 0U;

	if (unavailable_sample_count < UINT8_MAX) {
		unavailable_sample_count++;
	}

	return unavailable_sample_count;
}

static int rsrp_event_value_for_unavailable(void)
{
	if ((last_rsrp_dbm != INT32_MIN) and (last_rsrp_dbm < RSRP_UNKNOWN_DBM)) {
		return last_rsrp_dbm;
	}

	return RSRP_UNKNOWN_DBM;
}

static bool rsrp_unavailable_limit_reached(void)
{
	return unavailable_sample_count >= CONFIG_APP_MODEM_RSRP_LOSS_SAMPLE_COUNT;
}

static int rsrp_service_conn_eval_get(struct lte_lc_conn_eval_params *params)
{
	int err;

	if (params == NULL) {
		return -EINVAL;
	}

	err = lte_lc_conn_eval_params_get(params);
	if (err) {
		LOG_WRN("conn eval get failed: %d", err);
		return err;
	}

	LOG_INF("conn eval: rrc=%d energy=%d ce=%d",
		params->rrc_state, params->energy_estimate, params->ce_level);
	LOG_INF("conn eval: rsrp=%d rsrq=%d snr=%d dl_pl=%d tx_pwr=%d tx_rep=%d rx_rep=%d",
		params->rsrp, params->rsrq, params->snr, params->dl_pathloss,
		params->tx_power, params->tx_rep, params->rx_rep);
	LOG_INF("conn eval: earfcn=%d band=%d phy_cid=%d cell_id=%u mcc=%d mnc=%d",
		params->earfcn, params->band, params->phy_cid, params->cell_id,
		params->mcc, params->mnc);

#if defined(CONFIG_APP_FIELD_LOG) && defined(CONFIG_APP_FIELD_LOG_CONN_EVAL)
	field_log_note_conn_eval(params);
#endif

	return 0;
}

static bool parse_int_token(const char *token, int *value)
{
	char *end = NULL;
	long parsed;

	if (token == NULL || value == NULL) {
		return false;
	}

	parsed = strtol(token, &end, 10);
	if (end == token) {
		return false;
	}

	*value = (int)parsed;
	return true;
}

static void strip_quotes_inplace(char *token)
{
	size_t len;

	if (token == NULL) {
		return;
	}

	len = strlen(token);
	if (len >= 2U && token[0] == '"' && token[len - 1U] == '"') {
		memmove(token, token + 1U, len - 2U);
		token[len - 2U] = '\0';
	}
}

/* This parser handles the observed NTN %XMONITOR response format used for observability only. */
static int rsrp_service_ntn_monitor_get(struct ntn_monitor_sample *sample)
{
	char response[256] = {0};
	char payload[256] = {0};
	char *prefix;
	char *cursor;
	char *saveptr = NULL;
	char *tokens[16] = {0};
	int token_count = 0;
	int err;

	if (sample == NULL) {
		return -EINVAL;
	}

	err = nrf_modem_at_cmd(response, sizeof(response), "AT%%XMONITOR");
	if (err) {
		if (err > 0) {
			LOG_WRN("AT%%XMONITOR failed: raw=%d type=%d at_err=%d",
				err, nrf_modem_at_err_type(err), nrf_modem_at_err(err));
		} else {
			LOG_WRN("AT%%XMONITOR failed: lib err=%d", err);
		}
		return err;
	}

	prefix = strstr(response, "%XMONITOR:");
	if (prefix == NULL) {
		/* TODO: Confirm %XMONITOR response format for NTN firmware. */
		return -ENOTSUP;
	}

	cursor = prefix + strlen("%XMONITOR:");
	while (*cursor == ' ') {
		cursor++;
	}

	strncpy(payload, cursor, sizeof(payload) - 1U);
	for (char *p = payload; *p != '\0'; p++) {
		if (*p == '\r' || *p == '\n') {
			*p = '\0';
			break;
		}
	}

	for (char *token = strtok_r(payload, ",", &saveptr);
	     token != NULL && token_count < (int)ARRAY_SIZE(tokens);
	     token = strtok_r(NULL, ",", &saveptr)) {
		tokens[token_count++] = token;
	}

	if (token_count < 12) {
		LOG_WRN("XMONITOR parse failed: token_count=%d raw=%s", token_count, response);
		return -ENOTSUP;
	}

	strip_quotes_inplace(tokens[3]);
	strip_quotes_inplace(tokens[4]);
	strip_quotes_inplace(tokens[7]);

	/* Expected order: reg_status, full_name, short_name, plmn, tac, act, band,
	 * cell_id, phys_cell_id, earfcn, rsrp, snr.
	 */
	if (not parse_int_token(tokens[0], &sample->reg_status)) {
		LOG_WRN("XMONITOR parse failed: reg token[0]=%s raw=%s", tokens[0], response);
		return -ENOTSUP;
	}
	if (not parse_int_token(tokens[5], &sample->act)) {
		LOG_WRN("XMONITOR parse failed: act token[5]=%s raw=%s", tokens[5], response);
		return -ENOTSUP;
	}
	if (not parse_int_token(tokens[6], &sample->band)) {
		LOG_WRN("XMONITOR parse failed: band token[6]=%s raw=%s", tokens[6], response);
		return -ENOTSUP;
	}
	if (not parse_int_token(tokens[8], &sample->phys_cell_id)) {
		LOG_WRN("XMONITOR parse failed: pci token[8]=%s raw=%s", tokens[8], response);
		return -ENOTSUP;
	}
	if (not parse_int_token(tokens[9], &sample->earfcn)) {
		LOG_WRN("XMONITOR parse failed: earfcn token[9]=%s raw=%s", tokens[9], response);
		return -ENOTSUP;
	}
	if (not parse_int_token(tokens[10], &sample->rsrp)) {
		LOG_WRN("XMONITOR parse failed: rsrp token[10]=%s raw=%s", tokens[10], response);
		return -ENOTSUP;
	}
	if (not parse_int_token(tokens[11], &sample->snr)) {
		LOG_WRN("XMONITOR parse failed: snr token[11]=%s raw=%s", tokens[11], response);
		return -ENOTSUP;
	}

	strncpy(sample->plmn, tokens[3], sizeof(sample->plmn) - 1U);
	strncpy(sample->tac, tokens[4], sizeof(sample->tac) - 1U);
	strncpy(sample->cell_id, tokens[7], sizeof(sample->cell_id) - 1U);

	return 0;
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
			(void)record_rsrp_unavailable();
			monitor_poll_interval_sec = rsrp_target_poll_interval_sec();

			LOG_WRN("LTE-M RSRP unavailable: err=%d, count=%u/%u, next=%u s",
				err,
				(unsigned int)unavailable_sample_count,
				(unsigned int)CONFIG_APP_MODEM_RSRP_LOSS_SAMPLE_COUNT,
				monitor_poll_interval_sec);

			if ((not lte_poor_event_sent) and rsrp_unavailable_limit_reached()) {
				lte_poor_event_sent = true;
				current_mode = RSRP_MODE_IDLE;
				rsrp_dbm = rsrp_event_value_for_unavailable();

				LOG_WRN("LTE-M RSRP unavailable for %u samples, publishing LTE poor event",
					(unsigned int)unavailable_sample_count);
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
			(void)record_rsrp_unavailable();

			LOG_WRN("LTE probe RSRP unavailable: err=%d, count=%u/%u",
				err,
				(unsigned int)unavailable_sample_count,
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

		rsrp_history_add(rsrp_dbm);

		LOG_INF("LTE probe RSRP sample %u/%u: %d dBm",
			rsrp_history_count, probe_sample_target, rsrp_dbm);

		if (rsrp_history_count < probe_sample_target) {
			k_work_reschedule(&rsrp_work, K_MSEC(PROBE_POLL_MSEC));
			return;
		}

		probe_average_dbm = rsrp_history_average();

		if (probe_average_dbm >= CONFIG_APP_MODEM_RSRP_RECOVERY_DBM) {
			publish_rsrp_event(EVT_LTE_GOOD, probe_average_dbm);
		} else {
			publish_rsrp_event(EVT_LTE_POOR, probe_average_dbm);
		}

		LOG_INF("LTE probe complete: avg=%d dBm over %u samples",
			probe_average_dbm, probe_sample_target);
		current_mode = RSRP_MODE_IDLE;
		return;

	case RSRP_MODE_NTN_MONITOR: {
		struct ntn_monitor_sample sample = {0};
		uint32_t next_interval_sec;

		err = rsrp_service_ntn_monitor_get(&sample);
		next_interval_sec = monitor_poll_interval_sec;
		if (next_interval_sec == 0U) {
			next_interval_sec = CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC;
		}

		if (err) {
			LOG_WRN("NTN monitor read failed: err=%d, next=%u s",
				err, next_interval_sec);
			(void)k_work_reschedule(&rsrp_work, K_SECONDS(next_interval_sec));
			return;
		}

		LOG_INF("NTN monitor: reg=%d act=%d plmn=%s tac=%s band=%d cell=%s pci=%d earfcn=%d rsrp=%d snr=%d",
			sample.reg_status, sample.act, sample.plmn, sample.tac, sample.band,
			sample.cell_id, sample.phys_cell_id, sample.earfcn,
			sample.rsrp, sample.snr);

		(void)k_work_reschedule(&rsrp_work, K_SECONDS(next_interval_sec));
		return;
	}

	case RSRP_MODE_IDLE:
	default:
		return;
	}
}


int rsrp_service_get(int *rsrp_dbm)
{
	struct lte_lc_conn_eval_params params = {0};
	int err;

	if (rsrp_dbm == NULL) {
		return -EINVAL;
	}

	err = rsrp_service_conn_eval_get(&params);
	if (err) {
		return err;
	}

	if (params.rsrp == LTE_LC_CELL_RSRP_INVALID) {
		LOG_WRN("RSRP not known");
		return -ENOENT;
	}

	if (params.rsrp == 0) {
		LOG_WRN("RSRP < -140 dBm");
		*rsrp_dbm = -141;
		return 0;
	}

	*rsrp_dbm = params.rsrp - 141;

	return 0;
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

	if (current_mode == RSRP_MODE_MONITOR) {
		LOG_INF("LTE-M RSRP monitor already running");
		monitor_poll_interval_sec = rsrp_target_poll_interval_sec();
		return k_work_reschedule(&rsrp_work, K_SECONDS(monitor_poll_interval_sec));
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

int rsrp_service_start_ntn_monitor(void)
{
	if (not service_initialized) {
		return -EINVAL;
	}

	if (current_mode == RSRP_MODE_NTN_MONITOR) {
		LOG_INF("NTN monitor already running");
		if (monitor_poll_interval_sec == 0U) {
			monitor_poll_interval_sec = CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC;
		}
		return k_work_reschedule(&rsrp_work, K_SECONDS(monitor_poll_interval_sec));
	}

	current_mode = RSRP_MODE_NTN_MONITOR;
	reset_signal_tracking();
	if (monitor_poll_interval_sec == 0U) {
		monitor_poll_interval_sec = CONFIG_APP_MODEM_SIGNAL_POLL_INTERVAL_STILL_SEC;
	}

	LOG_INF("Starting NTN monitor: interval=%u s", monitor_poll_interval_sec);

	return k_work_reschedule(&rsrp_work, K_SECONDS(monitor_poll_interval_sec));
}


int rsrp_service_stop(void)
{
	if (current_mode == RSRP_MODE_IDLE) {
		LOG_INF("LTE-M RSRP monitor already stopped");
		// redundant?
		//reset_signal_tracking();
		return 0;
	}

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

	if (service_initialized and (current_mode == RSRP_MODE_MONITOR)) {
		LOG_INF("RSRP poll interval %u s (%s, speed=%u mm/s, accel=%u mg)",
			monitor_poll_interval_sec,
			moving ? "moving" : "still",
			motion_speed_mm_s,
			motion_linear_accel_mg);
		(void)k_work_reschedule(&rsrp_work, K_SECONDS(monitor_poll_interval_sec));
	}
}
