/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include "rsrp_parse.h"

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

ZTEST(rsrp_parse, test_unknown_rsrp_returns_enoent)
{
	int rsrp_dbm = 1234;
	int err = rsrp_parse_cesq_rsrp("+CESQ: 99,99,255,255,255,255", &rsrp_dbm);

	zassert_equal(-ENOENT, err, "raw 255 should report unknown RSRP");
	zassert_equal(1234, rsrp_dbm, "unknown RSRP should not overwrite output");
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

ZTEST_SUITE(rsrp_parse, NULL, NULL, NULL, NULL, NULL);
