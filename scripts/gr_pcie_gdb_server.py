#!/usr/bin/env python3

# Copyright (c) 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""GDB remote (RSP) server for Grendel SMC cores, driving the SMC RISC-V Debug Module over PCIe.

Usage:
    gr_pcie_gdb_server.py --target m0 [--device-id 0] [--port 3333] [--image zephyr.bin]

Then in GDB:
    (gdb) target remote :3333

Monitor commands: reset [halt|run], halt, resume, status, help.
"""

import argparse
import os
import select
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gr_pcie_dm import (  # noqa: E402
    ALLHALTED,
    ANYHAVERESET,
    CAUSE_HALTREQ,
    CHIPLET_CHOICES,
    CSR_DPC,
    REGNO_FPR,
    REGNO_GPR,
    DmError,
    NocDevice,
    SmcTarget,
    chiplet_spa_base,
)

DEFAULT_RESET_VECTOR = 0xC0067000
PACKET_SIZE = 0x4000
SIGINT, SIGTRAP = 2, 5
EBREAK_4 = b"\x73\x00\x10\x00"
EBREAK_2 = b"\x02\x90"  # c.ebreak

GPR_NAMES = (
    "zero ra sp gp tp t0 t1 t2 s0 s1 a0 a1 a2 a3 a4 a5 a6 a7 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 t3 t4 t5 t6"
).split()
FPR_NAMES = (
    "ft0 ft1 ft2 ft3 ft4 ft5 ft6 ft7 fs0 fs1 fa0 fa1 fa2 fa3 fa4 fa5 fa6 fa7 "
    "fs2 fs3 fs4 fs5 fs6 fs7 fs8 fs9 fs10 fs11 ft8 ft9 ft10 ft11"
).split()
PC_REGNUM = 32
FPR_REGNUM0 = 33
CSR_REGNUM0 = 65  # GDB numbers CSR n as 65 + n
FP_CSRS = {"fflags": 0x001, "frm": 0x002, "fcsr": 0x003}
M_CSRS = {
    "mstatus": 0x300,
    "misa": 0x301,
    "mie": 0x304,
    "mtvec": 0x305,
    "mscratch": 0x340,
    "mepc": 0x341,
    "mcause": 0x342,
    "mtval": 0x343,
    "mip": 0x344,
    "mhartid": 0xF14,
}
CSR_BY_REGNUM = {CSR_REGNUM0 + n: n for n in [*FP_CSRS.values(), *M_CSRS.values()]}


def build_target_xml():
    out = ['<?xml version="1.0"?>', '<!DOCTYPE target SYSTEM "gdb-target.dtd">', '<target version="1.0">']
    out.append("<architecture>riscv:rv64</architecture>")
    out.append('<feature name="org.gnu.gdb.riscv.cpu">')
    for i, name in enumerate(GPR_NAMES):
        kind = {"ra": "code_ptr", "sp": "data_ptr"}.get(name, "int")
        out.append(f'<reg name="{name}" bitsize="64" type="{kind}" regnum="{i}"/>')
    out.append(f'<reg name="pc" bitsize="64" type="code_ptr" regnum="{PC_REGNUM}"/>')
    out.append("</feature>")
    out.append('<feature name="org.gnu.gdb.riscv.fpu">')
    for i, name in enumerate(FPR_NAMES):
        out.append(f'<reg name="{name}" bitsize="64" type="ieee_double" regnum="{FPR_REGNUM0 + i}"/>')
    for name, num in FP_CSRS.items():
        out.append(f'<reg name="{name}" bitsize="32" type="int" regnum="{CSR_REGNUM0 + num}"/>')
    out.append("</feature>")
    out.append('<feature name="org.gnu.gdb.riscv.csr">')
    for name, num in M_CSRS.items():
        out.append(f'<reg name="{name}" bitsize="64" type="int" regnum="{CSR_REGNUM0 + num}"/>')
    out.append("</feature>")
    out.append("</target>")
    return "\n".join(out)


def hex_le(value, size=8):
    return (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little").hex()


def unhex_le(text):
    return int.from_bytes(bytes.fromhex(text), "little")


def escape(data):
    out = bytearray()
    for b in data:
        if b in b"$#}*":
            out += bytes([0x7D, b ^ 0x20])
        else:
            out.append(b)
    return bytes(out)


def unescape(data):
    out = bytearray()
    it = iter(data)
    for b in it:
        out.append(next(it) ^ 0x20 if b == 0x7D else b)
    return bytes(out)


class Rsp:
    """Minimal RSP framing over a socket."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = bytearray()

    def _fill(self, timeout):
        ready, _, _ = select.select([self.sock], [], [], timeout)
        if not ready:
            return False
        data = self.sock.recv(65536)
        if not data:
            raise EOFError
        self.buf += data
        return True

    def get_packet(self, timeout=None):
        """Returns a packet payload, b'\\x03' for an interrupt, or None on timeout."""
        deadline = None if timeout is None else time.monotonic() + timeout
        while True:
            while self.buf and self.buf[0] in b"+-":
                del self.buf[0]
            if self.buf and self.buf[0] == 3:
                del self.buf[0]
                return b"\x03"
            start = self.buf.find(b"$")
            if start >= 0:
                end = self.buf.find(b"#", start)
                if end >= 0 and len(self.buf) >= end + 3:
                    payload = bytes(self.buf[start + 1 : end])
                    want = int(self.buf[end + 1 : end + 3], 16)
                    del self.buf[: end + 3]
                    if sum(payload) & 0xFF == want:
                        self.sock.sendall(b"+")
                        return payload
                    self.sock.sendall(b"-")
                    continue
            elif self.buf:
                del self.buf[:]
            left = None if deadline is None else max(0.0, deadline - time.monotonic())
            if not self._fill(left):
                return None

    def interrupted(self, timeout):
        """Waits up to timeout for a Ctrl-C byte while the target runs."""
        if self._fill(timeout):
            if 3 in self.buf:
                self.buf.remove(3)
                return True
        return False

    def send(self, payload):
        if isinstance(payload, str):
            payload = payload.encode()
        payload = escape(payload)
        self.sock.sendall(b"$" + payload + b"#" + b"%02x" % (sum(payload) & 0xFF))


