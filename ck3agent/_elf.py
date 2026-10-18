"""Read the GNU BuildID note out of an ELF64 file. Stdlib only.

The bridge's offsets fit exactly one CK3 binary, identified by its 20-byte
``NT_GNU_BUILD_ID`` note (what ``readelf -n`` prints). Section headers are
tried first; a stripped file falls back to the PT_NOTE program headers.
"""
from __future__ import annotations

import struct
from pathlib import Path

NT_GNU_BUILD_ID = 3
SHT_NOTE = 7
PT_NOTE = 4
MAX_NOTE = 1 << 20


class NotAnElf(ValueError):
    pass


def _read(f, path, off: int, size: int, what: str) -> bytes:
    f.seek(off)
    data = f.read(size)
    if len(data) != size:
        raise NotAnElf(f"{path}: truncated ({what} at {off:#x}+{size:#x})")
    return data


def read_build_id(path: str | Path) -> str | None:
    """Return the BuildID as lowercase hex, or None if the file has none."""
    with open(path, "rb") as f:
        f.seek(0, 2)
        fsize = f.tell()
        ident = _read(f, path, 0, 16, "ident") if fsize >= 16 else b""
        if ident[:4] != b"\x7fELF":
            raise NotAnElf(f"{path}: not an ELF file")
        if ident[4] != 2:
            raise NotAnElf(f"{path}: not ELF64")
        if ident[5] != 1:
            raise NotAnElf(f"{path}: not little-endian")
        hdr = _read(f, path, 0, 64, "ELF header")
        (e_phoff, e_shoff) = struct.unpack_from("<QQ", hdr, 0x20)
        e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from("<HHHH", hdr, 0x36)

        def table(off: int, entsize: int, num: int, minsize: int, what: str) -> bytes:
            if off == 0 or num == 0:
                return b""
            if entsize < minsize or off + entsize * num > fsize:
                raise NotAnElf(f"{path}: bad {what} table")
            return _read(f, path, off, entsize * num, what)

        def note(off: int, size: int) -> str | None:
            if size > MAX_NOTE or off + size > fsize:
                raise NotAnElf(f"{path}: note out of bounds")
            return _find_build_id_note(_read(f, path, off, size, "note"), path)

        shdrs = table(e_shoff, e_shentsize, e_shnum, 64, "section header")
        for i in range(e_shnum if shdrs else 0):
            base = i * e_shentsize
            if struct.unpack_from("<I", shdrs, base + 4)[0] != SHT_NOTE:
                continue
            bid = note(*struct.unpack_from("<QQ", shdrs, base + 0x18))
            if bid:
                return bid

        phdrs = table(e_phoff, e_phentsize, e_phnum, 56, "program header")
        for i in range(e_phnum if phdrs else 0):
            base = i * e_phentsize
            if struct.unpack_from("<I", phdrs, base)[0] != PT_NOTE:
                continue
            (p_offset,) = struct.unpack_from("<Q", phdrs, base + 8)
            (p_filesz,) = struct.unpack_from("<Q", phdrs, base + 0x20)
            bid = note(p_offset, p_filesz)
            if bid:
                return bid
    return None


def _find_build_id_note(data: bytes, path="") -> str | None:
    pos = 0
    while pos + 12 <= len(data):
        namesz, descsz, ntype = struct.unpack_from("<III", data, pos)
        pos += 12
        name_end = pos + namesz
        desc_off = pos + ((namesz + 3) & ~3)
        desc_end = desc_off + descsz
        if desc_end > len(data):
            raise NotAnElf(f"{path}: note entry out of bounds")
        if ntype == NT_GNU_BUILD_ID and data[pos:name_end].rstrip(b"\0") == b"GNU":
            return data[desc_off:desc_end].hex()
        pos = desc_off + ((descsz + 3) & ~3)
    return None
