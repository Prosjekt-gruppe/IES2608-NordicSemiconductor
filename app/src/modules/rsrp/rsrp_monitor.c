/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "rsrp_monitor.h"

#include <stddef.h>

#define RSRP_NO_SAMPLE INT32_MIN

void rsrp_monitor_reset(struct rsrp_monitor_state *state)
{
	if (state == NULL) {
		return;
	}

	state->last_rsrp_dbm = RSRP_NO_SAMPLE;
	state->history_next_idx = 0U;
	state->history_count = 0U;
	state->weak_sample_count = 0U;
	state->unavailable_sample_count = 0U;
}

void rsrp_monitor_record_available(struct rsrp_monitor_state *state)
{
	if (state == NULL) {
		return;
	}

	state->unavailable_sample_count = 0U;
}

uint8_t rsrp_monitor_record_unavailable(struct rsrp_monitor_state *state)
{
	if (state == NULL) {
		return 0U;
	}

	state->weak_sample_count = 0U;

	if (state->unavailable_sample_count < UINT8_MAX) {
		state->unavailable_sample_count++;
	}

	return state->unavailable_sample_count;
}

bool rsrp_monitor_unavailable_limit_reached(const struct rsrp_monitor_state *state,
					    const struct rsrp_monitor_config *config)
{
	if ((state == NULL) || (config == NULL)) {
		return false;
	}

	return state->unavailable_sample_count >= config->unavailable_sample_limit;
}

int rsrp_monitor_unavailable_event_value(const struct rsrp_monitor_state *state,
					 const struct rsrp_monitor_config *config)
{
	if ((state != NULL) && (config != NULL) &&
	    (state->last_rsrp_dbm != RSRP_NO_SAMPLE) &&
	    (state->last_rsrp_dbm < config->fallback_dbm)) {
		return state->last_rsrp_dbm;
	}

	if (config == NULL) {
		return 0;
	}

	return config->fallback_dbm;
}

void rsrp_monitor_history_add(struct rsrp_monitor_state *state, int rsrp_dbm)
{
	if (state == NULL) {
		return;
	}

	state->history_dbm[state->history_next_idx] = rsrp_dbm;
	state->history_next_idx = (state->history_next_idx + 1U) % RSRP_MONITOR_HISTORY_LEN;

	if (state->history_count < RSRP_MONITOR_HISTORY_LEN) {
		state->history_count++;
	}
}

uint8_t rsrp_monitor_history_count(const struct rsrp_monitor_state *state)
{
	if (state == NULL) {
		return 0U;
	}

	return state->history_count;
}

int rsrp_monitor_history_average(const struct rsrp_monitor_state *state)
{
	int sum = 0;

	if ((state == NULL) || (state->history_count == 0U)) {
		return 0;
	}

	for (int i = 0; i < state->history_count; i++) {
		sum += state->history_dbm[i];
	}

	return sum / state->history_count;
}

bool rsrp_monitor_trend_worsening(const struct rsrp_monitor_state *state,
				  const struct rsrp_monitor_config *config)
{
	int oldest_idx;
	int middle_idx;
	int newest_idx;
	int total_drop_db;

	if ((state == NULL) || (config == NULL) || (state->history_count < 3U)) {
		return false;
	}

	newest_idx = (state->history_next_idx + RSRP_MONITOR_HISTORY_LEN - 1) %
		     RSRP_MONITOR_HISTORY_LEN;
	middle_idx = (state->history_next_idx + RSRP_MONITOR_HISTORY_LEN - 2) %
		     RSRP_MONITOR_HISTORY_LEN;
	oldest_idx = (state->history_next_idx + RSRP_MONITOR_HISTORY_LEN - 3) %
		     RSRP_MONITOR_HISTORY_LEN;

	if (!((state->history_dbm[newest_idx] < state->history_dbm[middle_idx]) &&
	      (state->history_dbm[middle_idx] < state->history_dbm[oldest_idx]))) {
		return false;
	}

	total_drop_db = state->history_dbm[newest_idx] - state->history_dbm[oldest_idx];
	return total_drop_db <= -config->drop_db;
}

bool rsrp_monitor_record_signal_sample(struct rsrp_monitor_state *state,
				       const struct rsrp_monitor_config *config,
				       int rsrp_dbm)
{
	bool weak_signal;
	bool sharp_drop = false;
	bool weak_for_too_long = false;

	if ((state == NULL) || (config == NULL)) {
		return false;
	}

	weak_signal = rsrp_dbm <= config->fallback_dbm;

	if (weak_signal) {
		if (state->weak_sample_count < UINT8_MAX) {
			state->weak_sample_count++;
		}

		weak_for_too_long = state->weak_sample_count >= config->weak_sample_limit;
	} else {
		state->weak_sample_count = 0U;
	}

	if (state->last_rsrp_dbm != RSRP_NO_SAMPLE) {
		int delta_db = rsrp_dbm - state->last_rsrp_dbm;

		sharp_drop = delta_db <= -config->drop_db;
	}

	rsrp_monitor_history_add(state, rsrp_dbm);
	state->last_rsrp_dbm = rsrp_dbm;

	return weak_signal && (sharp_drop ||
			       rsrp_monitor_trend_worsening(state, config) ||
			       weak_for_too_long);
}