THREAD_STATES = ((1, "dummy"), (2, "pending"), (4, "sleeping"), (8, "dead"), (16, "suspended"), (32, "aborting"))
# struct _callee_saved on rv64: sp, ra, s0, s1, s2..s11 (the first member is also the STACK_PTR offset)
CALLEE_SAVED_GPRS = [2, 1, 8, 9] + list(range(18, 28))


class ZephyrThreads:
    """Zephyr thread awareness (needs CONFIG_DEBUG_THREAD_INFO=y) from _kernel and the offset table."""

    # indices into _kernel_thread_info_offsets
    VERSION, K_CURR, K_THREADS, T_ENTRY, T_NEXT, T_STATE, T_OPTS, T_PRIO, T_SP, T_NAME = range(10)

    def __init__(self, elf, read_mem):
        self.read_mem = read_mem
        self.kernel = self.offsets_addr = None
        self.offsets = None
        try:
            from elftools.elf.elffile import ELFFile

            with open(elf, "rb") as f:
                symtab = ELFFile(f).get_section_by_name(".symtab")
                for name in ("_kernel", "_kernel_thread_info_offsets"):
                    sym = symtab.get_symbol_by_name(name)
                    if not sym:
                        self.kernel = None
                        return
                    if name == "_kernel":
                        self.kernel = sym[0]["st_value"]
                    else:
                        self.offsets_addr = sym[0]["st_value"]
        except (OSError, ImportError, AttributeError):
            self.kernel = None

    @property
    def available(self):
        return self.kernel is not None

    def _u(self, addr, size=8):
        return int.from_bytes(self.read_mem(addr, size), "little")

    def _load_offsets(self):
        if self.offsets is None:
            raw = self.read_mem(self.offsets_addr, 8 * 10)
            self.offsets = [int.from_bytes(raw[i : i + 8], "little") for i in range(0, len(raw), 8)]
        return self.offsets

    def current(self):
        o = self._load_offsets()
        return self._u(self.kernel + o[self.K_CURR])

    def threads(self):
        o = self._load_offsets()
        out, t = [], self._u(self.kernel + o[self.K_THREADS])
        while t and len(out) < 256:
            out.append(t)
            t = self._u(t + o[self.T_NEXT])
        return out

    def callee_saved(self, thread):
        base = thread + self._load_offsets()[self.T_SP]
        return {gpr: self._u(base + 8 * i) for i, gpr in enumerate(CALLEE_SAVED_GPRS)}

    def describe(self, thread):
        o = self._load_offsets()
        name = self.read_mem(thread + o[self.T_NAME], 32).split(b"\0")[0].decode("latin-1")
        state = self._u(thread + o[self.T_STATE], 1)
        flags = [n for bit, n in THREAD_STATES if state & bit] or ["ready" if state & 0x80 else "running"]
        prio = int.from_bytes(self.read_mem(thread + o[self.T_PRIO], 1), "little", signed=True)
        return f"{name or 'unnamed'} [{','.join(flags)}] prio {prio}"


