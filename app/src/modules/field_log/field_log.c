/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "field_log.h"
#include "app_zbus.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <pm_config.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>
#include <zephyr/zbus/zbus.h>

#if defined(CONFIG_APP_FIELD_LOG_SHELL)
#include <zephyr/shell/shell.h>
#endif

LOG_MODULE_REGISTER(field_log, LOG_LEVEL_INF);

#define FIELD_LOG_STACK_SIZE 1536
#define FIELD_LOG_PRIORITY 8

#define FIELD_LOG_RECORD_MAGIC 0x464cU
#define FIELD_LOG_RECORD_VERSION 1U
#define FIELD_LOG_RECORD_TYPE_BATTERY_SUMMARY 1U
#define FIELD_LOG_RECORD_SIZE 64U

#define FIELD_LOG_FLAG_NO_SAMPLES BIT(0)
#define FIELD_LOG_FLAG_STORAGE_DISABLED BIT(1)
#define FIELD_LOG_FLAG_VBUS_SEEN BIT(2)

struct field_log_battery_summary_record {
	uint16_t magic;
	uint8_t version;
	uint8_t type;
	uint32_t sequence;
	uint32_t uptime_s;
	uint16_t interval_s;
	uint16_t sample_count;
	int16_t avg_current_ma;
	int16_t min_current_ma;
	int16_t max_current_ma;
	uint16_t avg_voltage_mv;
	uint16_t min_voltage_mv;
	uint16_t last_voltage_mv;
	int16_t avg_temp_deci_c;
	int32_t net_energy_interval_uwh;
	uint32_t discharge_energy_interval_uwh;
	uint32_t discharge_energy_total_uwh;
	uint8_t vbus_sample_count;
	uint8_t flags;
	uint16_t reserved0;
	uint32_t reserved1;
	uint8_t reserved2[12];
	uint16_t crc16;
} __packed;

BUILD_ASSERT(sizeof(struct field_log_battery_summary_record) == FIELD_LOG_RECORD_SIZE);
BUILD_ASSERT((CONFIG_APP_FIELD_LOG_ERASE_BLOCK_SIZE % FIELD_LOG_RECORD_SIZE) == 0);

ZBUS_MSG_SUBSCRIBER_DEFINE(field_log_batt_sub);

union field_log_msg {
	struct app_battery_sample battery;
};

struct field_log_batt_accum {
	bool have_last_sample;
	struct app_battery_sample last_sample;
	int64_t last_energy_timestamp_ms;
	int64_t summary_start_ms;
	uint32_t sample_count;
	uint32_t vbus_sample_count;
	int64_t voltage_sum_mv;
	int64_t current_sum_ma;
	int64_t temp_sum_mdegc;
	int64_t min_voltage_mv;
	int64_t min_current_ma;
	int64_t max_current_ma;
	int64_t net_energy_interval_uwh;
	int64_t discharge_energy_interval_uwh;
	int64_t discharge_energy_total_uwh;
};

static K_THREAD_STACK_DEFINE(field_log_stack, FIELD_LOG_STACK_SIZE);
static struct k_thread field_log_thread_data;
static bool field_log_started;

static const struct flash_area *storage_area;
static size_t storage_capacity;
static off_t storage_write_offset;
static uint32_t next_sequence;
static bool storage_ready;
static bool storage_full;

K_MUTEX_DEFINE(field_log_storage_mutex);

static struct field_log_batt_accum batt_accum;

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

static bool record_is_erased(const struct field_log_battery_summary_record *record)
{
	const uint8_t *bytes = (const uint8_t *)record;

	for (size_t i = 0; i < sizeof(*record); i++) {
		if (bytes[i] != 0xffU) {
			return false;
		}
	}

	return true;
}

