/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "field_log.h"

#include "app_events.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <pm_config.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_APP_FIELD_LOG_SHELL)
#include <zephyr/shell/shell.h>
#endif

LOG_MODULE_REGISTER(field_log, LOG_LEVEL_INF);

#define FIELD_LOG_STACK_SIZE 1536
#define FIELD_LOG_PRIORITY 8
#define FIELD_LOG_MSGQ_LEN 24

#define FIELD_LOG_RECORD_MAGIC 0x464cU
#define FIELD_LOG_RECORD_VERSION 2U
#define FIELD_LOG_RECORD_SIZE 32U

#define FIELD_LOG_COORDINATE_SCALE 1000000
#define FIELD_LOG_LOCATION_STALE_MS (20 * 60 * 1000LL)
#define FIELD_LOG_LOCATION_REFRESH_GRACE_MS (2 * 60 * 1000LL)
#define FIELD_LOG_LOCATION_ACCURACY_UNKNOWN UINT16_MAX
#define FIELD_LOG_RSRP_UNKNOWN INT16_MIN

#define FIELD_LOG_SUMMARY_FLAG_NO_BATTERY_SAMPLES BIT(0)
#define FIELD_LOG_SUMMARY_FLAG_VBUS_SEEN BIT(1)
#define FIELD_LOG_SUMMARY_FLAG_STORAGE_DISABLED BIT(2)

#define FIELD_LOG_CSV_HEADER                                                        \
	"type,session,seq,uptime_s,from_state,to_state,reason,active_rat,next_rat," \
	"location_source,latitude,longitude,accuracy_m,last_rsrp_dbm,"             \
	"interval_s,power_interval_uwh,power_total_uwh,lte_losses_interval,"       \
	"lte_losses_total,switchbacks_interval,switchbacks_total,flags,"           \
	"dropped_messages"

enum field_log_record_type {
	FIELD_LOG_RECORD_TYPE_STATE_CHANGE = 1,
	FIELD_LOG_RECORD_TYPE_SUMMARY = 2,
};

enum field_log_message_type {
	FIELD_LOG_MSG_BATTERY_SAMPLE = 1,
	FIELD_LOG_MSG_LOCATION_UPDATE,
	FIELD_LOG_MSG_STATE_CHANGE,
};

enum field_log_slot_state {
	FIELD_LOG_SLOT_VALID,
	FIELD_LOG_SLOT_EMPTY,
	FIELD_LOG_SLOT_INVALID,
};

struct field_log_record_header {
	uint16_t magic;
	uint8_t version;
	uint8_t type;
	uint32_t sequence;
	uint32_t uptime_s;
} __packed;

struct field_log_state_record_payload {
	uint8_t from_state;
	uint8_t to_state;
	uint8_t reason_evt;
	uint8_t active_rat;
	uint8_t next_rat;
	uint8_t location_source;
	int16_t last_rsrp_dbm;
	int32_t latitude_e6;
	int32_t longitude_e6;
	uint16_t accuracy_m;
} __packed;

struct field_log_summary_record_payload {
	uint16_t interval_s;
	uint32_t power_total_uwh;
	uint32_t power_interval_uwh;
	uint16_t lte_losses_total;
	uint8_t lte_losses_interval;
	uint16_t switchbacks_total;
	uint8_t switchbacks_interval;
	uint8_t flags;
	uint8_t dropped_messages;
} __packed;

struct field_log_record {
	struct field_log_record_header header;
	union {
		struct field_log_state_record_payload state;
		struct field_log_summary_record_payload summary;
	} payload;
	uint16_t crc16;
} __packed;

struct field_log_storage_state {
	const struct flash_area *area;
	size_t capacity_bytes;
	off_t write_offset;
	uint32_t next_sequence;
	bool ready;
	bool full;
};

struct field_log_battery_accumulator {
	bool have_last_sample;
	struct app_battery_sample last_sample;
	int64_t last_energy_timestamp_ms;
	uint32_t sample_count;
	uint32_t vbus_sample_count;
	int64_t discharge_energy_interval_uwh;
	int64_t discharge_energy_total_uwh;
};

struct field_log_location_sample {
	bool valid;
	enum field_log_location_source source;
	int64_t timestamp_ms;
	int32_t latitude_e6;
	int32_t longitude_e6;
	uint16_t accuracy_m;
};

struct field_log_runtime_state {
	int64_t summary_start_ms;
	struct field_log_battery_accumulator battery;
	struct field_log_location_sample locations[FIELD_LOG_LOCATION_AGNSS + 1];
	uint16_t lte_losses_total;
	uint8_t lte_losses_interval;
	uint16_t switchbacks_total;
	uint8_t switchbacks_interval;
};

struct field_log_battery_message {
	struct app_battery_sample sample;
};

struct field_log_location_message {
	enum field_log_location_source source;
	int64_t timestamp_ms;
	int32_t latitude_e6;
	int32_t longitude_e6;
	uint16_t accuracy_m;
};

struct field_log_state_change_message {
	enum app_state from_state;
	enum app_state to_state;
	enum app_evt_type reason_evt;
	enum rat active_rat;
	enum rat next_rat;
	int16_t last_rsrp_dbm;
	int64_t timestamp_ms;
};

struct field_log_message {
	enum field_log_message_type type;
	union {
		struct field_log_battery_message battery;
		struct field_log_location_message location;
		struct field_log_state_change_message state;
	} data;
};

BUILD_ASSERT(sizeof(struct field_log_state_record_payload) == 18);
BUILD_ASSERT(sizeof(struct field_log_summary_record_payload) == 18);
BUILD_ASSERT(sizeof(struct field_log_record) == FIELD_LOG_RECORD_SIZE);
BUILD_ASSERT(FIELD_LOG_RECORD_SIZE == FIELD_LOG_RAW_RECORD_SIZE);
BUILD_ASSERT((CONFIG_APP_FIELD_LOG_ERASE_BLOCK_SIZE % FIELD_LOG_RECORD_SIZE) == 0);