class GdbServer:
    def __init__(self, smc, args):
        self.smc = smc
        self.args = args
        self.zephyr = ZephyrThreads(args.elf, self.read_memory) if args.elf else None
        self.reg_thread = None  # thread selected by Hg, None = the running one
        self.breakpoints = {}  # addr -> (kind, original bytes)
        self.target_xml = build_target_xml().encode()
        self.rsp = None
        self.last_signal = SIGTRAP

    def log(self, msg):
        print(f"[gr_pcie_gdb] {msg}", flush=True)

    # -- target control --

    def attach(self):
        self.smc.activate()
        if self.smc.core_in_reset():
            self.log("warning: core is held in reset; use 'monitor reset halt'")
            return
        if not self.smc.is_halted():
            self.smc.halt()
        self.smc.write_dcsr(self.smc.read_dcsr() | 1 << 15)
        self.last_signal = SIGTRAP

    def remove_all_breakpoints(self):
        for addr, (_, orig) in list(self.breakpoints.items()):
            try:
                self.smc.write_mem(addr, orig)
            except DmError:
                pass
        self.breakpoints.clear()

    def do_reset(self, halt):
        smc = self.smc
        smc.activate()
        smc.hold_reset()
        smc.clear_scratch()
        if self.args.image:
            with open(self.args.image, "rb") as f:
                smc.load_image(f.read(), self.args.reset_vector)
        smc.set_reset_vector(self.args.reset_vector)
        smc.ack_havereset()
        if halt:
            smc.request_halt()
        smc.release_reset()
        if halt:
            if not smc.wait_halted(5.0):
                raise DmError("hart did not halt after reset")
            smc.clear_halt_request()
            smc.write_dcsr(smc.read_dcsr() | 1 << 15)
        self.breakpoints.clear()  # the image reload discarded any inserted breakpoints
        self.last_signal = SIGTRAP

    def thread_list(self):
        """Returns (current, [threads]) once the Zephyr kernel is up, else None (single pseudo-thread)."""
        if not self.zephyr or not self.zephyr.available:
            return None
        try:
            cur, threads = self.zephyr.current(), self.zephyr.threads()
        except (DmError, ValueError, OSError):
            return None
        return (cur, threads) if cur and cur in threads else None

    def stop_reply(self):
        cause = self.smc.halt_cause()
        self.last_signal = SIGINT if cause == CAUSE_HALTREQ else SIGTRAP
        self.reg_thread = None
        info = self.thread_list()
        return f"T{self.last_signal:02x}thread:{info[0]:x};" if info else f"S{self.last_signal:02x}"

    def selected_thread(self):
        """The non-running thread chosen with Hg, or None when registers come from the hart."""
        info = self.thread_list() if self.reg_thread else None
        if info and self.reg_thread != info[0] and self.reg_thread in info[1]:
            return self.reg_thread
        return None

    def wait_stop(self):
        """Blocks until the hart halts; Ctrl-C from GDB requests a halt."""
        interrupted = False
        while True:
            if self.smc.status() & ALLHALTED:
                self.smc.clear_halt_request()
                return self.stop_reply()
            if self.rsp.interrupted(0.02) and not interrupted:
                interrupted = True
                self.smc.request_halt()

    # -- packet handlers --

    def read_registers(self):
        thread = self.selected_thread()
        if thread:
            saved = self.zephyr.callee_saved(thread)
            vals = [saved.get(i) for i in range(32)] + [saved[1]]  # resumes at the saved ra
            return "".join(hex_le(v) if v is not None else "x" * 16 for v in vals)
        regs = self.smc.read_regs([REGNO_GPR + i for i in range(32)] + [CSR_DPC])
        return "".join(hex_le(v) for v in regs)

    def write_registers(self, text):
        for i in range(32):
            self.smc.write_reg(REGNO_GPR + i, unhex_le(text[16 * i : 16 * i + 16]))
        self.smc.write_reg(CSR_DPC, unhex_le(text[16 * 32 : 16 * 33]))

    def regno_info(self, regnum):
        """Returns (debug regno, size in bytes) for a GDB register number."""
        if regnum < 32:
            return REGNO_GPR + regnum, 8
        if regnum == PC_REGNUM:
            return CSR_DPC, 8
        if FPR_REGNUM0 <= regnum < FPR_REGNUM0 + 32:
            return REGNO_FPR + regnum - FPR_REGNUM0, 8
        if regnum in CSR_BY_REGNUM:
            csr = CSR_BY_REGNUM[regnum]
            return csr, 4 if csr in FP_CSRS.values() else 8
        return None

    def read_memory(self, addr, length):
        data = bytearray(self.smc.read_mem(addr, length))
        for bp, (_, orig) in self.breakpoints.items():  # hide inserted breakpoint instructions
            for i, b in enumerate(orig):
                if addr <= bp + i < addr + length:
                    data[bp + i - addr] = b
        return bytes(data)

    def write_memory(self, addr, data):
        self.smc.write_mem(addr, data)

    def insert_breakpoint(self, addr, kind):
        if addr in self.breakpoints:
            return
        size = 2 if kind == 2 else 4
        orig = self.smc.read_mem(addr, size)
        self.smc.write_mem(addr, EBREAK_2 if size == 2 else EBREAK_4)
        self.breakpoints[addr] = (size, orig)

    def remove_breakpoint(self, addr):
        if addr in self.breakpoints:
            _, orig = self.breakpoints.pop(addr)
            self.smc.write_mem(addr, orig)

    def monitor(self, cmd):
        words = cmd.split()
        out = ""
        if words[:1] == ["reset"]:
            halt = words[1:2] != ["run"]
            self.do_reset(halt)
            out = f"reset: {'halted at' if halt else 'running from'} 0x{self.args.reset_vector:x}\n"
        elif words == ["halt"]:
            if not self.smc.is_halted():
                self.smc.halt()
            out = "halted\n"
        elif words in (["resume"], ["go"]):
            self.smc.run()
            out = "resumed\n"
        elif words == ["status"]:
            st = self.smc.status()
            out = (
                f"dmstatus=0x{st:08x} halted={bool(st & ALLHALTED)} havereset={bool(st & ANYHAVERESET)} "
                f"in_reset={self.smc.core_in_reset()}\n"
            )
        else:
            out = "monitor commands: reset [halt|run], halt, resume, status\n"
        return out

    def handle(self, pkt):
        """Returns the reply string, or None if the reply is deferred/absent."""
        smc = self.smc
        c = chr(pkt[0]) if pkt else ""
        body = pkt[1:].decode("latin-1")
        if c == "?":
            self.reg_thread = None
            info = self.thread_list()
            return f"T{self.last_signal:02x}thread:{info[0]:x};" if info else f"S{self.last_signal:02x}"
        if c == "g":
            return self.read_registers()
        if c == "G":
            self.write_registers(body)
            return "OK"
        if c == "p":
            info = self.regno_info(int(body, 16))
            if info is None:
                return "E01"
            thread = self.selected_thread()
            if thread:
                saved = self.zephyr.callee_saved(thread)
                regnum = int(body, 16)
                val = saved[1] if regnum == PC_REGNUM else saved.get(regnum) if regnum < 32 else None
                return hex_le(val, info[1]) if val is not None else "x" * (2 * info[1])
            return hex_le(smc.read_reg(info[0], 2 if info[1] == 4 else 3), info[1])
        if c == "P":
            if self.selected_thread():
                return "E01"
            num, val = body.split("=")
            info = self.regno_info(int(num, 16))
            if info is None:
                return "E01"
            smc.write_reg(info[0], unhex_le(val), 2 if info[1] == 4 else 3)
            return "OK"
        if c == "m":
            addr, length = (int(x, 16) for x in body.split(","))
            return self.read_memory(addr, length).hex()
        if c == "M":
            head, data = body.split(":")
            addr, length = (int(x, 16) for x in head.split(","))
            self.write_memory(addr, bytes.fromhex(data)[:length])
            return "OK"
        if c == "X":
            head, _, _ = pkt[1:].partition(b":")
            data = unescape(pkt[1 + len(head) + 1 :])
            addr, length = (int(x, 16) for x in head.decode().split(","))
            if length:
                self.write_memory(addr, data[:length])
            return "OK"
        if c in "cs":
            if body:
                smc.write_reg(CSR_DPC, int(body, 16))
            return self.resume(step=(c == "s"))
        if body.startswith("Cont") or pkt.startswith(b"vCont"):
            return self.handle_vcont(pkt.decode())
        if c in "Zz":
            kind, addr, length = body.split(",")[0], int(body.split(",")[1], 16), int(body.split(",")[2], 16)
            if kind != "0":
                return ""  # hardware breakpoints/watchpoints need triggers, which the SMC DM lacks
            if c == "Z":
                self.insert_breakpoint(addr, length)
            else:
                self.remove_breakpoint(addr)
            return "OK"
        if c == "H":
            if body[:1] == "g":
                tid = body[1:]
                self.reg_thread = int(tid, 16) if tid not in ("", "0", "-1") else None
            return "OK"
        if c == "T":
            info = self.thread_list()
            return "OK" if info is None or int(body, 16) in info[1] or body == "1" else "E01"
        if c == "D":
            self.remove_all_breakpoints()
            smc.release_to_system()
            return "OK"
        if c == "k" or pkt.startswith(b"vKill"):
            self.remove_all_breakpoints()
            smc.release_to_system()
            return "OK" if pkt.startswith(b"vKill") else None
        if pkt.startswith(b"qSupported"):
            return f"PacketSize={PACKET_SIZE:x};qXfer:features:read+;vContSupported+"
        if pkt.startswith(b"qXfer:features:read:target.xml:"):
            off, length = (int(x, 16) for x in body.rsplit(":", 1)[1].split(","))
            chunk = self.target_xml[off : off + length]
            return ("m" if off + length < len(self.target_xml) else "l") + chunk.decode()
        if pkt == b"qAttached":
            return "1"
        if pkt == b"qC":
            info = self.thread_list()
            return f"QC{info[0]:x}" if info else "QC1"
        if pkt == b"qfThreadInfo":
            info = self.thread_list()
            return "m" + ",".join(f"{t:x}" for t in info[1]) if info else "m1"
        if pkt.startswith(b"qThreadExtraInfo,"):
            info = self.thread_list()
            tid = int(body.split(",")[1], 16)
            if not info or tid not in info[1]:
                return "E01"
            return self.zephyr.describe(tid).encode().hex()
        if pkt == b"qsThreadInfo":
            return "l"
        if pkt.startswith(b"qRcmd,"):
            out = self.monitor(bytes.fromhex(body.split(",", 1)[1]).decode())
            if out:
                self.rsp.send("O" + out.encode().hex())
            return "OK"
        if pkt == b"vCont?":
            return "vCont;c;s;t"
        return ""

    def handle_vcont(self, pkt):
        if pkt == "vCont?":
            return "vCont;c;s;t"
        action = pkt.split(";")[1][0]
        if action in "cC":
            return self.resume(step=False)
        if action in "sS":
            return self.resume(step=True)
        if action == "t":
            self.smc.request_halt()
            self.smc.wait_halted(5.0)
            self.smc.clear_halt_request()
            return self.stop_reply()
        return "E01"

    def resume(self, step):
        self.reg_thread = None
        if step:
            self.smc.step()
            return self.stop_reply()
        self.smc.run()
        return self.wait_stop()

    def serve_client(self, conn):
        self.rsp = Rsp(conn)
        self.attach()
        try:
            while True:
                pkt = self.rsp.get_packet()
                if pkt == b"\x03":
                    self.smc.request_halt()
                    self.smc.wait_halted(5.0)
                    self.smc.clear_halt_request()
                    self.rsp.send(self.stop_reply())
                    continue
                if self.args.trace:
                    self.log(f"<- {pkt[:80]!r}")
                try:
                    reply = self.handle(pkt)
                except (DmError, ValueError, OSError) as e:
                    self.log(f"error handling {pkt[:40]!r}: {e}")
                    reply = "E01"
                if reply is not None:
                    self.rsp.send(reply)
                if pkt in (b"D",) or pkt.startswith(b"vKill") or pkt == b"k":
                    return
        except EOFError:
            self.log("client disconnected")
            try:
                self.remove_all_breakpoints()
                self.smc.release_to_system()
            except (DmError, OSError):
                pass


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--target", choices=CHIPLET_CHOICES, default="k0")
    p.add_argument("--spa-base", type=lambda s: int(s, 0), help="override the chiplet SMC SPA base")
    p.add_argument("--device-id", type=int, default=0)
    p.add_argument("--port", type=int, default=3333)
    p.add_argument("--image", help="zephyr.bin reloaded by 'monitor reset'")
    p.add_argument("--elf", help="zephyr.elf; enables Zephyr thread awareness (needs CONFIG_DEBUG_THREAD_INFO=y)")
    p.add_argument("--reset-vector", type=lambda s: int(s, 16), default=DEFAULT_RESET_VECTOR)
    p.add_argument("--ready-file", help="created once the server is listening (used by the west runner)")
    p.add_argument("--trace", action="store_true", help="log every received packet")
    p.add_argument("--once", action="store_true", help="exit after the first client disconnects")
    args = p.parse_args()

    base = args.spa_base if args.spa_base is not None else chiplet_spa_base(args.target)
    smc = SmcTarget(NocDevice(args.device_id), base)
    server = GdbServer(smc, args)

    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", args.port))
    listener.listen(1)
    server.log(f"{args.target} (SPA 0x{base:x}) listening on 127.0.0.1:{args.port}")
    if args.ready_file:
        open(args.ready_file, "w").close()
    while True:
        conn, addr = listener.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        server.log(f"GDB connected from {addr[0]}:{addr[1]}")
        try:
            server.serve_client(conn)
        finally:
            conn.close()
        if args.once:
            return


if __name__ == "__main__":
    main()
