#!/usr/bin/env bash
# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
#
# Reset every SMC in the MMK topology to ROM/BL0. Used as the twister pre_script in
# test-conf/hwmap-ttsim.yaml, which cannot pass arguments.

exec python3 "$(dirname "${BASH_SOURCE[0]}")/gr_reset_to_rom.py" k0 m0 m1 "$@"