static K_THREAD_STACK_DEFINE(field_log_stack, FIELD_LOG_STACK_SIZE);
static struct k_thread field_log_thread_data;
static bool field_log_started;

K_MSGQ_DEFINE(field_log_msgq, sizeof(struct field_log_message), FIELD_LOG_MSGQ_LEN, 4);
K_MUTEX_DEFINE(field_log_storage_mutex);

static struct field_log_storage_state storage;
static struct field_log_runtime_state runtime;
static atomic_t field_log_dropped_messages;

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xffffU;

	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;

		for (int bit = 0; bit < 8; bit++) {
			if ((crc & 0x8000U) != 0U) {
				crc = (crc << 1) ^ 0x1021U;
			} else {
				crc <<= 1;
			}
		}
	}

	return crc;
}

static bool record_is_erased(const struct field_log_record *record)
{
	const uint8_t *bytes = (const uint8_t *)record;

	for (size_t i = 0; i < sizeof(*record); i++) {
		if (bytes[i] != 0xffU) {
			return false;
		}
	}

	return true;
}

static bool record_type_is_valid(uint8_t type)
{
	return type == FIELD_LOG_RECORD_TYPE_STATE_CHANGE ||
	       type == FIELD_LOG_RECORD_TYPE_SUMMARY;
}

static bool record_is_valid(const struct field_log_record *record)
{
	uint16_t expected_crc;

	if (record->header.magic != FIELD_LOG_RECORD_MAGIC ||
	    record->header.version != FIELD_LOG_RECORD_VERSION ||
	    !record_type_is_valid(record->header.type)) {
		return false;
	}

	expected_crc = crc16_ccitt((const uint8_t *)record,
				   offsetof(struct field_log_record, crc16));

	return record->crc16 == expected_crc;
}

static int64_t clamp_i64(int64_t value, int64_t min_value, int64_t max_value)
{
	if (value < min_value) {
		return min_value;
	}

	if (value > max_value) {
		return max_value;
	}

	return value;
}

static int16_t clamp_s16(int64_t value)
{
	return (int16_t)clamp_i64(value, INT16_MIN, INT16_MAX);
}

static uint16_t clamp_u16(int64_t value)
{
	return (uint16_t)clamp_i64(value, 0, UINT16_MAX);
}

static int32_t clamp_s32(int64_t value)
{
	return (int32_t)clamp_i64(value, INT32_MIN, INT32_MAX);
}

static uint32_t clamp_u32(int64_t value)
{
	return (uint32_t)clamp_i64(value, 0, UINT32_MAX);
}

static int32_t degrees_to_e6(double degrees)
{
	double scaled = degrees * FIELD_LOG_COORDINATE_SCALE;
	double rounded = scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5;

	return clamp_s32((int64_t)rounded);
}

static uint16_t accuracy_to_u16(float accuracy_m)
{
	if (accuracy_m <= 0.0f) {
		return FIELD_LOG_LOCATION_ACCURACY_UNKNOWN;
	}

	if (accuracy_m >= (float)(UINT16_MAX - 1U)) {
		return UINT16_MAX - 1U;
	}

	return (uint16_t)(accuracy_m + 0.5f);
}

static bool field_log_storage_available(void)
{
	return storage.area != NULL && storage.capacity_bytes >= FIELD_LOG_RECORD_SIZE;
}

static void field_log_storage_reset_cursor(void)
{
	storage.write_offset = 0;
	storage.next_sequence = 0;
	storage.full = false;
}

static int field_log_storage_read_slot(off_t offset,
				       struct field_log_record *record,
				       enum field_log_slot_state *slot_state)
{
	int err = flash_area_read(storage.area, offset, record, sizeof(*record));

	if (err) {
		LOG_ERR("Field log read failed at 0x%lx: %d", (long)offset, err);
		return err;
	}

	if (record_is_erased(record)) {
		*slot_state = FIELD_LOG_SLOT_EMPTY;
		return 0;
	}

	if (!record_is_valid(record)) {
		*slot_state = FIELD_LOG_SLOT_INVALID;
		return 0;
	}

	*slot_state = FIELD_LOG_SLOT_VALID;
	return 0;
}

static int field_log_storage_erase_all(void)
{
	for (off_t offset = 0; offset < storage.capacity_bytes;
	     offset += CONFIG_APP_FIELD_LOG_ERASE_BLOCK_SIZE) {
		int err = flash_area_erase(storage.area, offset,
					   CONFIG_APP_FIELD_LOG_ERASE_BLOCK_SIZE);

		if (err) {
			LOG_ERR("Field log erase failed at 0x%lx: %d",
				(long)offset, err);
			return err;
		}
	}

	return 0;
}

static int field_log_storage_scan(void)
{
	struct field_log_record record;
	enum field_log_slot_state slot_state;

	field_log_storage_reset_cursor();

	for (off_t offset = 0; offset + sizeof(record) <= storage.capacity_bytes;
	     offset += sizeof(record)) {
		int err = field_log_storage_read_slot(offset, &record, &slot_state);

		if (err) {
			return err;
		}

		if (slot_state == FIELD_LOG_SLOT_EMPTY) {
			storage.write_offset = offset;
			return 0;
		}

		if (slot_state == FIELD_LOG_SLOT_INVALID) {
			LOG_WRN("Field log scan stopped at invalid record 0x%lx",
				(long)offset);
			storage.full = true;
			storage.write_offset = offset;
			return -EBADMSG;
		}

		storage.next_sequence = record.header.sequence + 1U;
		storage.write_offset = offset + sizeof(record);
	}

	storage.full = true;
	return 0;
}

