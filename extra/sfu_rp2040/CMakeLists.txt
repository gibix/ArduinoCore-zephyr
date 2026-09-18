# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sfu_rp2040)

target_sources(app PRIVATE src/main.c)

target_include_directories(app PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/../../variants/arduino_nano_connect_rp2040
)
