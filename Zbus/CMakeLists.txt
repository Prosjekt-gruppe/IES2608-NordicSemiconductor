cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cellular_fundamentals)

# NORDIC SDK APP START
target_include_directories(app PRIVATE include)

target_sources(app PRIVATE
	src/app_zbus.c
	src/gnss.c
	src/ltem.c
	src/main.c
)
# NORDIC SDK APP END