static int field_log_storage_init(void)
{
	int err;

#if !defined(PM_EXTERNAL_FLASH_ID)
	LOG_WRN("Field log storage disabled: no PM_EXTERNAL_FLASH_ID partition");
	return -ENODEV;
#else
	err = flash_area_open(PM_EXTERNAL_FLASH_ID, &storage.area);
	if (err) {
		LOG_WRN("Field log storage disabled: flash_area_open failed: %d", err);
		return err;
	}

	storage.capacity_bytes = MIN((size_t)storage.area->fa_size,
				     (size_t)CONFIG_APP_FIELD_LOG_STORAGE_BYTES);
	storage.capacity_bytes -= storage.capacity_bytes %
				  CONFIG_APP_FIELD_LOG_ERASE_BLOCK_SIZE;

	if (storage.capacity_bytes < sizeof(struct field_log_record)) {
		LOG_WRN("Field log storage disabled: capacity too small");
		return -ENOSPC;
	}

	if (IS_ENABLED(CONFIG_APP_FIELD_LOG_ERASE_ON_BOOT)) {
		err = field_log_storage_erase_all();
		if (err) {
			return err;
		}
	}

	err = field_log_storage_scan();
	if ((err == -EBADMSG) && (storage.write_offset == 0)) {
		LOG_WRN("Field log storage has no valid first record; erasing area");
		storage.full = false;

		err = field_log_storage_erase_all();
		if (err) {
			return err;
		}

		err = field_log_storage_scan();
	}

	if (err == -EBADMSG) {
		LOG_WRN("Field log append disabled to preserve corrupted data");
		return err;
	}

	if (err) {
		return err;
	}

	storage.ready = !storage.full;

	LOG_INF("Field log storage: area=%u offset=0x%lx size=%zu next=0x%lx seq=%u",
		PM_EXTERNAL_FLASH_ID,
		(long)storage.area->fa_off,
		storage.capacity_bytes,
		(long)storage.write_offset,
		storage.next_sequence);

	return 0;
#endif
}

static int field_log_storage_append_record(const struct field_log_record *record)
{
	int err;

	if (!storage.ready) {
		return -ENODEV;
	}

	if (storage.write_offset + sizeof(*record) > storage.capacity_bytes) {
		storage.full = true;
		storage.ready = false;
		LOG_WRN("Field log storage full at 0x%lx", (long)storage.write_offset);
		return -ENOSPC;
	}

	if ((storage.write_offset % CONFIG_APP_FIELD_LOG_ERASE_BLOCK_SIZE) == 0) {
		err = flash_area_erase(storage.area, storage.write_offset,
				       CONFIG_APP_FIELD_LOG_ERASE_BLOCK_SIZE);
		if (err) {
			LOG_ERR("Field log sector erase failed at 0x%lx: %d",
				(long)storage.write_offset, err);
			return err;
		}
	}

	err = flash_area_write(storage.area, storage.write_offset, record, sizeof(*record));
	if (err) {
		LOG_ERR("Field log write failed at 0x%lx: %d",
			(long)storage.write_offset, err);
		return err;
	}

	storage.write_offset += sizeof(*record);
	storage.next_sequence++;

	return 0;
}

static void field_log_runtime_reset_interval(int64_t start_ms)
{
	runtime.summary_start_ms = start_ms;
	runtime.battery.sample_count = 0;
	runtime.battery.vbus_sample_count = 0;
	runtime.battery.discharge_energy_interval_uwh = 0;
	runtime.lte_losses_interval = 0;
	runtime.switchbacks_interval = 0;
}

static void battery_accumulator_integrate_until(int64_t timestamp_ms)
{
	int64_t delta_ms;
	int64_t discharge_energy_uwh;

	if (!runtime.battery.have_last_sample) {
		return;
	}

	if (timestamp_ms <= runtime.battery.last_energy_timestamp_ms) {
		return;
	}

	delta_ms = timestamp_ms - runtime.battery.last_energy_timestamp_ms;

	if (runtime.battery.last_sample.current_ma < 0) {
		discharge_energy_uwh =
			(runtime.battery.last_sample.voltage_mv *
			 -runtime.battery.last_sample.current_ma *
			 delta_ms) / 3600000LL;
		runtime.battery.discharge_energy_interval_uwh += discharge_energy_uwh;
		runtime.battery.discharge_energy_total_uwh += discharge_energy_uwh;
	}

	runtime.battery.last_energy_timestamp_ms = timestamp_ms;
}

static void field_log_process_battery_sample(const struct app_battery_sample *sample)
{
	int64_t sample_time_ms = sample->timestamp_ms;

	if (sample_time_ms <= 0) {
		sample_time_ms = k_uptime_get();
	}

	battery_accumulator_integrate_until(sample_time_ms);

	runtime.battery.have_last_sample = true;
	runtime.battery.last_sample = *sample;
	runtime.battery.last_sample.timestamp_ms = sample_time_ms;
	runtime.battery.last_energy_timestamp_ms = sample_time_ms;
	runtime.battery.sample_count++;

	if (sample->vbus_present) {
		runtime.battery.vbus_sample_count++;
	}
}

static int field_log_location_source_rank(enum field_log_location_source source)
{
	switch (source) {
	case FIELD_LOG_LOCATION_AGNSS:
		return 4;
	case FIELD_LOG_LOCATION_GNSS:
		return 3;
	case FIELD_LOG_LOCATION_LTE:
		return 1;
	case FIELD_LOG_LOCATION_NONE:
	default:
		return 0;
	}
}

