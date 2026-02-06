# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(Hello_ZBus)

zephyr_include_directories(src/common)


target_sources(app PRIVATE src/main.c)


add_subdirectory(src/modules/hello)
add_subdirectory(src/modules/world)