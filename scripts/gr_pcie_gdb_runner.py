# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""West runner for Grendel SMC cores (Keraunos, Mimir, Quasar) through their RISC-V Debug Module.

Everything is done over PCIe (tt-kmd NOC ioctls); no JTAG or other probe is needed.

    west flash      -r gr_pcie_gdb --target m0        load zephyr.bin at the reset vector and run
    west flash      -r gr_pcie_gdb --halt             ... and leave the hart halted at the first instruction
    west reset      -r gr_pcie_gdb                    reset the SMC (reloads the image) and run
    west attach     -r gr_pcie_gdb                    GDB server + GDB, attach without resetting
    west debug      -r gr_pcie_gdb                    load the image, halt at reset, GDB server + GDB
    west debugserver -r gr_pcie_gdb                   GDB server only (connect with `target remote :3333`)
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time

from runners.core import RunnerCaps, ZephyrBinaryRunner

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gr_pcie_dm import (  # noqa: E402
    CHIPLET_CHOICES,
    DmError,
    SCRATCH_OFFSET,
    NocDevice,
    SmcTarget,
    chiplet_spa_base,
)

DEFAULT_RESET_VECTOR = "c0067000"


class GrPcieGdbBinaryRunner(ZephyrBinaryRunner):
    def __init__(self, cfg, target, spa_base, device_id, port, reset_vector, halt, no_reload, wipe_sram, vuart_timeout):
        super().__init__(cfg)
        self.target = target
        self.spa_base = spa_base
        self.device_id = device_id
        self.port = port
        self.reset_vector = int(reset_vector, 16)
        self.halt = halt
        self.no_reload = no_reload
        self.wipe_sram = wipe_sram
        self.vuart_timeout = vuart_timeout
        self.server_script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "gr_pcie_gdb_server.py")

    @classmethod
    def name(cls):
        return "gr_pcie_gdb"

    @classmethod
    def capabilities(cls):
        return RunnerCaps(commands={"flash", "debug", "debugserver", "attach", "reset"})

    @classmethod
    def do_add_parser(cls, parser):
        parser.add_argument(
            "--target",
            choices=CHIPLET_CHOICES,
            default="k0",
            help="SMC to talk to: k0, m0-m5, q0-q2 (k1 needs --spa-base; default: k0)",
        )
        parser.add_argument("--spa-base", help="hex SMC SPA base, overrides --target's table entry")
        parser.add_argument("--device-id", type=int, default=0, help="/dev/tenstorrent/<id> (default: 0)")
        parser.add_argument("--port", type=int, default=3333, help="GDB server TCP port (default: 3333)")
        parser.add_argument(
            "--reset-vector",
            default=DEFAULT_RESET_VECTOR,
            help=f"hex local address the image is loaded at and the hart starts from (default: {DEFAULT_RESET_VECTOR})",
        )
        parser.add_argument(
            "--halt", action="store_true", help="flash/reset: leave the hart halted at the reset vector"
        )
        parser.add_argument(
            "--no-reload", action="store_true", help="reset: do not reload the image from the build directory"
        )
        parser.add_argument(
            "--wipe-sram", action="store_true", help="flash: zero the SMC SRAM before loading (slow)"
        )

        parser.add_argument(
            "--vuart-timeout",
            type=float,
            default=0,
            help="flash/reset: after release, wait up to this many seconds for the image to publish its VUART "
            "(SCRATCH_2 non-zero); needed before a console attaches (default: 0, do not wait)",
        )
        parser.add_argument("--settle", help="accepted for old hwmap compatibility; ignored")

    @classmethod
    def do_create(cls, cfg, args):
        spa_base = int(args.spa_base, 16) if args.spa_base else None
        return cls(
            cfg,
            args.target,
            spa_base,
            args.device_id,
            args.port,
            args.reset_vector,
            args.halt,
            args.no_reload,
            args.wipe_sram,
            args.vuart_timeout,
        )

    # -- helpers --

    def _image(self):
        path = self.cfg.bin_file or os.path.join(self.cfg.build_dir, "zephyr", "zephyr.bin")
        if not os.path.exists(path):
            raise RuntimeError(f"{path} not found; build first")
        return path

    def _smc(self):
        base = self.spa_base if self.spa_base is not None else chiplet_spa_base(self.target)
        return SmcTarget(NocDevice(self.device_id), base)

    def _reset_and_load(self, load, halt):
        smc = self._smc()
        try:
            smc.activate()
            smc.hold_reset()
            smc.clear_scratch()
            if self.wipe_sram:
                self.logger.info("Wiping SRAM")
                smc.wipe_sram()
            if load:
                image = self._image()
                self.logger.info("Loading %s at 0x%x on %s", image, self.reset_vector, self.target)
                with open(image, "rb") as f:
                    smc.load_image(f.read(), self.reset_vector)
            smc.set_reset_vector(self.reset_vector)
            smc.ack_havereset()
            if halt:
                smc.request_halt()
            smc.release_reset()
            if halt:
                if not smc.wait_halted(5.0):
                    raise RuntimeError("hart did not halt at reset")
                smc.clear_halt_request()
                self.logger.info("%s halted at 0x%x", self.target, self.reset_vector)
            elif self.vuart_timeout:
                self._wait_vuart(smc)
        except DmError as e:
            raise RuntimeError(str(e)) from e

    def _wait_vuart(self, smc):
        deadline = time.monotonic() + self.vuart_timeout
        while time.monotonic() < deadline:
            if smc.dev.rd32(smc.base + SCRATCH_OFFSET + 2 * 8):
                return
            time.sleep(0.1)
        raise RuntimeError(f"{self.target}: VUART not published within {self.vuart_timeout}s")

    def _server_cmd(self, once, ready_file=None):
        cmd = [
            sys.executable,
            self.server_script,
            "--target",
            self.target,
            "--device-id",
            str(self.device_id),
            "--port",
            str(self.port),
            "--reset-vector",
            f"{self.reset_vector:x}",
        ]
        if self.spa_base is not None:
            cmd += ["--spa-base", hex(self.spa_base)]
        if not self.no_reload:
            try:
                cmd += ["--image", self._image()]
            except RuntimeError:
                pass
        elf = self.cfg.elf_file or os.path.join(self.cfg.build_dir, "zephyr", "zephyr.elf")
        if os.path.exists(elf):
            cmd += ["--elf", elf]
        if once:
            cmd.append("--once")
        if ready_file:
            cmd += ["--ready-file", ready_file]
        return cmd

    def _wait_for_server(self, proc, ready_file):
        # Probing the port would count as a GDB client and make the server halt/resume the hart.
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise RuntimeError("GDB server exited early")
            if os.path.exists(ready_file):
                return
            time.sleep(0.1)
        raise RuntimeError("GDB server did not start listening")

    def _gdb(self):
        gdb = self.cfg.gdb
        if gdb:
            return gdb
        sdk = os.environ.get("ZEPHYR_SDK_INSTALL_DIR", os.path.expanduser("~/zephyr-sdk-1.0.1"))
        cand = os.path.join(sdk, "gnu", "riscv64-zephyr-elf", "bin", "riscv64-zephyr-elf-gdb")
        return cand if os.path.exists(cand) else (shutil.which("riscv64-zephyr-elf-gdb") or "gdb-multiarch")

    def _run_server_and_gdb(self):
        # The server connection halts the hart itself; a single GDB session then owns it.
        with tempfile.TemporaryDirectory() as tmp:
            ready_file = os.path.join(tmp, "ready")
            proc = subprocess.Popen(self._server_cmd(once=True, ready_file=ready_file))
            try:
                self._wait_for_server(proc, ready_file)
                gdb_cmd = [self._gdb(), self.cfg.elf_file, "-ex", f"target remote 127.0.0.1:{self.port}"]
                self.logger.info("Starting GDB: %s", " ".join(gdb_cmd))
                subprocess.call(gdb_cmd)
            finally:
                proc.terminate()
                proc.wait()

    # -- commands --

    def do_run(self, command, **kwargs):
        if command == "flash":
            self._reset_and_load(load=True, halt=self.halt)
        elif command == "reset":
            self._reset_and_load(load=not self.no_reload, halt=self.halt)
        elif command == "attach":
            self._run_server_and_gdb()
        elif command == "debug":
            self._reset_and_load(load=True, halt=True)
            self._run_server_and_gdb()
        elif command == "debugserver":
            self._reset_and_load(load=True, halt=True)
            self.check_call(self._server_cmd(once=False))
        else:
            raise ValueError(f"unsupported command {command}")
