#!/usr/bin/env python3
"""Verify ck3_offsets.h address convention (ck-e58l.20 audit harness).

For every `constexpr <uint type> NAME = 0xVAL;` entry in the header, ask the
resident Ghidra query server (re/scripts/ghidra/ghserver.py, `func_at`) whether
a function STARTS at VAL and at VAL + image base:

  CORRECT_RAW      : entry at val+base, not at val. Header value is base-relative. OK
  SUSPECT_GHIDRA_VA: entry at val, not at val+base. Header holds a Ghidra VA: BUG.
  BOTH_ENTRY       : entries at both. Rare collision; inspect by name.
  NO_ENTRY         : neither (data, inlined or mid-function). Not a convention bug.
  DATA_RANGE       : val > --code-max; skipped (data globals are checked by a
                     runtime or core-dump read instead).
  SMALL_CONST      : val < 0x10000 (ids, sizes, slot indices).

Usage:
  re/scripts/ghidra/start.sh                     # server up, project open
  python3 re/scripts/utility/verify_offset_header_convention.py \
      [--header re/offsets/ck3_offsets.h] [--image-base 0x100000] \
      [--code-max 0x0a000000] [--port 18741]

Exit 0 with no SUSPECT_GHIDRA_VA; 1 with any; 2 if the server cannot be asked.
The defaults are the Linux 1.18.3.1 values; --code-max is the top of that
binary's code range, so set it from your binary's section table.
"""
import argparse
import json
import os
import re
import socket
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HEADER = ROOT / 're/offsets/ck3_offsets.h'
GHIDRA_HOST = '127.0.0.1'
GHIDRA_PORT = int(os.environ.get('GHIDRA_MCP_PORT', '18741'))
SHIFT = 0x00100000
CODE_MAX = 0x0a_000_000


class ServerUnavailable(RuntimeError):
    pass


def ask(req, host, port):
    """One request to ghserver.py: a JSON line out, a JSON line back."""
    try:
        with socket.create_connection((host, port), timeout=120) as s:
            s.sendall((json.dumps(req) + "\n").encode())
            buf = b''
            while not buf.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk:
                    break
                buf += chunk
    except OSError as e:
        raise ServerUnavailable(f"cannot reach ghserver on {host}:{port}: {e}")
    return json.loads(buf.decode())


def function_entry_at(va, host, port):
    """True if a function starts exactly at va; False if none starts there."""
    resp = ask({"cmd": "func_at", "va": f"0x{va:08x}"}, host, port)
    if not resp.get("ok"):
        # "none there" is an answer; any other failure is not, and must not
        # be counted as NO_ENTRY.
        if str(resp.get("error", "")).startswith("no function"):
            return False
        raise ServerUnavailable(f"func_at 0x{va:x} failed: {resp.get('error')}")
    try:
        return int(resp["entry"], 16) == va
    except (KeyError, ValueError):
        return False


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--header", type=Path, default=HEADER)
    ap.add_argument("--image-base", type=lambda v: int(v, 0), default=SHIFT,
                    help="Ghidra VA = offset + this (default 0x100000)")
    ap.add_argument("--code-max", type=lambda v: int(v, 0), default=CODE_MAX,
                    help="values above this are data, not probed (default 0x0a000000)")
    ap.add_argument("--host", default=GHIDRA_HOST)
    ap.add_argument("--port", type=int, default=GHIDRA_PORT,
                    help="ghserver port (default $GHIDRA_MCP_PORT or 18741)")
    return ap.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    shift = args.image_base
    pat = re.compile(
        r'^\s*constexpr\s+(uintptr_t|uint32_t|uint64_t|uint16_t|uint8_t)\s+'
        r'(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;(.*)$')

    entries = []
    with open(args.header) as f:
        for lineno, line in enumerate(f, 1):
            m = pat.match(line)
            if not m:
                continue
            typ, name, val_str, rest = m.groups()
            val = int(val_str, 16)
            entries.append((lineno, typ, name, val, rest.strip()))

    print(f"Scanning {len(entries)} constexpr entries...")

    try:
        pong = ask({"cmd": "ping"}, args.host, args.port)
    except ServerUnavailable as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    if not pong.get("ready"):
        print(f"ERROR: ghserver not ready: {pong}", file=sys.stderr)
        return 2
    print(f"Ghidra server: {pong.get('functions')} functions")

    counts = {'CORRECT_RAW': 0, 'SUSPECT_GHIDRA_VA': 0, 'BOTH_ENTRY': 0,
              'NO_ENTRY': 0, 'DATA_RANGE': 0, 'SMALL_CONST': 0}
    suspects = []

    for i, (lineno, typ, name, val, rest) in enumerate(entries):
        if val < 0x10000:
            counts['SMALL_CONST'] += 1
            continue
        if val > args.code_max:
            counts['DATA_RANGE'] += 1
            continue

        try:
            val_is_entry = function_entry_at(val, args.host, args.port)
            raw_is_entry = function_entry_at(val + shift, args.host, args.port)
        except ServerUnavailable as e:
            print(f"ERROR at L{lineno} {name}: {e}", file=sys.stderr)
            return 2

        if raw_is_entry and not val_is_entry:
            counts['CORRECT_RAW'] += 1
        elif val_is_entry and not raw_is_entry:
            counts['SUSPECT_GHIDRA_VA'] += 1
            suspects.append((lineno, name, val))
        elif val_is_entry and raw_is_entry:
            counts['BOTH_ENTRY'] += 1
        else:
            counts['NO_ENTRY'] += 1

        if (i + 1) % 100 == 0:
            print(f"  {i+1}/{len(entries)}...")

    print("\n=== RESULTS ===")
    for k, v in counts.items():
        print(f"  {k}: {v}")

    if suspects:
        print(f"\nSUSPECTS (value IS a Ghidra VA, should be val - 0x{shift:x}):")
        for lineno, name, val in suspects:
            print(f"  L{lineno} {name} = 0x{val:08x}  ->fix to 0x{val-shift:08x}")

    return 0 if counts['SUSPECT_GHIDRA_VA'] == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
