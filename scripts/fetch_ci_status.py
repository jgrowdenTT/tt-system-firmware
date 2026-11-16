#!/usr/bin/env python3
# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""Record the most recent result on main for each test suite.

Reads the twister.json files from recent CI runs that tested main:

- the "Unit" workflow (run-unit-tests.yml): pushes to main and merge-queue runs for main;
- the "HW Smoke" workflow (hardware-smoke.yml): merge-queue runs for main (it has no push
  trigger on main). Only its SMC and DMC smoke artifacts are read.

For every (suite, platform) it writes the latest result (passed or failed), and the newest
passing run. The documentation build uses this file to show each platform's result on a
test group page, and to link to the run.

This script never fails the build. If gh is missing, not logged in, or the data cannot
be read, it writes an empty result with a note explaining why, and exits 0. A test that
has never passed on main simply has no entry.

Used by doc/Makefile (`ci-status`) for local builds and by .github/workflows/docs.yml.
"""

import argparse
import datetime
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = "tenstorrent/tt-system-firmware"
MAX_RUNS = 10
RUN_FIELDS = "databaseId,headSha,url,createdAt,conclusion,headBranch"
MERGE_QUEUE_PREFIX = "gh-readonly-queue/main/"


def _is_unit_results(name: str) -> bool:
    return name == "unit-test-results"


def _is_hw_smoke_results(name: str) -> bool:
    return name.startswith(("SMC Smoke test results", "DMC Smoke test results"))


# Each source: the workflow file, whether pushes to main count, and which artifacts hold
# twister.json results.
SOURCES = [
    {"workflow": "run-unit-tests.yml", "pushes": True, "artifact": _is_unit_results},
    {"workflow": "hardware-smoke.yml", "pushes": False, "artifact": _is_hw_smoke_results},
]


def gh(*args: str) -> str:
    proc = subprocess.run(["gh", *args], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"gh {args[0]} failed")
    return proc.stdout


def recent_main_runs(source: dict) -> list:
    """Recent runs of one workflow that tested what is on main, newest first.

    A merge-queue run tests the commit that is then merged into main, so it counts as a
    main result. Pushes to main count only where the source says so.
    """
    queued = json.loads(gh("run", "list", "-R", REPO, "--workflow", source["workflow"],
                           "--event", "merge_group", "--limit", str(MAX_RUNS * 3),
                           "--json", RUN_FIELDS))
    runs = [r for r in queued if r["headBranch"].startswith(MERGE_QUEUE_PREFIX)]
    if source["pushes"]:
        runs += json.loads(gh("run", "list", "-R", REPO, "--workflow", source["workflow"],
                              "--branch", "main", "--event", "push",
                              "--limit", str(MAX_RUNS), "--json", RUN_FIELDS))
    runs.sort(key=lambda r: r["createdAt"], reverse=True)
    return [(source, run) for run in runs[:MAX_RUNS]]


def artifact_names(run_id: int, wanted) -> list:
    """Names of the non-expired artifacts of a run that the source wants."""
    out = gh("api", f"repos/{REPO}/actions/runs/{run_id}/artifacts", "--paginate",
             "-q", ".artifacts[] | select(.expired | not) | .name")
    return [name for name in out.splitlines() if wanted(name)]


FAILED_STATUSES = {"failed", "error"}


def record_results(twister_json: Path, run: dict, results: dict) -> None:
    """Record each (suite, platform) result from this run that is not yet recorded.

    Runs are visited newest first, so the first result seen for a key is its latest result
    on main ("latest"), and the first pass seen is the last passing run ("passing").
    Results that are neither a pass nor a failure (skipped, filtered) are ignored.
    """
    report = json.loads(twister_json.read_text())
    for suite in report.get("testsuites", []):
        status = suite.get("status")
        if status != "passed" and status not in FAILED_STATUSES:
            continue
        key = f"{suite['name']}|{suite['platform']}"
        entry = results.setdefault(key, {})
        info = {
            "status": status,
            "run_url": run["url"],
            "commit": run["headSha"],
            "created": run["createdAt"],
        }
        entry.setdefault("latest", info)
        if status == "passed":
            entry.setdefault("passing", info)


def collect() -> dict:
    result = {"generated": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "results": {}, "note": ""}
    runs = []
    for source in SOURCES:
        runs += recent_main_runs(source)
    if not runs:
        result["note"] = "no runs of the Unit or HW Smoke workflows on main yet"
        return result
    runs.sort(key=lambda sr: sr[1]["createdAt"], reverse=True)

    with tempfile.TemporaryDirectory(prefix="ci-status-") as tmp:
        for index, (source, run) in enumerate(runs):
            for name in artifact_names(run["databaseId"], source["artifact"]):
                dest = Path(tmp) / f"{index}-{name}"
                try:
                    gh("run", "download", str(run["databaseId"]), "-R", REPO,
                       "-n", name, "-D", str(dest))
                except RuntimeError:
                    continue  # artifact cancelled or unavailable; try the next one
                for twister_json in dest.rglob("twister.json"):
                    record_results(twister_json, run, result["results"])
    if not result["results"]:
        result["note"] = "no passing test results found on main in the last runs"
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    try:
        result = collect()
    except (RuntimeError, OSError, ValueError, json.JSONDecodeError) as exc:
        result = {"generated": None, "results": {},
                  "note": f"CI status unavailable: {exc}"}

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(f"fetch_ci_status: {len(result['results'])} results "
          f"({result['note'] or 'ok'})", file=sys.stderr)


if __name__ == "__main__":
    main()
