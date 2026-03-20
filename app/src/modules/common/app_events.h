/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */ 

#pragma once

#include "app_types.h"
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

ZBUS_CHAN_DECLARE(app_evt_chan);

int app_event_put(const struct app_event *ev, k_timeout_t timeout);
const char *app_evt_name(enum app_evt_type type); 