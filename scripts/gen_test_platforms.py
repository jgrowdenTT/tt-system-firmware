#!/usr/bin/env python3
# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""Generate a doxygen fragment listing the platforms each test group runs on.

Twister decides which boards a testsuite runs on (testcase.yaml platform_allow /
platform_exclude / integration_platforms, intersected with the boards Zephyr knows
about). This script asks twister for that selection in list-only mode (nothing is
built). It then finds which doxygen groups each suite's documented test cases belong
to, and writes one @addtogroup block per group, so the platforms show up on the group
pages.

Suites are matched to groups from the source itself: a test case documented with
@ingroup contributes its suite's platforms to that group. Suites with no documented
test cases are skipped. No per-suite table has to be maintained.

Invoked from doc/Makefile as part of `make doxygen`.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Test roots that twister scans. The whole tests tree is scanned, so new areas are
# picked up without editing this script.
TEST_ROOTS = [
    "tests",
]

# A documented test case: a /** ... */ block directly followed by ZTEST(...).
DOC_BEFORE_ZTEST = re.compile(r"/\*\*((?:(?!\*/).)*?)\*/\s*ZTEST\s*\(", re.S)
INGROUP = re.compile(r"@ingroup\s+(\w+)")


def run_twister(zephyr_base: Path, json_out: Path, outdir: Path) -> dict:
    twister = zephyr_base / "scripts" / "twister"
    if not twister.is_file():
        sys.exit(f"gen_test_platforms: twister not found at {twister}; "
                 "set ZEPHYR_BASE to a Zephyr checkout")

    cmd = [sys.executable, str(twister), "-l", "--save-tests", str(json_out),
           "-O", str(outdir)]
    for root in TEST_ROOTS:
        cmd += ["-T", str(REPO_ROOT / root)]

    env = dict(os.environ, ZEPHYR_BASE=str(zephyr_base))
    proc = subprocess.run(cmd, cwd=zephyr_base, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        tail = "\n".join(proc.stdout.splitlines()[-20:])
        sys.exit(f"gen_test_platforms: twister failed (exit {proc.returncode}):\n{tail}")

    with open(json_out) as f:
        return json.load(f)


def suite_dir(zephyr_base: Path, path: str) -> Path:
    """Twister reports suite paths relative to the Zephyr base; make them absolute."""
    return (zephyr_base / path).resolve()


def display_name(platform: str) -> str:
    """Shorten a twister platform name for the docs.

    The "@revision" suffix is kept, since it tells apart boards such as the Blackhole
    P-series ("tt_blackhole@p100a/tt_blackhole/smc"). A two-part name is board/soc, so the
    SoC is dropped: "nucleo_u385rg_q@D/stm32u385xx" -> "nucleo_u385rg_q@D". Longer names
    (board/soc/variant) are kept whole, since the variant says which build actually runs.
    """
    parts = platform.split("/")
    return parts[0] if len(parts) == 2 else platform


def documented_groups(directory: Path) -> set:
    """Doxygen groups that the documented test cases under directory belong to."""
    groups = set()
    for source in directory.rglob("*"):
        if source.suffix not in (".c", ".h") or not source.is_file():
            continue
        text = source.read_text(errors="replace")
        for match in DOC_BEFORE_ZTEST.finditer(text):
            groups.update(INGROUP.findall(match.group(1)))
    return groups


def board_of(platform: str) -> str:
    """The board part of a twister platform name: "tt_mimir/tt_mimir/smc" -> "tt_mimir"."""
    return re.split(r"[/@]", platform, maxsplit=1)[0]


# Boards outside this repository whose results are documented: the nucleo we run
# hardware-free tests on, and native_sim.
EXTRA_SHOWN_BOARDS = {"nucleo_u385rg_q", "native_sim"}


def shown_boards() -> set:
    """Boards whose platforms appear in the docs: the boards this repository defines
    (boards/tenstorrent/*), plus EXTRA_SHOWN_BOARDS. Other boards that twister selects
    (e.g. Zephyr's own boards, picked up by the gpio-emul filter in pgood) are left out."""
    boards = {d.name for d in (REPO_ROOT / "boards" / "tenstorrent").iterdir() if d.is_dir()}
    return boards | EXTRA_SHOWN_BOARDS


def is_shown(platform: str, boards: set) -> bool:
    return board_of(platform) in boards


def collect(report: dict, zephyr_base: Path) -> tuple[dict, list]:
    """Return ({group: {platform: [(suite name, raw platform), ...]}}, undocumented dirs)."""
    groups: dict[str, dict[str, list]] = defaultdict(lambda: defaultdict(list))
    undocumented = []
    boards = shown_boards()
    for suite in report["testsuites"]:
        directory = suite_dir(zephyr_base, suite["path"])
        found = documented_groups(directory)
        if not found:
            undocumented.append(os.path.relpath(directory, REPO_ROOT))
            continue
        if not is_shown(suite["platform"], boards):
            continue
        platform = display_name(suite["platform"])
        for group in found:
            groups[group][platform].append((suite["name"], suite["platform"]))
    return groups, sorted(set(undocumented))


def load_ci_status(path: Path) -> dict:
    """The file written by fetch_ci_status.py. Missing or unreadable means unavailable."""
    try:
        status = json.loads(path.read_text())
    except (OSError, ValueError):
        return {"generated": None, "results": {}, "note": f"no CI status file at {path}"}
    status.setdefault("results", {})
    return status


PASS = "✅"  # green check mark: passed on main
FAIL = "❌"  # red cross mark: failed on main
NO_RESULT = "⚠️"  # yellow warning: no pass or fail recorded on main
UNKNOWN = "—"  # em dash: CI status could not be read


def ci_entry(ci: dict, name: str, raw: str):
    """The CI result for a suite on a platform, or None.

    twister -l names native_sim "native_sim/native/64", but a run records the same suite
    as "native_sim/native", so fall back to that name.
    """
    entry = ci["results"].get(f"{name}|{raw}")
    if entry is None and raw.endswith("/64"):
        entry = ci["results"].get(f"{name}|{raw[:-len('/64')]}")
    return entry


def platform_row(platform: str, sources: list, ci: dict) -> str:
    """One table row: the board, its result on main, and a link to the relevant run.

    A red cross means a suite on this board failed in its latest run on main, and the
    link goes to that run. A green check links to the newest passing run. A yellow
    warning means nothing on main has passed or failed here yet.
    """
    # Doxygen reads a bare "@" as a command, so "tt_blackhole@p100a" needs "@@" to render.
    platform = platform.replace("@", "@@")
    if ci.get("generated") is None:
        return f" * | {platform} | {UNKNOWN} | Not Available |"
    entries = [e for e in (ci_entry(ci, name, raw) for name, raw in sources) if e]
    failed = [e["latest"] for e in entries if e["latest"]["status"] in ("failed", "error")]
    passing = [e["passing"] for e in entries if "passing" in e]
    if failed:
        newest = max(failed, key=lambda r: r["created"])
        return f" * | {platform} | {FAIL} | [link]({newest['run_url']}) |"
    if passing:
        newest = max(passing, key=lambda r: r["created"])
        return f" * | {platform} | {PASS} | [link]({newest['run_url']}) |"
    return f" * | {platform} | {NO_RESULT} | Not Available |"


def render(groups: dict, ci: dict) -> str:
    lines = [
        "/* Generated by scripts/gen_test_platforms.py from twister's selection.",
        " * Do not edit; run `make doxygen` in doc/ to regenerate. */",
        "",
    ]
    for group in sorted(groups):
        lines += [
            "/**",
            f" * @addtogroup {group}",
            " * @par Platforms",
            " *",
            " * | Board | Result on main | Run |",
            " * |---|:---:|---|",
        ]
        for platform in sorted(groups[group]):
            lines.append(platform_row(platform, groups[group][platform], ci))
        lines += [" */", ""]
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--zephyr-base", required=True, type=Path,
                        help="Zephyr checkout that provides scripts/twister")
    parser.add_argument("--output", required=True, type=Path,
                        help="path of the generated .dox file")
    parser.add_argument("--ci-status", type=Path,
                        help="ci_status.json from fetch_ci_status.py (optional)")
    args = parser.parse_args()

    zephyr_base = args.zephyr_base.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    ci = load_ci_status(args.ci_status) if args.ci_status else {"generated": None,
                                                              "results": {}}

    with tempfile.TemporaryDirectory(prefix="test-platforms-") as tmp:
        report = run_twister(zephyr_base, Path(tmp) / "tests.json", Path(tmp) / "out")
        groups, undocumented = collect(report, zephyr_base)

    args.output.write_text(render(groups, ci))
    print(f"gen_test_platforms: wrote {args.output} ({len(groups)} groups)")
    if ci.get("note"):
        print(f"gen_test_platforms: CI status: {ci['note']}")
    if undocumented:
        print(f"gen_test_platforms: {len(undocumented)} suites have no documented test "
              "cases and are not listed:")
        for path in undocumented:
            print(f"  {path}")


if __name__ == "__main__":
    main()
