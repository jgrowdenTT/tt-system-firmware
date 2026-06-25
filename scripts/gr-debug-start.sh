#!/usr/bin/env bash
# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
#
# VS Code preLaunchTask helper: run gr-debug.sh (build + gr_pcie_gdb server) detached, stream its
# output until the server listens, then exit 0 so the debugger can start. The server keeps running
# in the background and logs to twister-gr-debug/server.log; stop it with `pkill -f gr_pcie_gdb_server.py`.
#
#   gr-debug-start.sh "<scenario-id> <platform>" [target|auto] [port]

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOG="$WORKSPACE/twister-gr-debug/server.log"
PORT="${3:-3333}"
mkdir -p "$(dirname "$LOG")"
: >"$LOG"

pkill -f "gr_pcie_gdb_server.py.*--port $PORT" 2>/dev/null || true

setsid "$SCRIPT_DIR/gr-debug.sh" "$@" </dev/null >"$LOG" 2>&1 &
PID=$!

tail -n +1 -f "$LOG" &
TAIL=$!
trap 'kill $TAIL 2>/dev/null' EXIT

for _ in $(seq 1200); do
	if grep -q "listening on 127.0.0.1:$PORT" "$LOG"; then
		sleep 0.3
		echo "== GDB server ready on port $PORT (log: $LOG)"
		exit 0
	fi
	if ! kill -0 "$PID" 2>/dev/null; then
		sleep 0.3
		echo "== gr-debug.sh exited before the server was ready" >&2
		exit 1
	fi
	sleep 0.5
done
echo "== timed out waiting for the GDB server" >&2
exit 1