static bool field_log_location_is_stale(const struct field_log_location_sample *sample,
					int64_t now_ms)
{
	if (!sample->valid) {
		return true;
	}

	return now_ms - sample->timestamp_ms > FIELD_LOG_LOCATION_STALE_MS;
}

static bool field_log_accuracy_is_known(const struct field_log_location_sample *sample)
{
	return sample->accuracy_m != FIELD_LOG_LOCATION_ACCURACY_UNKNOWN;
}

static const struct field_log_location_sample *field_log_select_best_location(int64_t now_ms)
{
	const struct field_log_location_sample *best = NULL;

	for (size_t i = FIELD_LOG_LOCATION_LTE; i <= FIELD_LOG_LOCATION_AGNSS; i++) {
		const struct field_log_location_sample *candidate = &runtime.locations[i];
		bool candidate_stale;
		bool best_stale;

		if (!candidate->valid) {
			continue;
		}

		if (best == NULL) {
			best = candidate;
			continue;
		}

		candidate_stale = field_log_location_is_stale(candidate, now_ms);
		best_stale = field_log_location_is_stale(best, now_ms);

		if (candidate_stale != best_stale) {
			if (!candidate_stale) {
				best = candidate;
			}
			continue;
		}

		if (field_log_accuracy_is_known(candidate) &&
		    field_log_accuracy_is_known(best)) {
			if (candidate->accuracy_m + 25U < best->accuracy_m) {
				best = candidate;
				continue;
			}

			if (best->accuracy_m + 25U < candidate->accuracy_m) {
				continue;
			}
		} else if (field_log_accuracy_is_known(candidate) !=
			   field_log_accuracy_is_known(best)) {
			if (field_log_accuracy_is_known(candidate)) {
				best = candidate;
			}
			continue;
		}

		if (candidate->timestamp_ms >
		    best->timestamp_ms + FIELD_LOG_LOCATION_REFRESH_GRACE_MS) {
			best = candidate;
			continue;
		}

		if (best->timestamp_ms >
		    candidate->timestamp_ms + FIELD_LOG_LOCATION_REFRESH_GRACE_MS) {
			continue;
		}

		if (field_log_location_source_rank(candidate->source) >
		    field_log_location_source_rank(best->source)) {
			best = candidate;
			continue;
		}

		if (candidate->timestamp_ms > best->timestamp_ms) {
			best = candidate;
		}
	}

	return best;
}

static void field_log_process_location_update(const struct field_log_location_message *update)
{
	struct field_log_location_sample *slot;

	if (update->source <= FIELD_LOG_LOCATION_NONE ||
	    update->source > FIELD_LOG_LOCATION_AGNSS) {
		return;
	}

	slot = &runtime.locations[update->source];
	slot->valid = true;
	slot->source = update->source;
	slot->timestamp_ms = update->timestamp_ms;
	slot->latitude_e6 = update->latitude_e6;
	slot->longitude_e6 = update->longitude_e6;
	slot->accuracy_m = update->accuracy_m;
}

static bool transition_counts_as_lte_loss(const struct field_log_state_change_message *change)
{
	if (change->to_state != STATE_BACKOFF) {
		return false;
	}

	return change->from_state == STATE_LTEM_CONNECTING ||
	       change->from_state == STATE_LTEM_CONNECTED;
}

static bool transition_counts_as_switchback(const struct field_log_state_change_message *change)
{
	if (change->to_state != STATE_LTEM_CONNECTED) {
		return false;
	}

	return change->from_state == STATE_LTE_PROBE ||
	       change->active_rat == RAT_NTN;
}

static uint8_t summary_flags(void)
{
	uint8_t flags = 0;

	if (runtime.battery.sample_count == 0U) {
		flags |= FIELD_LOG_SUMMARY_FLAG_NO_BATTERY_SAMPLES;
	}

	if (runtime.battery.vbus_sample_count > 0U) {
		flags |= FIELD_LOG_SUMMARY_FLAG_VBUS_SEEN;
	}

	if (!storage.ready) {
		flags |= FIELD_LOG_SUMMARY_FLAG_STORAGE_DISABLED;
	}

	return flags;
}

static void field_log_fill_record_header(struct field_log_record *record,
					 enum field_log_record_type type,
					 int64_t uptime_ms)
{
	memset(record, 0, sizeof(*record));
	record->header.magic = FIELD_LOG_RECORD_MAGIC;
	record->header.version = FIELD_LOG_RECORD_VERSION;
	record->header.type = type;
	record->header.sequence = storage.next_sequence;
	record->header.uptime_s = clamp_u32(uptime_ms / 1000LL);
}

static void field_log_append_current_record(const struct field_log_record *record,
					    const char *kind)
{
	int err;

	k_mutex_lock(&field_log_storage_mutex, K_FOREVER);
	err = field_log_storage_append_record(record);
	k_mutex_unlock(&field_log_storage_mutex);

	if (err == -ENODEV || err == -ENOSPC) {
		return;
	}

	if (err) {
		LOG_WRN("Field log %s record was not persisted: %d", kind, err);
	}
}

static void field_log_format_coordinate(char *buffer, size_t len, int32_t value_e6)
{
	int32_t whole = value_e6 / FIELD_LOG_COORDINATE_SCALE;
	int32_t remainder = value_e6 % FIELD_LOG_COORDINATE_SCALE;
	uint32_t fraction = (uint32_t)(remainder < 0 ? -remainder : remainder);

	snprintk(buffer, len, "%d.%06u", whole, fraction);
}

static const char *field_log_location_source_name(enum field_log_location_source source)
{
	switch (source) {
	case FIELD_LOG_LOCATION_LTE:
		return "lte";
	case FIELD_LOG_LOCATION_GNSS:
		return "gnss";
	case FIELD_LOG_LOCATION_AGNSS:
		return "agnss";
	case FIELD_LOG_LOCATION_NONE:
	default:
		return "none";
	}
}

