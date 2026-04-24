/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "lds_service.h"

#include "field_log.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(lds_service, LOG_LEVEL_INF);

#define LDS_NODE DT_ALIAS(lds0)

#if !DT_NODE_HAS_STATUS(LDS_NODE, okay)
#error "CONFIG_APP_LDS requires a devicetree alias named lds0"
#endif

#define LDS_STACK_SIZE 1536
#define LDS_PRIORITY 9

#define LDS_FRAME_MAGIC 0x3153444cU
#define LDS_STATUS_MAGIC 0x3153544cU
#define LDS_PROTOCOL_VERSION 1U
#define LDS_CMD_FIELD_LOG_APPEND 1U
#define LDS_STATUS_OK 0U
#define LDS_STATUS_PENDING 1U
#define LDS_STATUS_BUSY 2U
#define LDS_STATUS_BAD_FRAME 3U
#define LDS_STATUS_SD_ERROR 4U
#define LDS_STATUS_QUEUE_FULL 5U
#define LDS_NO_SEQUENCE UINT32_MAX

#define LDS_RECORDS_PER_FRAME CONFIG_APP_LDS_FIELD_LOG_RECORDS_PER_FRAME
#define LDS_MAX_RECORDS_PER_FRAME 2U
#define LDS_PAYLOAD_BYTES (FIELD_LOG_RAW_RECORD_SIZE * LDS_MAX_RECORDS_PER_FRAME)

struct lds_field_log_frame {
	uint32_t magic;
	uint8_t version;
	uint8_t command;
	uint16_t frame_len;
	uint32_t first_sequence;
	uint8_t record_count;
	uint8_t record_size;
	uint16_t payload_len;
	uint16_t payload_crc16;
	uint8_t payload[LDS_PAYLOAD_BYTES];
	uint16_t frame_crc16;
} __packed;

struct lds_status_response {
	uint32_t magic;
	uint8_t version;
	uint8_t status;
	uint16_t reserved;
	uint32_t committed_frames;
	uint32_t committed_records;
	uint32_t last_committed_sequence;
	uint32_t rejected_frames;
	uint16_t crc16;
} __packed;

struct lds_record_batch {
	struct lds_field_log_frame frame;
	uint32_t last_sequence;
};

BUILD_ASSERT(LDS_RECORDS_PER_FRAME >= 1);
BUILD_ASSERT(LDS_RECORDS_PER_FRAME <= LDS_MAX_RECORDS_PER_FRAME);
BUILD_ASSERT(sizeof(struct lds_field_log_frame) <= 128);

static const struct i2c_dt_spec lds_i2c = I2C_DT_SPEC_GET(LDS_NODE);
static K_THREAD_STACK_DEFINE(lds_stack, LDS_STACK_SIZE);
static struct k_thread lds_thread_data;
static bool lds_started;
static uint32_t next_sequence;

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

static int lds_validate_status(const struct lds_status_response *status)
{
	uint16_t expected_crc;

	if (status->magic != LDS_STATUS_MAGIC ||
	    status->version != LDS_PROTOCOL_VERSION) {
		return -EBADMSG;
	}

	expected_crc = crc16_ccitt((const uint8_t *)status,
				   offsetof(struct lds_status_response, crc16));

	if (status->crc16 != expected_crc) {
		return -EBADMSG;
	}

	return 0;
}

static int lds_read_status(struct lds_status_response *status)
{
	int err = i2c_read_dt(&lds_i2c, (uint8_t *)status, sizeof(*status));

	if (err) {
		return err;
	}

	return lds_validate_status(status);
}

static void lds_update_cursor_from_status(void)
{
	struct lds_status_response status;
	int err = lds_read_status(&status);

	if (err) {
		LOG_WRN("LDS status unavailable, starting offload cursor at 0: %d", err);
		next_sequence = 0U;
		return;
	}

	if (status.last_committed_sequence == LDS_NO_SEQUENCE) {
		next_sequence = 0U;
		return;
	}

	next_sequence = status.last_committed_sequence + 1U;
	LOG_INF("LDS offload cursor starts at field log sequence %u", next_sequence);
}

static int lds_wait_for_commit(uint32_t wanted_sequence)
{
	int64_t deadline = k_uptime_get() + CONFIG_APP_LDS_COMMIT_TIMEOUT_MS;
	struct lds_status_response status;
	int last_err = 0;

	do {
		int err = lds_read_status(&status);

		if (err == 0) {
			if (status.last_committed_sequence != LDS_NO_SEQUENCE &&
			    status.last_committed_sequence >= wanted_sequence) {
				if (status.status == LDS_STATUS_OK ||
				    status.status == LDS_STATUS_PENDING) {
					return 0;
				}

				return -EIO;
			}

			if (status.status == LDS_STATUS_BAD_FRAME ||
			    status.status == LDS_STATUS_SD_ERROR ||
			    status.status == LDS_STATUS_QUEUE_FULL) {
				return -EIO;
			}
		}

		last_err = err;
		k_sleep(K_MSEC(CONFIG_APP_LDS_STATUS_RETRY_MS));
	} while (k_uptime_get() < deadline);

	return last_err != 0 ? last_err : -ETIMEDOUT;
}

