# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""RISC-V Debug Module (spec 0.13) driver for Grendel SMC cores, reached over PCIe.

All accesses go through the tt-kmd TENSTORRENT_IOCTL_NOC_READ/NOC_WRITE ioctls using system physical
addresses (SPA). Each SMC (Keraunos, Mimir, Quasar SMU) exposes an identical single-hart Debug Module
at core-local 0xC0001000, which appears at <smc_spa_base> + 0x1000 on the host.
"""

import fcntl
import os
import struct
import time

IOCTL_NOC_READ = 0xFA12  # _IO(0xFA, 18)
IOCTL_NOC_WRITE = 0xFA13  # _IO(0xFA, 19)
_NOC_IO = struct.Struct("<IIHHBB2xQQ")

SMC_LOCAL_BASE = 0xC0000000
DM_OFFSET = 0x1000
RESET_VECTOR0_OFFSET = 0x10000
RESET_CTRL_OFFSET = 0x10020
SCRATCH_OFFSET = 0x10100
SCRATCH_COUNT = 16
RESET_CTRL_CORE0_RESET_N = 1 << 0
SRAM_LOCAL_BASE = 0xC0060000
SRAM_SIZE = 0x100000

# Chiplet name -> SMC SPA base (grendel_topology.py / keraunos_reset). k1 is not part of the
# MMK or QSR1A1 topologies, so they need an explicit --spa-base. k1 is left unmapped (address remap unresolved).
CHIPLET_SPA_BASE = {
    "k0": 0x1202000000,
    "m0": 0x1300000000,
    "m1": 0x1308000000,
    "m2": 0x1310000000,
    "m3": 0x1318000000,
    "m4": 0x13A0000000,
    "m5": 0x13A8000000,
    "q0": 0x19D8000000,
    "q1": 0x1BD8000000,
    "q2": 0x1DD8000000,
}
CHIPLET_ALIASES = {"k": "k0", "m": "m1"}
KNOWN_UNMAPPED = ("k1",)
CHIPLET_CHOICES = tuple(CHIPLET_SPA_BASE) + tuple(CHIPLET_ALIASES) + KNOWN_UNMAPPED


def chiplet_spa_base(name):
    name = CHIPLET_ALIASES.get(name, name)
    if name not in CHIPLET_SPA_BASE:
        raise DmError(f"SPA base of '{name}' is unknown; pass --spa-base")
    return CHIPLET_SPA_BASE[name]


# dm registers
DATA0, DATA1 = 0x10, 0x14
DMCONTROL, DMSTATUS = 0x40, 0x44
ABSTRACTCS, COMMAND = 0x58, 0x5C
SBCS, SBADDRESS0, SBDATA0 = 0xE0, 0xE4, 0xF0

HALTREQ, RESUMEREQ, ACKHAVERESET, DMACTIVE = 1 << 31, 1 << 30, 1 << 28, 1
ALLHALTED, ALLRUNNING, ALLRESUMEACK, ANYHAVERESET = 1 << 9, 1 << 11, 1 << 17, 1 << 18
SBREADONADDR, SBAUTOINCREMENT, SBREADONDATA = 1 << 20, 1 << 16, 1 << 15

CSR_DCSR, CSR_DPC = 0x7B0, 0x7B1
DCSR_EBREAKM, DCSR_STEP = 1 << 15, 1 << 2
CAUSE_EBREAK, CAUSE_HALTREQ, CAUSE_STEP = 1, 3, 4

REGNO_GPR, REGNO_FPR = 0x1000, 0x1020


class DmError(Exception):
    pass


class NocDevice:
    def __init__(self, device_id=0, path=None):
        self.fd = os.open(path or f"/dev/tenstorrent/{device_id}", os.O_RDWR | os.O_APPEND)

    def close(self):
        os.close(self.fd)

    def _io(self, req, spa, width, value=0):
        buf = bytearray(_NOC_IO.pack(_NOC_IO.size, 0, 0, 0, 0, width, spa, value))
        fcntl.ioctl(self.fd, req, buf, True)
        return _NOC_IO.unpack(bytes(buf))[7]

    def rd32(self, spa):
        return self._io(IOCTL_NOC_READ, spa, 4) & 0xFFFFFFFF

    def wr32(self, spa, value):
        self._io(IOCTL_NOC_WRITE, spa, 4, value & 0xFFFFFFFF)

    def wr64(self, spa, value):
        self._io(IOCTL_NOC_WRITE, spa, 8, value)


class SmcTarget:
    """One SMC core: reset control plus its Debug Module."""

    def __init__(self, dev, spa_base):
        self.dev = dev
        self.base = spa_base
        self.dm_base = spa_base + DM_OFFSET

    # -- reset / image loading (direct SPA access; independent of the Debug Module) --

    def local_to_spa(self, local):
        return self.base + (local - SMC_LOCAL_BASE)

    def _set_core_reset_n(self, released):
        spa = self.base + RESET_CTRL_OFFSET
        val = self.dev.rd32(spa)
        val = (val | RESET_CTRL_CORE0_RESET_N) if released else (val & ~RESET_CTRL_CORE0_RESET_N)
        self.dev.wr32(spa, val)
        if bool(self.dev.rd32(spa) & RESET_CTRL_CORE0_RESET_N) != released:
            raise DmError("core0_reset_n did not take the requested value")

    def hold_reset(self):
        self._set_core_reset_n(False)

    def release_reset(self):
        self._set_core_reset_n(True)

    def clear_scratch(self):
        for i in range(SCRATCH_COUNT):
            self.dev.wr32(self.base + SCRATCH_OFFSET + 8 * i, 0)

    def wipe_sram(self):
        spa = self.local_to_spa(SRAM_LOCAL_BASE)
        for off in range(0, SRAM_SIZE, 8):
            self.dev.wr64(spa + off, 0)

    def load_image(self, data, local_addr):
        spa = self.local_to_spa(local_addr)
        data = bytes(data) + b"\0" * (-len(data) % 8)
        for off in range(0, len(data), 8):
            self.dev.wr64(spa + off, int.from_bytes(data[off : off + 8], "little"))

    def set_reset_vector(self, local_addr):
        spa = self.base + RESET_VECTOR0_OFFSET
        self.dev.wr32(spa, local_addr)
        if self.dev.rd32(spa) != local_addr:
            raise DmError("RESET_VECTOR[0] readback mismatch")

    def get_reset_vector(self):
        return self.dev.rd32(self.base + RESET_VECTOR0_OFFSET)

    def core_in_reset(self):
        return not (self.dev.rd32(self.base + RESET_CTRL_OFFSET) & RESET_CTRL_CORE0_RESET_N)

    # -- Debug Module registers --

    def rd(self, off):
        return self.dev.rd32(self.dm_base + off)

    def wr(self, off, val):
        self.dev.wr32(self.dm_base + off, val)

    def activate(self):
        self.wr(DMCONTROL, DMACTIVE)
        if not self.rd(DMCONTROL) & DMACTIVE:
            raise DmError("Debug Module did not activate")

    def status(self):
        return self.rd(DMSTATUS)

    def is_halted(self):
        return bool(self.status() & ALLHALTED)

    def wait_halted(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            if self.is_halted():
                return True
            if time.monotonic() > deadline:
                return False
            time.sleep(0.005)

    def ack_havereset(self):
        self.wr(DMCONTROL, DMACTIVE | ACKHAVERESET)

    def request_halt(self):
        self.wr(DMCONTROL, DMACTIVE | HALTREQ)

    def clear_halt_request(self):
        self.wr(DMCONTROL, DMACTIVE)

    def halt(self, timeout=5.0):
        self.request_halt()
        ok = self.wait_halted(timeout)
        self.clear_halt_request()
        if not ok:
            raise DmError("hart did not halt (held in reset?)")

    def resume(self):
        self.wr(DMCONTROL, DMACTIVE | RESUMEREQ)

    # -- abstract commands --

    def _check_cmderr(self):
        cmderr = (self.rd(ABSTRACTCS) >> 8) & 7
        if cmderr:
            self.wr(ABSTRACTCS, 7 << 8)
            raise DmError(f"abstract command failed: cmderr={cmderr}")

    def _command(self, regno, write, size=3):
        self.wr(COMMAND, (size << 20) | (1 << 17) | (int(write) << 16) | regno)

    def read_regs(self, regnos, size=3):
        values = []
        for regno in regnos:
            self._command(regno, False, size)
            lo = self.rd(DATA0)
            hi = self.rd(DATA1) if size == 3 else 0
            values.append(lo | (hi << 32))
        self._check_cmderr()
        return values

    def read_reg(self, regno, size=3):
        return self.read_regs([regno], size)[0]

    def write_reg(self, regno, value, size=3):
        self.wr(DATA0, value & 0xFFFFFFFF)
        if size == 3:
            self.wr(DATA1, (value >> 32) & 0xFFFFFFFF)
        self._command(regno, True, size)
        self._check_cmderr()

    def read_dcsr(self):
        return self.read_reg(CSR_DCSR, 2)

    def write_dcsr(self, value):
        self.write_reg(CSR_DCSR, value, 2)

    def halt_cause(self):
        return (self.read_dcsr() >> 6) & 7

    def step(self, timeout=5.0):
        dcsr = self.read_dcsr()
        self.write_dcsr(dcsr | DCSR_STEP | DCSR_EBREAKM)
        self.resume()
        ok = self.wait_halted(timeout)
        self.write_dcsr(dcsr & ~DCSR_STEP | DCSR_EBREAKM)
        if not ok:
            raise DmError("hart did not halt after single-step")

    def run(self):
        """Resume free-running with ebreak entering debug mode."""
        self.write_dcsr(self.read_dcsr() & ~DCSR_STEP | DCSR_EBREAKM)
        self.resume()

    def release_to_system(self):
        """Leave the hart running with no debug influence (ebreak traps to firmware again)."""
        if self.is_halted():
            self.write_dcsr(self.read_dcsr() & ~(DCSR_STEP | DCSR_EBREAKM))
            self.resume()

    # -- system bus access --

    def _check_sberror(self, addr):
        sbcs = self.rd(SBCS)
        err = (sbcs >> 12) & 7
        if err:
            self.wr(SBCS, 7 << 12)
            raise DmError(f"memory access failed at 0x{addr:x}: sberror={err}")

    def _sba_one(self, addr, size, write, value=0):
        access = {1: 0, 2: 1, 4: 2}[size]
        if write:
            self.wr(SBCS, access << 17)
            self.wr(SBADDRESS0, addr)
            self.wr(SBDATA0, value)
            self._check_sberror(addr)
            return 0
        self.wr(SBCS, (access << 17) | SBREADONADDR)
        self.wr(SBADDRESS0, addr)
        value = self.rd(SBDATA0)
        self._check_sberror(addr)
        return value

    def read_mem(self, addr, length):
        out = bytearray()
        while length:
            if addr % 4 == 0 and length >= 4:
                count = min(length // 4, 1024)
                self.wr(SBCS, (2 << 17) | SBREADONADDR | SBAUTOINCREMENT | SBREADONDATA)
                self.wr(SBADDRESS0, addr)
                for i in range(count):
                    if i == count - 1:
                        self.wr(SBCS, (2 << 17) | SBAUTOINCREMENT)  # last word: no read-ahead past the range
                    out += self.rd(SBDATA0).to_bytes(4, "little")
                self._check_sberror(addr)
                addr += 4 * count
                length -= 4 * count
            elif addr % 2 == 0 and length >= 2:
                out += self._sba_one(addr, 2, False).to_bytes(2, "little")
                addr += 2
                length -= 2
            else:
                out += self._sba_one(addr, 1, False).to_bytes(1, "little")
                addr += 1
                length -= 1
        return bytes(out)

    def write_mem(self, addr, data):
        data = bytes(data)
        pos = 0
        while pos < len(data):
            left = len(data) - pos
            if addr % 4 == 0 and left >= 4:
                count = min(left // 4, 1024)
                self.wr(SBCS, (2 << 17) | SBAUTOINCREMENT)
                self.wr(SBADDRESS0, addr)
                for i in range(count):
                    self.wr(SBDATA0, int.from_bytes(data[pos + 4 * i : pos + 4 * i + 4], "little"))
                self._check_sberror(addr)
                step = 4 * count
            elif addr % 2 == 0 and left >= 2:
                self._sba_one(addr, 2, True, int.from_bytes(data[pos : pos + 2], "little"))
                step = 2
            else:
                self._sba_one(addr, 1, True, data[pos])
                step = 1
            addr += step
            pos += step
