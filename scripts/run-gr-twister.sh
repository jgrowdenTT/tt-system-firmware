#!/usr/bin/env bash
# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
#
# Usage: run-gr-twister.sh <mmk|qsr1a1> [twister build-phase args...]
#
# Run the device tests for a topology on a Grendel device via twister:
#   mmk     tt_mmk (Keraunos and Mimir), uses the gr_pcie_gdb runner
#   qsr1a1  tt_qsr1a1 (Keraunos, Mimir and Quasar)
# WARNING: the VM must be built with the matching topology; running on a device built with a
# different topology (e.g. qsr1a1 tests on an MMK VM) crashes it.
#
# Like scripts/ci/run-smoke.sh, scenarios are selected by the "smoke" tag across all test roots;
# a test opts in via `tags: smoke` and `platform_allow: <platform>/<soc>/smc` in its testcase.yaml.
# Upstream Zephyr kernel tests (tag "kernel", no platform_allow) run on every SMC in the topology.
#
# mmk runs build and test in one twister invocation (-j 8): builds are parallel and twister locks
# each hardware-map entry, so tests queue per chiplet (K and M may overlap).
# qsr1a1 runs two phases: all images are built in parallel (--build-only), then the device tests run
# one at a time (--test-only -j 1) since its chiplets interfere with each other.
# Extra arguments go to the (first) build invocation only (e.g. -s <test.id> to narrow the run), so the
# VS Code tasks just pass the options they need:
#   --clobber-output        clean build of everything, then run
#   --aggressive-no-clean   reuse the existing build dirs (incremental rebuild, no CMake) and rerun

set -euo pipefail

usage() {
	echo "Usage: ${0##*/} <mmk|qsr1a1> [twister build-phase args...]" >&2
	exit 2
}

[ $# -ge 1 ] || usage
topology="$1"
shift

case "$topology" in
mmk)
	platforms=(-p tt_mmk/tt_keraunos/smc -p tt_mmk/tt_mimir/smc)
	;;
qsr1a1)
	platforms=(-p tt_qsr1a1/tt_keraunos/smc -p tt_qsr1a1/tt_mimir/smc -p tt_qsr1a1/tt_quasar/smc)
	;;
*)
	usage
	;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$WORKSPACE"

source .venv/bin/activate
source zephyr/zephyr-env.sh >/dev/null 2>&1
export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-$HOME/zephyr-sdk-1.0.1}"

common=(
	--device-testing
	--hardware-map "tt-system-firmware/test-conf/hwmap-grendel.yaml"
	# Flash directly rather than via west (~0.07s vs ~0.25s per test); target comes from the hwmap id.
	--flash-command "$SCRIPT_DIR/gr_pcie_flash.py,--vuart-timeout=30"
	--tag smoke
	--tag kernel
	--alt-config-root ./tt-system-firmware/test-conf/samples
	--alt-config-root ./tt-system-firmware/test-conf/tests
	-T zephyr/samples -T zephyr/tests
	-T tt-system-firmware/tests -T tt-system-firmware/samples
	"${platforms[@]}"
	--outdir "twister-grendel-$topology" -v
)

# ttsim models one hart per SMC and no U-mode or PMP enforcement, so build single-CPU without them; SMP and userspace tests are filtered.
xconfig=(
	-x=CONFIG_RISCV_PMP=n
	-x=CONFIG_USERSPACE=n
	-x=CONFIG_MP_MAX_NUM_CPUS=1
	-x=CONFIG_UART_TT_VIRT_TX_BLOCK=y
	# thread_metric: 5s measurement periods instead of 30s (~15s -> ~3s per benchmark on ttsim).
	# Only read by tests/benchmarks/thread_metric; other tests ignore it.
	-x=TM_TEST_DURATION=5
)

rc=0
# Builds run in parallel; twister locks each hardware-map entry, so tests queue per chiplet.
west twister "${common[@]}" "${xconfig[@]}" "$@" -j 8 || rc=$?

exit $rc
