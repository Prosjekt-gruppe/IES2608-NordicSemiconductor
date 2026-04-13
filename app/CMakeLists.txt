#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app)

target_sources(app PRIVATE src/main.c
    src/modules/common/app_events.c
    src/modules/common/app_zbus.c
    src/modules/core/app_sm.c
    src/modules/gnss/gnss_service.c
    src/modules/lte/lte_service.c
    src/modules/modem/modem_service.c
    src/modules/ntn/ntn_service.c
    src/modules/rsrp/rsrp_service.c
    src/modules/location/location_service.c
    src/modules/cloud/cloud_service.c
)


target_include_directories(app PRIVATE
    src/modules/common
    src/modules/core
    src/modules/gnss
    src/modules/lte
    src/modules/modem
    src/modules/ntn
    src/modules/rsrp
    src/modules/location
    src/modules/cloud
    src/modules/sensors
)

if(CONFIG_APP_SENSOR_ACCEL_DEMO)
    target_sources(app PRIVATE src/modules/sensors/accel.c)
endif()

if(CONFIG_APP_SENSOR_BATT_DEMO)
    target_sources(app PRIVATE src/modules/sensors/batt.c)
endif()
