#!/usr/bin/env python3
# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""Console wrapper for twister's serial_pty: runs keraunos_vuart_console, retrying while the
firmware has not yet published its VUART descriptor (the tool exits immediately in that case).

Usage: tt_keraunos_console.py <k|m0|m1|m2|m3|q0> <device_id> [--also TARGET]... [--tools-dir DIR]
                              [--timeout SEC] [--start-delay SEC]
"""

import argparse
import os
import re
import signal
import subprocess
import sys
import threading
import time

PASS_TOKEN = 0xACAFACA1  # test_pass() in soc/tenstorrent/tt_grendel/tt_grendel_smc/soc.h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target")
    ap.add_argument("device_id")
    ap.add_argument(
        "--tools-dir",
        default=os.environ.get("TT_KMD_TOOLS", os.path.expanduser("~/tt-kmd/tools")),
    )
    ap.add_argument("--timeout", type=float, default=30.0, help="max seconds to wait for VUART")
    ap.add_argument(
        "--start-delay",
        type=float,
        default=0.1,
        help="seconds to wait before attaching so twister opens the pty first (else output is lost)",
    )
    ap.add_argument(
        "--also",
        action="append",
        default=[],
        metavar="TARGET",
        help="also show this chiplet's console, merged line-by-line into the output (repeatable; "
        "e.g. m1 so a remote-booted Mimir's ztest output reaches twister). Retried until the "
        "chiplet publishes its VUART.",
    )
    ap.add_argument(
        "--pass-scratch",
        metavar="TARGET",
        help="poll TARGET's SCRATCH_0 for the test_pass() token (0xacafaca1) and, once seen, print "
        "'PROJECT EXECUTION SUCCESSFUL' so twister's ztest harness can pass. For tests (e.g. "
        "tt_smc_remoteproc) whose verdict is that register rather than console output.",
    )
    args = ap.parse_args()

    tool = os.path.join(args.tools_dir, "keraunos_vuart_console")
    deadline = time.monotonic() + args.timeout
    stop = threading.Event()
    out_lock = threading.Lock()
    procs = []

    def emit(line):
        with out_lock:
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()

    def pump(proc):
        for line in iter(proc.stdout.readline, b""):
            emit(line)

    def secondary(target):
        while not stop.is_set():
            p = subprocess.Popen(
                [tool, target, args.device_id], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
            )
            procs.append(p)
            pump(p)
            p.wait()
            p.stdout.close()  # procs keeps the Popen alive, so close the pipe or the fds leak on every retry
            stop.wait(0.5)

    def watch_pass_token(target):
        while not stop.is_set():
            res = subprocess.run(
                [tool, target, "--helper", args.device_id], capture_output=True, text=True
            )
            m = re.search(r"SCRATCH_0 .*= 0x([0-9a-fA-F]+)", res.stdout)
            if m and int(m.group(1), 16) == PASS_TOKEN:
                emit(f"[tt_keraunos_console] {target} SCRATCH_0 = 0x{PASS_TOKEN:x} (test_pass)\n".encode())
                emit(b"PROJECT EXECUTION SUCCESSFUL\n")
                return
            stop.wait(0.5)

    def shutdown(sig, _frame):
        stop.set()
        for p in procs:
            if p.poll() is None:
                p.send_signal(signal.SIGINT)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    time.sleep(args.start_delay)
    for tgt in args.also:
        threading.Thread(target=secondary, args=(tgt,), daemon=True).start()
    if args.pass_scratch:
        threading.Thread(target=watch_pass_token, args=(args.pass_scratch,), daemon=True).start()

    rc = 0
    while not stop.is_set():
        start = time.monotonic()
        proc = subprocess.Popen([tool, args.target, args.device_id], stdout=subprocess.PIPE)
        procs.append(proc)
        pump(proc)
        rc = proc.wait()
        # A run that lasted a while was a real session; only retry quick start-up failures.
        if rc == 0 or time.monotonic() - start > 2 or time.monotonic() > deadline:
            break
        time.sleep(0.5)

    shutdown(signal.SIGINT, None)
    return rc


if __name__ == "__main__":
    sys.exit(main())
