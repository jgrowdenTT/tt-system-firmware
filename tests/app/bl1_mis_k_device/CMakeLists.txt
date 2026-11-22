# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tt_bl1_mis_k_device)

set(SYSFW_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# The real BL1 K launcher is built unchanged. src/main.c defines gr_smc_jump_to() itself as an FFF
# fake, since the real one is in lib/tenstorrent/gr_smc and is not built here. The remote chiplet
# launch is wrapped at link time and faked in src/main.c, since it would load and boot real Mimir
# chiplets.
target_include_directories(app PRIVATE ${SYSFW_ROOT}/app/bl1)
zephyr_link_libraries(-Wl,--wrap=tt_chiplet_remote_boot_launch_mis)

target_sources(app PRIVATE
  src/main.c
  ${SYSFW_ROOT}/app/bl1/mis_launch_k.c
)
