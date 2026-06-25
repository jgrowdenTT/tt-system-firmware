#!/usr/bin/env bash
# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
#
# Run the tt_qsr1a1 (Keraunos, Mimir and Quasar) device tests on a ttsim VM via twister.
# WARNING: the VM must be built with the QSR1A1 topology; on an MMK-topology VM this crashes it.
# Scenarios are selected by the "smoke" tag, as in run-twister-mmk.sh; a test opts in via
# `platform_allow: tt_qsr1a1/<soc>/smc` in its testcase.yaml.
#
# Runs in two phases: all images are built in parallel (--build-only), then the device tests run
# one at a time (--test-only -j 1) since every test shares, and resets, the same simulated device.
# Upstream Zephyr kernel tests (tag "kernel", no platform_allow) run on every SMC in the topology.
# Extra arguments go to the build phase only (e.g. -s <test.id>, --clobber-output,
# --aggressive-no-clean); the test phase runs whatever that phase built.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$WORKSPACE"

source .venv/bin/activate
source zephyr/zephyr-env.sh >/dev/null 2>&1
export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-$HOME/zephyr-sdk-1.0.1}"

common=(
	--device-testing
	--hardware-map tt-system-firmware/test-conf/hwmap-ttsim-qsr1a1.yaml
	--tag smoke
	--tag kernel
	--alt-config-root ./tt-system-firmware/test-conf/samples
	--alt-config-root ./tt-system-firmware/test-conf/tests
	-T zephyr/samples -T zephyr/tests
	-T tt-system-firmware/tests -T tt-system-firmware/samples
	-p tt_qsr1a1/tt_keraunos/smc
	--outdir twister-ttsim-qsr1a1 -v
)

# ttsim models one hart per SMC and no U-mode or PMP enforcement, so build single-CPU without them; SMP and userspace tests are filtered.
no_userspace=(
	-x=CONFIG_RISCV_PMP=n
	-x=CONFIG_USERSPACE=n
	-x=CONFIG_MP_MAX_NUM_CPUS=1
)

rc=0
west twister "${common[@]}" "${no_userspace[@]}" --build-only "$@" || rc=$?
west twister "${common[@]}" --test-only -j 1 || rc=$?
exit $rc
