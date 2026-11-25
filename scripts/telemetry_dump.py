#!/usr/bin/env python3
# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
"""Dump every telemetry tag from a Blackhole card, with names and units.

Reads the ARC telemetry table with pyluwen AXI reads (read-only). The table
pointer and data base come from the same registers the firmware publishes in
TELEMETRY_TABLE_REG_ADDR / TELEMETRY_DATA_REG_ADDR (see scripts/ccfgovr_utils.py).

Tag names are taken from lib/tenstorrent/fw_common/telemetry.h, so new tags
show up by name without editing this script. Units and decoders come from
TAG_INFO below; a tag missing from TAG_INFO is shown as raw hex.

Usage:
    python3 scripts/telemetry_dump.py [-d DEVICE_INDEX] [--raw]
"""

import argparse
import os
import re
import sys

import pyluwen

TELEMETRY_TABLE_REG_ADDR = 0x80030434
TELEMETRY_DATA_REG_ADDR = 0x80030430
TELEMETRY_VERSION = 0x00000100
FAN_DISABLED = 0xFFFFFFFF

HEADER = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..",
    "lib",
    "tenstorrent",
    "fw_common",
    "telemetry.h",
)

# name -> (decoder kind, unit). Kinds:
#   u       unsigned integer, shown with the unit
#   hex     opaque value, shown as hex (encoding not decoded here)
#   bits    bitmask, shown as hex plus the set bit numbers
#   q16     signed 16.16 fixed point
#   fan     unsigned, 0xFFFFFFFF means control disabled
#   gddr_temp  four 8-bit die temperatures packed into one word
#   arb     low 16 bits MHz, high 16 bits arbiter number
#   kernel  bit 0 enable, bits 31:16 stop-NOPs frequency in MHz (0 = default)
#   jedec   0x00MMTTCC packed RDID bytes
#   ccfg    ccfgovr state bits, see telemetry.h
#   nibbles 4-bit fields, nibble i is GDDR instance i (0xF = disabled)
TAG_INFO = {
    "BOARD_ID_HIGH": ("hex", ""),
    "BOARD_ID_LOW": ("hex", ""),
    "ASIC_ID": ("hex", ""),
    "HARVESTING_STATE": ("bits", ""),
    "UPDATE_TELEM_SPEED": ("u", "ms"),
    "VCORE": ("u", "mV"),
    "TDP": ("u", "W"),
    "TDC": ("u", "A"),
    "VDD_LIMITS": ("hex", ""),  # packing not documented in telemetry.h
    "THM_LIMIT_SHUTDOWN": ("u", "°C"),
    "ASIC_TEMPERATURE": ("q16", "°C"),
    "VREG_TEMPERATURE": ("u", "°C"),
    "BOARD_TEMPERATURE": ("u", "°C"),
    "AICLK": ("u", "MHz"),
    "AXICLK": ("u", "MHz"),
    "ARCCLK": ("u", "MHz"),
    "L2CPUCLK0": ("u", "MHz"),
    "L2CPUCLK1": ("u", "MHz"),
    "L2CPUCLK2": ("u", "MHz"),
    "L2CPUCLK3": ("u", "MHz"),
    "ETH_LIVE_STATUS": ("bits", ""),
    "GDDR_STATUS": ("bits", ""),
    "GDDR_SPEED": ("u", "Mb/s"),
    "ETH_FW_VERSION": ("hex", ""),
    "GDDR_FW_VERSION": ("hex", ""),
    "DM_APP_FW_VERSION": ("hex", ""),
    "DM_BL_FW_VERSION": ("hex", ""),
    "FLASH_BUNDLE_VERSION": ("hex", ""),
    "CM_FW_VERSION": ("hex", ""),
    "L2CPU_FW_VERSION": ("hex", ""),
    "FAN_SPEED": ("fan", "%"),
    "TIMER_HEARTBEAT": ("u", "ticks"),
    "TELEM_ENUM_COUNT": ("u", "tags"),
    "ENABLED_TENSIX_COL": ("bits", ""),
    "ENABLED_ETH": ("bits", ""),
    "ENABLED_GDDR": ("bits", ""),
    "ENABLED_L2CPU": ("bits", ""),
    "PCIE_USAGE": ("hex", ""),
    "INPUT_CURRENT": ("u", "A"),
    "NOC_TRANSLATION": ("u", "flag"),
    "FAN_RPM": ("fan", "RPM"),
    "GDDR_0_1_TEMP": ("gddr_temp", "°C"),
    "GDDR_2_3_TEMP": ("gddr_temp", "°C"),
    "GDDR_4_5_TEMP": ("gddr_temp", "°C"),
    "GDDR_6_7_TEMP": ("gddr_temp", "°C"),
    "GDDR_0_1_CORR_ERRS": ("u", "errors"),
    "GDDR_2_3_CORR_ERRS": ("u", "errors"),
    "GDDR_4_5_CORR_ERRS": ("u", "errors"),
    "GDDR_6_7_CORR_ERRS": ("u", "errors"),
    "GDDR_UNCORR_ERRS": ("u", "errors"),
    "MAX_GDDR_TEMP": ("u", "°C"),
    "ASIC_LOCATION": ("hex", ""),
    "BOARD_POWER_LIMIT": ("u", "W"),
    "INPUT_POWER": ("u", "W"),
    "TDC_LIMIT_MAX": ("u", "A"),
    "THM_LIMIT_THROTTLE": ("u", "°C"),
    "FW_BUILD_DATE": ("hex", ""),
    "TT_FLASH_VERSION": ("hex", ""),
    "ENABLED_TENSIX_ROW": ("bits", ""),
    "THERM_TRIP_COUNT": ("u", "trips"),
    "ASIC_ID_HIGH": ("hex", ""),
    "ASIC_ID_LOW": ("hex", ""),
    "AICLK_LIMIT_MAX": ("u", "MHz"),
    "TDP_LIMIT_MAX": ("u", "W"),
    "AICLK_ARB_MIN": ("arb", "MHz"),
    "AICLK_ARB_MAX": ("arb", "MHz"),
    "ENABLED_MIN_ARB": ("bits", ""),
    "ENABLED_MAX_ARB": ("bits", ""),
    "AICLK_PPM_INFO": ("hex", ""),
    "HOST_AICLK_LIMIT": ("u", "MHz, 0 = unset"),
    "SMBUS_ERRORS": ("u", "errors"),
    "GDDR_MRISC_NOC2AXI_PORT": ("nibbles", ""),
    "GDDR_WEST_IO_POWER": ("u", "W"),
    "GDDR_EAST_IO_POWER": ("u", "W"),
    "KERNEL_THROTTLER": ("kernel", ""),
    "NOP_START_COUNT": ("u", "count"),
    "NOP_ON_DURATION": ("u", "ms"),
    "FW_CAPABILITIES_0": ("bits", ""),
    "FW_ACTIVE_CONFIG_0": ("bits", ""),
    "FLASH_JEDEC_ID": ("jedec", ""),
    "CCFGOVR_STATE": ("ccfg", ""),
    "CCFGOVR_SEQ": ("u", ""),
    "BOARD_ID_EXT_HIGH": ("hex", ""),
    "BOARD_ID_EXT_MIDDLE": ("hex", ""),
    "BOARD_ID_EXT_LOW": ("hex", ""),
    "VCOREM_POWER": ("u", "W"),
    "SERDES_VDDL_POWER": ("u", "W"),
    "SERDES_VDD_POWER": ("u", "W"),
    "SERDES_VDDH_POWER": ("u", "W"),
    "GDDR_VDDR_POWER": ("u", "W"),
    "GDDR_VDDA_EAST_POWER": ("u", "W"),
    "GDDR_VDDA_WEST_POWER": ("u", "W"),
}

