#!/usr/bin/env python3
"""check_no_va_output.py — rc2 B2 census: no ungated code puts a process
virtual address in what a client receives.

A VA in a reply defeats ASLR, and handed the addresses a raw-pointer sink
needs. This walks bridge/src and bridge/include (comments stripped, every
`#ifdef CK3_BRIDGE_DEBUG_VERBS` block removed: debug builds may keep them)
and fails on:

  R1  a reply key that names an address: ResponseBuilder .key("...") or a raw
      `\\"...\\":` in a string literal, matching *_ptr, *_addr, address,
      pointer, vtable, *_va (a base-relative *_rva is fine);
  R2  a `%p` conversion in a string literal outside a log call (fprintf,
      *LOG*, wlog, ...): such a string ends up in a reply (console_output,
      a reason, a _diag field);
  R3  snprintf/format of a hex conversion whose arguments include
      reinterpret_cast<uintptr_t> or a variable named like a pointer
      (*ptr*, *addr*, mgr, lobby, idef, entry, tpl, g_base*), outside a log call;
  R4  value_uint/value_int/value_hex(...) of reinterpret_cast<> or a *ptr*/*addr*
      variable.

Logs (bridge.log, 0600 in the private dir) may carry addresses; replies may not.

    check_no_va_output.py <bridge-dir>     exit 0 clean, 1 findings
    check_no_va_output.py --selftest       controls: known-bad text MUST flag
    check_no_va_output.py --include-debug <bridge-dir>
                                           control: MUST fail (debug-only VAs seen)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ADDR_KEY = re.compile(r"(?:^|_)(?:ptr|addr|address|pointer|vtable)(?:_|$)|(?:^|_)va$")
LOGGER = re.compile(r"^(?:fprintf|dprintf|vfprintf|perror|syslog|\w*LOG\w*|\w*[lL]og\w*)$")

#: Log-only formatters: they build strings with addresses for the fault log.
#: Exempt by path, and ONLY while every caller is in the listed files, so a
#: new reply-side caller is not covered by the exemption.
LOG_ONLY_HELPERS = {
    "include/fault_site.h": (("format_fault_site", "format_fault_regs"),
                             {"src/bridge_init.cpp", "include/fault_site.h"}),
}
PTR_ARG = re.compile(r"reinterpret_cast<\s*(?:uintptr_t|unsigned long|uint64_t)\s*>|"
                     r"\b\w*(?:ptr|addr)\w*\b|\b(?:mgr|lobby|idef|entry|tpl|g_base\w*)\b")
HEX_CONV = re.compile(r"%#?0?\d*(?:l|ll|j|z)?[xX]")


def strip_comments(src: str) -> str:
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            seg = src[i:n if j < 0 else j + 2]
            out.append("\n" * seg.count("\n"))
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def drop_debug_blocks(text: str) -> str:
    out, stack = [], []
    for line in text.splitlines(keepends=True):
        s = line.strip()
        if re.match(r"#\s*if(n?def)?\b", s):
            stack.append([bool(re.match(r"#\s*ifdef\s+CK3_BRIDGE_DEBUG_VERBS\b", s)), False])
            out.append("\n")
            continue
        if re.match(r"#\s*else\b", s) and stack:
            stack[-1][1] = True
            out.append("\n")
            continue
        if re.match(r"#\s*endif\b", s) and stack:
            stack.pop()
            out.append("\n")
            continue
        out.append("\n" if any(d and not e for d, e in stack) else line)
    return "".join(out)


def literals(text: str):
    """(start, end, body) of every string literal."""
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == '"':
            if i >= 1 and text[i - 1] == "R":       # raw string R"(...)"
                k = text.find("(", i)
                delim = text[i + 1:k]
                end = text.find(")" + delim + '"', k)
                end = n if end < 0 else end + len(delim) + 2
                yield i, end, text[k + 1:end - len(delim) - 2]
                i = end
                continue
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            yield i, j + 1, text[i + 1:j]
            i = j + 1
            continue
        i += 1


def enclosing_call(text: str, pos: int) -> tuple[str, int]:
    """Name and '(' offset of the innermost call whose parens contain pos."""
    depth, i = 0, pos
    while i > 0:
        i -= 1
        c = text[i]
        if c == ")":
            depth += 1
        elif c == "(":
            if depth == 0:
                m = re.search(r"([\w:]+)\s*$", text[:i])
                return (m.group(1).split("::")[-1] if m else ""), i
            depth -= 1
        elif c in ";{}" and depth == 0:
            return "", -1
    return "", -1


def call_text(text: str, open_paren: int) -> str:
    depth, i = 0, open_paren
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren:i + 1]
        i += 1
    return text[open_paren:]


def scan(rel: str, src: str) -> list[str]:
    text = drop_debug_blocks(strip_comments(src))
    bad = []

    def line(p: int) -> int:
        return text.count("\n", 0, p) + 1

    for m in re.finditer(r"\.key\(\s*\"([^\"]+)\"\s*\)", text):
        if ADDR_KEY.search(m.group(1)) and not m.group(1).endswith("rva"):
            bad.append(f"{rel}:{line(m.start())}: R1 reply key \"{m.group(1)}\" names an address")
    for m in re.finditer(r"\.value_(?:u?int\w*|hex|double)\(\s*(?:\([^()]*\)\s*)?"
                         r"(reinterpret_cast<[^>]*>|[\w.>-]*(?:ptr|addr)\w*\s*\))", text):
        bad.append(f"{rel}:{line(m.start())}: R4 a pointer-named value emitted as a number")
    for start, _end, body in literals(text):
        raw = text[start - 1:start] == "R"
        keys = re.findall(r'"([A-Za-z_]\w*)"\s*:' if raw else r'\\"([A-Za-z_][\w]*)\\"\s*:', body)
        for k in keys:
            if ADDR_KEY.search(k) and not k.endswith("rva"):
                bad.append(f"{rel}:{line(start)}: R1 raw reply key \"{k}\" names an address")
        has_p = "%p" in body
        has_hex = bool(HEX_CONV.search(body))
        if not (has_p or has_hex):
            continue
        name, op = enclosing_call(text, start)
        if LOGGER.match(name or ""):
            continue
        if has_p:
            bad.append(f"{rel}:{line(start)}: R2 %p in a string built by {name or '?'}(), not a log")
        if has_hex and name in ("snprintf", "sprintf", "format", "vsnprintf") and op >= 0:
            args = call_text(text, op)
            if PTR_ARG.search(args.split(body, 1)[-1] if body in args else args):
                bad.append(f"{rel}:{line(start)}: R3 hex of a pointer-named value via {name}()")
    return bad


def census(bridge: Path) -> list[str]:
    files = sorted(p for d in ("src", "include") for p in (bridge / d).rglob("*")
                   if p.suffix in (".cpp", ".c", ".h", ".inc")
                   and "test" not in p.relative_to(bridge / d).parts[:-1])
    if len(files) < 20:
        return [f"{bridge}: only {len(files)} source files; wrong directory?"]
    bad = []
    texts = {p.relative_to(bridge).as_posix(): p.read_text(errors="replace") for p in files}
    for rel, src in texts.items():
        if rel in LOG_ONLY_HELPERS:
            fns, callers = LOG_ONLY_HELPERS[rel]
            for other, osrc in texts.items():
                code = strip_comments(osrc)
                for fn in fns:
                    if other not in callers and re.search(r"\b" + fn + r"\s*\(", code):
                        bad.append(f"{other}: calls log-only {fn}() from {rel}; "
                                   "its exemption does not cover this caller")
            continue
        bad += scan(rel, src)
    for rel in LOG_ONLY_HELPERS:
        if rel not in texts:
            bad.append(f"{rel}: exempted log-only helper missing")
    return bad


SELFTEST_BAD = {
    "key ptr": 'r.key("lobby_view_ptr"); r.value_string(hex);',
    "key addr": 'r.key("fault_addr"); r.value_string(b);',
    "raw key": 'o += "\\",\\"addr\\":\\"" + hex(a);',
    "%p in snprintf": 'snprintf(buf, sizeof buf, "template=%p", tpl);',
    "hex of ptr": 'snprintf(h, sizeof h, "0x%lx", reinterpret_cast<uintptr_t>(lobby));',
    "hex of mgr": 'snprintf(h, sizeof h, "0x%lx", mgr);',
    "number of ptr": 'r.key("x"); r.value_uint(reinterpret_cast<uintptr_t>(obj));',
    "raw string key": 'snprintf(b, sizeof b, R"({"error":"x","option_array_ptr":"%d"})", n);',
}
SELFTEST_GOOD = {
    "log %p": 'fprintf(g_logfile, "x=%p\\n", p);',
    "macro log": 'ADLOG("obj=%p tier=%d\\n", (void*)o, t);',
    "offset hex": 'snprintf(off, sizeof off, "0x%04x", s.offset);',
    "rva key": 'r.key("vtable_rva"); r.value_string(h);',
    "debug gated": '#ifdef CK3_BRIDGE_DEBUG_VERBS\nr.key("war_ptr");\n#endif',
    "comment": '// r.key("entry_ptr");',
}


def selftest() -> int:
    fails = 0
    for k, src in SELFTEST_BAD.items():
        b = scan("x.cpp", src)
        print(f"{'ok  ' if b else 'FAIL'} control flags {k}")
        fails += not b
    for k, src in SELFTEST_GOOD.items():
        b = scan("x.cpp", src)
        print(f"{'ok  ' if not b else 'FAIL'} control passes {k} {b if b else ''}")
        fails += bool(b)
    return 1 if fails else 0


def main(argv: list[str]) -> int:
    global drop_debug_blocks
    if argv[1:2] == ["--selftest"]:
        return selftest()
    if argv[1:2] == ["--include-debug"]:
        # CONTROL: read the tree as a CK3_BRIDGE_DEBUG_VERBS build would. The
        # debug verbs are of exactly this class, so this MUST fail (ctest
        # WILL_FAIL): proof the census can see one.
        drop_debug_blocks = lambda t: t      # noqa: E731
        argv = argv[:1] + argv[2:]
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
