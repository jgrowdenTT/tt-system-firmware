#!/usr/bin/env bash
# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
#
# Build one twister scenario (same conf/overlays as CI, via test-conf alt-config-root) and
# start the gr_pcie_gdb GDB server on it, halted at the reset vector.
#
#   gr-debug.sh "<scenario-id> <platform>" [target|auto] [port]
#   gr-debug.sh "sample.basic.helloworld.grendel tt_mmk/tt_keraunos/smc" k0
#
# target "auto" (default) follows hwmap-ttsim.yaml: keraunos k0, mimir m1 on tt_mmk (m0 on tt_qsr1a1), quasar q0.
#
# Reuses an existing build dir with an incremental ninja build; delete the dir to force twister.
# Leaves the ELF at twister-gr-debug/current.elf (the VS Code launch config uses that path).
# The server prints "listening on" when ready, which the VS Code task uses as its ready signal.

set -euo pipefail

read -r SCENARIO PLATFORM <<<"${1:?\"<scenario-id> <platform>\"}"
: "${PLATFORM:?platform missing, expected \"<scenario-id> <platform>\"}"
TARGET="${2:-auto}"
PORT="${3:-3333}"

if [ "$TARGET" = auto ]; then
	case "$PLATFORM" in
	tt_mmk/tt_keraunos/* | tt_qsr1a1/tt_keraunos/*) TARGET=k0 ;;
	tt_mmk/tt_mimir/*) TARGET=m1 ;;
	tt_qsr1a1/tt_mimir/*) TARGET=m0 ;;
	tt_qsr1a1/tt_quasar/*) TARGET=q0 ;;
	*) echo "cannot derive target for $PLATFORM; pass one explicitly" >&2; exit 1 ;;
	esac
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$WORKSPACE"

source .venv/bin/activate
source zephyr/zephyr-env.sh >/dev/null 2>&1
export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-$HOME/zephyr-sdk-1.0.1}"

OUTDIR=twister-gr-debug

# Stop a server left over from the previous debug session.
pkill -f "gr_pcie_gdb_server.py.*--port $PORT" 2>/dev/null || true

app_cfg_list() {
	find "$1" -maxdepth 3 \( -name "*.conf" -o -name "*.overlay" -o -name "*.dts*" \) | sort
}

find_elf() {
	find "$OUTDIR/${PLATFORM//\//_}" -path "*/$SCENARIO/zephyr/zephyr.elf" -print -quit 2>/dev/null || true
}

# Reuse an existing build: ninja re-runs CMake itself when CMakeLists, conf, overlays or Kconfig
# change. Only fall back to twister (slow test collection) for a first build, or when the twister
# yaml that sets extra_args/extra_configs changed since the last twister run (stamp file); in that
# case twister runs without --aggressive-no-clean so CMake is re-run with the new arguments.
# Zephyr resolves the app-dir board .conf/.overlay set once and caches it (CONF_FILE,
# DTC_OVERLAY_FILE), so adding or removing such a file needs a pristine build: that is detected
# from a saved file list and the build dir is deleted and rebuilt by twister.
ELF="$(find_elf)"
NEED_TWISTER=1
if [ -n "$ELF" ]; then
	BUILD_DIR="$(dirname "$(dirname "$ELF")")"
	STAMP="$BUILD_DIR/.gr-debug-twister-stamp"
	APP_SRC="$(sed -n "s/^APPLICATION_SOURCE_DIR:[A-Z]*=//p" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null)"
	if [ -f "$BUILD_DIR/build.ninja" ] && [ -f "$STAMP" ] && [ -n "$APP_SRC" ] &&
		[ -z "$(find "$APP_SRC" -maxdepth 1 -name "*.yaml" -newer "$STAMP" -print -quit)" ] &&
		[ -z "$(find tt-system-firmware/test-conf -name "*.yaml" -newer "$STAMP" -print -quit)" ]; then
		NEED_TWISTER=0
	fi
	# Thread awareness (CONFIG_DEBUG_THREAD_INFO) is added through twister, so builds made without it
	# need one reconfigure; the marker keeps scenarios that cannot enable it from looping.
	[ -f "$BUILD_DIR/.gr-debug-thread-info" ] || NEED_TWISTER=1
	if [ -n "$APP_SRC" ] && [ "$(app_cfg_list "$APP_SRC")" != "$(cat "$BUILD_DIR/.gr-debug-cfg-list" 2>/dev/null)" ]; then
		echo "Config file set changed in $APP_SRC, pristine rebuild of $BUILD_DIR"
		rm -rf "$BUILD_DIR"
		ELF=""
		NEED_TWISTER=1
	fi
fi

if [ "$NEED_TWISTER" = 1 ]; then
	NOCLEAN=(--no-clean)
	[ -n "$ELF" ] || NOCLEAN+=(--aggressive-no-clean)
	west twister --build-only "${NOCLEAN[@]}" \
		--alt-config-root ./tt-system-firmware/test-conf/samples \
		--alt-config-root ./tt-system-firmware/test-conf/tests \
		-T zephyr/samples -T zephyr/tests \
		-T tt-system-firmware/tests -T tt-system-firmware/samples \
		-x=CONFIG_DEBUG_THREAD_INFO=y \
		-p "$PLATFORM" -s "$SCENARIO" \
		--outdir "$OUTDIR" 1>&2
	ELF="$(find_elf)"
	if [ -n "$ELF" ]; then
		BUILD_DIR="$(dirname "$(dirname "$ELF")")"
		touch "$BUILD_DIR/.gr-debug-twister-stamp" "$BUILD_DIR/.gr-debug-thread-info"
		APP_SRC="$(sed -n "s/^APPLICATION_SOURCE_DIR:[A-Z]*=//p" "$BUILD_DIR/CMakeCache.txt")"
		app_cfg_list "$APP_SRC" >"$BUILD_DIR/.gr-debug-cfg-list"
	fi
else
	echo "Incremental build: $BUILD_DIR"
	ninja -C "$BUILD_DIR" 1>&2
fi

if [ -z "$ELF" ]; then
	echo "no zephyr.elf found for $SCENARIO in $OUTDIR" >&2
	exit 1
fi
BUILD_DIR="$(dirname "$(dirname "$ELF")")"
ln -sf "$WORKSPACE/$ELF" "$OUTDIR/current.elf"
echo "ELF: $WORKSPACE/$OUTDIR/current.elf -> $WORKSPACE/$ELF"

exec west debugserver -d "$BUILD_DIR" -r gr_pcie_gdb --target "$TARGET" --port "$PORT"
