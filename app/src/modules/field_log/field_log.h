/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stdint.h>

#include "app_types.h"
#include "app_zbus.h"

enum field_log_location_source {
	FIELD_LOG_LOCATION_NONE = 0,
	FIELD_LOG_LOCATION_LTE,
	FIELD_LOG_LOCATION_GNSS,
	FIELD_LOG_LOCATION_AGNSS,
};

#define FIELD_LOG_RAW_RECORD_SIZE 32U

typedef int (*field_log_raw_record_cb_t)(uint32_t sequence,
					 const uint8_t record[FIELD_LOG_RAW_RECORD_SIZE],
					 void *user_data);

int field_log_start(void);

int field_log_for_each_record_from(uint32_t first_sequence,
				   uint16_t max_records,
				   field_log_raw_record_cb_t callback,
				   void *user_data,
				   uint16_t *records_read);

void field_log_note_battery_sample(const struct app_battery_sample *sample);
void field_log_note_location(enum field_log_location_source source,
			     double latitude,
			     double longitude,
			     float accuracy_m);
void field_log_note_state_change(enum app_state from_state,
				 enum app_state to_state,
				 enum app_evt_type reason,
				 const struct app_ctx *ctx);
