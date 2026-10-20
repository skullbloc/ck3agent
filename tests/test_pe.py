"""ck3agent/_pe.py on synthesised PE files. No game binary needed.

The pinned values are those of the Windows 1.18.3.1 ck3.exe as
tools/inspect_binary.py read them from the depot (CodeView GUID
3E26A827-C47A-4FB0-A863-3288849287C8 age 1, SizeOfImage 94806016, ImageBase
0x140000000, TimeDateStamp 1770134324). A synthetic PE carrying them must read
back as them, and its key must equal the bridge DLL's pinned constant
(bridge/src/os/windows/pinned_identity.h) when that header is in the tree, so
the Python reader and the DLL's gate cannot disagree on the spelling.
"""
from __future__ import annotations

import importlib.util
import re
import struct
import sys
import uuid
from pathlib import Path

import pytest

from ck3agent import _pe

ROOT = Path(__file__).resolve().parents[1]

PIN_GUID = uuid.UUID("3E26A827-C47A-4FB0-A863-3288849287C8")
PIN_AGE = 1
PIN_SIZE = 94806016
PIN_BASE = 0x140000000
PIN_STAMP = 1770134324
PIN_PDB = r"C:\mnt\gsg\ck3\titus\outputdir_windows\binaries\ck3.pdb"
PIN_KEY = "3e26a827c47a4fb0a8633288849287c8" "00000001"
OTHER_GUID = uuid.UUID("0DF59548-663D-4C98-A61C-878362242E2B")  # 1.20.1.1


def build_pe(guid=PIN_GUID, age=PIN_AGE, size_of_image=PIN_SIZE, image_base=PIN_BASE,
             stamp=PIN_STAMP, pdb=PIN_PDB, rsds=True, pe32=False, debug_dir=True, extra_entries=0):
    """A minimal PE: headers, one .rdata section holding the debug directory
    (an optional non-CodeView entry first, then CodeView) and the RSDS record."""
    sec_rva, sec_ptr, sec_raw = 0x1000, 0x400, 0x400
    rdata = bytearray(sec_raw)
    cv = b"RSDS" + guid.bytes_le + struct.pack("<I", age) + pdb.encode() + b"\0"
    dbg_rva = sec_rva
    entries = []
    for _ in range(extra_entries):  # e.g. IMAGE_DEBUG_TYPE_POGO (13) before CodeView
        entries.append(struct.pack("<IIHHIIII", 0, stamp, 0, 0, 13, 4, sec_rva + 0x300, sec_ptr + 0x300))
    cv_off = 0x100
    if rsds:
        entries.append(struct.pack("<IIHHIIII", 0, stamp, 0, 0, 2, len(cv), sec_rva + cv_off, sec_ptr + cv_off))
        rdata[cv_off:cv_off + len(cv)] = cv
    for i, e in enumerate(entries):
        rdata[28 * i:28 * (i + 1)] = e

    e_lfanew = 0x80
    opt_size = 224 if pe32 else 240
    opt = bytearray(opt_size)
    struct.pack_into("<H", opt, 0, 0x10B if pe32 else 0x20B)
    if pe32:
        struct.pack_into("<I", opt, 28, image_base & 0xFFFFFFFF)
        ndir_off, dirs_off = 92, 96
    else:
        struct.pack_into("<Q", opt, 24, image_base)
        ndir_off, dirs_off = 108, 112
    struct.pack_into("<I", opt, 56, size_of_image)
    struct.pack_into("<I", opt, 60, 0x400)  # SizeOfHeaders
    struct.pack_into("<I", opt, ndir_off, 16)
    if debug_dir and entries:
        struct.pack_into("<II", opt, dirs_off + 8 * 6, dbg_rva, 28 * len(entries))
    fh = struct.pack("<HHIIIHH", 0x14C if pe32 else 0x8664, 1, stamp, 0, 0, opt_size, 0x22)
    sect = struct.pack("<8sIIIIIIHHI", b".rdata", sec_raw, sec_rva, sec_raw, sec_ptr, 0, 0, 0, 0, 0x40000040)
    hdr = bytearray(sec_ptr)
    hdr[0:2] = b"MZ"
    struct.pack_into("<I", hdr, 0x3C, e_lfanew)
    blob = b"PE\0\0" + fh + bytes(opt) + sect
    hdr[e_lfanew:e_lfanew + len(blob)] = blob
    return bytes(hdr) + bytes(rdata)


def write(tmp_path, data, name="ck3.exe"):
    p = tmp_path / name
    p.write_bytes(data)
    return p


