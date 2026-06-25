# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_SOC_TT_QUASAR_SMC)
  board_runner_args(gr_pcie_gdb "--target=q0")
elseif(CONFIG_SOC_TT_MIMIR_SMC)
  board_runner_args(gr_pcie_gdb "--target=m0")
else()
  board_runner_args(gr_pcie_gdb "--target=k0")
endif()

board_set_flasher_ifnset(gr_pcie_gdb)
board_finalize_runner_args(gr_pcie_gdb)
