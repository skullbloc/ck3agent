"""Read the build identity out of a PE (Windows) image file. Stdlib only.

The sibling of ``_elf.py`` for the Windows build (ADR 0001, "Binary identity
on the Python side"). The bridge's offsets fit exactly one ``ck3.exe``; the
bridge DLL identifies it by the CodeView ``RSDS`` record (PDB GUID + age) and
``SizeOfImage``, with the file SHA-256 as the fallback key
(``bridge/src/os/windows/identity_gate.h``). This module reads the same
fields from the file on disk:

* the CodeView RSDS record from the debug directory: GUID, age, PDB path;
* the file header ``TimeDateStamp``;
* the optional header ``SizeOfImage`` and ``ImageBase``.

``PeIdentity.codeview_key`` spells the record the way the bridge does
(``os::BinaryIdentity::observed_hex``, ``pe::codeview_hex``): the canonical
GUID digits lowercase without dashes, then the age as 8 hex digits, so a
Python reading and a bridge log line can be compared as strings.

Not wired into ``cmd_setup``/``cmd_doctor`` yet: that is the Windows
installer's phase. Nothing here hashes the file (``hashlib`` does that in one
line for the fallback key).
"""
from __future__ import annotations

import struct
import uuid
from dataclasses import dataclass
from pathlib import Path

IMAGE_DEBUG_TYPE_CODEVIEW = 2
PE32 = 0x10B
PE32_PLUS = 0x20B
MAX_DEBUG_ENTRIES = 64
MAX_PDB_PATH = 4096


class NotAPe(ValueError):
    pass


@dataclass(frozen=True)
class CodeView:
    guid: uuid.UUID
    age: int
    pdb_path: str

    @property
    def guid_str(self) -> str:
        """Upper-case dashed GUID, as tools/inspect_binary.py prints it."""
        return str(self.guid).upper()


@dataclass(frozen=True)
class PeIdentity:
    machine: int
    magic: int                # 0x20B PE32+, 0x10B PE32
    time_date_stamp: int      # file header, seconds since the epoch (0 or a hash when /Brepro)
    size_of_image: int
    image_base: int
    codeview: CodeView | None  # None: no readable RSDS record

    @property
    def codeview_key(self) -> str | None:
        """The bridge's spelling of the CodeView record, or None."""
        if self.codeview is None:
            return None
        return self.codeview.guid.hex + f"{self.codeview.age:08x}"


def _read(f, path, off: int, size: int, what: str) -> bytes:
    if off < 0:
        raise NotAPe(f"{path}: negative offset for {what}")
    f.seek(off)
    data = f.read(size)
    if len(data) != size:
        raise NotAPe(f"{path}: truncated ({what} at {off:#x}+{size:#x})")
    return data


def read_pe_identity(path: str | Path) -> PeIdentity:
    """Parse the identity fields; raise NotAPe on anything malformed."""
    with open(path, "rb") as f:
        f.seek(0, 2)
        fsize = f.tell()
        if fsize < 0x40 or _read(f, path, 0, 2, "DOS header") != b"MZ":
            raise NotAPe(f"{path}: not a PE file (no MZ header)")
        (e_lfanew,) = struct.unpack("<I", _read(f, path, 0x3C, 4, "e_lfanew"))
        if _read(f, path, e_lfanew, 4, "PE signature") != b"PE\0\0":
            raise NotAPe(f"{path}: no PE signature at {e_lfanew:#x}")
        fh = _read(f, path, e_lfanew + 4, 20, "file header")
        machine, nsect, stamp, _, _, opt_size, _ = struct.unpack("<HHIIIHH", fh)
        opt_off = e_lfanew + 24
        opt = _read(f, path, opt_off, opt_size, "optional header")
        if len(opt) < 2:
            raise NotAPe(f"{path}: no optional header")
        (magic,) = struct.unpack_from("<H", opt, 0)
        if magic == PE32_PLUS:
            if opt_size < 112:
                raise NotAPe(f"{path}: optional header too short ({opt_size})")
            (image_base,) = struct.unpack_from("<Q", opt, 24)
            ndir_off, dirs_off = 108, 112
        elif magic == PE32:
            if opt_size < 96:
                raise NotAPe(f"{path}: optional header too short ({opt_size})")
            (image_base,) = struct.unpack_from("<I", opt, 28)
            ndir_off, dirs_off = 92, 96
        else:
            raise NotAPe(f"{path}: unknown optional-header magic {magic:#x}")
        (size_of_image,) = struct.unpack_from("<I", opt, 56)
        (ndirs,) = struct.unpack_from("<I", opt, ndir_off)

        sects = []
        st = _read(f, path, opt_off + opt_size, 40 * nsect, "section table")
        for i in range(nsect):
            vsize, va, rsize, rptr = struct.unpack_from("<IIII", st, 40 * i + 8)
            sects.append((va, max(vsize, rsize), rptr, rsize))

        def rva_to_off(rva: int, n: int) -> int:
            for va, span, rptr, rsize in sects:
                if va <= rva and rva + n <= va + span:
                    o = rva - va
                    if o + n > rsize:
                        raise NotAPe(f"{path}: rva {rva:#x}+{n} is past the raw data of its section")
                    return rptr + o
            raise NotAPe(f"{path}: rva {rva:#x} outside every section")

        codeview = None
        if ndirs > 6 and dirs_off + 8 * 7 <= opt_size:
            dbg_rva, dbg_size = struct.unpack_from("<II", opt, dirs_off + 8 * 6)
            if dbg_rva and dbg_size:
                n = min(dbg_size // 28, MAX_DEBUG_ENTRIES)
                table = _read(f, path, rva_to_off(dbg_rva, 28 * n), 28 * n, "debug directory")
                for i in range(n):
                    (_, _, _, _, dtype, dsize, _, dptr) = struct.unpack_from("<IIHHIIII", table, 28 * i)
                    if dtype != IMAGE_DEBUG_TYPE_CODEVIEW or dsize < 24 or dptr == 0:
                        continue
                    if dptr + dsize > fsize:
                        raise NotAPe(f"{path}: CodeView record out of bounds")
                    rec = _read(f, path, dptr, min(dsize, 24 + MAX_PDB_PATH), "CodeView record")
                    if rec[:4] != b"RSDS":
                        continue
                    guid = uuid.UUID(bytes_le=rec[4:20])
                    (age,) = struct.unpack_from("<I", rec, 20)
                    pdb = rec[24:].split(b"\0", 1)[0].decode("utf-8", "replace")
                    codeview = CodeView(guid, age, pdb)
                    break
        return PeIdentity(machine, magic, stamp, size_of_image, image_base, codeview)
