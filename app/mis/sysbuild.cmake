# SPDX-License-Identifier: Apache-2.0

if(TARGET mis_mimir)
  return()
endif()

# Resolve board family
string(REPLACE "/" ";" BOARD_PARTS "${BOARD}")
list(GET BOARD_PARTS 0 BOARD_FAMILY)

# SoCs built for each board family
if(BOARD_FAMILY STREQUAL "tt_mmk")
  set(SOCS mimir keraunos)
elseif(BOARD_FAMILY STREQUAL "tt_qsr1a1")
  set(SOCS mimir keraunos quasar)
else()
  message(FATAL_ERROR "sysbuild: no SoC mapping for board family '${BOARD_FAMILY}'")
endif()

# Every SoC gets a bl1 and a mis image, except Keraunos, which has no mis image.
foreach(soc ${SOCS})
  ExternalZephyrProject_Add(
    APPLICATION bl1_${soc}
    SOURCE_DIR ${APP_DIR}/../bl1
    BOARD ${BOARD_FAMILY}/tt_${soc}/smc
    BUILD_ONLY 1
  )

  if(NOT soc STREQUAL "keraunos")
    ExternalZephyrProject_Add(
      APPLICATION mis_${soc}
      SOURCE_DIR ${APP_DIR}
      BOARD ${BOARD_FAMILY}/tt_${soc}/smc
      BUILD_ONLY 1
    )
  endif()
endforeach()

# bl0p5 is only built for Keraunos.
if("keraunos" IN_LIST SOCS)
  ExternalZephyrProject_Add(
    APPLICATION bl0p5_keraunos
    SOURCE_DIR ${APP_DIR}/../bl0p5
    BOARD ${BOARD_FAMILY}/tt_keraunos/smc
    BUILD_ONLY 1
  )
endif()
