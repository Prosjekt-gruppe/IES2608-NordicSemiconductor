#
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensors_service)

target_include_directories(testbinary PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../app/src/modules/sensors
)

target_sources(testbinary PRIVATE
    src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../app/src/modules/sensors/accel_logic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../app/src/modules/sensors/batt_logic.c
)
