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

A second pass reads clang's own AST of the same compile (-fsyntax-only
-Xclang -ast-dump) for a construct that is not a token: a reinterpret_cast from
an integer or enum to its own type. [expr.reinterpret.cast]/2 allows it and
clang-cl accepts it (cast kind NoOp); cl.exe refuses it, C2440 ("cannot
convert from 'uintptr_t' to 'uintptr_t'"), which was the one cl.exe error left
on 2026.12.10 (query_handlers.cpp, `reinterpret_cast<uintptr_t>(liege_ptr)` on a
uintptr_t). Flagged: a CXXReinterpretCastExpr with cast kind NoOp, a prvalue
(reference casts are not this case), whose type is not a pointer or a member
pointer, at a location in a bridge file. Location tracking follows clang's
dumper: a full path when the file changes, `line:` when the line does, `col:`
otherwise.

Fails OPEN: a compile command it cannot preprocess or dump, a compile database
with no bridge entries, or a missing database is an error, never a pass.

Usage: msvc_standin_lint.py --compdb <build>/compile_commands.json --root <bridge dir>
       msvc_standin_lint.py --selftest      the AST reader against canned dump text
"""
from __future__ import annotations

import argparse
import concurrent.futures
import functools
import json
import os
import re
import shlex
import subprocess
import tempfile
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
# The AST dumper's source locations: a path (absolute, or clang's own <built-in>
# style buffers) with line and column, `line:L:C`, or `col:C`. Quoted text (type
# names, which can carry "(lambda at /path:1:2)") is blanked before the scan.
AST_LOC = re.compile(
    r"(?<![\w.])(?:line:(\d+):\d+|col:\d+"
    r"|((?:/|[A-Za-z]:[\\/]|<(?:built-in|scratch space|command line)>)[^\s<>,']*?):(\d+):\d+)")
AST_QUOTED = re.compile(r"'(?:[^'\\]|\\.)*'")
# `CXXReinterpretCastExpr 0x.. <range> 'T'[:'desugared'] [lvalue|xvalue ]reinterpret_cast<T> <NoOp>`
AST_RCAST = re.compile(
    r"CXXReinterpretCastExpr 0x[0-9a-f]+ .*?'([^']*)'(?::'([^']*)')? (?:(lvalue|xvalue) )?"
    r"reinterpret_cast<.*> <NoOp>\s*$")
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


def same_type_casts(lines, root: Path, base: str = "") -> list[str]:
    """`file:line: reinterpret_cast<T>` for every same-type integer/enum
    reinterpret_cast an AST dump places in a bridge file."""
    hits: list[str] = []
    cur, line = "", 0
    for raw in lines:
        if "<" not in raw:
            continue
        text = AST_QUOTED.sub("''", raw)
        first: tuple[str, int] | None = None
        for m in AST_LOC.finditer(text):
            if m.group(2) is not None:
                cur, line = m.group(2), int(m.group(3))
            elif m.group(1) is not None:
                line = int(m.group(1))
            if first is None:
                first = (cur, line)
        if "CXXReinterpretCastExpr" not in raw or first is None:
            continue
        c = AST_RCAST.search(raw)
        if not c or c.group(3):
            continue
        ty = c.group(2) or c.group(1)
        if "*" in ty or "(" in ty or "&" in ty:
            continue
        path, ln = first
        if path.startswith("<") or not own(path, root, base):
            continue
        rel = Path(os.path.normpath(os.path.join(base, path))).resolve().relative_to(root).as_posix()
        hits.append(f"{rel}:{ln}: reinterpret_cast<{c.group(1)}> from its own type")
    return hits


def ast_cmd(entry: dict) -> list[str]:
    return preprocess_cmd(entry)[:-3] + ["-fsyntax-only", "-Xclang", "-ast-dump", "--", entry["file"]]


def scan_ast(entry: dict, root: Path) -> tuple[list[str], str | None]:
    return run_dump(ast_cmd(entry), entry.get("directory") or None, root)


def run_dump(cmd: list[str], cwd: str | None, root: Path) -> tuple[list[str], str | None]:
    # stderr to a file, not a second pipe: the dump is read off stdout to its end
    # before anything reads stderr, so a file whose warnings fill a stderr pipe
    # (64 KiB) blocked clang in its write and this in its read for good (lane
    # VREAD's gate: ai_eval_hook.cpp, runtime_resolver.cpp, snapshot.cpp, an hour).
    with tempfile.TemporaryFile(mode="w+", errors="replace") as ef:
        p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=ef, text=True, errors="replace")
        hits = same_type_casts(p.stdout, root, cwd or "")
        p.wait()
        ef.seek(0)
        err = ef.read()
    if p.returncode != 0:
        return [], f"AST dump failed ({p.returncode}): {err.strip()[:400]}"
    return hits, None


def scan(entry: dict, root: Path) -> tuple[str, list[str], str | None]:
    hits, err = scan_ast(entry, root)
    if err:
        return entry["file"], [], err
    tok_file, tok_hits, tok_err = scan_tokens(entry, root)
    return tok_file, hits + tok_hits, tok_err


def scan_tokens(entry: dict, root: Path) -> tuple[str, list[str], str | None]:
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
                hits.append(f"{Path(cur).resolve().relative_to(root).as_posix()}:{line}: {t.group(0)} "
                            "(cl.exe has no such token)")
        line += 1
    return entry["file"], hits, None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--compdb")
    ap.add_argument("--root", help="the bridge source directory")
    ap.add_argument("--stamp", help="file to touch on success")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not (a.compdb and a.root):
        ap.error("--compdb and --root are required")
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
    # Processes, not threads: reading the AST dumps is Python-bound.
    with concurrent.futures.ProcessPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for src, hits, err in ex.map(functools.partial(scan, root=root), entries):
            if err:
                errors.append(f"{src}: {err}")
            for h in hits:
                seen.setdefault(h, None)
    for h in seen:
        print(f"msvc_standin_lint: {h}", file=sys.stderr)
    for e in errors:
        print(f"msvc_standin_lint: REFUSED: {e}", file=sys.stderr)
    if seen or errors:
        print(f"msvc_standin_lint: FAIL {len(seen)} site(s) cl.exe rejects, {len(errors)} refusal(s) "
              f"over {len(entries)} translation units", file=sys.stderr)
        return 1
    print(f"msvc_standin_lint: ok, {len(entries)} translation units, no GNU-only token, "
          "no same-type reinterpret_cast")
    if a.stamp:
        Path(a.stamp).write_text("ok\n", encoding="utf-8")
    return 0


# Canned clang -ast-dump text (shapes copied from clang-cl's dump of
# query_handlers.cpp on 2026.12.10). The root is /r; /r/third_party and /sys
# are not bridge files.
_SELFTEST_DUMP = """\
TranslationUnitDecl 0x1 <<invalid sloc>> <invalid sloc>
|-FunctionDecl 0x2 </sys/include/atomic:10:1, line:20:1> line:10:6 f 'void ()'
| `-CXXReinterpretCastExpr 0x3 <line:12:16, col:50> '_TVal':'long' lvalue reinterpret_cast<_TVal &> <NoOp>
| `-CXXReinterpretCastExpr 0x4 <line:13:16, col:50> 'unsigned long long' reinterpret_cast<unsigned long long> <NoOp>
|-FunctionDecl 0x5 </r/src/q.cpp:100:1, line:200:1> line:100:6 g 'void ()'
| |-BinaryOperator 0x6 <line:150:21, col:62> 'uintptr_t':'unsigned long long' lvalue '='
| | `-CXXReinterpretCastExpr 0x7 <col:25, col:62> 'uintptr_t':'unsigned long long' reinterpret_cast<uintptr_t> <NoOp>
| |-CXXReinterpretCastExpr 0x8 <line:151:3, col:40> 'char *' reinterpret_cast<char *> <NoOp>
| |-CXXReinterpretCastExpr 0x9 <line:152:3, col:40> 'uintptr_t':'unsigned long long' reinterpret_cast<uintptr_t> <PointerToIntegral>
| |-LambdaExpr 0xa <line:153:3, col:9> '(lambda at /sys/x.h:1:1)'
| |-CXXReinterpretCastExpr 0xb <line:154:3, col:40> 'Kind':'Kind' reinterpret_cast<Kind> <NoOp>
| |-CXXReinterpretCastExpr 0xc <line:155:3, col:40> 'int' xvalue reinterpret_cast<int &&> <NoOp>
| `-CXXReinterpretCastExpr 0xd <line:156:3, col:40> 'void (*)(int)' reinterpret_cast<void (*)(int)> <NoOp>
|-FunctionDecl 0xe </r/third_party/z.h:5:1, col:9> col:6 h 'void ()'
| `-CXXReinterpretCastExpr 0xf <line:6:3, col:30> 'int' reinterpret_cast<int> <NoOp>
`-FunctionDecl 0x10 </r/include/m.h:7:1, line:9:1> line:7:6 k 'void ()'
  `-CXXReinterpretCastExpr 0x11 <line:8:3, col:30> 'int' reinterpret_cast<int> <NoOp>
"""
_SELFTEST_WANT = [
    "src/q.cpp:150: reinterpret_cast<uintptr_t> from its own type",
    "src/q.cpp:154: reinterpret_cast<Kind> from its own type",
    "include/m.h:8: reinterpret_cast<int> from its own type",
]


def selftest() -> int:
    """Controls that run on any host: the bad shapes must flag at the right
    file:line (a quoted lambda path must not move the file), and the good shapes
    (pointer, reference, real conversion, system and third_party headers) must
    not."""
    got = same_type_casts(_SELFTEST_DUMP.splitlines(), Path("/r"))
    if got != _SELFTEST_WANT:
        print(f"msvc_standin_lint selftest: FAIL\n got  {got}\n want {_SELFTEST_WANT}", file=sys.stderr)
        return 1
    # A dumper that writes more warnings than a pipe holds before its dump must
    # still be read to the end (run_dump; the gate hung an hour on three files).
    import threading
    fake = [sys.executable, "-c", "import sys; sys.stderr.write('w' * 300000); sys.stderr.flush(); "
            f"sys.stdout.write({_SELFTEST_DUMP!r})"]
    box: dict = {}
    t = threading.Thread(target=lambda: box.update(r=run_dump(fake, None, Path("/r"))), daemon=True)
    t.start()
    t.join(60)
    if t.is_alive() or box.get("r") != (_SELFTEST_WANT, None):
        print("msvc_standin_lint selftest: FAIL: a dumper with 300000 bytes of stderr "
              + ("never finished (pipe deadlock)" if t.is_alive() else f"answered {box.get('r')}"), file=sys.stderr)
        return 1
    print(f"msvc_standin_lint selftest: ok ({len(got)} flagged, 7 controls not flagged; "
          "a 300000-byte stderr read to the end)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
