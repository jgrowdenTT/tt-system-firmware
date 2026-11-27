# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
# Test-local bindings for the stub mailbox compatible live in dts/bindings.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tt_bl1_mis_mq_device)

set(SYSFW_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# The real BL1 Mimir/Quasar launcher is built unchanged. Its mailbox and gr_smc calls are
# replaced by FFF fakes in src/main.c, not by link-time wrapping.
target_include_directories(app PRIVATE ${SYSFW_ROOT}/app/bl1)

target_sources(app PRIVATE
  src/main.c
  ${SYSFW_ROOT}/app/bl1/mis_launch_mq.c
)
