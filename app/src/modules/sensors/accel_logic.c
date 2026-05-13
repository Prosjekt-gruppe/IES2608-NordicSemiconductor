/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "accel_logic.h"

#include <stddef.h>

int32_t accel_logic_abs32(int32_t value)
{
	return (value < 0) ? -value : value;
}

static uint32_t accel_logic_isqrt64(uint64_t value)
{
	uint64_t result = 0;
	uint64_t bit = UINT64_C(1) << 62;

	while (bit > value) {
		bit >>= 2;
	}

	while (bit != 0U) {
		if (value >= result + bit) {
			value -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}

		bit >>= 2;
	}

	return (uint32_t)result;
}

uint32_t accel_logic_vector_magnitude_mg(int32_t x_mg, int32_t y_mg,
					 int32_t z_mg)
{
	uint64_t sum = ((int64_t)x_mg * x_mg) +
		       ((int64_t)y_mg * y_mg) +
		       ((int64_t)z_mg * z_mg);

	return accel_logic_isqrt64(sum);
}

uint32_t accel_logic_raw_magnitude_mg(const int32_t xyz_mg[3])
{
	return accel_logic_vector_magnitude_mg(xyz_mg[0], xyz_mg[1], xyz_mg[2]);
}

bool accel_logic_sample_is_gravity_like(const int32_t xyz_mg[3])
{
	uint32_t magnitude_mg = accel_logic_raw_magnitude_mg(xyz_mg);

	return (magnitude_mg >= ACCEL_LOGIC_GRAVITY_MIN_MG) &&
	       (magnitude_mg <= ACCEL_LOGIC_GRAVITY_MAX_MG);
}

bool accel_logic_velocity_is_zero(const int32_t velocity_mm_s[3])
{
	return (velocity_mm_s[0] == 0) &&
	       (velocity_mm_s[1] == 0) &&
	       (velocity_mm_s[2] == 0);
}

void accel_logic_zero_xyz(int32_t xyz[3])
{
	xyz[0] = 0;
	xyz[1] = 0;
	xyz[2] = 0;
}

void accel_logic_copy_xyz(int32_t dst[3], const int32_t src[3])
{
	dst[0] = src[0];
	dst[1] = src[1];
	dst[2] = src[2];
}

void accel_logic_apply_deadband(int32_t xyz_mg[3])
{
	for (size_t i = 0; i < 3; i++) {
		if (accel_logic_abs32(xyz_mg[i]) < ACCEL_LOGIC_LINEAR_DEADBAND_MG) {
			xyz_mg[i] = 0;
		}
	}
}

void accel_logic_update_baseline(int32_t baseline_xyz_mg[3],
				 const int32_t xyz_mg[3])
{
	for (size_t i = 0; i < 3; i++) {
		baseline_xyz_mg[i] +=
			(xyz_mg[i] - baseline_xyz_mg[i]) / ACCEL_LOGIC_BASELINE_ADAPT_DIV;
	}
}

void accel_logic_integrate_velocity(int32_t velocity_mm_s[3],
				    const int32_t linear_xyz_mg[3],
				    uint32_t dt_ms)
{
	for (size_t i = 0; i < 3; i++) {
		int64_t delta_v_mm_s =
			((int64_t)linear_xyz_mg[i] * ACCEL_LOGIC_MG_TO_MM_S2 * dt_ms) /
			1000000;

		velocity_mm_s[i] += (int32_t)delta_v_mm_s;
	}
}

uint32_t accel_logic_normalize_dt_ms(uint32_t dt_ms, uint32_t default_dt_ms)
{
	if ((dt_ms == 0U) || (dt_ms > ACCEL_LOGIC_MAX_DT_MS)) {
		return default_dt_ms;
	}

	return dt_ms;
}
