#!/usr/bin/env python3
"""check_msvc_portable.py — no GNU-only construct in a file cl.exe compiles.

The public CI's "platform layer (windows-latest)" job builds the whole Windows
bridge with real MSVC (cl.exe): ck3_bridge_core_win (the Linux bridge sources
over the POSIX shim), ck3_os (the Windows cells), version.dll and their tests.
The node cross-builds the same targets with clang-cl, which ACCEPTS the GNU
spellings cl.exe rejects, so 2026.12.1-2026.12.9 built on the node and failed
there (1491 cl.exe errors on 2026.12.9: __attribute__ from CK3_EARLY_INIT and
weak/format attributes, the shim's __extension__ statement expression, the
__atomic_* builtins). This census stands in for cl.exe on a Linux host that has
none. Every GNU-only spelling a Windows-compiled file needs goes through ONE
header, bridge/include/ck3_compiler.h, which is the only file allowed to carry
them.

WHICH FILES: enumerated from the build, not from a name grep. bridge/CMakeLists.txt
is read from `set(CK3_PLATFORM` to the platform layer's `return()`, its if/elseif/
else branches are taken as a Windows configure takes them (CK3_PLATFORM windows,
MSVC true, every other condition UNKNOWN and so taken: fail open), and every
`src/...cpp` an add_library/add_executable there names is a root, with
${CK3_BRIDGE_SOURCES} (minus its REMOVE_ITEMs), ${CK3_OS_PORTABLE} and the
foreach expansions over CK3_WIN_CELLS / CK3_OS_PORTABLE_TESTS resolved. Every
`#include "..."` a root reaches in a branch cl.exe takes is followed (the shim
directory, include/, re/offsets/, the file's own directory).

WHAT IS LOOKED AT: code only (bridge/scripts/cpp_strip.py, the one tokenizer:
comments and literals blanked), and only in preprocessor branches a cl.exe
build can take: _WIN32/_WIN64/_MSC_VER are defined, __linux__/__GNUC__/
__clang__/__APPLE__/__unix__/_GNU_SOURCE/__MINGW32__ are not, and any other
condition is UNKNOWN, so BOTH its branches are scanned (fail open: a guess
reports more, never less).

WHAT IS FLAGGED (each a cl.exe error, or a GNU meaning cl.exe does not share):
  token     __attribute__, __extension__, __builtin_*, __atomic_*, __sync_*,
            __typeof__/typeof, __asm__/asm, __thread, __int128,
            __PRETTY_FUNCTION__, __label__, __restrict__, __inline__,
            __has_builtin, __has_attribute, __has_include_next
  include   #include_next; a POSIX/Linux header name the shim
            (src/os/windows/posix/) does not provide; a GCC-only header
            (x86intrin.h, cpuid.h, execinfo.h, cxxabi.h)
  shape     a statement expression `({ ...; })`; `?:` with the middle
            operand omitted; a case range `case a ... b:`; a zero-length
            member array `T name[0];`; a C++20 designated initializer
            `{ .x = ... }` (the bridge is C++17; cl.exe /std:c++17 is C7555)
Not seen (the clang-cl stand-in flags and msvc_standin_lint.py on the node
see them; cl.exe in the public CI is the authority): VLAs, two-phase lookup and
other front-end differences.

FAILS OPEN: an anchor missing from CMakeLists.txt, fewer roots than the floor,
a listed source or a quoted include that does not exist, or a file the
tokenizer cannot place is a red row, never a pass.

    check_msvc_portable.py <bridge-dir>     exit 0 clean, 1 findings
    check_msvc_portable.py --selftest       controls: known-bad text MUST flag,
                                            known-good text (comments, strings,
                                            Linux-only branches, the abstraction
                                            header) must not
    check_msvc_portable.py --list <bridge-dir>   print the files scanned
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cpp_strip  # noqa: E402  (bridge/scripts/cpp_strip.py: the one C++ tokenizer)

# The one file allowed to spell GNU-only constructs (inside its non-MSVC branch).
ABSTRACTION = "include/ck3_compiler.h"
# Fewer roots than this means the CMake reading broke, not that the tree shrank.
ROOT_FLOOR = 60

TOKEN = re.compile(
    r"(?<![\w$])(?:__attribute__|__attribute|__extension__|__typeof__|__typeof|typeof"
    r"|__asm__|__asm|asm|__thread|__int128|__PRETTY_FUNCTION__|__label__|__restrict__"
    r"|__inline__|__has_builtin|__has_attribute|__has_include_next"
    r"|__builtin_\w+|__atomic_\w+|__sync_\w+)(?![\w$])")
INCLUDE_NEXT = re.compile(r"^[ \t]*#[ \t]*include_next\b", re.M)
ELVIS = re.compile(r"\?\s*:(?!:)")
CASE_RANGE = re.compile(r"\bcase\b[^;:{}]*?\.\.\.[^;{}]*?:")
ZERO_ARRAY = re.compile(
    r"(?m)(?:^|[;{}])[ \t]*(?!(?:return|delete|case|throw|else|do|co_return|co_yield|sizeof|goto)\b)"
    r"(?:[A-Za-z_][\w:<>]*[ \t*&]+)+([A-Za-z_]\w*)[ \t]*\[[ \t]*0+[ \t]*\][ \t]*;")
DESIGNATOR = re.compile(r"[{,]\s*\.\s*[A-Za-z_]\w*\s*=(?!=)")
STMT_EXPR_OPEN = re.compile(r"\(\s*\{")

# POSIX/Linux header names: each must be provided by the shim directory.
# sys/ names the UCRT itself provides.
CRT_SYS = {"sys/stat.h", "sys/types.h", "sys/timeb.h", "sys/utime.h", "sys/locking.h"}
POSIX_HEADER = re.compile(
    r"^(?:unistd|pthread|dlfcn|dirent|poll|pwd|grp|spawn|ucontext|elf|link|sched|semaphore"
    r"|termios|syslog|libgen|fnmatch|glob|netdb|ifaddrs|strings|err|mntent|utmp|wordexp)\.h$"
    r"|^(?:sys|linux|asm|netinet|arpa|bits|net)/")
GCC_ONLY_HEADER = {"x86intrin.h", "cpuid.h", "execinfo.h", "cxxabi.h", "features.h", "malloc.h"}
# Generated at build time into the binary dir (CMakeLists.txt gen_provenance_win,
# gen_win_offsets); their generators write plain constants.
GENERATED = {"build_provenance.h"}

KNOWN_DEFINED = {"_WIN32": 1, "_WIN64": 1, "_MSC_VER": 1940, "_M_X64": 100, "_M_AMD64": 100,
                 "__cplusplus": 201703, "WIN32_LEAN_AND_MEAN": 1, "NOMINMAX": 1}
KNOWN_UNDEFINED = {"__linux__", "__linux", "linux", "__gnu_linux__", "__GNUC__", "__clang__",
                   "__APPLE__", "__MACH__", "__unix__", "__unix", "_GNU_SOURCE", "__MINGW32__",
                   "__MINGW64__", "__CYGWIN__", "__FreeBSD__", "__ELF__"}


class Refusal(Exception):
    """A fact the census cannot establish: reported as a red row."""


# ── the #if evaluator: True / False / None (unknown → both branches scanned) ──
def _eval_if(expr: str) -> bool | None:
    toks = re.findall(r"defined|\|\||&&|==|!=|<=|>=|[()!<>]|\d+[uUlL]*|[A-Za-z_]\w*|\S", expr)
    pos = 0

    def peek() -> str | None:
        return toks[pos] if pos < len(toks) else None

    def take() -> str:
        nonlocal pos
        pos += 1
        return toks[pos - 1]

    # values are ints or None (unknown); comparisons/logic propagate None (Kleene)
    def primary():
        t = take() if peek() is not None else None
        if t is None:
            raise ValueError("empty")
        if t == "(":
            v = lor()
            if take() != ")":
                raise ValueError("paren")
            return v
        if t == "!":
            v = primary()
            return None if v is None else int(not v)
        if t == "defined":
            if peek() == "(":
                take()
                name = take()
                if take() != ")":
                    raise ValueError("defined(")
            else:
                name = take()
            if name in KNOWN_DEFINED:
                return 1
            if name in KNOWN_UNDEFINED:
                return 0
            return None
        if re.fullmatch(r"\d+[uUlL]*", t):
            return int(re.sub(r"[uUlL]+$", "", t), 0) if not t.startswith("0") or t == "0" \
                else int(re.sub(r"[uUlL]+$", "", t), 8)
        if re.fullmatch(r"[A-Za-z_]\w*", t):
            if peek() == "(":           # a function-like macro (__has_include, ...): unknown
                depth = 0
                while peek() is not None:
                    x = take()
                    depth += x == "("
                    depth -= x == ")"
                    if depth == 0:
                        break
                return None
            if t in KNOWN_DEFINED:
                return KNOWN_DEFINED[t]
            if t in KNOWN_UNDEFINED:
                return 0
            return None
        raise ValueError(t)

    def cmp():
        v = primary()
        while peek() in ("==", "!=", "<", ">", "<=", ">="):
            op = take()
            w = primary()
            if v is None or w is None:
                v = None
            else:
                v = int({"==": v == w, "!=": v != w, "<": v < w, ">": v > w,
                         "<=": v <= w, ">=": v >= w}[op])
        return v

    def land():
        v = cmp()
        while peek() == "&&":
            take()
            w = cmp()
            v = 0 if (v == 0 or w == 0) else (None if (v is None or w is None) else 1)
        return v

    def lor():
        v = land()
        while peek() == "||":
            take()
            w = land()
            v = 1 if (v not in (0, None) or w not in (0, None)) else (None if (v is None or w is None) else 0)
        return v

    try:
        v = lor()
        if pos != len(toks):
            return None
    except (ValueError, IndexError):
        return None
    return None if v is None else bool(v)


def active_mask(code: str) -> list[bool]:
    """Per line: can a cl.exe build compile this line? (directive lines: True)"""
    lines = code.split("\n")
    mask: list[bool] = []
    # stack of (parent_active, a branch definitely taken, every branch so far definitely not)
    stack: list[tuple[bool, bool, bool]] = []
    cur = True
    i = 0
    while i < len(lines):
        line = lines[i]
        # join continuations for the directive
        full = line
        n = 1
        while full.endswith("\\") and i + n < len(lines):
            full = full[:-1] + " " + lines[i + n]
            n += 1
        m = re.match(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$", full)
        if m:
            kw, rest = m.group(1), m.group(2).strip()
            if kw in ("if", "ifdef", "ifndef"):
                if kw == "if":
                    st = _eval_if(rest)
                else:
                    name = rest.split()[0] if rest.split() else ""
                    st = (True if name in KNOWN_DEFINED else False if name in KNOWN_UNDEFINED
                          else None)
                    if kw == "ifndef" and st is not None:
                        st = not st
                stack.append((cur, st is True, st is False))
                cur = cur and st is not False
            elif kw == "elif":
                if not stack:
                    raise Refusal("#elif without #if")
                parent, taken, all_false = stack[-1]
                st = False if taken else _eval_if(rest)
                stack[-1] = (parent, taken or st is True, all_false and st is False)
                cur = parent and st is not False
            elif kw == "else":
                if not stack:
                    raise Refusal("#else without #if")
                parent, taken, all_false = stack[-1]
                st = False if taken else (True if all_false else None)
                stack[-1] = (parent, True, False)
                cur = parent and st is not False
            else:
                if not stack:
                    raise Refusal("#endif without #if")
                cur = stack.pop()[0]
            mask.extend([True] * n)
            i += n
            continue
        mask.extend([cur] * n)
        i += n
    if stack:
        raise Refusal("unterminated #if")
    return mask


def _line_starts(text: str) -> list[int]:
    out = [0]
    for k, ch in enumerate(text):
        if ch == "\n":
            out.append(k + 1)
    return out


def _lineno(starts: list[int], pos: int) -> int:
    lo, hi = 0, len(starts) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if starts[mid] <= pos:
            lo = mid
        else:
            hi = mid - 1
    return lo + 1


def _stmt_exprs(code: str) -> list[int]:
    """Positions of `({ ...; })`: a brace group directly inside parentheses
    holding a top-level `;` (a braced init list `f({a, b})` holds none)."""
    hits = []
    for m in STMT_EXPR_OPEN.finditer(code):
        b = m.end() - 1
        depth = 0
        k = b
        semi = False
        while k < len(code):
            c = code[k]
            if c in "{([":
                depth += 1
            elif c in "})]":
                depth -= 1
                if depth == 0:
                    break
            elif c == ";" and depth == 1:
                semi = True
            k += 1
        if semi and k < len(code) and code[k] == "}":
            after = code[k + 1:].lstrip()
            if after.startswith(")"):
                hits.append(m.start())
    return hits


def scan_text(rel: str, src: str) -> tuple[list[str], list[tuple[str, str]]]:
    """(findings, includes [(kind '"'|'<', name)]) for one file's text."""
    try:
        code = cpp_strip.strip(src)                         # literals and comments blanked
        incl_text = cpp_strip.strip(src, keep_strings=True)  # literals kept, for include names
    except cpp_strip.CppParseError as e:
        raise Refusal(f"{rel}: tokenizer cannot place a character: {e}") from e
    mask = active_mask(code)
    starts = _line_starts(code)
    out: list[str] = []
    includes: list[tuple[str, str]] = []

    def live(pos: int) -> bool:
        return mask[_lineno(starts, pos) - 1]

    def add(pos: int, what: str) -> None:
        out.append(f"{rel}:{_lineno(starts, pos)}: {what}")

    for ln, line in enumerate(incl_text.split("\n"), 1):
        if not mask[ln - 1]:
            continue
        m = re.match(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', line)
        if m:
            includes.append((m.group(1), m.group(2).strip()))
    if rel == ABSTRACTION:
        return out, includes
    for m in TOKEN.finditer(code):
        ls = code.rfind("\n", 0, m.start()) + 1
        if re.match(r"[ \t]*#[ \t]*include\b", code[ls:m.start()]):
            continue                      # a header name (<asm/prctl.h>), not a token
        if live(m.start()):
            add(m.start(), f"GNU-only token `{m.group(0)}`")
    for m in INCLUDE_NEXT.finditer(code):
        if live(m.start()):
            add(m.start(), "#include_next (cl.exe has none: C1021)")
    for kind, name in includes:
        base = name.rsplit("/", 1)[-1]
        if kind == "<" and base in GCC_ONLY_HEADER:
            out.append(f"{rel}: #include <{name}>: a GCC/glibc-only header")
    for pos in _stmt_exprs(code):
        if live(pos):
            add(pos, "statement expression `({ ...; })`")
    for m in ELVIS.finditer(code):
        if live(m.start()):
            add(m.start(), "`?:` with the middle operand omitted")
    for m in CASE_RANGE.finditer(code):
        if live(m.start()):
            add(m.start(), "case range `case a ... b:`")
    for m in ZERO_ARRAY.finditer(code):
        if live(m.start()):
            add(m.start(), f"zero-length array `{m.group(1)}[0]`")
    for m in DESIGNATOR.finditer(code):
        if live(m.start()):
            add(m.start(), "designated initializer (C++20; the bridge is C++17)")
    return out, includes


# ── which files: read from bridge/CMakeLists.txt ─────────────────────────────
def _cmake_code(text: str) -> str:
    out = []
    for line in text.split("\n"):
        s, q = [], False
        for ch in line:
            if ch == '"':
                q = not q
            if ch == "#" and not q:
                break
            s.append(ch)
        out.append("".join(s))
    return "\n".join(out)


def _cmake_calls(code: str):
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", code):
        i = m.end() - 1
        depth = 0
        k = i
        while k < len(code):
            if code[k] == "(":
                depth += 1
            elif code[k] == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        yield m.group(1).lower(), code[i + 1:k], m.start()


def _set_list(code: str, var: str) -> list[str]:
    for name, args, _ in _cmake_calls(code):
        a = args.split()
        if name == "set" and a and a[0] == var:
            return [x.strip('"') for x in a[1:]]
    raise Refusal(f"CMakeLists.txt: no set({var} ...)")


def windows_roots(bridge: Path) -> list[str]:
    text = (bridge / "CMakeLists.txt").read_text(encoding="utf-8")
    code = _cmake_code(text)
    a = code.find("set(CK3_PLATFORM ")
    b = code.find("return()", a)
    if a < 0 or b < 0:
        raise Refusal("CMakeLists.txt: the platform-layer anchors (set(CK3_PLATFORM, return()) are gone")
    sources = _set_list(code, "CK3_BRIDGE_SOURCES")
    portable = _set_list(code, "CK3_OS_PORTABLE")
    cells = _set_list(code, "CK3_WIN_CELLS")
    ptests = _set_list(code, "CK3_OS_PORTABLE_TESTS")
    region = code[a:b]
    # branch state per call position, a Windows configure's view of if/elseif/else
    events = []
    for m in re.finditer(r"\b(if|elseif|else|endif)\s*\(([^\n]*)", region):
        events.append((m.start(), m.group(1), m.group(2)))

    def cond(c: str) -> bool | None:
        c = c.strip().rstrip(")")
        m = re.fullmatch(r'(NOT\s+)?CK3_PLATFORM\s+STREQUAL\s+"(\w+)"', c.strip())
        if m:
            v = m.group(2) == "windows"
            return (not v) if m.group(1) else v
        if c.strip() in ("MSVC",):
            return True
        if c.strip() in ("NOT MSVC",):
            return False
        return None

    def active_at(pos: int) -> bool:
        return frame_at(pos)[0]

    def frame_at(pos: int) -> tuple[bool, list[tuple[bool, str]]]:
        """(active, the foreach-variable filters in force: (equal?, cell))."""
        stack: list[tuple[bool, bool, tuple[bool, str] | None]] = []   # (parent_active, taken, filter)
        cur = True
        for p, kw, c in events:
            if p >= pos:
                break
            f = re.fullmatch(r'\s*_c\s+STREQUAL\s+"(\w+)"\s*\)?\s*', c)
            if kw == "if":
                v = cond(c)
                stack.append((cur, v is True, (True, f.group(1)) if f else None))
                cur = cur and v is not False
            elif kw == "elseif":
                parent, taken, _ = stack[-1]
                v = False if taken else cond(c)
                stack[-1] = (parent, taken or v is True, None)
                cur = parent and v is not False
            elif kw == "else":
                parent, taken, flt = stack[-1]
                stack[-1] = (parent, True, (False, flt[1]) if flt else None)
                cur = parent and not taken
            else:
                cur = stack.pop()[0] if stack else True
        return cur, [s3[2] for s3 in stack if s3[2] is not None]

    removed: list[str] = []
    for name, args, pos in _cmake_calls(region):
        a2 = args.split()
        if name == "list" and len(a2) >= 3 and a2[0] == "REMOVE_ITEM" and a2[1] == "_core_srcs" \
                and active_at(pos):
            removed += a2[2:]
    roots: list[str] = []
    for name, args, pos in _cmake_calls(region):
        if name not in ("add_library", "add_executable") or not active_at(pos):
            continue
        for tok in args.split()[1:]:
            tok = tok.strip('"')
            if tok in ("${CK3_BRIDGE_SOURCES}", "${_core_srcs}"):
                roots += [s for s in sources if tok == "${CK3_BRIDGE_SOURCES}" or s not in removed]
            elif tok == "${CK3_OS_PORTABLE}":
                roots += portable
            elif tok == "${_cells}":
                roots += ["src/os/session_pass.cpp" if c == "session_pass" else f"src/os/windows/{c}.cpp"
                          for c in cells]
            elif tok.endswith((".cpp", ".cc", ".c")):
                if "${_c}" in tok:
                    flts = frame_at(pos)[1]
                    roots += [tok.replace("${_c}", c) for c in cells
                              if all((c == x) == eq for eq, x in flts)]
                elif "${_t}" in tok:
                    roots += [tok.replace("${_t}", t) for t in ptests]
                elif "${CK3_PLATFORM}" in tok:
                    roots.append(tok.replace("${CK3_PLATFORM}", "windows"))
                elif "${" in tok:
                    raise Refusal(f"CMakeLists.txt: cannot expand {tok}")
                else:
                    roots.append(tok)
    seen: dict[str, None] = {}
    for r in roots:
        seen.setdefault(r, None)
    return list(seen)


def census(bridge: Path) -> tuple[list[str], list[str]]:
    """(findings, scanned files)."""
    bad: list[str] = []
    try:
        roots = windows_roots(bridge)
    except Refusal as e:
        return [f"REFUSED: {e}"], []
    if len(roots) < ROOT_FLOOR:
        return [f"REFUSED: {len(roots)} Windows sources read from CMakeLists.txt, floor {ROOT_FLOOR}"], []
    inc_dirs = [bridge / "src/os/windows/posix", bridge / "include", bridge.parent / "re/offsets",
                bridge.parent / "re/offsets/windows", bridge / "src", bridge / "third_party"]
    shim = bridge / "src/os/windows/posix"
    todo = [bridge / r for r in roots]
    done: dict[Path, None] = {}
    for r, p in zip(roots, todo):
        if not p.is_file():
            bad.append(f"REFUSED: {r} is listed in CMakeLists.txt and does not exist")
    while todo:
        p = todo.pop()
        if p in done or not p.is_file():
            continue
        done[p] = None
        rel = p.relative_to(bridge).as_posix() if p.is_relative_to(bridge) else \
            "../" + p.relative_to(bridge.parent).as_posix()
        try:
            found, includes = scan_text(rel, p.read_text(encoding="utf-8", errors="replace"))
        except Refusal as e:
            bad.append(f"REFUSED: {rel}: {e}")
            continue
        bad += found
        for kind, name in includes:
            if kind == '"':
                cands = [p.parent / name] + [d / name for d in inc_dirs]
                hit = next((c for c in cands if c.is_file()), None)
                if hit is None:
                    if name.rsplit("/", 1)[-1] not in GENERATED:
                        bad.append(f'REFUSED: {rel}: #include "{name}" resolves to no file')
                    continue
                if "third_party" not in hit.resolve().parts:
                    todo.append(hit.resolve())
            else:
                if POSIX_HEADER.search(name) and name not in CRT_SYS:
                    if (shim / name).is_file():
                        todo.append((shim / name).resolve())
                    else:
                        bad.append(f"{rel}: #include <{name}>: a POSIX/Linux header the Windows "
                                   "shim (src/os/windows/posix/) does not provide")
    return bad, sorted(str(q.relative_to(bridge.parent)) for q in done)


# ── controls ─────────────────────────────────────────────────────────────────
BAD = {
    "attr": "static int x __attribute__((unused));\n",
    "early": "static const Foo g CK3_X __attribute__((init_priority(101)))(1);\n",
    "ext": "#define S(b) (__extension__({ int r = f(b); r; }))\n",
    "stmt": "int y = ({ int r = 1; r; });\n",
    "atomic": "v = __atomic_load_n(p, __ATOMIC_ACQUIRE);\n",
    "builtin": "if (__builtin_expect(x, 0)) {}\n",
    "sync": "__sync_synchronize();\n",
    "elvis": "const char* s = p ?: \"none\";\n",
    "case": "switch (c) { case 'a' ... 'z': break; }\n",
    "zero": "struct S { int n; char data[0]; };\n",
    "desig": "Opts o = { .flags = 1, .mode = 2 };\n",
    "next": "#include_next <string.h>\n",
    "typeof": "__typeof__(x) y = x;\n",
    "thread": "static __thread int t;\n",
    "asm": "asm volatile(\"pause\");\n",
    "pretty": "log(__PRETTY_FUNCTION__);\n",
    "unknown_branch": "#if SOME_FLAG\nint a __attribute__((weak));\n#endif\n",
    "msvc_else_unknown": "#if FOO\n#else\nvoid f() __attribute__((weak));\n#endif\n",
    "ifndef_msc": "#ifndef _MSC_VER\n#else\nint z __attribute__((weak));\n#endif\n",
}
GOOD = {
    "comment": "// __attribute__((weak)) is GNU; ({ x; }) too\nint a;\n",
    "block": "/* __atomic_load_n(p) and case 1 ... 2: */ int b;\n",
    "string": "const char* s = \"__attribute__ ({ x; }) ?: .x = 1\";\n",
    "raw": "auto r = R\"x(__builtin_trap() ({a;}))x\";\n",
    "linux_only": "#ifdef __linux__\nint a __attribute__((weak));\n#endif\n",
    "gnuc_only": "#if defined(__GNUC__) && !defined(_MSC_VER)\nint a __attribute__((weak));\n#endif\n",
    "not_win": "#ifndef _WIN32\nstatic int x __attribute__((unused));\n#else\nint y;\n#endif\n",
    "msvc_branch": "#if defined(_MSC_VER)\nint y;\n#else\nint x __attribute__((weak));\n#endif\n",
    "init_list": "f({1, 2}); v.push_back({a, b}); g({[] { return 1; }()});\n",
    "ternary": "int z = a ? b : c; auto q = p ? ::std::size_t(1) : 2;\n",
    "index": "x = a[0]; return b[0]; y[0] = 1;\n",
    "member_dot": "s.x = 1; f(a, .5); if (x == .5) {}\n",
    "lambda_arg": "run([&]() { a(); b(); });\n",
}


def selftest() -> int:
    fails = []
    for name, text in BAD.items():
        found, _ = scan_text(f"bad_{name}.cpp", text)
        if not found:
            fails.append(f"BAD row {name!r} was not flagged")
    for name, text in GOOD.items():
        found, _ = scan_text(f"good_{name}.cpp", text)
        if found:
            fails.append(f"GOOD row {name!r} was flagged: {found}")
    # the abstraction header is the one exemption
    found, _ = scan_text(ABSTRACTION, BAD["attr"])
    if found:
        fails.append("the abstraction header was flagged")
    # the #if evaluator's corners
    for expr, want in (("defined(_MSC_VER)", True), ("!defined(_WIN32)", False),
                       ("defined(__linux__) || defined(__APPLE__)", False),
                       ("defined(CK3_X)", None), ("defined(CK3_X) && defined(__linux__)", False),
                       ("defined(CK3_X) || defined(_WIN32)", True), ("_MSC_VER >= 1900", True),
                       ("__has_include(<x.h>)", None)):
        got = _eval_if(expr)
        if got is not want:
            fails.append(f"#if {expr}: {got}, want {want}")
    for f in fails:
        print(f"check_msvc_portable selftest: {f}", file=sys.stderr)
    print(f"check_msvc_portable selftest: {len(BAD)} bad rows, {len(GOOD)} good rows, "
          f"{len(fails)} failure(s)")
    return 1 if fails else 0


def main(argv: list[str]) -> int:
    if argv[1:2] == ["--selftest"]:
        return selftest()
    listing = argv[1:2] == ["--list"]
    args = argv[2:] if listing else argv[1:]
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    bad, files = census(Path(args[0]).resolve())
    if listing:
        print("\n".join(files))
    for b in bad:
        print(f"check_msvc_portable: {b}", file=sys.stderr)
    if bad:
        print(f"check_msvc_portable: FAIL {len(bad)} finding(s) over {len(files)} files", file=sys.stderr)
        return 1
    print(f"check_msvc_portable: ok, {len(files)} files a cl.exe build compiles, no GNU-only construct")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
