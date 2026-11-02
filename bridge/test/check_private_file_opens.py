#!/usr/bin/env python3
"""check_private_file_opens.py — rc2 B1 census: every file the bridge writes
is opened without following symlinks, with an explicit private mode.

SECURITY.md says bridge files are 0600 and opened without following symlinks.
This walks bridge/src and bridge/include (comments stripped) and fails on:

  * fopen/freopen whose mode is not a literal read mode ("r", "rb"); write
    opens go through private_fopen / instance_fopen_private / instance_state_fopen;
  * open/openat/posix_spawn_file_actions_addopen with O_CREAT and no
    O_NOFOLLOW. A flags argument held in a variable is resolved from the
    assignments to that variable in the preceding 40 lines; one that cannot
    be resolved FAILS (fail open: an unreadable site is not a clean one);
  * creat(), std::ofstream, std::fstream.

The one allowed write-mode fopen is the host-target fallback in
include/private_fopen.h; the census also fails if that file stops containing
exactly one.

    check_private_file_opens.py <bridge-dir>     exit 0 clean, 1 findings
    check_private_file_opens.py --selftest       the control: known-bad text MUST be flagged
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cpp_strip  # noqa: E402  (bridge/scripts/cpp_strip.py: the one C++ tokenizer)

ALLOWED_FOPEN = {"include/private_fopen.h": 1}
CALL = re.compile(r"\b(fopen|freopen|open|openat|posix_spawn_file_actions_addopen|creat)\s*\(")
STREAM = re.compile(r"(?<![A-Za-z_])(?:std::)?(ofstream|fstream)\b")


def strip_comments(src: str) -> str:
    """Comments blanked, string and char literals and newlines kept (cpp_strip)."""
    return cpp_strip.strip(src, keep_strings=True)


def call_args(text: str, open_paren: int) -> list[str] | None:
    """Top-level comma-split arguments of the call whose '(' is at open_paren
    (cpp_strip.split_args: literals and comments cannot move the depth)."""
    return cpp_strip.split_args(text, open_paren)


def resolve_flags(text: str, pos: int, ident: str) -> str | None:
    """Text of every assignment to `ident` in the 40 lines before `pos`."""
    start = text.rfind("\n", 0, pos)
    for _ in range(40):
        start = text.rfind("\n", 0, start)
        if start < 0:
            start = 0
            break
    window = text[start:pos]
    hits = re.findall(r"\b" + re.escape(ident) + r"\s*(?:\|=|=)([^;]*);", window)
    return " ".join(hits) if hits else None


def scan(rel: str, src: str) -> tuple[list[str], int]:
    text = re.sub(r"(?m)^[ \t]*#[ \t]*include\b.*$", "", strip_comments(src))
    code = cpp_strip.strip(text)        # same offsets, literals blanked: calls in code only
    bad, fopens = [], 0
    for m in CALL.finditer(code):
        name = m.group(1)
        before = text[max(0, m.start() - 2):m.start()]
        if before.endswith(".") or before.endswith("->"):
            continue                                   # a member, not libc
        line = text.count("\n", 0, m.start()) + 1
        args = call_args(text, m.end() - 1)
        if args is None:
            bad.append(f"{rel}:{line}: {name}( unparsed")
            continue
        if name in ("fopen", "freopen"):
            fopens += 1
            mode = args[1] if len(args) > 1 else ""
            if re.fullmatch(r'"rb?"', mode):
                continue
            if rel in ALLOWED_FOPEN and name == "fopen":
                continue
            bad.append(f"{rel}:{line}: {name}(..., {mode or '?'}) is not a read; "
                       "use private_fopen / instance_fopen_private")
        elif name == "creat":
            bad.append(f"{rel}:{line}: creat() follows symlinks")
        else:
            idx = {"open": 1, "openat": 2, "posix_spawn_file_actions_addopen": 3}[name]
            if len(args) <= idx:
                continue                               # not the libc shape (a member decl)
            flags = args[idx]
            if re.fullmatch(r"[A-Za-z_]\w*", flags) and not flags.startswith("O_"):
                resolved = resolve_flags(text, m.start(), flags)
                if resolved is None:
                    bad.append(f"{rel}:{line}: {name}( flags `{flags}` not resolvable")
                    continue
                flags = flags + " " + resolved
            if "O_CREAT" in flags and "O_NOFOLLOW" not in flags:
                bad.append(f"{rel}:{line}: {name}( O_CREAT without O_NOFOLLOW")
    for m in STREAM.finditer(code):
        line = text.count("\n", 0, m.start()) + 1
        bad.append(f"{rel}:{line}: std::{m.group(1)} writes without O_NOFOLLOW")
    return bad, fopens


def census(bridge: Path) -> list[str]:
    bad: list[str] = []
    # Everything that can be linked into the .so; host test sources excluded.
    files = sorted(p for d in ("src", "include") for p in (bridge / d).rglob("*")
                   if p.suffix in (".cpp", ".c", ".h", ".inc", ".hpp")
                   and "test" not in p.relative_to(bridge / d).parts[:-1])
    if len(files) < 20:
        return [f"{bridge}: only {len(files)} source files found; wrong directory?"]
    for p in files:
        rel = p.relative_to(bridge).as_posix()
        try:
            b, fopens = scan(rel, p.read_text(encoding="utf-8", errors="replace"))
        except cpp_strip.CppParseError as e:      # fail OPEN
            bad.append(f"{rel}: cannot be tokenized ({e}); nothing in it was checked")
            continue
        bad += b
        if rel in ALLOWED_FOPEN and fopens != ALLOWED_FOPEN[rel]:
            bad.append(f"{rel}: expected exactly {ALLOWED_FOPEN[rel]} fopen, found {fopens}")
    for rel in ALLOWED_FOPEN:
        if not (bridge / rel).exists():
            bad.append(f"{rel}: allowed fallback file missing")
    return bad


SELFTEST_BAD = {
    "fopen append": 'FILE* f = ::fopen(p.c_str(), "a");',
    "fopen variable mode": "FILE* f = fopen(path, mode);",
    "open O_CREAT": "int fd = ::open(p, O_WRONLY | O_CREAT, 0644);",
    "openat via var": "int fl = O_WRONLY | O_CREAT;\nint fd = openat(d, n, fl, 0600);",
    "addopen": "posix_spawn_file_actions_addopen(&fa, 1, p, O_WRONLY|O_CREAT|O_TRUNC, 0644);",
    "unresolvable flags": "int fd = ::open(p, flags_from_elsewhere, 0600);",
    "ofstream": "std::ofstream o(path);",
    "creat": "int fd = creat(p, 0600);",
    "fstream": "std::fstream io(path, std::ios::out);",
    # L13 FF3 (witness X4): the scan stays in phase across a raw string and a
    # comment with an apostrophe. The witness's row (an escaped-looking quote) is
    # flagged by the pre-FF3 checker too: its quotes pair (non-discriminating);
    # the row after it (one bare quote inside the raw string, so a later "//"
    # reads as a comment) is the one the pre-FF3 checker misses.
    "fopen after a raw string": 'a = R"x(\\")x";\nint b = 1; // it\'s\nFILE* f = fopen("x","w");',
    "fopen after a raw string with a bare quote": 'a = R"x(")x"; b = "//"; FILE* f = fopen("x","w");',
}
SELFTEST_GOOD = {
    "fopen read": 'FILE* f = fopen("/proc/self/maps", "r");',
    "commented": '// FILE* f = fopen(p, "w");',
    "member open": "tickets.open(id, sid, rid, a, p, now);",
    "nofollow": "int fd = ::open(p, O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);",
    "ifstream": "std::ifstream f; f.open(path);",
    "include line": "#include <fstream>",
}


def selftest() -> int:
    fails = 0
    for k, src in SELFTEST_BAD.items():
        b, _ = scan("x.cpp", src)
        print(f"{'ok  ' if b else 'FAIL'} control flags {k}")
        fails += not b
    for k, src in SELFTEST_GOOD.items():
        b, _ = scan("x.cpp", src)
        print(f"{'ok  ' if not b else 'FAIL'} control passes {k} {b if b else ''}")
        fails += bool(b)
    return 1 if fails else 0


def main(argv: list[str]) -> int:
    if argv[1:2] == ["--selftest"]:
        return selftest()
    if len(argv) != 2:
        print(__doc__)
        return 2
    bad = census(Path(argv[1]))
    for b in bad:
        print("FAIL", b)
    print(f"{'FAILED' if bad else 'ok'}: {len(bad)} finding(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
