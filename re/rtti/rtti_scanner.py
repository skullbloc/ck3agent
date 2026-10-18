#!/usr/bin/env python3
"""
Scan CK3 ELF binary for RTTI typeinfo structs and vtables.
Output: CSV of vtable_addr,typeinfo_addr,class_name

Itanium ABI layout:
  vtable:    [offset_to_top (8B), typeinfo_ptr (8B), vfunc0, vfunc1, ...]
  typeinfo:  [ti_class_vtable_ptr (8B), name_ptr (8B), ...]
  name:      mangled type name string starting with digit

Strategy: scan .data.rel.ro for 8-byte pointers into .rodata that point to
strings starting with a digit (mangled RTTI name). Those are the name_ptr
field at typeinfo+8. Then find vtables that reference each typeinfo.

Usage:
  python3 re/rtti/rtti_scanner.py <path/to/ck3> > rtti_vtables.csv
  CK3_BINARY=<path/to/ck3> python3 re/rtti/rtti_scanner.py > rtti_vtables.csv

Addresses are Ghidra VAs: section VA + --image-base (0x100000, Ghidra's
default for this PIE ELF). Subtract it for a ck3_offsets.h offset.
ELF (Linux, Itanium ABI) only; Mach-O and MSVC RTTI need other parsers.
"""

import argparse
import os
import struct
import sys

IMAGE_BASE = 0x100000

def read_elf_sections(data):
    """Parse ELF section headers to find key sections."""
    # ELF64 header
    e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x3A)[0]
    e_shnum = struct.unpack_from('<H', data, 0x3C)[0]
    e_shstrndx = struct.unpack_from('<H', data, 0x3E)[0]

    # Read section name string table
    shstr_off = struct.unpack_from('<Q', data, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]
    shstr_size = struct.unpack_from('<Q', data, e_shoff + e_shstrndx * e_shentsize + 0x20)[0]

    sections = {}
    for i in range(e_shnum):
        sh = e_shoff + i * e_shentsize
        name_idx = struct.unpack_from('<I', data, sh)[0]
        # Read name from string table
        name_end = data.index(b'\x00', shstr_off + name_idx)
        name = data[shstr_off + name_idx:name_end].decode('ascii')

        sh_addr = struct.unpack_from('<Q', data, sh + 0x10)[0]
        sh_offset = struct.unpack_from('<Q', data, sh + 0x18)[0]
        sh_size = struct.unpack_from('<Q', data, sh + 0x20)[0]

        sections[name] = {
            'addr': sh_addr + IMAGE_BASE,  # Virtual address with image base
            'offset': sh_offset,
            'size': sh_size,
        }

    return sections


def is_valid_mangled_name(s):
    """Check if string looks like a mangled RTTI type name."""
    if not s or not s[0].isdigit():
        return False
    # Extract length prefix
    i = 0
    while i < len(s) and s[i].isdigit():
        i += 1
    if i == 0 or i > 5:
        return False
    length = int(s[:i])
    if length <= 0 or length > 200:
        return False
    if len(s) < i + length:
        return False
    return True


def demangle_simple(mangled):
    """Simple demangling of Itanium ABI type names."""
    if mangled.startswith('N'):
        # Nested name
        return mangled

    parts = []
    i = 0
    while i < len(mangled):
        if not mangled[i].isdigit():
            break
        num_start = i
        while i < len(mangled) and mangled[i].isdigit():
            i += 1
        length = int(mangled[num_start:i])
        if i + length > len(mangled):
            break
        parts.append(mangled[i:i+length])
        i += length

    return '::'.join(parts) if parts else mangled


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description="Scan an ELF game binary for Itanium RTTI typeinfo and "
                    "vtables; CSV on stdout, progress on stderr.")
    ap.add_argument("binary", nargs="?", default=os.environ.get("CK3_BINARY"),
                    help="path to the ELF binary (default: $CK3_BINARY)")
    ap.add_argument("--image-base", type=lambda v: int(v, 0), default=IMAGE_BASE,
                    help="added to section VAs (default: 0x100000)")
    args = ap.parse_args(argv)
    if not args.binary:
        ap.error("no binary: pass a path or set CK3_BINARY")
    return args