static const char *field_log_state_name(enum app_state state)
{
	switch (state) {
	case STATE_BOOT:
		return "STATE_BOOT";
	case STATE_IDLE:
		return "STATE_IDLE";
	case STATE_GNSS_ACQUIRE:
		return "STATE_GNSS_ACQUIRE";
	case STATE_NTN_CONNECTING:
		return "STATE_NTN_CONNECTING";
	case STATE_NTN_CONNECTED:
		return "STATE_NTN_CONNECTED";
	case STATE_LTEM_CONNECTING:
		return "STATE_LTEM_CONNECTING";
	case STATE_LTEM_CONNECTED:
		return "STATE_LTEM_CONNECTED";
	case STATE_CLOUD_CONNECTING:
		return "STATE_CLOUD_CONNECTING";
	case STATE_LTE_LOCATION:
		return "STATE_LTE_LOCATION";
	case STATE_LTE_PROBE:
		return "STATE_LTE_PROBE";
	case STATE_BACKOFF:
		return "STATE_BACKOFF";
	default:
		return "STATE_UNKNOWN";
	}
}

static const char *field_log_rat_name(enum rat rat)
{
	switch (rat) {
	case RAT_LTEM:
		return "LTE-M";
	case RAT_NTN:
		return "NTN";
	default:
		return "UNKNOWN";
	}
}

static void field_log_log_state_record(const struct field_log_record *record)
{
	char latitude[20];
	char longitude[20];
	const struct field_log_state_record_payload *payload = &record->payload.state;

	if (payload->location_source != FIELD_LOG_LOCATION_NONE &&
	    payload->accuracy_m != FIELD_LOG_LOCATION_ACCURACY_UNKNOWN) {
		field_log_format_coordinate(latitude, sizeof(latitude), payload->latitude_e6);
		field_log_format_coordinate(longitude, sizeof(longitude), payload->longitude_e6);

		LOG_INF("Field log state #%u: %s -> %s reason=%s rat=%s/%s loc=%s %s,%s acc=%u m rsrp=%d dBm",
			record->header.sequence,
			field_log_state_name(payload->from_state),
			field_log_state_name(payload->to_state),
			app_evt_name(payload->reason_evt),
			field_log_rat_name(payload->active_rat),
			field_log_rat_name(payload->next_rat),
			field_log_location_source_name(payload->location_source),
			latitude,
			longitude,
			payload->accuracy_m,
			payload->last_rsrp_dbm);
		return;
	}

	if (payload->location_source != FIELD_LOG_LOCATION_NONE) {
		field_log_format_coordinate(latitude, sizeof(latitude), payload->latitude_e6);
		field_log_format_coordinate(longitude, sizeof(longitude), payload->longitude_e6);

		LOG_INF("Field log state #%u: %s -> %s reason=%s rat=%s/%s loc=%s %s,%s acc=unknown rsrp=%d dBm",
			record->header.sequence,
			field_log_state_name(payload->from_state),
			field_log_state_name(payload->to_state),
			app_evt_name(payload->reason_evt),
			field_log_rat_name(payload->active_rat),
			field_log_rat_name(payload->next_rat),
			field_log_location_source_name(payload->location_source),
			latitude,
			longitude,
			payload->last_rsrp_dbm);
		return;
	}

	LOG_INF("Field log state #%u: %s -> %s reason=%s rat=%s/%s loc=none rsrp=%d dBm",
		record->header.sequence,
		field_log_state_name(payload->from_state),
		field_log_state_name(payload->to_state),
		app_evt_name(payload->reason_evt),
		field_log_rat_name(payload->active_rat),
		field_log_rat_name(payload->next_rat),
		payload->last_rsrp_dbm);
}

static void field_log_process_state_change(const struct field_log_state_change_message *change)
{
	struct field_log_record record;
	const struct field_log_location_sample *best_location;

	if (transition_counts_as_lte_loss(change)) {
		runtime.lte_losses_total = clamp_u16(runtime.lte_losses_total + 1U);
		runtime.lte_losses_interval =
			(uint8_t)MIN((uint16_t)UINT8_MAX,
				     (uint16_t)(runtime.lte_losses_interval + 1U));
	}

	if (transition_counts_as_switchback(change)) {
		runtime.switchbacks_total =
			clamp_u16(runtime.switchbacks_total + 1U);
		runtime.switchbacks_interval =
			(uint8_t)MIN((uint16_t)UINT8_MAX,
				     (uint16_t)(runtime.switchbacks_interval + 1U));
	}

	field_log_fill_record_header(&record, FIELD_LOG_RECORD_TYPE_STATE_CHANGE,
				     change->timestamp_ms);

	record.payload.state.from_state = change->from_state;
	record.payload.state.to_state = change->to_state;
	record.payload.state.reason_evt = change->reason_evt;
	record.payload.state.active_rat = change->active_rat;
	record.payload.state.next_rat = change->next_rat;
	record.payload.state.last_rsrp_dbm = change->last_rsrp_dbm;

	best_location = field_log_select_best_location(change->timestamp_ms);
	if (best_location != NULL) {
		record.payload.state.location_source = best_location->source;
		record.payload.state.latitude_e6 = best_location->latitude_e6;
		record.payload.state.longitude_e6 = best_location->longitude_e6;
		record.payload.state.accuracy_m = best_location->accuracy_m;
	} else {
		record.payload.state.location_source = FIELD_LOG_LOCATION_NONE;
		record.payload.state.accuracy_m = FIELD_LOG_LOCATION_ACCURACY_UNKNOWN;
	}

	record.crc16 = crc16_ccitt((const uint8_t *)&record,
				   offsetof(struct field_log_record, crc16));

	field_log_log_state_record(&record);
	field_log_append_current_record(&record, "state");
}

