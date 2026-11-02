"""The Windows POSIX shim must not shadow CRT header names.

It used to: posix/string.h, time.h, signal.h, setjmp.h, sys/stat.h and sys/types.h
each did `#include_next <the CRT header>` and then added the POSIX names. clang-cl
(the cross build) accepts #include_next; cl.exe does not (C1021 on the public CI's
first real-MSVC run). The additions now reach every translation unit by
force-include (/FI ck3_posix.h, bridge/CMakeLists.txt). No MSVC runs here, so these
checks stand in for it: they red on the tree that broke cl.exe."""
from __future__ import annotations

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
if not (ROOT / "bridge").is_dir():
    pytest.skip("bridge/ is not part of this tree (sdist)", allow_module_level=True)
SHIM = ROOT / "bridge/src/os/windows/posix"

# The ISO C headers and the UCRT/Windows SDK headers a shim name could shadow.
CRT_HEADERS = {
    "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h", "inttypes.h", "iso646.h",
    "limits.h", "locale.h", "math.h", "setjmp.h", "signal.h", "stdalign.h", "stdarg.h",
    "stdbool.h", "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "stdnoreturn.h", "string.h",
    "tgmath.h", "threads.h", "time.h", "uchar.h", "wchar.h", "wctype.h",
    "conio.h", "corecrt.h", "direct.h", "dos.h", "fcntl.h", "io.h", "malloc.h", "mbctype.h",
    "memory.h", "process.h", "search.h", "share.h", "sys/locking.h", "sys/stat.h",
    "sys/timeb.h", "sys/types.h", "sys/utime.h", "windows.h", "winsock2.h", "ws2tcpip.h",
}
INCLUDE_NEXT = re.compile(r"^\s*#\s*include_next\b", re.M)


def test_shim_exists():
    assert (SHIM / "ck3_posix.h").is_file(), "bridge/src/os/windows/posix/ck3_posix.h is missing"


def test_no_source_uses_include_next():
    hits = []
    for p in sorted((ROOT / "bridge").rglob("*")):
        if p.suffix in {".h", ".hpp", ".c", ".cc", ".cpp", ".inc"} and p.is_file() \
                and "third_party" not in p.parts and "build" not in p.relative_to(ROOT).parts[:2]:
            if INCLUDE_NEXT.search(p.read_text(encoding="utf-8", errors="replace")):
                hits.append(p.relative_to(ROOT).as_posix())
    assert not hits, f"#include_next (cl.exe has none): {hits}"


def test_no_shim_header_shares_a_crt_name():
    shim = {p.relative_to(SHIM).as_posix() for p in SHIM.rglob("*.h")}
    assert shim, "no shim headers found"
    assert not sorted(shim & CRT_HEADERS)


def test_the_shim_is_force_included_where_its_directory_is_on_the_path():
    text = (ROOT / "bridge/CMakeLists.txt").read_text(encoding="utf-8")
    users = re.findall(r"target_include_directories\((\S+)\s+BEFORE\s+PRIVATE[^)]*src/os/windows/posix", text)
    assert users, "no target puts the shim directory on its include path"
    fi = re.findall(r"target_compile_options\((\S+)\s+PRIVATE\s+\$\{_ck3_posix_fi\}\)", text)
    assert set(users) <= set(fi), f"shim on the path but not force-included: {sorted(set(users) - set(fi))}"
    assert re.search(r"/FI\$\{CMAKE_CURRENT_SOURCE_DIR\}/src/os/windows/posix/ck3_posix\.h", text)
