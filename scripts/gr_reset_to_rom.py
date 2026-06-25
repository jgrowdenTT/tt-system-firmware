# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""Reset Grendel SMC cores so they restart from ROM/BL0 (KER_SMC_BL0_RESET_VECTOR) without loading an image.

    gr_reset_to_rom.py k0 m0 q0 [--device-id 0]
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gr_pcie_dm import CHIPLET_CHOICES, DmError, NocDevice, SmcTarget, chiplet_spa_base  # noqa: E402

BL0_RESET_VECTOR = 0xC0040000


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("targets", nargs="+", choices=CHIPLET_CHOICES)
    p.add_argument("--device-id", type=int, default=0)
    p.add_argument("--reset-vector", type=lambda s: int(s, 16), default=BL0_RESET_VECTOR)
    args = p.parse_args()

    dev = NocDevice(args.device_id)
    try:
        for name in args.targets:
            smc = SmcTarget(dev, chiplet_spa_base(name))
            smc.activate()
            smc.hold_reset()
            smc.clear_scratch()
            smc.set_reset_vector(args.reset_vector)
            smc.ack_havereset()
            smc.release_reset()
            print(f"{name}: reset to 0x{args.reset_vector:x}")
    except DmError as e:
        sys.exit(f"ERROR: {e}")
    finally:
        dev.close()


if __name__ == "__main__":
    main()