static void field_log_log_summary_record(const struct field_log_record *record)
{
	const struct field_log_summary_record_payload *payload = &record->payload.summary;

	LOG_INF("Field log summary #%u: interval=%u s power=%u/%u uWh lte_losses=%u/%u switchbacks=%u/%u flags=0x%02x dropped=%u",
		record->header.sequence,
		payload->interval_s,
		payload->power_interval_uwh,
		payload->power_total_uwh,
		payload->lte_losses_interval,
		payload->lte_losses_total,
		payload->switchbacks_interval,
		payload->switchbacks_total,
		payload->flags,
		payload->dropped_messages);
}

static void field_log_finalize_summary(void)
{
	struct field_log_record record;
	int64_t now_ms = k_uptime_get();
	int64_t interval_ms = now_ms - runtime.summary_start_ms;
	uint8_t dropped_messages =
		(uint8_t)MIN((atomic_val_t)UINT8_MAX,
			     atomic_set(&field_log_dropped_messages, 0));

	battery_accumulator_integrate_until(now_ms);

	field_log_fill_record_header(&record, FIELD_LOG_RECORD_TYPE_SUMMARY, now_ms);
	record.payload.summary.interval_s = clamp_u16(interval_ms / 1000LL);
	record.payload.summary.power_total_uwh =
		clamp_u32(runtime.battery.discharge_energy_total_uwh);
	record.payload.summary.power_interval_uwh =
		clamp_u32(runtime.battery.discharge_energy_interval_uwh);
	record.payload.summary.lte_losses_total = runtime.lte_losses_total;
	record.payload.summary.lte_losses_interval = runtime.lte_losses_interval;
	record.payload.summary.switchbacks_total = runtime.switchbacks_total;
	record.payload.summary.switchbacks_interval = runtime.switchbacks_interval;
	record.payload.summary.flags = summary_flags();
	record.payload.summary.dropped_messages = dropped_messages;
	record.crc16 = crc16_ccitt((const uint8_t *)&record,
				   offsetof(struct field_log_record, crc16));

	field_log_log_summary_record(&record);
	field_log_append_current_record(&record, "summary");
	field_log_runtime_reset_interval(now_ms);
}

static int field_log_enqueue_message(const struct field_log_message *message)
{
	int err;

	if (!field_log_started) {
		return -EACCES;
	}

	err = k_msgq_put(&field_log_msgq, message, K_NO_WAIT);
	if (err) {
		atomic_inc(&field_log_dropped_messages);
	}

	return err;
}

static void field_log_thread(void *arg1, void *arg2, void *arg3)
{
	int64_t next_summary_ms;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	memset(&runtime, 0, sizeof(runtime));
	field_log_runtime_reset_interval(k_uptime_get());
	next_summary_ms = runtime.summary_start_ms +
			  (CONFIG_APP_FIELD_LOG_SUMMARY_INTERVAL_SEC * 1000LL);

	while (true) {
		struct field_log_message message;
		int64_t now_ms = k_uptime_get();
		int64_t wait_ms = MAX(0LL, next_summary_ms - now_ms);
		int err;

		if (now_ms >= next_summary_ms) {
			field_log_finalize_summary();
			next_summary_ms += CONFIG_APP_FIELD_LOG_SUMMARY_INTERVAL_SEC * 1000LL;
			continue;
		}

		err = k_msgq_get(&field_log_msgq, &message, K_MSEC(wait_ms));
		if (err == -EAGAIN) {
			field_log_finalize_summary();
			next_summary_ms += CONFIG_APP_FIELD_LOG_SUMMARY_INTERVAL_SEC * 1000LL;
			continue;
		}

		if (err) {
			LOG_WRN("Field log queue wait failed: %d", err);
			continue;
		}

		switch (message.type) {
		case FIELD_LOG_MSG_BATTERY_SAMPLE:
			field_log_process_battery_sample(&message.data.battery.sample);
			break;
		case FIELD_LOG_MSG_LOCATION_UPDATE:
			field_log_process_location_update(&message.data.location);
			break;
		case FIELD_LOG_MSG_STATE_CHANGE:
			field_log_process_state_change(&message.data.state);
			break;
		default:
			LOG_WRN("Field log ignored unknown queue message: %d", message.type);
			break;
		}
	}
}

int field_log_start(void)
{
	int err;

	if (field_log_started) {
		return 0;
	}

	err = field_log_storage_init();
	if (err) {
		LOG_WRN("Field log will still print summaries, but flash persistence is disabled");
	}

	field_log_started = true;
	(void)atomic_set(&field_log_dropped_messages, 0);

	k_thread_create(&field_log_thread_data, field_log_stack,
			K_THREAD_STACK_SIZEOF(field_log_stack),
			field_log_thread, NULL, NULL, NULL,
			FIELD_LOG_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&field_log_thread_data, "field_log");

	LOG_INF("Field log started: summary interval=%d s",
		CONFIG_APP_FIELD_LOG_SUMMARY_INTERVAL_SEC);

	return 0;
}

