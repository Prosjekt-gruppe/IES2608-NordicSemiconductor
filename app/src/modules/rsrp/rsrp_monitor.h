/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RSRP_MONITOR_HISTORY_LEN 6

struct rsrp_monitor_config {
	int fallback_dbm;
	int drop_db;
	uint8_t weak_sample_limit;
	uint8_t unavailable_sample_limit;
};

struct rsrp_monitor_state {
	int last_rsrp_dbm;
	int history_dbm[RSRP_MONITOR_HISTORY_LEN];
	uint8_t history_next_idx;
	uint8_t history_count;
	uint8_t weak_sample_count;
	uint8_t unavailable_sample_count;
};

void rsrp_monitor_reset(struct rsrp_monitor_state *state);
void rsrp_monitor_record_available(struct rsrp_monitor_state *state);
uint8_t rsrp_monitor_record_unavailable(struct rsrp_monitor_state *state);
bool rsrp_monitor_unavailable_limit_reached(const struct rsrp_monitor_state *state,
					    const struct rsrp_monitor_config *config);
int rsrp_monitor_unavailable_event_value(const struct rsrp_monitor_state *state,
					 const struct rsrp_monitor_config *config);

void rsrp_monitor_history_add(struct rsrp_monitor_state *state, int rsrp_dbm);
uint8_t rsrp_monitor_history_count(const struct rsrp_monitor_state *state);
int rsrp_monitor_history_average(const struct rsrp_monitor_state *state);
bool rsrp_monitor_trend_worsening(const struct rsrp_monitor_state *state,
				  const struct rsrp_monitor_config *config);
bool rsrp_monitor_record_signal_sample(struct rsrp_monitor_state *state,
				       const struct rsrp_monitor_config *config,
				       int rsrp_dbm);