def test_pinned_values_read_back(tmp_path):
    ident = _pe.read_pe_identity(write(tmp_path, build_pe()))
    assert ident.magic == 0x20B and ident.machine == 0x8664
    assert ident.size_of_image == PIN_SIZE
    assert ident.image_base == PIN_BASE
    assert ident.time_date_stamp == PIN_STAMP
    assert ident.codeview is not None
    assert ident.codeview.guid_str == "3E26A827-C47A-4FB0-A863-3288849287C8"
    assert ident.codeview.age == PIN_AGE
    assert ident.codeview.pdb_path == PIN_PDB
    assert ident.codeview_key == PIN_KEY


def test_the_other_build_has_another_key(tmp_path):
    ident = _pe.read_pe_identity(write(tmp_path, build_pe(guid=OTHER_GUID, size_of_image=0x61D9000)))
    assert ident.codeview.guid == OTHER_GUID
    assert ident.codeview_key == "0df59548663d4c98a61c878362242e2b00000001"
    assert ident.codeview_key != PIN_KEY
    # the age is part of the key
    assert _pe.read_pe_identity(write(tmp_path, build_pe(age=2), "a2.exe")).codeview_key != PIN_KEY


def test_codeview_found_after_other_debug_entries(tmp_path):
    ident = _pe.read_pe_identity(write(tmp_path, build_pe(extra_entries=2)))
    assert ident.codeview_key == PIN_KEY


@pytest.mark.parametrize("kw", [{"rsds": False}, {"debug_dir": False}])
def test_no_codeview_is_none_not_a_default(tmp_path, kw):
    ident = _pe.read_pe_identity(write(tmp_path, build_pe(**kw)))
    assert ident.codeview is None and ident.codeview_key is None
    assert ident.size_of_image == PIN_SIZE  # the rest still reads


def test_pe32(tmp_path):
    ident = _pe.read_pe_identity(write(tmp_path, build_pe(pe32=True, image_base=0x400000)))
    assert ident.magic == 0x10B and ident.image_base == 0x400000 and ident.codeview_key == PIN_KEY


@pytest.mark.parametrize("mutate", [
    lambda b: b"ZM" + b[2:],                              # no MZ
    lambda b: b[:0x80] + b"PX\0\0" + b[0x84:],            # no PE signature
    lambda b: b[:0x100],                                  # truncated inside the optional header
    lambda b: b[:0x450],                                  # CodeView record cut off
    lambda b: b[:0x98] + b"\x0b\x03" + b[0x9a:],          # unknown optional-header magic
    lambda b: b"",                                        # empty
])
def test_malformed_is_an_error_not_a_traceback(tmp_path, mutate):
    with pytest.raises(_pe.NotAPe):
        _pe.read_pe_identity(write(tmp_path, mutate(build_pe())))


def test_an_elf_is_not_a_pe(tmp_path):
    with pytest.raises(_pe.NotAPe):
        _pe.read_pe_identity(write(tmp_path, b"\x7fELF" + bytes(60), "ck3"))


def test_key_equals_the_bridge_dll_pin():
    hdr = next((p / "bridge/src/os/windows/pinned_identity.h" for p in (ROOT, *ROOT.parents)
                if (p / "bridge/src/os/windows/pinned_identity.h").is_file()), None)
    if hdr is None:
        pytest.skip("bridge/src/os/windows/pinned_identity.h is not in this tree")
    text = hdr.read_text(encoding="utf-8")
    m = re.search(r'kCodeViewHex\s*=\s*"([0-9a-f]+)"\s*"([0-9a-f]+)"', text)
    assert m, "kCodeViewHex not found in pinned_identity.h"
    assert m.group(1) + m.group(2) == PIN_KEY
    assert re.search(r"kSizeOfImage\s*=\s*%d\b" % PIN_SIZE, text)
    assert re.search(r"kImageBase\s*=\s*0x%xull" % PIN_BASE, text)
    assert re.search(r"kTimeDateStamp\s*=\s*%du" % PIN_STAMP, text)


def test_agrees_with_inspect_binary(tmp_path):
    """A second reader that shares no code: tools/inspect_binary.py."""
    tool = next((p / "tools/inspect_binary.py" for p in (ROOT, *ROOT.parents)
                 if (p / "tools/inspect_binary.py").is_file()), None)
    if tool is None:
        pytest.skip("tools/ is not part of this tree (sdist)")
    spec = importlib.util.spec_from_file_location("inspect_binary_for_pe", tool)
    ib = importlib.util.module_from_spec(spec)
    sys.modules["inspect_binary_for_pe"] = ib
    spec.loader.exec_module(ib)
    p = write(tmp_path, build_pe())
    r = ib.inspect_file(str(p), use_lief=False)
    ident = _pe.read_pe_identity(p)
    assert r["codeview"]["guid"] == ident.codeview.guid_str
    assert r["codeview"]["age"] == ident.codeview.age
    assert r["size_of_image"] == ident.size_of_image
    assert int(r["image_base"], 16) == ident.image_base
    assert r["time_date_stamp"] == ident.time_date_stamp
