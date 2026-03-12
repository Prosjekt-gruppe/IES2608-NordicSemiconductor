/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once

#include "app_types.h"
#include <zephyr/kernel.h>

int app_event_put(const struct app_event *ev, k_timeout_t timeout);
int app_event_get(struct app_event *ev, k_timeout_t timeout);