static int lds_collect_record(uint32_t sequence,
			      const uint8_t record[FIELD_LOG_RAW_RECORD_SIZE],
			      void *user_data)
{
	struct lds_record_batch *batch = user_data;
	size_t offset = batch->frame.record_count * FIELD_LOG_RAW_RECORD_SIZE;

	if (batch->frame.record_count >= LDS_RECORDS_PER_FRAME) {
		return -ENOSPC;
	}

	memcpy(&batch->frame.payload[offset], record, FIELD_LOG_RAW_RECORD_SIZE);
	batch->frame.record_count++;
	batch->last_sequence = sequence;

	return 0;
}

static int lds_build_batch(struct lds_record_batch *batch, uint16_t *records_read)
{
	int err;

	memset(batch, 0, sizeof(*batch));
	batch->last_sequence = LDS_NO_SEQUENCE;
	batch->frame.magic = LDS_FRAME_MAGIC;
	batch->frame.version = LDS_PROTOCOL_VERSION;
	batch->frame.command = LDS_CMD_FIELD_LOG_APPEND;
	batch->frame.frame_len = sizeof(batch->frame);
	batch->frame.first_sequence = next_sequence;
	batch->frame.record_size = FIELD_LOG_RAW_RECORD_SIZE;

	err = field_log_for_each_record_from(next_sequence,
					     LDS_RECORDS_PER_FRAME,
					     lds_collect_record,
					     batch,
					     records_read);
	if (err) {
		return err;
	}

	batch->frame.payload_len =
		batch->frame.record_count * FIELD_LOG_RAW_RECORD_SIZE;
	batch->frame.payload_crc16 =
		crc16_ccitt(batch->frame.payload, batch->frame.payload_len);
	batch->frame.frame_crc16 =
		crc16_ccitt((const uint8_t *)&batch->frame,
			    offsetof(struct lds_field_log_frame, frame_crc16));

	return 0;
}

static int lds_send_batch(const struct lds_record_batch *batch)
{
	return i2c_write_dt(&lds_i2c,
			    (const uint8_t *)&batch->frame,
			    sizeof(batch->frame));
}

static void lds_offload_once(void)
{
	uint16_t copied_this_run = 0U;

	while (copied_this_run < CONFIG_APP_LDS_OFFLOAD_MAX_RECORDS_PER_RUN) {
		struct lds_record_batch batch;
		uint16_t records_read = 0U;
		int err = lds_build_batch(&batch, &records_read);

		if (err == -ENODEV) {
			LOG_WRN("Field log storage is unavailable for LDS offload");
			return;
		}

		if (err == -EBADMSG) {
			LOG_WRN("Field log scan hit an invalid record during LDS offload");
			return;
		}

		if (err) {
			LOG_WRN("LDS batch build failed: %d", err);
			return;
		}

		if (records_read == 0U || batch.frame.record_count == 0U) {
			return;
		}

		err = lds_send_batch(&batch);
		if (err) {
			LOG_WRN("LDS I2C write failed: %d", err);
			return;
		}

		err = lds_wait_for_commit(batch.last_sequence);
		if (err) {
			LOG_WRN("LDS did not confirm field log sequence %u: %d",
				batch.last_sequence, err);
			return;
		}

		next_sequence = batch.last_sequence + 1U;
		copied_this_run += batch.frame.record_count;
	}
}

static void lds_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	lds_update_cursor_from_status();

	while (true) {
		lds_offload_once();
		k_sleep(K_SECONDS(CONFIG_APP_LDS_OFFLOAD_INTERVAL_SEC));
	}
}

int lds_service_start(void)
{
	if (lds_started) {
		return 0;
	}

	if (!device_is_ready(lds_i2c.bus)) {
		LOG_WRN("LDS I2C bus is not ready");
		return -ENODEV;
	}

	lds_started = true;
	next_sequence = 0U;

	k_thread_create(&lds_thread_data, lds_stack,
			K_THREAD_STACK_SIZEOF(lds_stack),
			lds_thread, NULL, NULL, NULL,
			LDS_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&lds_thread_data, "lds_service");

	LOG_INF("LDS field log offload started: addr=0x%02x interval=%d s",
		lds_i2c.addr, CONFIG_APP_LDS_OFFLOAD_INTERVAL_SEC);

	return 0;
}
