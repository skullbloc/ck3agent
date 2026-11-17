#!/usr/bin/env python3
"""The clang-cl cross build standing in for cl.exe: no GNU-only token survives
preprocessing in a bridge source.

clang-cl accepts GNU extensions cl.exe rejects (__attribute__, __atomic_*
builtins, statement expressions, ...). The node builds the Windows targets with
clang-cl, so a GNU spelling in shared code compiled there and failed only on the
public windows-latest leg (1489 cl.exe errors on 2026.12.5). This script
re-runs every compile command in compile_commands.json with /E and scans the
preprocessed lines that come FROM a bridge file (system headers are skipped:
clang's own intrinsic headers use __attribute__ and are not what cl.exe reads)
for the GNU-only tokens below. Preprocessing first means #if branches the
Windows build does not take are not flagged, and macros (CK3_WEAK,
CK3_EARLY_INIT) are judged by what they expand to there. Statement expressions
and VLAs are not tokens; the same build makes them errors with -Werror flags
(bridge/CMakeLists.txt, CK3_MSVC_STANDIN_FLAGS).

Fails OPEN: a compile command it cannot preprocess, a compile database with no
bridge entries, or a missing database is an error, never a pass.

Usage: msvc_standin_lint.py --compdb <build>/compile_commands.json --root <bridge dir>
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

# The GNU-only tokens cl.exe has no meaning for. __builtin_offsetof and the
# va_* builtins are excluded: they are what the standard offsetof/va_start
# macros expand to under clang-cl, and cl.exe's own headers spell those macros
# with names it knows.
FORBIDDEN = re.compile(
    r"\b(?:__attribute__|__attribute|__extension__|__typeof__|__typeof|typeof|__thread"
    r"|__int128|__asm__|__PRETTY_FUNCTION__|__label__"
    r"|__atomic_\w+|__sync_\w+"
    r"|__builtin_(?!offsetof\b|va_start\b|va_end\b|va_arg\b|va_copy\b)\w+)\b")
# clang -E line markers: GNU `# 12 "file" 1` and MS `#line 12 "file"`.
MARKER = re.compile(r'^#(?:line)?\s*(\d+)\s+"((?:[^"\\]|\\.)*)"')
# String and character literals are text, not tokens: blanked before the scan.
LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"' + r"|'(?:[^'\\\n]|\\.)*'")
DROP_EXACT = {"/c", "-c", "/showIncludes", "-showIncludes", "--"}
DROP_PREFIX = ("/Fo", "-Fo", "/Fd", "-Fd", "/FS", "-FS")


def preprocess_cmd(entry: dict) -> list[str]:
    args = entry.get("arguments") or shlex.split(entry["command"], posix=True)
    out: list[str] = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-o", "/o"):
            skip = True
            continue
        if a in DROP_EXACT or a.startswith(DROP_PREFIX):
            continue
        out.append(a)
    src = entry["file"]
    if out and out[-1] == src:
        out.pop()
    return out + ["/E", "--", src]


def own(path: str, root: Path, base: str = "") -> bool:
    try:
        rel = Path(os.path.normpath(os.path.join(base, path))).resolve().relative_to(root)
    except (ValueError, OSError):
        return False
    return rel.parts[:1] != ("third_party",) and "_deps" not in rel.parts


def scan(entry: dict, root: Path) -> tuple[str, list[str], str | None]:
    cmd = preprocess_cmd(entry)
    p = subprocess.run(cmd, cwd=entry.get("directory") or None, capture_output=True,
                       text=True, errors="replace")
    if p.returncode != 0:
        return entry["file"], [], f"preprocess failed ({p.returncode}): {p.stderr.strip()[:400]}"
    hits: list[str] = []
    cur, line = "", 0
    mine = False
    for raw in p.stdout.splitlines():
        m = MARKER.match(raw)
        if m:
            line, cur = int(m.group(1)), m.group(2).replace("\\\\", "\\")
            cur = os.path.normpath(os.path.join(entry.get("directory") or "", cur))
            mine = own(cur, root)
            continue
        if mine:
            for t in FORBIDDEN.finditer(LITERAL.sub('""', raw)):
                hits.append(f"{Path(cur).resolve().relative_to(root).as_posix()}:{line}: {t.group(0)}")
        line += 1
    return entry["file"], hits, None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--compdb", required=True)
    ap.add_argument("--root", required=True, help="the bridge source directory")
    ap.add_argument("--stamp", help="file to touch on success")
    a = ap.parse_args()
    root = Path(a.root).resolve()
    db = Path(a.compdb)
    if not db.is_file():
        print(f"msvc_standin_lint: REFUSED: no {db} (configure with a Ninja or Makefile generator)",
              file=sys.stderr)
        return 2
    entries = [e for e in json.loads(db.read_text(encoding="utf-8"))
               if own(e["file"], root, e.get("directory") or "") and Path(e["file"]).suffix in (".cpp", ".cc", ".c")]
    if not entries:
        print(f"msvc_standin_lint: REFUSED: no bridge sources in {db}", file=sys.stderr)
        return 2
    seen: dict[str, None] = {}
    errors: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for src, hits, err in ex.map(lambda e: scan(e, root), entries):
            if err:
                errors.append(f"{src}: {err}")
            for h in hits:
                seen.setdefault(h, None)
    for h in seen:
        print(f"msvc_standin_lint: {h} (cl.exe has no such token)", file=sys.stderr)
    for e in errors:
        print(f"msvc_standin_lint: REFUSED: {e}", file=sys.stderr)
    if seen or errors:
        print(f"msvc_standin_lint: FAIL {len(seen)} GNU-only token site(s), {len(errors)} refusal(s) "
              f"over {len(entries)} translation units", file=sys.stderr)
        return 1
    print(f"msvc_standin_lint: ok, {len(entries)} translation units, no GNU-only token")
    if a.stamp:
        Path(a.stamp).write_text("ok\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
