/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include "rsrp_monitor.h"
#include "rsrp_parse.h"

static const struct rsrp_monitor_config test_config = {
	.fallback_dbm = -120,
	.drop_db = 5,
	.weak_sample_limit = 3,
	.unavailable_sample_limit = 3,
};

ZTEST(rsrp_parse, test_valid_cesq_response_returns_dbm)
{
	int rsrp_dbm = 0;
	int err = rsrp_parse_cesq_rsrp("+CESQ: 99,99,255,255,18,44", &rsrp_dbm);

	zassert_equal(0, err, "valid CESQ response should parse");
	zassert_equal(-97, rsrp_dbm, "RSRP raw 44 should map to -97 dBm");
}

ZTEST(rsrp_parse, test_minimum_raw_rsrp_maps_below_range)
{
	int rsrp_dbm = 0;
	int err = rsrp_parse_cesq_rsrp("+CESQ: 99,99,255,255,0,0", &rsrp_dbm);

	zassert_equal(0, err, "minimum raw RSRP should parse");
	zassert_equal(-141, rsrp_dbm, "RSRP raw 0 should map to below -140 dBm");
}

ZTEST(rsrp_parse, test_maximum_raw_rsrp_maps_to_strongest_value)
{
	int rsrp_dbm = 0;
	int err = rsrp_parse_cesq_rsrp("+CESQ: 99,99,255,255,97,97", &rsrp_dbm);

	zassert_equal(0, err, "maximum raw RSRP should parse");
	zassert_equal(-44, rsrp_dbm, "RSRP raw 97 should map to -44 dBm");
}

ZTEST(rsrp_parse, test_unknown_rsrp_returns_enoent)
{
	int rsrp_dbm = 1234;
	int err = rsrp_parse_cesq_rsrp("+CESQ: 99,99,255,255,255,255", &rsrp_dbm);

	zassert_equal(-ENOENT, err, "raw 255 should report unknown RSRP");
	zassert_equal(1234, rsrp_dbm, "unknown RSRP should not overwrite output");
}

ZTEST(rsrp_parse, test_reserved_raw_rsrp_returns_erange)
{
	int rsrp_dbm = 1234;
	int err = rsrp_parse_cesq_rsrp("+CESQ: 99,99,255,255,98,98", &rsrp_dbm);

	zassert_equal(-ERANGE, err, "reserved raw RSRP should fail");
	zassert_equal(1234, rsrp_dbm, "reserved RSRP should not overwrite output");
}

ZTEST(rsrp_parse, test_malformed_response_returns_eio)
{
	int rsrp_dbm = 1234;
	int err = rsrp_parse_cesq_rsrp("+CESQ: 99,99,255", &rsrp_dbm);

	zassert_equal(-EIO, err, "malformed CESQ response should fail");
	zassert_equal(1234, rsrp_dbm, "parse failure should not overwrite output");
}

ZTEST(rsrp_parse, test_null_arguments_return_einval)
{
	int rsrp_dbm;

	zassert_equal(-EINVAL, rsrp_parse_cesq_rsrp(NULL, &rsrp_dbm),
		      "NULL response should fail");
	zassert_equal(-EINVAL, rsrp_parse_cesq_rsrp("+CESQ: 99,99,255,255,18,44", NULL),
		      "NULL output pointer should fail");
}

ZTEST(rsrp_monitor, test_strong_samples_do_not_request_lte_fallback)
{
	struct rsrp_monitor_state state;

	rsrp_monitor_reset(&state);

	zassert_false(rsrp_monitor_record_signal_sample(&state, &test_config, -95));
	zassert_false(rsrp_monitor_record_signal_sample(&state, &test_config, -101));
	zassert_false(rsrp_monitor_record_signal_sample(&state, &test_config, -119));
	zassert_equal(3, rsrp_monitor_history_count(&state));
}

ZTEST(rsrp_monitor, test_weak_signal_requires_configured_sample_count)
{
	struct rsrp_monitor_state state;

	rsrp_monitor_reset(&state);

	zassert_false(rsrp_monitor_record_signal_sample(&state, &test_config, -121));
	zassert_false(rsrp_monitor_record_signal_sample(&state, &test_config, -122));
	zassert_true(rsrp_monitor_record_signal_sample(&state, &test_config, -121));
}