def load_tag_names(header=HEADER):
    """Map tag id -> name from the '#define TAG_<NAME> <id>' lines."""
    names = {}
    pattern = re.compile(r"^#define\s+TAG_([A-Z0-9_]+)\s+(\d+)\s*$")
    with open(header, encoding="utf-8") as fh:
        for line in fh:
            m = pattern.match(line)
            if m and m.group(1) != "COUNT":
                names.setdefault(int(m.group(2)), m.group(1))
    return names


def load_tag_count(header=HEADER):
    """Return TAG_COUNT from telemetry.h, or None if it is not found."""
    pattern = re.compile(r"^#define\s+TAG_COUNT\s+(\d+)\s*$")
    with open(header, encoding="utf-8") as fh:
        for line in fh:
            m = pattern.match(line)
            if m:
                return int(m.group(1))
    return None


def set_bits(value):
    return [i for i in range(32) if value & (1 << i)]


def decode(name, kind, unit, value):
    """Return the display string for one tag value."""
    if kind == "u":
        return f"{value} {unit}".rstrip()
    if kind == "hex":
        return f"0x{value:08x}"
    if kind == "bits":
        bits = set_bits(value)
        return f"0x{value:08x} (set bits: {', '.join(map(str, bits)) or 'none'})"
    if kind == "q16":
        signed = value - (1 << 32) if value & 0x80000000 else value
        return f"{signed / 65536:.4f} {unit}"
    if kind == "fan":
        if value == FAN_DISABLED:
            return "disabled"
        return f"{value} {unit}"
    if kind == "gddr_temp":
        labels = {
            "GDDR_0_1_TEMP": ("GDDR0 bottom", "GDDR0 top", "GDDR1 bottom", "GDDR1 top"),
            "GDDR_2_3_TEMP": ("GDDR2 bottom", "GDDR2 top", "GDDR3 bottom", "GDDR3 top"),
            "GDDR_4_5_TEMP": ("GDDR4 bottom", "GDDR4 top", "GDDR5 bottom", "GDDR5 top"),
            "GDDR_6_7_TEMP": ("GDDR6 bottom", "GDDR6 top", "GDDR7 bottom", "GDDR7 top"),
        }[name]
        parts = [f"{labels[i]} {(value >> (8 * i)) & 0xFF}" for i in range(4)]
        return ", ".join(p + " °C" for p in parts)
    if kind == "arb":
        return f"{value & 0xFFFF} {unit} (arbiter {value >> 16})"
    if kind == "kernel":
        enabled = "enabled" if value & 1 else "disabled"
        mhz = value >> 16
        stop = f"{mhz} MHz" if mhz else "firmware default"
        return f"{enabled}, stop-NOPs frequency {stop}"
    if kind == "jedec":
        if value == 0:
            return "not read"
        return (
            f"manufacturer 0x{(value >> 16) & 0xFF:02x}, "
            f"type 0x{(value >> 8) & 0xFF:02x}, capacity 0x{value & 0xFF:02x}"
        )
    if kind == "ccfg":
        state = {0: "not applied", 1: "pending", 2: "failed", 3: "applied"}
        bank_a = state.get(value & 0x3, "?")
        bank_b = state.get((value >> 2) & 0x3, "?")
        body = "non-empty" if value & 0x10 else "empty"
        return f"bank a {bank_a}, bank b {bank_b}, applied body {body}"
    if kind == "nibbles":
        cells = []
        for i in range(8):
            nib = (value >> (4 * i)) & 0xF
            cells.append(f"GDDR{i}:" + ("off" if nib == 0xF else f"port {nib}"))
        return " ".join(cells)
    return f"0x{value:08x}"