def main(argv=None):
    global IMAGE_BASE
    args = parse_args(argv)
    IMAGE_BASE = args.image_base
    print("Reading binary...", file=sys.stderr)
    with open(args.binary, 'rb') as f:
        data = f.read()
    if data[:4] != b'\x7fELF':
        print("ERROR: not an ELF file: %s" % args.binary, file=sys.stderr)
        return 2

    print(f"Binary size: {len(data)} bytes", file=sys.stderr)

    sections = read_elf_sections(data)

    rodata = sections.get('.rodata')
    data_rel_ro = sections.get('.data.rel.ro')

    if not rodata or not data_rel_ro:
        print("ERROR: Missing required sections", file=sys.stderr)
        return 2

    print(f".rodata: VA=0x{rodata['addr']:x}, offset=0x{rodata['offset']:x}, size={rodata['size']}", file=sys.stderr)
    print(f".data.rel.ro: VA=0x{data_rel_ro['addr']:x}, offset=0x{data_rel_ro['offset']:x}, size={data_rel_ro['size']}", file=sys.stderr)

    ro_va_start = rodata['addr']
    ro_va_end = rodata['addr'] + rodata['size']
    ro_file_start = rodata['offset']

    drr_va_start = data_rel_ro['addr']
    drr_va_end = data_rel_ro['addr'] + data_rel_ro['size']
    drr_file_start = data_rel_ro['offset']
    drr_size = data_rel_ro['size']

    # Phase 1: Find typeinfo structs
    # Scan .data.rel.ro for pointers into .rodata that point to digit-starting strings
    print("Phase 1: Scanning for typeinfo structs...", file=sys.stderr)

    typeinfo_map = {}  # VA -> class_name

    for offset in range(0, drr_size - 16, 8):
        # Read potential name pointer (at typeinfo+8)
        file_pos = drr_file_start + offset
        val = struct.unpack_from('<Q', data, file_pos)[0]

        # Check if it points into .rodata
        if val < ro_va_start or val >= ro_va_end:
            continue

        # Read the string at that address
        str_file_pos = ro_file_start + (val - ro_va_start)
        if str_file_pos >= len(data):
            continue

        first_byte = data[str_file_pos]
        if first_byte < ord('0') or first_byte > ord('9'):
            continue

        # Read the string
        end = data.index(b'\x00', str_file_pos)
        name_str = data[str_file_pos:end].decode('ascii', errors='replace')

        if not is_valid_mangled_name(name_str):
            continue

        # This is at typeinfo+8. Check typeinfo+0
        # The ti vtable ptr may be 0 in the file (resolved by relocations at runtime)
        # or may point to __class_type_info vtable in libstdc++ (external)
        # or may point into .data.rel.ro (for __si_class_type_info / __vmi_class_type_info)
        # So we just require offset >= 8 and that the name is valid
        if offset < 8:
            continue

        typeinfo_va = drr_va_start + offset - 8
        class_name = demangle_simple(name_str)
        typeinfo_map[typeinfo_va] = class_name

    print(f"Found {len(typeinfo_map)} typeinfo structs", file=sys.stderr)

    # Phase 2: Find vtables referencing typeinfo
    print("Phase 2: Scanning for vtables...", file=sys.stderr)

    vtables = []

    for offset in range(8, drr_size - 8, 8):
        file_pos = drr_file_start + offset
        val = struct.unpack_from('<Q', data, file_pos)[0]

        if val not in typeinfo_map:
            continue

        class_name = typeinfo_map[val]

        # This is the typeinfo pointer in a vtable
        # vtable layout: [offset_to_top, typeinfo_ptr, vfunc0, ...]
        # So vtable_start = this_addr - 8
        # Conventional label at vfunc0 = this_addr + 8

        vtable_start_va = drr_va_start + offset - 8
        vtable_vfunc_va = drr_va_start + offset + 8

        # Check offset_to_top
        offset_to_top = struct.unpack_from('<q', data, file_pos - 8)[0]  # signed

        suffix = "" if offset_to_top == 0 else f"_off{offset_to_top}"

        vtables.append((vtable_vfunc_va, vtable_start_va, val, class_name, offset_to_top))

    print(f"Found {len(vtables)} vtable entries", file=sys.stderr)

    # Output CSV
    print("vtable_vfunc_va,vtable_start_va,typeinfo_va,class_name,offset_to_top")
    for vfunc_va, start_va, ti_va, name, ott in sorted(vtables, key=lambda x: x[0]):
        print(f"0x{vfunc_va:x},0x{start_va:x},0x{ti_va:x},{name},{ott}")

    # Stats
    unique_classes = set(name for _, _, _, name, _ in vtables)
    primary_vtables = sum(1 for _, _, _, _, ott in vtables if ott == 0)
    print(f"\nUnique classes: {len(unique_classes)}", file=sys.stderr)
    print(f"Primary vtables (offset_to_top=0): {primary_vtables}", file=sys.stderr)
    print(f"Secondary vtables: {len(vtables) - primary_vtables}", file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
