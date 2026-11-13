# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/git_version.cmake)

target_sources(app PRIVATE main.c)
if(CONFIG_SOC_TT_KERAUNOS_SMC)
    target_sources(app PRIVATE mis_launch_k.c)
elseif(CONFIG_SOC_TT_MIMIR_SMC)
    target_sources(app PRIVATE mis_launch_mq.c)
elseif(CONFIG_SOC_TT_QUASAR_SMC)
    target_sources(app PRIVATE mis_launch_mq.c)
else()
    message(FATAL_ERROR "BL1 app is only supported on keraunos_smc, mimir_smc, or quasar_smc")
endif()