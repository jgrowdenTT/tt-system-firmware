# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tt_smc_remoteproc_error)

# Calls from the driver to the OCCP library go to the __wrap_ fakes in src/main.c instead, so the
# real OCCP functions are linked but never called.
zephyr_link_libraries(-Wl,--wrap=occp_write_data -Wl,--wrap=occp_execute_image)

target_sources(app PRIVATE src/main.c)
