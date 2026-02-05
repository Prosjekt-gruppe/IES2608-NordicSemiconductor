# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
# NTN Prototype Application for Thingy:91 X

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ntn_prototype)

# Application sources
target_sources(app PRIVATE src/main.c)
