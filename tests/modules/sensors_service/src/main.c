/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "accel_logic.h"
#include "batt_logic.h"

ZTEST(accel_logic, test_vector_magnitude_uses_integer_square_root)
{
	zassert_equal(1300, accel_logic_vector_magnitude_mg(300, 400, 1200));
	zassert_equal(5, accel_logic_vector_magnitude_mg(3, 4, 0));
}

ZTEST(accel_logic, test_gravity_detection_includes_configured_boundaries)
{
	const int32_t below_min[3] = { 699, 0, 0 };
	const int32_t at_min[3] = { 700, 0, 0 };
	const int32_t at_max[3] = { 1300, 0, 0 };
	const int32_t above_max[3] = { 1301, 0, 0 };

	zassert_false(accel_logic_sample_is_gravity_like(below_min));
	zassert_true(accel_logic_sample_is_gravity_like(at_min));
	zassert_true(accel_logic_sample_is_gravity_like(at_max));
	zassert_false(accel_logic_sample_is_gravity_like(above_max));
}

ZTEST(accel_logic, test_deadband_zeros_values_strictly_inside_threshold)
{
	int32_t xyz_mg[3] = {
		ACCEL_LOGIC_LINEAR_DEADBAND_MG - 1,
		-ACCEL_LOGIC_LINEAR_DEADBAND_MG + 1,
		ACCEL_LOGIC_LINEAR_DEADBAND_MG,
	};

	accel_logic_apply_deadband(xyz_mg);

	zassert_equal(0, xyz_mg[0]);
	zassert_equal(0, xyz_mg[1]);
	zassert_equal(ACCEL_LOGIC_LINEAR_DEADBAND_MG, xyz_mg[2]);
}

ZTEST(accel_logic, test_baseline_update_uses_truncated_integer_adaptation)
{
	int32_t baseline[3] = { 1000, -1000, 0 };
	const int32_t sample[3] = { 1160, -840, 15 };

	accel_logic_update_baseline(baseline, sample);

	zassert_equal(1010, baseline[0]);
	zassert_equal(-990, baseline[1]);
	zassert_equal(0, baseline[2]);
}

ZTEST(accel_logic, test_velocity_integration_accumulates_signed_axes)
{
	int32_t velocity_mm_s[3] = { 1, -1, 0 };
	const int32_t linear_mg[3] = { 100, -50, 0 };

	accel_logic_integrate_velocity(velocity_mm_s, linear_mg, 200);

	zassert_equal(197, velocity_mm_s[0]);
	zassert_equal(-99, velocity_mm_s[1]);
	zassert_equal(0, velocity_mm_s[2]);
}

ZTEST(accel_logic, test_dt_normalization_replaces_zero_or_large_deltas)
{
	zassert_equal(ACCEL_LOGIC_MAX_DT_MS, accel_logic_normalize_dt_ms(
			      ACCEL_LOGIC_MAX_DT_MS, 200));
	zassert_equal(200, accel_logic_normalize_dt_ms(0, 200));
	zassert_equal(200, accel_logic_normalize_dt_ms(ACCEL_LOGIC_MAX_DT_MS + 1U, 200));
}

ZTEST(accel_logic, test_vector_helpers_copy_zero_and_detect_zero_velocity)
{
	int32_t dst[3] = { 1, 2, 3 };
	const int32_t src[3] = { -3, 0, 5 };

	accel_logic_copy_xyz(dst, src);
	zassert_equal(-3, dst[0]);
	zassert_equal(0, dst[1]);
	zassert_equal(5, dst[2]);
	zassert_false(accel_logic_velocity_is_zero(dst));

	accel_logic_zero_xyz(dst);
	zassert_true(accel_logic_velocity_is_zero(dst));
}

ZTEST(batt_logic, test_battery_level_strings_cover_voltage_boundaries)
{
	zassert_equal(0, strcmp("critical", batt_logic_level_string(3399)));
	zassert_equal(0, strcmp("low", batt_logic_level_string(3400)));
	zassert_equal(0, strcmp("medium", batt_logic_level_string(3600)));
	zassert_equal(0, strcmp("high", batt_logic_level_string(3850)));
	zassert_equal(0, strcmp("full", batt_logic_level_string(4100)));
}

ZTEST(batt_logic, test_charge_state_strings_follow_status_priority)
{
	zassert_equal(0, strcmp("idle", batt_logic_charge_state_string(0)));
	zassert_equal(0, strcmp("topping-off",
				batt_logic_charge_state_string(BATT_LOGIC_STATUS_CV_MASK)));
	zassert_equal(0, strcmp("charging",
				batt_logic_charge_state_string(BATT_LOGIC_STATUS_CC_MASK)));
	zassert_equal(0, strcmp("trickle",
				batt_logic_charge_state_string(BATT_LOGIC_STATUS_TRICKLE_MASK)));
	zassert_equal(0, strcmp("complete",
				batt_logic_charge_state_string(BATT_LOGIC_STATUS_COMPLETE_MASK |
							      BATT_LOGIC_STATUS_CC_MASK)));
}

ZTEST_SUITE(accel_logic, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(batt_logic, NULL, NULL, NULL, NULL, NULL);
