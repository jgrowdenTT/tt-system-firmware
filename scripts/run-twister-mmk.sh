#!/usr/bin/env bash
# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
#
# Run the tt_mmk (Keraunos and Mimir) device tests on the ttsim VM via twister.
# Like scripts/ci/run-smoke.sh, scenarios are selected by the "smoke" tag across all test roots;
# a test opts in via `tags: smoke` and `platform_allow: tt_mmk/tt_keraunos/smc` and/or `tt_mmk/tt_mimir/smc` in its testcase.yaml.
#
# Runs in two phases: all images are built in parallel (--build-only), then the device tests run
# Upstream Zephyr kernel tests (tag "kernel", no platform_allow) run on every SMC in the topology.
# one at a time (--test-only -j 1) since every test shares, and resets, the same simulated device.
# Extra arguments go to the build phase only (e.g. -s <test.id> to narrow the run), so the
# VS Code tasks just pass the options they need:
#   --clobber-output        clean build of everything, then run
#   --aggressive-no-clean   reuse the existing build dirs (incremental rebuild, no CMake) and rerun
#
# Uses the gr_pcie_gdb runner (see test-conf/hwmap-ttsim.yaml).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$WORKSPACE"

source .venv/bin/activate
source zephyr/zephyr-env.sh >/dev/null 2>&1
export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-$HOME/zephyr-sdk-1.0.1}"

common=(
	--device-testing
	--hardware-map tt-system-firmware/test-conf/hwmap-ttsim.yaml
	--tag smoke
	--tag kernel
	--alt-config-root ./tt-system-firmware/test-conf/samples
	--alt-config-root ./tt-system-firmware/test-conf/tests
	-T zephyr/samples -T zephyr/tests
	-T tt-system-firmware/tests -T tt-system-firmware/samples
	-p tt_mmk/tt_keraunos/smc -p tt_mmk/tt_mimir/smc
	--outdir twister-ttsim-mmk -v
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
