/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ACCEL_LOGIC_MOVEMENT_THRESHOLD_MG 120
#define ACCEL_LOGIC_QUIET_THRESHOLD_MG 40
#define ACCEL_LOGIC_LINEAR_DEADBAND_MG 25
#define ACCEL_LOGIC_MOVING_SPEED_MM_S 250
#define ACCEL_LOGIC_ZERO_VELOCITY_MS 4000
#define ACCEL_LOGIC_BASELINE_ADAPT_DIV 16
#define ACCEL_LOGIC_BASELINE_SAMPLES 8
#define ACCEL_LOGIC_GRAVITY_MIN_MG 700
#define ACCEL_LOGIC_GRAVITY_MAX_MG 1300
#define ACCEL_LOGIC_MG_TO_MM_S2 9807
#define ACCEL_LOGIC_MAX_DT_MS 1000

int32_t accel_logic_abs32(int32_t value);
uint32_t accel_logic_vector_magnitude_mg(int32_t x_mg, int32_t y_mg,
					 int32_t z_mg);
uint32_t accel_logic_raw_magnitude_mg(const int32_t xyz_mg[3]);
bool accel_logic_sample_is_gravity_like(const int32_t xyz_mg[3]);
bool accel_logic_velocity_is_zero(const int32_t velocity_mm_s[3]);
void accel_logic_zero_xyz(int32_t xyz[3]);
void accel_logic_copy_xyz(int32_t dst[3], const int32_t src[3]);
void accel_logic_apply_deadband(int32_t xyz_mg[3]);
void accel_logic_update_baseline(int32_t baseline_xyz_mg[3],
				 const int32_t xyz_mg[3]);
void accel_logic_integrate_velocity(int32_t velocity_mm_s[3],
				    const int32_t linear_xyz_mg[3],
				    uint32_t dt_ms);
uint32_t accel_logic_normalize_dt_ms(uint32_t dt_ms, uint32_t default_dt_ms);