static bool record_is_valid(const struct field_log_battery_summary_record *record)
{
	uint16_t expected_crc;

	if (record->magic != FIELD_LOG_RECORD_MAGIC ||
	    record->version != FIELD_LOG_RECORD_VERSION ||
	    record->type != FIELD_LOG_RECORD_TYPE_BATTERY_SUMMARY) {
		return false;
	}

	expected_crc = crc16_ccitt((const uint8_t *)record,
				  offsetof(struct field_log_battery_summary_record, crc16));

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

static int field_log_storage_erase_range(void)
{
	for (off_t offset = 0; offset < storage_capacity;
	     offset += CONFIG_APP_FIELD_LOG_ERASE_BLOCK_SIZE) {
		int err = flash_area_erase(storage_area, offset,
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
	struct field_log_battery_summary_record record;

	storage_write_offset = 0;
	next_sequence = 0;

	for (off_t offset = 0; offset + sizeof(record) <= storage_capacity;
	     offset += sizeof(record)) {
		int err = flash_area_read(storage_area, offset, &record, sizeof(record));

		if (err) {
			LOG_ERR("Field log scan read failed at 0x%lx: %d",
				(long)offset, err);
			return err;
		}

		if (record_is_erased(&record)) {
			storage_write_offset = offset;
			return 0;
		}

		if (!record_is_valid(&record)) {
			LOG_WRN("Field log scan stopped at invalid record 0x%lx", (long)offset);
			storage_full = true;
			storage_write_offset = offset;
			return -EBADMSG;
		}

		next_sequence = record.sequence + 1U;
		storage_write_offset = offset + sizeof(record);
	}

	storage_full = true;
	return 0;
}

static int field_log_storage_init(void)
{
	int err;

#if !defined(PM_EXTERNAL_FLASH_ID)
	LOG_WRN("Field log storage disabled: no PM_EXTERNAL_FLASH_ID partition");
	return -ENODEV;
#else
	err = flash_area_open(PM_EXTERNAL_FLASH_ID, &storage_area);
	if (err) {
		LOG_WRN("Field log storage disabled: flash_area_open failed: %d", err);
		return err;
	}

	storage_capacity = MIN((size_t)storage_area->fa_size,
			       (size_t)CONFIG_APP_FIELD_LOG_STORAGE_BYTES);
	storage_capacity -= storage_capacity % CONFIG_APP_FIELD_LOG_ERASE_BLOCK_SIZE;

	if (storage_capacity < sizeof(struct field_log_battery_summary_record)) {
		LOG_WRN("Field log storage disabled: capacity too small");
		return -ENOSPC;
	}

	if (IS_ENABLED(CONFIG_APP_FIELD_LOG_ERASE_ON_BOOT)) {
		err = field_log_storage_erase_range();
		if (err) {
			return err;
		}
	}

	err = field_log_storage_scan();
	if ((err == -EBADMSG) && (storage_write_offset == 0)) {
		LOG_WRN("Field log storage has no valid first record; erasing prototype area");
		storage_full = false;

		err = field_log_storage_erase_range();
		if (err) {
			return err;
		}

		err = field_log_storage_scan();
	}

	if (err == -EBADMSG) {
		LOG_WRN("Field log append disabled to preserve existing corrupted data");
		return err;
	}

	if (err) {
		return err;
	}

	storage_ready = !storage_full;

	LOG_INF("Field log storage: area=%u offset=0x%lx size=%zu next=0x%lx seq=%u",
		PM_EXTERNAL_FLASH_ID,
		(long)storage_area->fa_off,
		storage_capacity,
		(long)storage_write_offset,
		next_sequence);

	return 0;
#endif
}

static int field_log_storage_write(const struct field_log_battery_summary_record *record)
{
	int err;

	if (!storage_ready) {
		return -ENODEV;
	}

	if (storage_write_offset + sizeof(*record) > storage_capacity) {
		storage_full = true;
		storage_ready = false;
		LOG_WRN("Field log storage full at 0x%lx", (long)storage_write_offset);
		return -ENOSPC;
	}

	if ((storage_write_offset % CONFIG_APP_FIELD_LOG_ERASE_BLOCK_SIZE) == 0) {
		err = flash_area_erase(storage_area, storage_write_offset,
				       CONFIG_APP_FIELD_LOG_ERASE_BLOCK_SIZE);
		if (err) {
			LOG_ERR("Field log sector erase failed at 0x%lx: %d",
				(long)storage_write_offset, err);
			return err;
		}
	}

	err = flash_area_write(storage_area, storage_write_offset, record, sizeof(*record));
	if (err) {
		LOG_ERR("Field log write failed at 0x%lx: %d",
			(long)storage_write_offset, err);
		return err;
	}

	storage_write_offset += sizeof(*record);

	return 0;
}

static bool field_log_storage_available(void)
{
	return storage_area != NULL && storage_capacity >= FIELD_LOG_RECORD_SIZE;
}

static void field_log_batt_accum_reset_interval(int64_t start_ms)
{
	batt_accum.summary_start_ms = start_ms;
	batt_accum.sample_count = 0;
	batt_accum.vbus_sample_count = 0;
	batt_accum.voltage_sum_mv = 0;
	batt_accum.current_sum_ma = 0;
	batt_accum.temp_sum_mdegc = 0;
	batt_accum.min_voltage_mv = INT64_MAX;
	batt_accum.min_current_ma = INT64_MAX;
	batt_accum.max_current_ma = INT64_MIN;
	batt_accum.net_energy_interval_uwh = 0;
	batt_accum.discharge_energy_interval_uwh = 0;
}

static void field_log_integrate_until(int64_t timestamp_ms)
{
	int64_t delta_ms;
	int64_t net_energy_uwh;
	int64_t discharge_energy_uwh;

	if (!batt_accum.have_last_sample) {
		return;
	}

	if (timestamp_ms <= batt_accum.last_energy_timestamp_ms) {
		return;
	}

	delta_ms = timestamp_ms - batt_accum.last_energy_timestamp_ms;
	net_energy_uwh = (batt_accum.last_sample.voltage_mv *
			  batt_accum.last_sample.current_ma *
			  delta_ms) / 3600000LL;

	batt_accum.net_energy_interval_uwh += net_energy_uwh;

	if (batt_accum.last_sample.current_ma < 0) {
		discharge_energy_uwh = (batt_accum.last_sample.voltage_mv *
					-batt_accum.last_sample.current_ma *
					delta_ms) / 3600000LL;
		batt_accum.discharge_energy_interval_uwh += discharge_energy_uwh;
		batt_accum.discharge_energy_total_uwh += discharge_energy_uwh;
	}

	batt_accum.last_energy_timestamp_ms = timestamp_ms;
}

static void field_log_process_battery_sample(const struct app_battery_sample *sample)
{
	int64_t sample_time_ms = sample->timestamp_ms;

	if (sample_time_ms <= 0) {
		sample_time_ms = k_uptime_get();
	}

	field_log_integrate_until(sample_time_ms);

	batt_accum.have_last_sample = true;
	batt_accum.last_sample = *sample;
	batt_accum.last_sample.timestamp_ms = sample_time_ms;
	batt_accum.last_energy_timestamp_ms = sample_time_ms;

	batt_accum.sample_count++;
	batt_accum.voltage_sum_mv += sample->voltage_mv;
	batt_accum.current_sum_ma += sample->current_ma;
	batt_accum.temp_sum_mdegc += sample->temp_mdegc;
	batt_accum.min_voltage_mv = MIN(batt_accum.min_voltage_mv, sample->voltage_mv);
	batt_accum.min_current_ma = MIN(batt_accum.min_current_ma, sample->current_ma);
	batt_accum.max_current_ma = MAX(batt_accum.max_current_ma, sample->current_ma);

	if (sample->vbus_present) {
		batt_accum.vbus_sample_count++;
	}
}

static void field_log_build_battery_record(struct field_log_battery_summary_record *record,
					   int64_t now_ms)
{
	uint32_t sample_count = batt_accum.sample_count;
	int64_t interval_ms = now_ms - batt_accum.summary_start_ms;
	uint8_t flags = 0;

	memset(record, 0, sizeof(*record));

	if (sample_count == 0) {
		flags |= FIELD_LOG_FLAG_NO_SAMPLES;
	}

	if (!storage_ready) {
		flags |= FIELD_LOG_FLAG_STORAGE_DISABLED;
	}

	if (batt_accum.vbus_sample_count > 0) {
		flags |= FIELD_LOG_FLAG_VBUS_SEEN;
	}

	record->magic = FIELD_LOG_RECORD_MAGIC;
	record->version = FIELD_LOG_RECORD_VERSION;
	record->type = FIELD_LOG_RECORD_TYPE_BATTERY_SUMMARY;
	record->sequence = next_sequence;
	record->uptime_s = clamp_u32(now_ms / 1000);
	record->interval_s = clamp_u16(interval_ms / 1000);
	record->sample_count = clamp_u16(sample_count);
	record->flags = flags;
	record->vbus_sample_count = clamp_u16(batt_accum.vbus_sample_count);

	if (sample_count > 0) {
		record->avg_current_ma = clamp_s16(batt_accum.current_sum_ma / sample_count);
		record->min_current_ma = clamp_s16(batt_accum.min_current_ma);
		record->max_current_ma = clamp_s16(batt_accum.max_current_ma);
		record->avg_voltage_mv = clamp_u16(batt_accum.voltage_sum_mv / sample_count);
		record->min_voltage_mv = clamp_u16(batt_accum.min_voltage_mv);
		record->last_voltage_mv = clamp_u16(batt_accum.last_sample.voltage_mv);
		record->avg_temp_deci_c =
			clamp_s16((batt_accum.temp_sum_mdegc / sample_count) / 100);
	}

	record->net_energy_interval_uwh = clamp_s32(batt_accum.net_energy_interval_uwh);
	record->discharge_energy_interval_uwh =
		clamp_u32(batt_accum.discharge_energy_interval_uwh);
	record->discharge_energy_total_uwh =
		clamp_u32(batt_accum.discharge_energy_total_uwh);
	record->crc16 = crc16_ccitt((const uint8_t *)record,
				    offsetof(struct field_log_battery_summary_record, crc16));
}

static void field_log_publish_battery_summary(void)
{
	struct field_log_battery_summary_record record;
	int64_t now_ms = k_uptime_get();
	int err;

	field_log_integrate_until(now_ms);

	k_mutex_lock(&field_log_storage_mutex, K_FOREVER);

	field_log_build_battery_record(&record, now_ms);

	if (record.sample_count == 0) {
		LOG_WRN("Field log battery summary #%u: no samples, flags=0x%02x",
			record.sequence, record.flags);
	} else {
		LOG_INF("Field log battery summary #%u: samples=%u avg=%d mA min/max=%d/%d mA voltage avg/min/last=%u/%u/%u mV net=%d uWh discharge=%u/%u uWh vbus=%u flags=0x%02x",
			record.sequence,
			record.sample_count,
			record.avg_current_ma,
			record.min_current_ma,
			record.max_current_ma,
			record.avg_voltage_mv,
			record.min_voltage_mv,
			record.last_voltage_mv,
			record.net_energy_interval_uwh,
			record.discharge_energy_interval_uwh,
			record.discharge_energy_total_uwh,
			record.vbus_sample_count,
			record.flags);
	}

	err = field_log_storage_write(&record);
	if (err == 0) {
		next_sequence++;
	} else if (err != -ENODEV && err != -ENOSPC) {
		LOG_WRN("Field log summary was not persisted: %d", err);
	}

	k_mutex_unlock(&field_log_storage_mutex);

	field_log_batt_accum_reset_interval(now_ms);
}

static void field_log_thread(void *arg1, void *arg2, void *arg3)
{
	int64_t next_summary_ms = k_uptime_get() +
				  (CONFIG_APP_FIELD_LOG_SUMMARY_INTERVAL_SEC * 1000LL);

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	field_log_batt_accum_reset_interval(k_uptime_get());

	while (true) {
		const struct zbus_channel *chan;
		union field_log_msg msg = {0};
		int64_t now_ms = k_uptime_get();
		int64_t wait_ms = next_summary_ms - now_ms;
		int err;

		if (wait_ms <= 0) {
			field_log_publish_battery_summary();
			next_summary_ms += CONFIG_APP_FIELD_LOG_SUMMARY_INTERVAL_SEC * 1000LL;
			continue;
		}

		err = zbus_sub_wait_msg(&field_log_batt_sub, &chan, &msg, K_MSEC(wait_ms));
		if (err == -EAGAIN) {
			field_log_publish_battery_summary();
			next_summary_ms += CONFIG_APP_FIELD_LOG_SUMMARY_INTERVAL_SEC * 1000LL;
			continue;
		}

		if (err) {
			LOG_WRN("Field log zbus wait failed: %d", err);
			continue;
		}

		if (chan == &battery_sample_chan) {
			field_log_process_battery_sample(&msg.battery);
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

	k_thread_create(&field_log_thread_data, field_log_stack,
			K_THREAD_STACK_SIZEOF(field_log_stack),
			field_log_thread, NULL, NULL, NULL,
			FIELD_LOG_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&field_log_thread_data, "field_log");

	field_log_started = true;

	LOG_INF("Field log prototype started: battery summary interval=%d s",
		CONFIG_APP_FIELD_LOG_SUMMARY_INTERVAL_SEC);

	return 0;
}

#if defined(CONFIG_APP_FIELD_LOG_SHELL)
static int field_log_count_records(uint32_t *valid_records, off_t *stop_offset,
				   bool *stopped_on_invalid)
{
	struct field_log_battery_summary_record record;
	uint32_t count = 0;

	*stopped_on_invalid = false;
	*stop_offset = 0;

	for (off_t offset = 0; offset + sizeof(record) <= storage_capacity;
	     offset += sizeof(record)) {
		int err = flash_area_read(storage_area, offset, &record, sizeof(record));

		if (err) {
			return err;
		}

		if (record_is_erased(&record)) {
			*stop_offset = offset;
			*valid_records = count;
			return 0;
		}

		if (!record_is_valid(&record)) {
			*stopped_on_invalid = true;
			*stop_offset = offset;
			*valid_records = count;
			return 0;
		}

		count++;
		*stop_offset = offset + sizeof(record);
	}

	*valid_records = count;
	return 0;
}

static int cmd_fieldlog_info(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t valid_records = 0;
	off_t stop_offset = 0;
	bool stopped_on_invalid = false;
	int err = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&field_log_storage_mutex, K_FOREVER);

	if (!field_log_storage_available()) {
		k_mutex_unlock(&field_log_storage_mutex);
		shell_error(shell, "field log storage is not available");
		return -ENODEV;
	}

	err = field_log_count_records(&valid_records, &stop_offset, &stopped_on_invalid);

	shell_print(shell, "fieldlog storage:");
#if defined(PM_EXTERNAL_FLASH_ID)
	shell_print(shell, "  area_id: %u", PM_EXTERNAL_FLASH_ID);
#else
	shell_print(shell, "  area_id: unavailable");
#endif
	shell_print(shell, "  area_offset: 0x%lx", (long)storage_area->fa_off);
	shell_print(shell, "  capacity_bytes: %zu", storage_capacity);
	shell_print(shell, "  record_size_bytes: %u", FIELD_LOG_RECORD_SIZE);
	shell_print(shell, "  valid_records: %u", valid_records);
	shell_print(shell, "  stop_offset: 0x%lx", (long)stop_offset);
	shell_print(shell, "  write_offset: 0x%lx", (long)storage_write_offset);
	shell_print(shell, "  next_sequence: %u", next_sequence);
	shell_print(shell, "  storage_ready: %u", storage_ready ? 1U : 0U);
	shell_print(shell, "  storage_full: %u", storage_full ? 1U : 0U);
	shell_print(shell, "  stopped_on_invalid: %u", stopped_on_invalid ? 1U : 0U);

	k_mutex_unlock(&field_log_storage_mutex);

	if (err) {
		shell_error(shell, "failed to count records: %d", err);
	}

	return err;
}

static void shell_print_battery_record(const struct shell *shell,
				       const struct field_log_battery_summary_record *record)
{
	shell_print(shell,
		    "%u,%u,%u,%u,%d,%d,%d,%u,%u,%u,%d,%d,%u,%u,%u,0x%02x",
		    record->sequence,
		    record->uptime_s,
		    record->interval_s,
		    record->sample_count,
		    record->avg_current_ma,
		    record->min_current_ma,
		    record->max_current_ma,
		    record->avg_voltage_mv,
		    record->min_voltage_mv,
		    record->last_voltage_mv,
		    record->avg_temp_deci_c,
		    record->net_energy_interval_uwh,
		    record->discharge_energy_interval_uwh,
		    record->discharge_energy_total_uwh,
		    record->vbus_sample_count,
		    record->flags);
}

static int cmd_fieldlog_dump(const struct shell *shell, size_t argc, char **argv)
{
	struct field_log_battery_summary_record record;
	uint32_t count = 0;
	int err = 0;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&field_log_storage_mutex, K_FOREVER);

	if (!field_log_storage_available()) {
		k_mutex_unlock(&field_log_storage_mutex);
		shell_error(shell, "field log storage is not available");
		return -ENODEV;
	}

	shell_print(shell, "# fieldlog battery summary csv v1");
	shell_print(shell,
		    "seq,uptime_s,interval_s,samples,avg_current_ma,min_current_ma,max_current_ma,avg_voltage_mv,min_voltage_mv,last_voltage_mv,avg_temp_deci_c,net_energy_interval_uwh,discharge_energy_interval_uwh,discharge_energy_total_uwh,vbus_samples,flags");

	for (off_t offset = 0; offset + sizeof(record) <= storage_capacity;
	     offset += sizeof(record)) {
		err = flash_area_read(storage_area, offset, &record, sizeof(record));
		if (err) {
			shell_error(shell, "read failed at 0x%lx: %d", (long)offset, err);
			break;
		}

		if (record_is_erased(&record)) {
			break;
		}

		if (!record_is_valid(&record)) {
			shell_error(shell, "invalid record at 0x%lx; stopping dump",
				    (long)offset);
			break;
		}

		shell_print_battery_record(shell, &record);
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

	err = field_log_storage_erase_range();
	if (err) {
		k_mutex_unlock(&field_log_storage_mutex);
		shell_error(shell, "erase failed: %d", err);
		return err;
	}

	storage_write_offset = 0;
	next_sequence = 0;
	storage_full = false;
	storage_ready = true;

	k_mutex_unlock(&field_log_storage_mutex);

	shell_print(shell, "field log erased");

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(fieldlog_cmds,
	SHELL_CMD(info, NULL, "Show field log storage status", cmd_fieldlog_info),
	SHELL_CMD(dump, NULL, "Dump battery summaries as CSV", cmd_fieldlog_dump),
	SHELL_CMD(erase, NULL, "Erase field log storage", cmd_fieldlog_erase),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(fieldlog, &fieldlog_cmds, "Field log commands", NULL);
#endif
