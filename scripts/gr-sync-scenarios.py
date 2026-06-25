#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

"""Refresh the VS Code "grTest" picker from twister's collection.

Runs `twister --save-tests` (collection only, no build) for the smoke tag on the Grendel SMC platforms and
rewrites the options of the grTest input in a .code-workspace file as "<scenario> <platform>".

    Platforms are limited to $TTSIM_TOPOLOGY (mmk or qsr1a1) when set, see --topology.

    gr-sync-scenarios.py [--workspace ~/home.code-workspace] [--tag smoke]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

PLATFORMS = [
    "tt_mmk/tt_keraunos/smc",
    "tt_mmk/tt_mimir/smc",
    "tt_qsr1a1/tt_keraunos/smc",
    "tt_qsr1a1/tt_mimir/smc",
    "tt_qsr1a1/tt_quasar/smc",
]


def load_jsonc(path):
    """Load JSON that may contain // or /* */ comments and trailing commas (as VS Code allows)."""
    token = re.compile(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/', re.S)
    text = token.sub(lambda m: m.group(0) if m.group(0)[0] == '"' else "", open(path).read())
    text = re.sub(r'("(?:\\.|[^"\\])*")|,(\s*[}\]])', lambda m: m.group(1) or m.group(2), text)
    return json.loads(text)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    workspace_root = os.path.abspath(os.path.join(here, "..", ".."))
    p = argparse.ArgumentParser()
    p.add_argument("--workspace", default=os.path.expanduser("~/home.code-workspace"))
    p.add_argument("--tag", default="smoke")
    p.add_argument("--topology", default=os.environ.get("TTSIM_TOPOLOGY", ""),
                   help="limit to platforms of this SoC (e.g. mmk, qsr1a1); defaults to $TTSIM_TOPOLOGY, empty = all")
    args = p.parse_args()

    platforms = [pl for pl in PLATFORMS if not args.topology or pl.split("/")[0] == f"tt_{args.topology}"]
    if not platforms:
        raise SystemExit(f"no platforms match topology '{args.topology}'")

    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "tests.json")
        cmd = [sys.executable, "-m", "west", "twister", "--save-tests", out, "--outdir", os.path.join(tmp, "out")]
        cmd += ["--tag", args.tag]
        cmd += ["--tag", "kernel"]
        for root in ("samples", "tests"):
            cmd += ["--alt-config-root", f"./tt-system-firmware/test-conf/{root}"]
        for root in ("zephyr/samples", "zephyr/tests", "tt-system-firmware/tests", "tt-system-firmware/samples"):
            cmd += ["-T", root]
        for plat in platforms:
            cmd += ["-p", plat]
        subprocess.run(cmd, cwd=workspace_root, check=True, stdout=subprocess.DEVNULL)
        with open(out) as f:
            suites = json.load(f)["testsuites"]

    options = sorted({f"{s['name']} {s['platform']}" for s in suites})
    ws = load_jsonc(args.workspace)
    inputs = ws["tasks"]["inputs"]
    entry = next((i for i in inputs if i["id"] == "grTest"), None)
    if entry is None:
        raise SystemExit("workspace has no grTest input")
    entry["options"] = options
    if entry.get("default") not in options:
        entry["default"] = options[0]
    with open(args.workspace, "w") as f:
        json.dump(ws, f, indent="\t")
        f.write("\n")
    print(f"{len(options)} scenario/platform pairs written to {args.workspace}")


if __name__ == "__main__":
    main()
