#!/usr/bin/env python3
"""
Thin client for the resident Ghidra query server (ghserver.py, started by
start.sh, which can also stage a copy of both; see its header).

Usage:
    python3 gh.py '{"cmd":"func_at","va":"0x062b8cf0"}'
    python3 gh.py '{"cmd":"decompile","va":"0x062b8cf0"}'
    python3 gh.py '{"cmd":"decompile","name":"FUN_062b8cf0"}'
    python3 gh.py '{"cmd":"xrefs","va":"0x0674dca0"}'
    python3 gh.py '{"cmd":"callees","va":"0x062b8cf0"}'
    python3 gh.py '{"cmd":"by_name","name":"AI_PERSONALITY_EVAL"}'
    python3 gh.py '{"cmd":"findstr","s":"10CAIManager"}'
    python3 gh.py '{"cmd":"findptr","va":"0x..."}'
    python3 gh.py '{"cmd":"read","va":"0x...","n":16}'
    python3 gh.py '{"cmd":"ping"}'
    python3 gh.py 'func_at 0x062b8cf0'          (shorthand: cmd va)

Queries take IMAGE VAs (Ghidra addresses).
bridge header_offset = IMAGE VA - image base (0x100000 for the Linux ELF).
Port: $GHIDRA_MCP_PORT, default 18741.

Output: the server's JSON reply. If the reply has a "c" field (decompiler
output), the C source is printed below the JSON header for readability.
"""
import os
import sys
import json
import socket

HOST = "127.0.0.1"
PORT = int(os.environ.get("GHIDRA_MCP_PORT", "18741"))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    if sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        sys.exit(0)
    req = sys.argv[1]
    # Validate / normalize: allow raw JSON or a bare 'cmd va' shorthand.
    try:
        json.loads(req)
        payload = req
    except json.JSONDecodeError:
        parts = req.split()
        if len(parts) == 2:
            payload = json.dumps({"cmd": parts[0], "va": parts[1]})
        else:
            print("ERROR: argument must be a JSON object", file=sys.stderr)
            sys.exit(2)

    try:
        s = socket.create_connection((HOST, PORT), timeout=120)
    except (ConnectionRefusedError, OSError) as e:
        print(json.dumps({"ok": False, "error": "cannot connect to ghserver on %s:%d (%s). Is ghserver.py running and ready? See start.sh for its log" % (HOST, PORT, e)}))
        sys.exit(1)

    s.sendall((payload.strip() + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()

    try:
        resp = json.loads(buf.decode())
    except Exception:
        print(buf.decode())
        sys.exit(1)

    c = resp.pop("c", None)
    print(json.dumps(resp, indent=2))
    if c is not None:
        print("\n/* ---- decompiled C ---- */")
        print(c)
    sys.exit(0 if resp.get("ok") else 1)


if __name__ == "__main__":
    main()
