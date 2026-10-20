# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tt_pcie)

target_sources(app PRIVATE src/main.c src/stage_bundle.c)

# Embed the real SERDES ICCM/DCCM images from the SiVal SDK
set(SIVAL_EXTDIR ${CMAKE_BINARY_DIR}/grendel-sival-sdk)
set(SIVAL_BLOBS_TARBALL ${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/blobs/grendel-sival-sdk.tar.gz)
set(SERDES_FW_DIR ${SIVAL_EXTDIR}/grendel_sival_sdk/firmware/drivers/pcie/fw)

file(MAKE_DIRECTORY ${SIVAL_EXTDIR})
execute_process(COMMAND tar -xzf ${SIVAL_BLOBS_TARBALL} WORKING_DIRECTORY ${SIVAL_EXTDIR}
  RESULT_VARIABLE SIVAL_TAR_RESULT)
if(NOT SIVAL_TAR_RESULT EQUAL 0)
  message(FATAL_ERROR "Failed to extract ${SIVAL_BLOBS_TARBALL}")
endif()

set(GEN_DIR ${ZEPHYR_BINARY_DIR}/include/generated)
foreach(mem iccm dccm)
  set(fw ${SERDES_FW_DIR}/serdes_${mem}_fw.bin)
  file(SIZE ${fw} fw_size)
  if(fw_size GREATER 65536)
    message(FATAL_ERROR "${fw} is ${fw_size} bytes; the driver reserves 64 KiB for ${mem}")
  endif()
  generate_inc_file_for_target(app ${fw} ${GEN_DIR}/serdes_${mem}_fw.bin.inc)
endforeach()