def read_telemetry(chip):
    """Return (version, entries) with entries = [(tag_id, raw_value)], read-only."""
    bh = chip.as_bh()
    table = bh.axi_read32(TELEMETRY_TABLE_REG_ADDR)
    data = bh.axi_read32(TELEMETRY_DATA_REG_ADDR)
    version = bh.axi_read32(table)
    count = bh.axi_read32(table + 4)
    entries = []
    for i in range(min(count, 0x100)):
        entry = bh.axi_read32(table + 8 + 4 * i)
        tag = entry & 0xFFFF
        offset = entry >> 16
        value = bh.axi_read32(data + 4 * offset)
        entries.append((tag, value))
    return version, count, entries


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-d", "--device", type=int, default=0, help="device index (default 0)")
    parser.add_argument("--raw", action="store_true", help="also show the raw 32-bit hex value")
    args = parser.parse_args(argv)

    chips = pyluwen.detect_chips()
    if args.device >= len(chips):
        print(f"no device {args.device}: found {len(chips)} chip(s)", file=sys.stderr)
        return 1
    chip = chips[args.device]

    version, count, entries = read_telemetry(chip)
    names = load_tag_names()

    if version != TELEMETRY_VERSION:
        print(
            f"warning: telemetry version 0x{version:08x}, expected 0x{TELEMETRY_VERSION:08x}",
            file=sys.stderr,
        )
    header_count = load_tag_count()
    if count != header_count:
        print(
            f"note: device reports {count} table entries, telemetry.h defines TAG_COUNT {header_count}",
            file=sys.stderr,
        )

    # The firmware table can list a tag more than once and leaves unused slots
    # as tag 0. Keep the first entry for each tag; duplicates point at the same
    # value, so nothing is lost.
    seen = {}
    for tag, value in entries:
        if tag == 0:
            continue
        seen.setdefault(tag, value)
    missing = sorted(set(names) - set(seen))

    print(f"Device {args.device}: telemetry version 0x{version:08x}, {len(seen)} tags")
    print()

    width = max((len(names.get(t, f"TAG_{t}")) for t in seen), default=10)
    for tag, value in sorted(seen.items()):
        name = names.get(tag, f"TAG_{tag}")
        kind, unit = TAG_INFO.get(name, ("hex", ""))
        text = decode(name, kind, unit, value) if name in TAG_INFO else f"0x{value:08x} (no decoder)"
        line = f"{tag:3d}  {name:<{width}}  {text}"
        if args.raw and kind != "hex":
            line += f"   [raw 0x{value:08x}]"
        print(line)

    if missing:
        print()
        print("Not in the firmware's telemetry table (defined in telemetry.h):")
        for tag in missing:
            print(f"  {tag:3d}  {names[tag]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
