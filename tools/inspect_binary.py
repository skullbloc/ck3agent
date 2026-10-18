#!/usr/bin/env python3
"""Print the facts about a game binary that the port needs (ADR 0001, W0/M0).

    python3 tools/inspect_binary.py PATH [--json]

Reads PE (ck3.exe), Mach-O (thin or fat) and ELF files. Stdlib only; if
`lief` is importable it cross-checks the PE result (imports, timestamp).
The file is only read, never executed or modified. Paste `--json` output into
a binary-report issue. Do not attach the binary.

Exit status: 0 ok, 1 unreadable or unrecognised file, 2 usage.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import importlib.util
import json
import re
import struct
import sys
import uuid
from pathlib import Path

PROXY_DLLS = ("version.dll", "winmm.dll", "dinput8.dll")


class InspectError(ValueError):
    pass


class Reader:
    """Bounds-checked random access to a file."""

    def __init__(self, f, size):
        self.f, self.size = f, size

    def at(self, off, n):
        if off < 0 or n < 0 or off + n > self.size:
            raise InspectError(f"read of {n} bytes at {off:#x} is outside the file ({self.size} bytes)")
        self.f.seek(off)
        b = self.f.read(n)
        if len(b) != n:
            raise InspectError(f"short read at {off:#x}")
        return b

    def cstr(self, off, limit=260):
        self.f.seek(off)
        b = self.f.read(limit)
        return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def count_bytes(path, needle):
    """Occurrences of `needle` in the file, streamed."""
    n, tail = 0, b""
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            buf = tail + chunk
            n += buf.count(needle)
            tail = buf[-(len(needle) - 1):] if len(needle) > 1 else b""
    return n


# ── PE ──────────────────────────────────────────────────────────────────────

MACHINES = {0x14C: "x86", 0x8664: "x86_64", 0xAA64: "arm64", 0x1C4: "arm"}
DLL_FLAGS = {
    "high_entropy_va": 0x0020,
    "aslr": 0x0040,
    "force_integrity": 0x0080,
    "nx": 0x0100,
    "no_seh": 0x0400,
    "cfg": 0x4000,
}
DIR_EXPORT, DIR_IMPORT, DIR_EXCEPTION, DIR_SECURITY, DIR_RELOC, DIR_DEBUG = 0, 1, 3, 4, 5, 6
DIR_TLS, DIR_DELAY_IMPORT = 9, 13
DBG_CODEVIEW, DBG_REPRO, DBG_EX_DLLCHARS = 2, 16, 20


def _rva_to_off(sections, headers_size, rva):
    for s in sections:
        span = max(s["vsize"], s["raw_size"])
        if s["rva"] <= rva < s["rva"] + span:
            return s["raw_ptr"] + (rva - s["rva"])
    if rva < headers_size:
        return rva
    return None


def inspect_pe(path, rd):
    mz = rd.at(0, 0x40)
    (e_lfanew,) = struct.unpack_from("<I", mz, 0x3C)
    if rd.at(e_lfanew, 4) != b"PE\0\0":
        raise InspectError("MZ header without a PE signature")
    machine, nsec, stamp, _, _, opt_size, chars = struct.unpack("<HHIIIHH", rd.at(e_lfanew + 4, 20))
    opt_off = e_lfanew + 24
    opt = rd.at(opt_off, opt_size)
    (magic,) = struct.unpack_from("<H", opt, 0)
    if magic == 0x20B:
        kind = "PE32+"
        (image_base,) = struct.unpack_from("<Q", opt, 24)
        dir_base, nd_off, ptr = 112, 108, 8
    elif magic == 0x10B:
        kind = "PE32"
        (image_base,) = struct.unpack_from("<I", opt, 28)
        dir_base, nd_off, ptr = 96, 92, 4
    else:
        raise InspectError(f"unknown optional header magic {magic:#x}")
    size_of_headers, = struct.unpack_from("<I", opt, 60)
    size_of_image, = struct.unpack_from("<I", opt, 56)
    (dll_chars,) = struct.unpack_from("<H", opt, 70)
    (ndirs,) = struct.unpack_from("<I", opt, nd_off)
    dirs = []
    for i in range(min(ndirs, 16)):
        if dir_base + 8 * (i + 1) > len(opt):
            break
        dirs.append(struct.unpack_from("<II", opt, dir_base + 8 * i))

    def directory(i):
        return dirs[i] if i < len(dirs) else (0, 0)

    sections = []
    sec_raw = rd.at(opt_off + opt_size, 40 * nsec)
    for i in range(nsec):
        name, vsize, rva, raw_size, raw_ptr = struct.unpack_from("<8sIIII", sec_raw, 40 * i)
        sections.append({"name": name.rstrip(b"\0").decode("latin-1"), "rva": rva, "vsize": vsize,
                         "raw_size": raw_size, "raw_ptr": raw_ptr})

    def off(rva):
        return _rva_to_off(sections, size_of_headers, rva)

    # imports
    def read_dll_names(table_rva, stride, name_field):
        names, o = [], off(table_rva) if table_rva else None
        if o is None:
            return names
        for i in range(4096):
            ent = rd.at(o + i * stride, stride)
            if not any(ent):
                break
            (name_rva,) = struct.unpack_from("<I", ent, name_field)
            no = off(name_rva)
            if no is not None:
                names.append(rd.cstr(no))
        return names

    imports = read_dll_names(directory(DIR_IMPORT)[0], 20, 12)
    delay = read_dll_names(directory(DIR_DELAY_IMPORT)[0], 32, 4)
    lowered = {n.lower() for n in imports + delay}
    proxies = [d for d in PROXY_DLLS if d in lowered]

    # debug directory: CodeView RSDS, repro marker, CET flag
    codeview, cet, repro, dbg_types = None, None, False, []
    drva, dsize = directory(DIR_DEBUG)
    if drva and off(drva) is not None:
        for i in range(min(dsize // 28, 64)):
            ent = rd.at(off(drva) + 28 * i, 28)
            _, _, _, _, dtype, dlen, _, dptr = struct.unpack("<IIHHIIII", ent)
            dbg_types.append(dtype)
            if dtype == DBG_CODEVIEW and codeview is None and dlen >= 24 and dptr:
                blob = rd.at(dptr, min(dlen, 24 + 260))
                if blob[:4] == b"RSDS":
                    age = struct.unpack_from("<I", blob, 20)[0]
                    codeview = {"guid": str(uuid.UUID(bytes_le=blob[4:20])).upper(), "age": age,
                                "pdb": blob[24:].split(b"\0", 1)[0].decode("utf-8", "replace")}
            elif dtype == DBG_REPRO:
                repro = True
            elif dtype == DBG_EX_DLLCHARS and dlen >= 4 and dptr:
                cet = bool(struct.unpack("<I", rd.at(dptr, 4))[0] & 1)

    # TLS callbacks (AddressOfCallBacks is a VA)
    tls_count = 0
    trva, _ = directory(DIR_TLS)
    if trva and off(trva) is not None:
        cb_va = struct.unpack_from("<Q" if ptr == 8 else "<I", rd.at(off(trva) + 3 * ptr, ptr))[0]
        co = off(cb_va - image_base) if cb_va else None
        if co is not None:
            while tls_count < 256 and struct.unpack("<Q" if ptr == 8 else "<I", rd.at(co + tls_count * ptr, ptr))[0]:
                tls_count += 1

    pdata = next((s for s in sections if s["name"] == ".pdata"), None)
    return {
        "format": "pe",
        "pe_kind": kind,
        "machine": MACHINES.get(machine, f"unknown({machine:#x})"),
        "time_date_stamp": stamp,
        "time_date_stamp_utc": datetime.datetime.fromtimestamp(stamp, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "reproducible_build": repro,
        "codeview": codeview,
        "image_base": f"{image_base:#x}",
        "size_of_image": size_of_image,
        "dll_characteristics": f"{dll_chars:#06x}",
        "flags": {**{k: bool(dll_chars & v) for k, v in DLL_FLAGS.items()}, "cet_compat": cet,
                  "relocs_stripped": bool(chars & 1)},
        "tls_callbacks": tls_count,
        "pdata_size": directory(DIR_EXCEPTION)[1] or (pdata["vsize"] if pdata else 0),
        "authenticode_present": directory(DIR_SECURITY)[1] > 0,
        "imports": imports,
        "delay_imports": delay,
        "proxy_candidates": proxies,
        "msvc_rtti_strings": count_bytes(path, b".?AV"),
        "sections": [{k: v for k, v in s.items() if k != "raw_ptr"} for s in sections],
    }


# ── Mach-O ──────────────────────────────────────────────────────────────────

CPU = {7: "i386", 0x01000007: "x86_64", 12: "arm", 0x0100000C: "arm64", 0x0200000C: "arm64_32"}
LC_SEGMENT_64, LC_UUID, LC_CODE_SIGNATURE = 0x19, 0x1B, 0x1D
LC_ENCRYPTION_INFO, LC_ENCRYPTION_INFO_64 = 0x21, 0x2C
LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_BUILD_VERSION = 0xC, 0x80000018, 0x8000001F, 0x32
MACHO_THIN = {0xFEEDFACF: 64, 0xFEEDFACE: 32}  # little-endian files only
FILETYPES = {1: "object", 2: "execute", 6: "dylib", 8: "bundle"}
CS_FLAGS = {"adhoc": 0x2, "forced_lv": 0x10, "hard": 0x100, "kill": 0x200, "restrict": 0x800,
            "require_lv": 0x2000, "runtime": 0x10000, "linker_signed": 0x20000}
CS_SUPERBLOB, CS_CODEDIR, CS_ENTITLEMENTS, CS_DER_ENTITLEMENTS = 0xFADE0CC0, 0xFADE0C02, 0xFADE7171, 0xFADE7172
ENTITLEMENT_HINTS = {
    "allow_dyld_environment_variables": b"com.apple.security.cs.allow-dyld-environment-variables",
    "disable_library_validation": b"com.apple.security.cs.disable-library-validation",
    "allow_unsigned_executable_memory": b"com.apple.security.cs.allow-unsigned-executable-memory",
    "allow_jit": b"com.apple.security.cs.allow-jit",
}


def _parse_signature(blob):
    out = {"present": True, "flags": None, "identifier": None, "entitlements": None,
           "der_entitlements": False, "hardened_runtime": False, "library_validation": False}
    if len(blob) < 12 or struct.unpack_from(">I", blob, 0)[0] != CS_SUPERBLOB:
        out["note"] = "unrecognised signature blob"
        return out
    count = struct.unpack_from(">I", blob, 8)[0]
    for i in range(min(count, 64)):
        if 12 + 8 * i + 8 > len(blob):
            break
        _, boff = struct.unpack_from(">II", blob, 12 + 8 * i)
        if boff + 8 > len(blob):
            continue
        magic, length = struct.unpack_from(">II", blob, boff)
        body = blob[boff:boff + length]
        if magic == CS_CODEDIR and len(body) >= 24:
            flags = struct.unpack_from(">I", body, 12)[0]
            ident_off = struct.unpack_from(">I", body, 20)[0]
            out["flags"] = f"{flags:#x}"
            out["flag_names"] = [k for k, v in CS_FLAGS.items() if flags & v]
            out["hardened_runtime"] = bool(flags & CS_FLAGS["runtime"])
            out["library_validation"] = bool(flags & (CS_FLAGS["require_lv"] | CS_FLAGS["forced_lv"]))
            if ident_off < len(body):
                out["identifier"] = body[ident_off:].split(b"\0", 1)[0].decode("utf-8", "replace")
        elif magic == CS_ENTITLEMENTS:
            raw = body[8:]
            out["entitlements"] = raw.decode("utf-8", "replace")
            out["entitlement_hints"] = {k: v in raw for k, v in ENTITLEMENT_HINTS.items()}
        elif magic == CS_DER_ENTITLEMENTS:
            out["der_entitlements"] = True
    return out


def _inspect_slice(rd, base, size):
    magic = struct.unpack("<I", rd.at(base, 4))[0]
    bits = MACHO_THIN.get(magic)
    if bits is None:
        raise InspectError(f"slice at {base:#x} is not a little-endian Mach-O (magic {magic:#x})")
    hdr_len = 32 if bits == 64 else 28
    _, cputype, _, filetype, ncmds, sizeofcmds, _ = struct.unpack("<IiIIIII", rd.at(base, 28))
    cmds = rd.at(base + hdr_len, min(sizeofcmds, 1 << 20))
    res = {"arch": CPU.get(cputype, f"unknown({cputype:#x})"), "offset": base, "size": size,
           "filetype": FILETYPES.get(filetype, str(filetype)), "uuid": None, "encrypted": False,
           "restrict_segment": False, "dylibs": [],
           "code_signature": {"present": False}}
    pos = 0
    for _ in range(min(ncmds, 4096)):
        if pos + 8 > len(cmds):
            break
        cmd, cmdsize = struct.unpack_from("<II", cmds, pos)
        if cmdsize < 8 or pos + cmdsize > len(cmds):
            break
        body = cmds[pos:pos + cmdsize]
        if cmd == LC_UUID and cmdsize >= 24:
            res["uuid"] = str(uuid.UUID(bytes=body[8:24])).upper()
        elif cmd == LC_CODE_SIGNATURE and cmdsize >= 16:
            dataoff, datasize = struct.unpack_from("<II", body, 8)
            if datasize and dataoff + datasize <= size and datasize <= 16 << 20:
                res["code_signature"] = _parse_signature(rd.at(base + dataoff, datasize))
            else:
                res["code_signature"] = {"present": True, "note": "signature outside the slice"}
        elif cmd in (LC_ENCRYPTION_INFO, LC_ENCRYPTION_INFO_64) and cmdsize >= 20:
            res["encrypted"] = struct.unpack_from("<I", body, 16)[0] != 0
        elif cmd == LC_SEGMENT_64 and cmdsize >= 24:
            if body[8:24].split(b"\0", 1)[0] == b"__RESTRICT":
                res["restrict_segment"] = True
        elif cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB) and cmdsize >= 24:
            name_off = struct.unpack_from("<I", body, 8)[0]
            res["dylibs"].append(body[name_off:].split(b"\0", 1)[0].decode("utf-8", "replace"))
        pos += cmdsize
    return res


def inspect_macho(path, rd):
    (magic,) = struct.unpack(">I", rd.at(0, 4))
    if magic in (0xCAFEBABE, 0xCAFEBABF):
        is64 = magic == 0xCAFEBABF
        (n,) = struct.unpack(">I", rd.at(4, 4))
        if n == 0 or n > 16:
            raise InspectError("not a fat Mach-O (implausible arch count)")
        rec = 32 if is64 else 20
        slices = []
        for i in range(n):
            ent = rd.at(8 + rec * i, rec)
            if is64:
                _, _, off, size = struct.unpack_from(">iiQQ", ent, 0)
            else:
                _, _, off, size = struct.unpack_from(">iiII", ent, 0)
            slices.append(_inspect_slice(rd, off, size))
        return {"format": "macho-fat", "slices": slices}
    return {"format": "macho", "slices": [_inspect_slice(rd, 0, rd.size)]}


# ── ELF ─────────────────────────────────────────────────────────────────────

def _load_elf_module():
    """ck3agent/_elf.py: the installed package, else the file next to this repo."""
    try:
        from ck3agent import _elf  # type: ignore
        return _elf
    except ImportError:
        p = Path(__file__).resolve().parents[1] / "ck3agent" / "_elf.py"
        if not p.is_file():
            raise InspectError("ck3agent/_elf.py not found; run from a ck3agent checkout or install ck3agent")
        spec = importlib.util.spec_from_file_location("_ck3agent_elf", p)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod


def inspect_elf(path, rd):
    elf = _load_elf_module()
    try:
        build_id = elf.read_build_id(path)
    except elf.NotAnElf as e:
        raise InspectError(str(e))
    ident = rd.at(0, 20)
    machine = struct.unpack_from("<H", ident, 18)[0] if ident[5] == 1 else None
    return {"format": "elf", "elf_class": {1: 32, 2: 64}.get(ident[4]), "machine": {0x3E: "x86_64", 0xB7: "arm64"}.get(machine, machine),
            "build_id": build_id}


# ── optional lief cross-check ───────────────────────────────────────────────

def lief_crosscheck(path, res):
    try:
        import lief  # type: ignore
    except ImportError:
        return None
    try:
        b = lief.parse(str(path))
        if b is None:
            return {"version": lief.__version__, "parsed": False}
        out = {"version": lief.__version__, "parsed": True}
        if res["format"] == "pe":
            theirs = sorted({i.name.lower() for i in b.imports})
            ours = sorted({n.lower() for n in res["imports"]})
            out["imports_agree"] = theirs == ours
            out["time_date_stamp_agrees"] = int(b.header.time_date_stamps) == res["time_date_stamp"]
        return out
    except Exception as e:  # lief's API moves between releases; never fail the report over it
        return {"version": getattr(lief, "__version__", "?"), "error": f"{type(e).__name__}: {e}"}


# ── driver ──────────────────────────────────────────────────────────────────

def detect(head):
    if head[:2] == b"MZ":
        return "pe"
    if head[:4] == b"\x7fELF":
        return "elf"
    if head[:4] in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
        return "macho"
    return None


def inspect_file(path, use_lief=True):
    path = Path(path)
    if not path.is_file():
        raise InspectError(f"{path}: not a file")
    size = path.stat().st_size
    with open(path, "rb") as f:
        rd = Reader(f, size)
        kind = detect(f.read(4))
        if kind is None:
            raise InspectError(f"{path}: not a PE, Mach-O or ELF file")
        res = {"path": str(path), "size": size, "sha256": sha256_file(path)}
        res.update({"pe": inspect_pe, "macho": inspect_macho, "elf": inspect_elf}[kind](path, rd))
    if use_lief and kind == "pe":  # lief 1.0 crashed on a hand-built fat Mach-O, so PE only
        lc = lief_crosscheck(path, res)
        if lc is not None:
            res["lief"] = lc
    return res


def _fmt_flags(d):
    # None = the image carries no marker (cet_compat comes from the debug directory only)
    return " ".join(f"{k}={'n/a' if v is None else int(v)}" for k, v in d.items())


def render_text(r):
    L = [f"file:    {r['path']}", f"size:    {r['size']}", f"sha256:  {r['sha256']}", f"format:  {r['format']}"]
    if r["format"] == "pe":
        cv = r["codeview"]
        L += [f"machine: {r['machine']} ({r['pe_kind']})",
              f"stamp:   {r['time_date_stamp']} ({r['time_date_stamp_utc']})" + ("  /Brepro: not a clock time" if r["reproducible_build"] else ""),
              "rsds:    " + (f"{cv['guid']} age {cv['age']} {cv['pdb']}" if cv else "none"),
              f"image:   base {r['image_base']} size {r['size_of_image']:#x}",
              f"dllchar: {r['dll_characteristics']}  {_fmt_flags(r['flags'])}",
              f"tls:     {r['tls_callbacks']} callbacks",
              f"pdata:   {r['pdata_size']} bytes",
              f"signed:  {'authenticode present' if r['authenticode_present'] else 'no authenticode'}",
              f"rtti:    {r['msvc_rtti_strings']} '.?AV' strings",
              f"imports: {len(r['imports'])} dlls" + (f" (+{len(r['delay_imports'])} delay)" if r["delay_imports"] else "")]
        L += ["         " + n + ("   <- proxy candidate" if n.lower() in PROXY_DLLS else "") for n in r["imports"] + r["delay_imports"]]
        L.append("proxy:   " + (", ".join(r["proxy_candidates"]) or "none of " + "/".join(PROXY_DLLS)))
    elif r["format"].startswith("macho"):
        for i, s in enumerate(r["slices"]):
            cs = s["code_signature"]
            L += [f"slice {i}: {s['arch']} {s['filetype']} uuid {s['uuid']}  encrypted={int(s['encrypted'])} __RESTRICT={int(s['restrict_segment'])}"]
            if cs["present"]:
                L.append(f"  signature: flags {cs.get('flags')} {','.join(cs.get('flag_names', [])) or '-'}  id {cs.get('identifier')}"
                         f"  runtime={int(cs.get('hardened_runtime', False))} library_validation={int(cs.get('library_validation', False))}")
                if cs.get("entitlements"):
                    L += ["  entitlements:"] + ["    " + ln for ln in cs["entitlements"].splitlines()]
            else:
                L.append("  signature: none")
    else:
        L += [f"machine: {r['machine']} ({r['elf_class']}-bit)", f"buildid: {r['build_id']}"]
    if "lief" in r:
        L.append(f"lief:    {r['lief']}")
    return "\n".join(L)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Print identity and loader facts for a PE, Mach-O or ELF file.")
    ap.add_argument("path")
    ap.add_argument("--json", action="store_true", help="machine-readable output (paste into a binary-report issue)")
    ap.add_argument("--no-lief", action="store_true", help="skip the optional lief cross-check")
    args = ap.parse_args(argv)
    try:
        r = inspect_file(args.path, use_lief=not args.no_lief)
    except (InspectError, OSError, struct.error) as e:
        print(f"inspect_binary: {e}", file=sys.stderr)
        return 1
    print(json.dumps(r, indent=2) if args.json else render_text(r))
    return 0


if __name__ == "__main__":
    sys.exit(main())
