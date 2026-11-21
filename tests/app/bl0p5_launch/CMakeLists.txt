# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tt_bl0p5_launch)

set(SYSFW_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# The launcher is built as it is in the BL0.5 image, so the tests run the real code.
target_include_directories(app PRIVATE ${SYSFW_ROOT}/app/bl0p5/src)
target_sources(app PRIVATE
  src/main.c
  ${SYSFW_ROOT}/app/bl0p5/src/bl0p5_launch.c
)
