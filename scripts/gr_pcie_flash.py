#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""Flash a Zephyr build onto a Grendel SMC over PCIe, without west.

Does what `west flash -r gr_pcie_gdb` does, minus west's startup (manifest load, importing every
runner), which dominates the flash time of a small test image. Meant as twister's --flash-command:
twister appends `--build-dir <dir> --board-id <hwmap id>`, and the target SMC is taken from the
hwmap id's last dash-separated field (mmk-k0 -> k0, qsr1a1-q0 -> q0) unless --target is given.

    twister --device-testing --flash-command "scripts/gr_pcie_flash.py,--vuart-timeout=30" ...
"""

import argparse
import os
import sys

from gr_pcie_dm import CHIPLET_CHOICES, DEFAULT_RESET_VECTOR, DmError, NocDevice, SmcTarget, chiplet_spa_base


def images(build_dir):
    """zephyr.bin of each domain to flash, in order, like west flash does for sysbuild."""
    domains_yaml = os.path.join(build_dir, "domains.yaml")
    if not os.path.exists(domains_yaml):
        return [os.path.join(build_dir, "zephyr", "zephyr.bin")]

    import yaml

    with open(domains_yaml) as f:
        domains = yaml.safe_load(f)
    dirs = {d["name"]: d["build_dir"] for d in domains["domains"]}
    order = domains.get("flash_order") or [domains["default"]]
    return [os.path.join(dirs[name], "zephyr", "zephyr.bin") for name in order]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--build-dir", required=True, help="Zephyr (or sysbuild) build directory")
    p.add_argument("--board-id", help="twister hwmap id; its last '-' field names the target SMC")
    p.add_argument("--target", choices=CHIPLET_CHOICES, help="SMC to flash (overrides --board-id)")
    p.add_argument("--spa-base", help="hex SMC SPA base, overrides the target's table entry")
    p.add_argument("--device-id", type=int, default=0, help="/dev/tenstorrent/<id> (default: 0)")
    p.add_argument(
        "--reset-vector",
        type=lambda s: int(s, 16),
        default=DEFAULT_RESET_VECTOR,
        help=f"hex local load/start address (default: {DEFAULT_RESET_VECTOR:x})",
    )
    p.add_argument("--wipe-sram", action="store_true", help="zero the SMC SRAM before loading")
    p.add_argument(
        "--vuart-timeout",
        type=float,
        default=0,
        help="after release, wait up to this many seconds for the image to publish its VUART (default: 0)",
    )
    args = p.parse_args()

    target = args.target or (args.board_id.rsplit("-", 1)[-1] if args.board_id else None)
    if not target:
        p.error("need --target or --board-id")
    if target not in CHIPLET_CHOICES:
        p.error(f"target {target!r} (from --board-id {args.board_id!r}) is not one of {', '.join(CHIPLET_CHOICES)}")

    try:
        base = int(args.spa_base, 16) if args.spa_base else chiplet_spa_base(target)
        smc = SmcTarget(NocDevice(args.device_id), base)
        for path in images(args.build_dir):
            print(f"Loading {path} at 0x{args.reset_vector:x} on {target}", flush=True)
            with open(path, "rb") as f:
                image = f.read()
            smc.reset_and_load(
                image,
                args.reset_vector,
                wipe=args.wipe_sram,
                vuart_timeout=args.vuart_timeout,
                log=lambda msg: print(f"{target}: {msg}", flush=True),
            )
    except (DmError, OSError, ValueError) as e:
        print(f"{target}: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