int field_log_for_each_record_from(uint32_t first_sequence,
				   uint16_t max_records,
				   field_log_raw_record_cb_t callback,
				   void *user_data,
				   uint16_t *records_read)
{
	struct field_log_record record;
	enum field_log_slot_state slot_state;
	uint16_t copied = 0U;
	int err = 0;

	if (callback == NULL) {
		return -EINVAL;
	}

	if (records_read != NULL) {
		*records_read = 0U;
	}

	if (max_records == 0U) {
		return 0;
	}

	k_mutex_lock(&field_log_storage_mutex, K_FOREVER);

	if (!field_log_storage_available()) {
		err = -ENODEV;
		goto out;
	}

	for (off_t offset = 0; offset + sizeof(record) <= storage.capacity_bytes;
	     offset += sizeof(record)) {
		err = field_log_storage_read_slot(offset, &record, &slot_state);
		if (err) {
			goto out;
		}

		if (slot_state == FIELD_LOG_SLOT_EMPTY) {
			break;
		}

		if (slot_state == FIELD_LOG_SLOT_INVALID) {
			err = -EBADMSG;
			goto out;
		}

		if (record.header.sequence < first_sequence) {
			continue;
		}

		err = callback(record.header.sequence,
			       (const uint8_t *)&record,
			       user_data);
		if (err) {
			goto out;
		}

		copied++;
		if (copied >= max_records) {
			break;
		}
	}

out:
	if (records_read != NULL) {
		*records_read = copied;
	}

	k_mutex_unlock(&field_log_storage_mutex);

	return err;
}

void field_log_note_battery_sample(const struct app_battery_sample *sample)
{
	struct field_log_message message = {
		.type = FIELD_LOG_MSG_BATTERY_SAMPLE,
	};

	if (sample == NULL) {
		return;
	}

	message.data.battery.sample = *sample;
	(void)field_log_enqueue_message(&message);
}

void field_log_note_location(enum field_log_location_source source,
			     double latitude,
			     double longitude,
			     float accuracy_m)
{
	struct field_log_message message = {
		.type = FIELD_LOG_MSG_LOCATION_UPDATE,
	};

	if (source <= FIELD_LOG_LOCATION_NONE || source > FIELD_LOG_LOCATION_AGNSS) {
		return;
	}

	if (latitude < -90.0 || latitude > 90.0 ||
	    longitude < -180.0 || longitude > 180.0) {
		return;
	}

	message.data.location.source = source;
	message.data.location.timestamp_ms = k_uptime_get();
	message.data.location.latitude_e6 = degrees_to_e6(latitude);
	message.data.location.longitude_e6 = degrees_to_e6(longitude);
	message.data.location.accuracy_m = accuracy_to_u16(accuracy_m);

	(void)field_log_enqueue_message(&message);
}

void field_log_note_state_change(enum app_state from_state,
				 enum app_state to_state,
				 enum app_evt_type reason,
				 const struct app_ctx *ctx)
{
	struct field_log_message message = {
		.type = FIELD_LOG_MSG_STATE_CHANGE,
	};

	if (ctx == NULL || from_state == to_state) {
		return;
	}

	message.data.state.from_state = from_state;
	message.data.state.to_state = to_state;
	message.data.state.reason_evt = reason;
	message.data.state.active_rat = ctx->active_rat;
	message.data.state.next_rat = ctx->next_rat;
	message.data.state.last_rsrp_dbm = clamp_s16(ctx->rsrp_dbm);
	message.data.state.timestamp_ms = k_uptime_get();

	(void)field_log_enqueue_message(&message);
}

#if defined(CONFIG_APP_FIELD_LOG_SHELL)
struct field_log_record_counts {
	uint32_t total;
	uint32_t state_changes;
	uint32_t summaries;
	off_t stop_offset;
	bool stopped_on_invalid;
};

static int field_log_count_records(struct field_log_record_counts *counts)
{
	struct field_log_record record;
	enum field_log_slot_state slot_state;

	memset(counts, 0, sizeof(*counts));

	for (off_t offset = 0; offset + sizeof(record) <= storage.capacity_bytes;
	     offset += sizeof(record)) {
		int err = field_log_storage_read_slot(offset, &record, &slot_state);

		if (err) {
			return err;
		}

		if (slot_state == FIELD_LOG_SLOT_EMPTY) {
			counts->stop_offset = offset;
			return 0;
		}

		if (slot_state == FIELD_LOG_SLOT_INVALID) {
			counts->stopped_on_invalid = true;
			counts->stop_offset = offset;
			return 0;
		}

		counts->total++;
		if (record.header.type == FIELD_LOG_RECORD_TYPE_STATE_CHANGE) {
			counts->state_changes++;
		} else if (record.header.type == FIELD_LOG_RECORD_TYPE_SUMMARY) {
			counts->summaries++;
		}
		counts->stop_offset = offset + sizeof(record);
	}

	return 0;
}

static int cmd_fieldlog_info(const struct shell *shell, size_t argc, char **argv)
{
	struct field_log_record_counts counts;
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&field_log_storage_mutex, K_FOREVER);

	if (!field_log_storage_available()) {
		k_mutex_unlock(&field_log_storage_mutex);
		shell_error(shell, "field log storage is not available");
		return -ENODEV;
	}

	err = field_log_count_records(&counts);

	shell_print(shell, "fieldlog storage:");
#if defined(PM_EXTERNAL_FLASH_ID)
	shell_print(shell, "  area_id: %u", PM_EXTERNAL_FLASH_ID);
#else
	shell_print(shell, "  area_id: unavailable");
#endif
	shell_print(shell, "  area_offset: 0x%lx", (long)storage.area->fa_off);
	shell_print(shell, "  capacity_bytes: %zu", storage.capacity_bytes);
	shell_print(shell, "  record_size_bytes: %u", FIELD_LOG_RECORD_SIZE);
	shell_print(shell, "  total_records: %u", counts.total);
	shell_print(shell, "  state_changes: %u", counts.state_changes);
	shell_print(shell, "  summaries: %u", counts.summaries);
	shell_print(shell, "  stop_offset: 0x%lx", (long)counts.stop_offset);
	shell_print(shell, "  write_offset: 0x%lx", (long)storage.write_offset);
	shell_print(shell, "  next_sequence: %u", storage.next_sequence);
	shell_print(shell, "  storage_ready: %u", storage.ready ? 1U : 0U);
	shell_print(shell, "  storage_full: %u", storage.full ? 1U : 0U);
	shell_print(shell, "  stopped_on_invalid: %u",
		    counts.stopped_on_invalid ? 1U : 0U);

	k_mutex_unlock(&field_log_storage_mutex);

	if (err) {
		shell_error(shell, "failed to count records: %d", err);
	}

	return err;
}

