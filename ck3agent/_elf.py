"""Read the GNU BuildID note out of an ELF64 file. Stdlib only.

The bridge's offsets are valid for exactly one CK3 binary, identified by the
20-byte ``NT_GNU_BUILD_ID`` note. ``readelf -n`` prints it; this does the same
without requiring binutils on the user's machine.
"""
from __future__ import annotations

import struct
from pathlib import Path

NT_GNU_BUILD_ID = 3
SHT_NOTE = 7


class NotAnElf(ValueError):
    pass


def read_build_id(path: str | Path) -> str | None:
    """Return the BuildID as lowercase hex, or None if the file has none."""
    with open(path, "rb") as f:
        ident = f.read(16)
        if len(ident) < 16 or ident[:4] != b"\x7fELF":
            raise NotAnElf(f"{path}: not an ELF file")
        if ident[4] != 2:
            raise NotAnElf(f"{path}: not ELF64")
        if ident[5] != 1:
            raise NotAnElf(f"{path}: not little-endian")
        f.seek(0)
        hdr = f.read(64)
        (e_shoff,) = struct.unpack_from("<Q", hdr, 0x28)
        e_shentsize, e_shnum = struct.unpack_from("<HH", hdr, 0x3A)
        if e_shoff == 0 or e_shnum == 0:
            return None
        f.seek(e_shoff)
        shdrs = f.read(e_shentsize * e_shnum)
        for i in range(e_shnum):
            off = i * e_shentsize
            sh_type = struct.unpack_from("<I", shdrs, off + 4)[0]
            if sh_type != SHT_NOTE:
                continue
            sh_offset, sh_size = struct.unpack_from("<QQ", shdrs, off + 0x18)
            f.seek(sh_offset)
            data = f.read(sh_size)
            bid = _find_build_id_note(data)
            if bid:
                return bid
    return None


def _find_build_id_note(data: bytes) -> str | None:
    pos = 0
    while pos + 12 <= len(data):
        namesz, descsz, ntype = struct.unpack_from("<III", data, pos)
        pos += 12
        name = data[pos:pos + namesz]
        pos += (namesz + 3) & ~3
        desc = data[pos:pos + descsz]
        pos += (descsz + 3) & ~3
        if ntype == NT_GNU_BUILD_ID and name.rstrip(b"\0") == b"GNU":
            return desc.hex()
    return None
