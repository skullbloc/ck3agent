#!/usr/bin/env python3
"""Build the ck3agent Windows release zip (ADR 0001, phase P7 prep).

    package_windows.py --dll version.dll --wheels <dir> --version 2026.11.0 \\
        --pinned bridge/src/os/windows/pinned_identity.h --out ck3agent-<v>-windows-x86_64.zip

Layout (one top directory, ck3agent-<v>-windows-x86_64/):
  version.dll      the bridge (the proxy DLL built with the bridge core)
  wheels/          ck3agent and its dependencies, for pip --no-index
  install.ps1, uninstall.ps1, README.txt
  VERSION          the release version
  pin.json         the ONE build this release supports, read from the same
                   pinned_identity.h the DLL's identity gate is compiled from
                   (codeview_hex = GUID+age, size_of_image), so the installer and
                   the gate cannot disagree
  SHA256SUMS       every other file, checked by install.ps1 before anything else

Signing (P7): version.dll must be Authenticode-signed before it is zipped. That
needs the owner's code-signing certificate and is not done here: --require-signed
refuses an unsigned DLL (a PE with no certificate table) so a release run cannot
ship one by accident.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def read_pin(path: Path) -> dict:
    src = path.read_text()

    def s(name: str) -> str:
        m = re.search(name + r'\s*=\s*((?:"[^"]*"\s*)+);', src)
        if not m:
            raise SystemExit(f"{path}: {name} not found")
        return "".join(re.findall(r'"([^"]*)"', m.group(1)))

    def n(name: str) -> int:
        m = re.search(name + r"\s*=\s*(0x[0-9A-Fa-f]+|\d+)", src)
        if not m:
            raise SystemExit(f"{path}: {name} not found")
        return int(m.group(1), 0)

    label = s("kLabel")
    branch = re.search(r"Steam branch ([0-9.]+)", label)
    return {"label": label, "codeview_hex": s("kCodeViewHex").lower(), "size_of_image": n("kSizeOfImage"),
            "image_sha256": s("kImageSha256").lower(), "steam_branch": branch.group(1) if branch else ""}


def signed(dll: Path) -> bool:
    """True if the PE has an Authenticode certificate table (data directory 4)."""
    b = dll.read_bytes()
    e = struct.unpack_from("<I", b, 0x3C)[0]
    opt = e + 24
    magic = struct.unpack_from("<H", b, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    rva, size = struct.unpack_from("<II", b, dd + 4 * 8)
    return size > 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dll", required=True); ap.add_argument("--wheels", required=True)
    ap.add_argument("--version", required=True); ap.add_argument("--pinned", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--require-signed", action="store_true", help="refuse an unsigned version.dll (release runs)")
    a = ap.parse_args()
    dll, wheels = Path(a.dll), Path(a.wheels)
    if a.require_signed and not signed(dll):
        raise SystemExit(f"{dll} has no Authenticode signature (--require-signed)")
    whl = sorted(wheels.glob("*.whl"))
    if not any(w.name.startswith("ck3agent-") for w in whl):
        raise SystemExit(f"no ck3agent wheel in {wheels}")
    top = f"ck3agent-{a.version}-windows-x86_64"
    files: dict[str, bytes] = {
        "version.dll": dll.read_bytes(),
        "install.ps1": (HERE / "install.ps1").read_bytes(),
        "uninstall.ps1": (HERE / "uninstall.ps1").read_bytes(),
        "README.txt": (HERE / "README.txt").read_bytes(),
        "VERSION": (a.version + "\n").encode(),
        "pin.json": (json.dumps(read_pin(Path(a.pinned)), indent=1) + "\n").encode(),
    }
    for w in whl:
        files[f"wheels/{w.name}"] = w.read_bytes()
    sums = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in sorted(files.items()))
    files["SHA256SUMS"] = sums.encode()
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in sorted(files.items()):
            z.writestr(f"{top}/{name}", data)
    print(f"{a.out}: {len(files)} files, dll signed={signed(dll)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
