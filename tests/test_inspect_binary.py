"""tools/inspect_binary.py on synthesised PE, Mach-O and ELF files. No game binary needed."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import struct
import sys
import uuid
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[1] / "tools" / "inspect_binary.py"
if not TOOL.is_file():
    pytest.skip("tools/ is not part of this tree (sdist)", allow_module_level=True)
_spec = importlib.util.spec_from_file_location("inspect_binary", TOOL)
ib = importlib.util.module_from_spec(_spec)
sys.modules["inspect_binary"] = ib
_spec.loader.exec_module(ib)

IMAGE_BASE = 0x140000000
GUID = uuid.UUID("12345678-9abc-def0-1122-334455667788")


# ── builders ────────────────────────────────────────────────────────────────

def build_pe(imports=("VERSION.dll", "KERNEL32.dll"), tls_callbacks=2, cet=True, stamp=1_700_000_000,
             dll_chars=0x4160, pdb="ck3.pdb", rsds=True, signed=False, rtti=3):
    ptr_rdata, rva_rdata, ptr_pdata, rva_pdata = 0x200, 0x1000, 0x600, 0x2000

    def r2o(rva):
        return ptr_rdata + rva - rva_rdata

    rdata = bytearray(0x400)
    # import descriptors (20 bytes each) then names
    name_rva = rva_rdata + 0x100
    for i, n in enumerate(imports):
        struct.pack_into("<IIIII", rdata, 20 * i, 0x1F00, 0, 0, name_rva, 0x1F00)
        nb = n.encode() + b"\0"
        rdata[name_rva - rva_rdata:name_rva - rva_rdata + len(nb)] = nb
        name_rva += len(nb)
    # debug directory: CodeView + EX_DLLCHARACTERISTICS
    dbg_rva = rva_rdata + 0x200
    cv = b"RSDS" + GUID.bytes_le + struct.pack("<I", 7) + pdb.encode() + b"\0"
    cv_rva, ex_rva = rva_rdata + 0x240, rva_rdata + 0x300
    entries = []
    if rsds:
        entries.append(struct.pack("<IIHHIIII", 0, stamp, 0, 0, 2, len(cv), cv_rva, r2o(cv_rva)))
        rdata[cv_rva - rva_rdata:cv_rva - rva_rdata + len(cv)] = cv
    entries.append(struct.pack("<IIHHIIII", 0, stamp, 0, 0, 20, 4, ex_rva, r2o(ex_rva)))
    rdata[ex_rva - rva_rdata:ex_rva - rva_rdata + 4] = struct.pack("<I", 1 if cet else 0)
    for i, e in enumerate(entries):
        rdata[dbg_rva - rva_rdata + 28 * i:dbg_rva - rva_rdata + 28 * (i + 1)] = e
    # TLS directory (PE32+): start, end, index, callbacks VA
    tls_rva, cb_rva = rva_rdata + 0x320, rva_rdata + 0x380
    struct.pack_into("<QQQQ", rdata, tls_rva - rva_rdata, 0, 0, 0, IMAGE_BASE + cb_rva)
    for i in range(tls_callbacks):
        struct.pack_into("<Q", rdata, cb_rva - rva_rdata + 8 * i, IMAGE_BASE + 0x1000 + i)
    if rtti:
        rdata[0x3A0:0x3A0 + 4 * rtti] = b".?AV" * rtti

    dirs = [(0, 0)] * 16
    dirs[1] = (rva_rdata, 20 * (len(imports) + 1))
    dirs[3] = (rva_pdata, 36)
    dirs[4] = (0x5000, 0x200) if signed else (0, 0)
    dirs[6] = (dbg_rva, 28 * len(entries))
    dirs[9] = (tls_rva, 40)
    opt = bytearray(240)
    struct.pack_into("<H", opt, 0, 0x20B)
    struct.pack_into("<Q", opt, 24, IMAGE_BASE)
    struct.pack_into("<IIII", opt, 32, 0x1000, 0x200, 6, 0)  # section/file align, os version
    struct.pack_into("<I", opt, 56, 0x3000)                  # SizeOfImage
    struct.pack_into("<I", opt, 60, 0x200)                   # SizeOfHeaders
    struct.pack_into("<H", opt, 70, dll_chars)
    struct.pack_into("<I", opt, 108, 16)
    for i, (a, b) in enumerate(dirs):
        struct.pack_into("<II", opt, 112 + 8 * i, a, b)
    coff = struct.pack("<HHIIIHH", 0x8664, 2, stamp, 0, 0, len(opt), 0x22)
    sec = (struct.pack("<8sIIIIIIHHI", b".rdata", 0x400, rva_rdata, 0x400, ptr_rdata, 0, 0, 0, 0, 0x40000040) +
           struct.pack("<8sIIIIIIHHI", b".pdata", 0x24, rva_pdata, 0x200, ptr_pdata, 0, 0, 0, 0, 0x40000040))
    mz = bytearray(0x40)
    mz[:2] = b"MZ"
    struct.pack_into("<I", mz, 0x3C, 0x40)
    head = bytes(mz) + b"PE\0\0" + coff + bytes(opt) + sec
    return head.ljust(0x200, b"\0") + bytes(rdata) + b"\0" * 0x200


def _blob(magic, body):
    return struct.pack(">II", magic, 8 + len(body)) + body


def build_signature(flags, ident=b"com.test.ck3", entitlements=None):
    cd = bytearray(48)
    struct.pack_into(">IIII", cd, 0, 0xFADE0C02, 0, 0x20400, flags)
    struct.pack_into(">I", cd, 20, len(cd))
    cd += ident + b"\0"
    struct.pack_into(">I", cd, 4, len(cd))
    blobs = [(0, bytes(cd))]
    if entitlements is not None:
        blobs.append((5, _blob(0xFADE7171, entitlements)))
    idx_len = 12 + 8 * len(blobs)
    out, entries, off = b"", b"", idx_len
    for t, b in blobs:
        entries += struct.pack(">II", t, off)
        out += b
        off += len(b)
    return struct.pack(">III", 0xFADE0CC0, idx_len + len(out), len(blobs)) + entries + out


def build_macho_slice(cputype=0x01000007, uid=GUID, signature=None, restrict=False, dylib=b"/usr/lib/libSystem.B.dylib"):
    cmds = struct.pack("<II16s", 0x1B, 24, uid.bytes)
    name = dylib + b"\0"
    name = name.ljust((len(name) + 7) & ~7, b"\0")
    cmds += struct.pack("<IIIIII", 0xC, 24 + len(name), 24, 0, 0, 0) + name
    if restrict:
        cmds += struct.pack("<II16sQQQQiiII", 0x19, 72, b"__RESTRICT", 0, 0, 0, 0, 0, 0, 0, 0)
    ncmds = 2 + int(restrict)
    sig_off = 0x1000
    if signature is not None:
        cmds += struct.pack("<IIII", 0x1D, 16, sig_off, len(signature))
        ncmds += 1
    hdr = struct.pack("<IiiIIIII", 0xFEEDFACF, cputype, 3, 2, ncmds, len(cmds), 0, 0)
    body = (hdr + cmds).ljust(sig_off, b"\0")
    return body + (signature or b"")


def build_fat(slices):
    entries, off, data = b"", 0x1000, b""
    for cputype, s in slices:
        entries += struct.pack(">iiIII", cputype, 3, off, len(s), 12)
        data += s.ljust(((len(s) + 0xFFF) & ~0xFFF), b"\0")
        off += (len(s) + 0xFFF) & ~0xFFF
    return (struct.pack(">II", 0xCAFEBABE, len(slices)) + entries).ljust(0x1000, b"\0") + data


def build_elf(build_id=bytes(range(20))):
    note = struct.pack("<III", 4, len(build_id), 3) + b"GNU\0" + build_id
    shoff = 64 + len(note)
    hdr = bytearray(64)
    hdr[:4] = b"\x7fELF"
    hdr[4:7] = bytes([2, 1, 1])
    struct.pack_into("<HH", hdr, 16, 3, 0x3E)
    struct.pack_into("<Q", hdr, 0x28, shoff)
    struct.pack_into("<HHH", hdr, 0x34, 64, 0, 0)
    struct.pack_into("<HH", hdr, 0x3A, 64, 2)
    null_sh = bytes(64)
    note_sh = bytearray(64)
    struct.pack_into("<II", note_sh, 0, 0, 7)
    struct.pack_into("<QQ", note_sh, 0x18, 64, len(note))
    return bytes(hdr) + note + null_sh + bytes(note_sh)


def write(tmp_path, name, data):
    p = tmp_path / name
    p.write_bytes(data)
    return p


# ── PE ──────────────────────────────────────────────────────────────────────

def test_pe_fields(tmp_path):
    data = build_pe()
    r = ib.inspect_file(write(tmp_path, "ck3.exe", data))
    assert r["format"] == "pe" and r["pe_kind"] == "PE32+" and r["machine"] == "x86_64"
    assert r["sha256"] == hashlib.sha256(data).hexdigest()
    assert r["time_date_stamp"] == 1_700_000_000
    assert r["time_date_stamp_utc"] == "2023-11-14T22:13:20Z"
    assert r["codeview"] == {"guid": str(GUID).upper(), "age": 7, "pdb": "ck3.pdb"}
    assert r["image_base"] == hex(IMAGE_BASE) and r["size_of_image"] == 0x3000
    f = r["flags"]
    assert f["aslr"] and f["high_entropy_va"] and f["nx"] and f["cfg"] and f["cet_compat"] is True
    assert not f["force_integrity"] and not f["relocs_stripped"]
    assert r["tls_callbacks"] == 2
    assert r["pdata_size"] == 36
    assert r["imports"] == ["VERSION.dll", "KERNEL32.dll"]
    assert r["proxy_candidates"] == ["version.dll"]
    assert r["msvc_rtti_strings"] == 3
    assert r["authenticode_present"] is False


def test_pe_variants(tmp_path):
    r = ib.inspect_file(write(tmp_path, "a.exe", build_pe(
        imports=("winmm.dll", "dinput8.dll"), tls_callbacks=0, cet=False, dll_chars=0x0100,
        rsds=False, signed=True, rtti=0)))
    assert r["codeview"] is None
    assert r["flags"]["cet_compat"] is False and not r["flags"]["aslr"] and not r["flags"]["cfg"]
    assert r["tls_callbacks"] == 0
    assert r["proxy_candidates"] == ["winmm.dll", "dinput8.dll"]
    assert r["msvc_rtti_strings"] == 0
    assert r["authenticode_present"] is True


def test_rtti_count_spans_chunk_boundary(tmp_path):
    p = write(tmp_path, "x.bin", b"a" * 99 + b".?AV" + b"b" * 10)
    assert ib.count_bytes(p, b".?AV") == 1
    # force a boundary between the second and third byte of the needle
    big = b"\0" * ((1 << 22) - 2) + b".?AV" + b"\0" * 8
    assert ib.count_bytes(write(tmp_path, "y.bin", big), b".?AV") == 1


def test_truncated_pe_is_an_error_not_a_traceback(tmp_path, capsys):
    p = write(tmp_path, "cut.exe", build_pe()[:0x120])
    assert ib.main([str(p)]) == 1
    assert "inspect_binary:" in capsys.readouterr().err


# ── Mach-O ──────────────────────────────────────────────────────────────────

def test_macho_thin_with_signature(tmp_path):
    ents = b"<plist><dict><key>com.apple.security.cs.allow-dyld-environment-variables</key><true/></dict></plist>"
    sig = build_signature(0x10000 | 0x2000, entitlements=ents)  # runtime + require_lv
    r = ib.inspect_file(write(tmp_path, "ck3", build_macho_slice(signature=sig, restrict=True)))
    assert r["format"] == "macho" and len(r["slices"]) == 1
    s = r["slices"][0]
    assert s["arch"] == "x86_64" and s["uuid"] == str(GUID).upper() and s["filetype"] == "execute"
    assert s["restrict_segment"] is True and s["dylibs"] == ["/usr/lib/libSystem.B.dylib"]
    cs = s["code_signature"]
    assert cs["present"] and cs["identifier"] == "com.test.ck3" and cs["flags"] == "0x12000"
    assert cs["hardened_runtime"] and cs["library_validation"]
    assert "runtime" in cs["flag_names"] and "require_lv" in cs["flag_names"]
    assert "allow-dyld-environment-variables" in cs["entitlements"]
    assert cs["entitlement_hints"]["allow_dyld_environment_variables"] is True
    assert cs["entitlement_hints"]["allow_jit"] is False


def test_macho_unsigned(tmp_path):
    s = ib.inspect_file(write(tmp_path, "ck3", build_macho_slice()))["slices"][0]
    assert s["code_signature"] == {"present": False}
    assert s["restrict_segment"] is False


def test_macho_fat_has_one_uuid_per_slice(tmp_path):
    other = uuid.UUID("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
    data = build_fat([(0x01000007, build_macho_slice(0x01000007, GUID)),
                      (0x0100000C, build_macho_slice(0x0100000C, other, signature=build_signature(0x2)))])
    r = ib.inspect_file(write(tmp_path, "ck3", data))
    assert r["format"] == "macho-fat"
    assert [(s["arch"], s["uuid"]) for s in r["slices"]] == [("x86_64", str(GUID).upper()), ("arm64", str(other).upper())]
    assert r["slices"][0]["code_signature"]["present"] is False
    assert r["slices"][1]["code_signature"]["flag_names"] == ["adhoc"]
    assert r["sha256"] == hashlib.sha256(data).hexdigest()


# ── ELF ─────────────────────────────────────────────────────────────────────

def test_elf_synthetic_build_id(tmp_path):
    r = ib.inspect_file(write(tmp_path, "ck3", build_elf()))
    assert r["format"] == "elf" and r["machine"] == "x86_64" and r["elf_class"] == 64
    assert r["build_id"] == bytes(range(20)).hex()


@pytest.mark.skipif(sys.platform != "linux", reason="the host python is an ELF only on Linux")
def test_elf_matches_ck3agent_reader_on_this_python(tmp_path):
    from ck3agent._elf import read_build_id
    exe = Path(sys.executable).resolve()
    r = ib.inspect_file(exe)
    assert r["format"] == "elf" and r["build_id"] == read_build_id(exe)
    assert r["sha256"] == hashlib.sha256(exe.read_bytes()).hexdigest()


# ── CLI ─────────────────────────────────────────────────────────────────────

def test_cli_json_and_text(tmp_path, capsys):
    p = write(tmp_path, "ck3.exe", build_pe())
    assert ib.main([str(p), "--json"]) == 0
    assert json.loads(capsys.readouterr().out)["codeview"]["age"] == 7
    assert ib.main([str(p)]) == 0
    out = capsys.readouterr().out
    assert "proxy candidate" in out and "sha256:" in out and "tls:     2 callbacks" in out


def test_cli_rejects_non_binaries(tmp_path, capsys):
    assert ib.main([str(write(tmp_path, "notes.txt", b"hello"))]) == 1
    assert ib.main([str(tmp_path / "missing")]) == 1
    assert capsys.readouterr().err.count("inspect_binary:") == 2