static void shell_print_state_record(const struct shell *shell,
				     uint16_t session_id,
				     const struct field_log_record *record)
{
	char latitude[20] = "";
	char longitude[20] = "";
	const struct field_log_state_record_payload *payload = &record->payload.state;
	const char *accuracy_text = "";

	if (payload->location_source != FIELD_LOG_LOCATION_NONE) {
		field_log_format_coordinate(latitude, sizeof(latitude), payload->latitude_e6);
		field_log_format_coordinate(longitude, sizeof(longitude), payload->longitude_e6);
	}

	if (payload->accuracy_m != FIELD_LOG_LOCATION_ACCURACY_UNKNOWN) {
		static char accuracy_buffer[8];

		snprintk(accuracy_buffer, sizeof(accuracy_buffer), "%u",
			 payload->accuracy_m);
		accuracy_text = accuracy_buffer;
	}

	shell_print(shell,
		    "state,%u,%u,%u,%s,%s,%s,%s,%s,%s,%s,%s,%s,%d,,,,,,,,,",
		    session_id,
		    record->header.sequence,
		    record->header.uptime_s,
		    field_log_state_name(payload->from_state),
		    field_log_state_name(payload->to_state),
		    app_evt_name(payload->reason_evt),
		    field_log_rat_name(payload->active_rat),
		    field_log_rat_name(payload->next_rat),
		    field_log_location_source_name(payload->location_source),
		    latitude,
		    longitude,
		    accuracy_text,
		    payload->last_rsrp_dbm);
}

static void shell_print_summary_record(const struct shell *shell,
				       uint16_t session_id,
				       const struct field_log_record *record)
{
	const struct field_log_summary_record_payload *payload = &record->payload.summary;

	shell_print(shell,
		    "summary,%u,%u,%u,,,,,,,,,,,%u,%u,%u,%u,%u,%u,%u,0x%02x,%u",
		    session_id,
		    record->header.sequence,
		    record->header.uptime_s,
		    payload->interval_s,
		    payload->power_interval_uwh,
		    payload->power_total_uwh,
		    payload->lte_losses_interval,
		    payload->lte_losses_total,
		    payload->switchbacks_interval,
		    payload->switchbacks_total,
		    payload->flags,
		    payload->dropped_messages);
}

static int cmd_fieldlog_dump(const struct shell *shell, size_t argc, char **argv)
{
	struct field_log_record record;
	enum field_log_slot_state slot_state;
	uint32_t count = 0;
	uint16_t session_id = 1U;
	uint32_t previous_uptime_s = 0U;
	bool have_previous_uptime = false;
	int err = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&field_log_storage_mutex, K_FOREVER);

	if (!field_log_storage_available()) {
		k_mutex_unlock(&field_log_storage_mutex);
		shell_error(shell, "field log storage is not available");
		return -ENODEV;
	}

	shell_print(shell, "# fieldlog csv v2");
	shell_print(shell, FIELD_LOG_CSV_HEADER);

	for (off_t offset = 0; offset + sizeof(record) <= storage.capacity_bytes;
	     offset += sizeof(record)) {
		err = field_log_storage_read_slot(offset, &record, &slot_state);
		if (err) {
			shell_error(shell, "read failed at 0x%lx: %d", (long)offset, err);
			break;
		}

		if (slot_state == FIELD_LOG_SLOT_EMPTY) {
			break;
		}

		if (slot_state == FIELD_LOG_SLOT_INVALID) {
			shell_error(shell, "invalid record at 0x%lx; stopping dump",
				    (long)offset);
			break;
		}

		if (have_previous_uptime && record.header.uptime_s < previous_uptime_s &&
		    session_id < UINT16_MAX) {
			session_id++;
		}

		if (record.header.type == FIELD_LOG_RECORD_TYPE_STATE_CHANGE) {
			shell_print_state_record(shell, session_id, &record);
		} else {
			shell_print_summary_record(shell, session_id, &record);
		}

		previous_uptime_s = record.header.uptime_s;
		have_previous_uptime = true;
		count++;
	}

	shell_print(shell, "# records=%u", count);

	k_mutex_unlock(&field_log_storage_mutex);

	return err;
}

static int cmd_fieldlog_erase(const struct shell *shell, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&field_log_storage_mutex, K_FOREVER);

	if (!field_log_storage_available()) {
		k_mutex_unlock(&field_log_storage_mutex);
		shell_error(shell, "field log storage is not available");
		return -ENODEV;
	}

	err = field_log_storage_erase_all();
	if (err) {
		k_mutex_unlock(&field_log_storage_mutex);
		shell_error(shell, "erase failed: %d", err);
		return err;
	}

	field_log_storage_reset_cursor();
	storage.ready = true;

	k_mutex_unlock(&field_log_storage_mutex);

	shell_print(shell, "field log erased");

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(fieldlog_cmds,
	SHELL_CMD(info, NULL, "Show field log storage status", cmd_fieldlog_info),
	SHELL_CMD(dump, NULL, "Dump field log as CSV", cmd_fieldlog_dump),
	SHELL_CMD(erase, NULL, "Erase field log storage", cmd_fieldlog_erase),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(fieldlog, &fieldlog_cmds, "Field log commands", NULL);
#endif
