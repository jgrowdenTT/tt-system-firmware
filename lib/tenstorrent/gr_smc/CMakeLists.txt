# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(gr_smc)

zephyr_library_sources(grendel_utils.c)

zephyr_library_sources_ifdef(CONFIG_TT_TELEMETRY telemetry_gr.c)

zephyr_library_sources_ifdef(CONFIG_TT_GR_SMC_LOG_BRIDGE grendel_log_bridge.c)

# Sival SDK compilations
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/sival_sdk.cmake)
tt_sival_sdk_extract(${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr/blobs/grendel-sival-sdk.tar.gz)
set(SIVAL_SDK ${SIVAL_EXTDIR}/grendel_sival_sdk)

set(SIVAL_SDK_SOURCES
  ${SIVAL_SDK}/firmware/drivers/firewall/src/firewall.c
  ${SIVAL_SDK}/firmware/drivers/noc/src/itn.c
  ${SIVAL_SDK}/firmware/drivers/smc/src/smc_init.c
  ${SIVAL_SDK}/firmware/libs/basic_init/src/basic_init.c
  ${SIVAL_SDK}/firmware/libs/clock/src/clock.c
  ${SIVAL_SDK}/firmware/libs/hsio_tile/src/hsio_tile_init.c
  ${SIVAL_SDK}/firmware/libs/noc/src/noc_init.c)

zephyr_library_sources(${SIVAL_SDK_SOURCES})

set(SIVAL_NO_WERROR_SOURCES ${SIVAL_SDK_SOURCES})

if (CONFIG_SOC_TT_KERAUNOS_SMC OR CONFIG_SOC_TT_MIMIR_SMC)
zephyr_library_sources(${SIVAL_SDK}/firmware/drivers/addr_remap/src/remapper.c)
# remapper.c switches over an enum without handling the terminating member,
# which is flagged by -Werror=switch
list(APPEND SIVAL_NO_WERROR_SOURCES ${SIVAL_SDK}/firmware/drivers/addr_remap/src/remapper.c)
endif()

set_source_files_properties(${SIVAL_NO_WERROR_SOURCES}
  PROPERTIES COMPILE_OPTIONS "-Wno-error")

zephyr_library_include_directories(
  ${SIVAL_SDK}/common/include
  ${SIVAL_SDK}/firmware/common/include
  ${SIVAL_SDK}/firmware/libs/clock/include
  ${SIVAL_SDK}/firmware/libs/basic_init/include
  ${SIVAL_SDK}/firmware/libs/hsio_tile/include
  ${SIVAL_SDK}/firmware/drivers/firewall/include
  ${SIVAL_SDK}/firmware/drivers/smc/include)

# clock.c also pulls in the Quasar NOC/SMN headers, which only exist for the
# Quasar SoC.
if(CONFIG_SOC_TT_QUASAR_SMC)
zephyr_library_include_directories(
  ${SIVAL_SDK}/firmware/drivers/quasar_noc/include
  ${SIVAL_SDK}/firmware/libs/quasar_noc_api/include
  ${SIVAL_SDK}/firmware/drivers/quasar_smn/include
  ${SIVAL_SDK}/firmware/libs/quasar_smn_api/include)
endif()

zephyr_library_sources_ifdef(CONFIG_TT_GR_SMC_ERR_STR ${SIVAL_SDK}/firmware/common/src/err.c)