ZTEST(rsrp_monitor, test_sharp_drop_requests_fallback_when_signal_is_weak)
{
	struct rsrp_monitor_state state;

	rsrp_monitor_reset(&state);

	zassert_false(rsrp_monitor_record_signal_sample(&state, &test_config, -110));
	zassert_true(rsrp_monitor_record_signal_sample(&state, &test_config, -121));
}

ZTEST(rsrp_monitor, test_sharp_drop_does_not_request_fallback_while_signal_is_strong)
{
	struct rsrp_monitor_state state;

	rsrp_monitor_reset(&state);

	zassert_false(rsrp_monitor_record_signal_sample(&state, &test_config, -90));
	zassert_false(rsrp_monitor_record_signal_sample(&state, &test_config, -96));
}

ZTEST(rsrp_monitor, test_worsening_trend_requests_fallback)
{
	struct rsrp_monitor_state state;

	rsrp_monitor_reset(&state);

	zassert_false(rsrp_monitor_record_signal_sample(&state, &test_config, -118));
	zassert_false(rsrp_monitor_record_signal_sample(&state, &test_config, -121));
	zassert_true(rsrp_monitor_record_signal_sample(&state, &test_config, -124));
	zassert_true(rsrp_monitor_trend_worsening(&state, &test_config));
}

ZTEST(rsrp_monitor, test_unavailable_samples_require_configured_limit)
{
	struct rsrp_monitor_state state;

	rsrp_monitor_reset(&state);

	zassert_equal(1, rsrp_monitor_record_unavailable(&state));
	zassert_false(rsrp_monitor_unavailable_limit_reached(&state, &test_config));
	zassert_equal(2, rsrp_monitor_record_unavailable(&state));
	zassert_false(rsrp_monitor_unavailable_limit_reached(&state, &test_config));
	zassert_equal(3, rsrp_monitor_record_unavailable(&state));
	zassert_true(rsrp_monitor_unavailable_limit_reached(&state, &test_config));
}

ZTEST(rsrp_monitor, test_unavailable_event_uses_last_weaker_signal)
{
	struct rsrp_monitor_state state;

	rsrp_monitor_reset(&state);

	zassert_false(rsrp_monitor_record_signal_sample(&state, &test_config, -126));
	zassert_equal(-126, rsrp_monitor_unavailable_event_value(&state, &test_config));
}

ZTEST(rsrp_monitor, test_unavailable_event_uses_fallback_without_weaker_signal)
{
	struct rsrp_monitor_state state;

	rsrp_monitor_reset(&state);
	zassert_equal(-120, rsrp_monitor_unavailable_event_value(&state, &test_config));

	zassert_false(rsrp_monitor_record_signal_sample(&state, &test_config, -110));
	zassert_equal(-120, rsrp_monitor_unavailable_event_value(&state, &test_config));
}

ZTEST(rsrp_monitor, test_probe_history_average_uses_latest_ring_buffer_samples)
{
	struct rsrp_monitor_state state;

	rsrp_monitor_reset(&state);

	rsrp_monitor_history_add(&state, -100);
	rsrp_monitor_history_add(&state, -101);
	rsrp_monitor_history_add(&state, -102);
	rsrp_monitor_history_add(&state, -103);
	rsrp_monitor_history_add(&state, -104);
	rsrp_monitor_history_add(&state, -105);
	rsrp_monitor_history_add(&state, -106);

	zassert_equal(RSRP_MONITOR_HISTORY_LEN, rsrp_monitor_history_count(&state));
	zassert_equal(-103, rsrp_monitor_history_average(&state));
}

ZTEST(rsrp_monitor, test_record_available_resets_unavailable_count)
{
	struct rsrp_monitor_state state;

	rsrp_monitor_reset(&state);

	zassert_equal(1, rsrp_monitor_record_unavailable(&state));
	rsrp_monitor_record_available(&state);
	zassert_false(rsrp_monitor_unavailable_limit_reached(&state, &test_config));
	zassert_equal(1, rsrp_monitor_record_unavailable(&state));
}

ZTEST_SUITE(rsrp_parse, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(rsrp_monitor, NULL, NULL, NULL, NULL, NULL);
