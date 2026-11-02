#!/usr/bin/env python3
"""check_raw_pointer_inputs.py — rc2 B2 census: no ungated verb takes a raw
pointer from the client.

set_cb_selector_override shipped ungated and took three client-supplied
64-bit pointers that the engine later called through. The old census only
diffed the CK3_BRIDGE_DEBUG_VERBS block against a pinned list, so a NEW
ungated verb of the same class reddened nothing. This walks the handlers.

  1. The release verb set comes from the code: action_registry.inc and
     dispatch_router.cpp with every `#ifdef CK3_BRIDGE_DEBUG_VERBS` block
     removed. Every `do_*` handler they name is a release handler.
  2. Each handler's body is found in bridge/src (plus the bodies of static
     helpers it calls in the same file, one hop). A handler that cannot be
     found FAILS: an unread site is not a clean one.
  3. A body fails on:
       * a client key that names a pointer: "*_ref", "*_ptr", "*addr",
         "address", "pointer", "vtable", "va";
       * a value read from the request (json_get_*, Get*(), strtoull, ...),
         carried through casts and arithmetic but not through other calls,
         reaching reinterpret_cast<T*>, a C cast to a pointer, or a raw
         memory reader (is_mapped, safe_read*, read_ptr, process_vm_readv,
         memcpy).

    check_raw_pointer_inputs.py <bridge-dir>      exit 0 clean, 1 findings
    check_raw_pointer_inputs.py --selftest        controls: known-bad MUST flag
    check_raw_pointer_inputs.py --include-debug <bridge-dir>
                                                  control: MUST fail (debug verbs seen)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cpp_strip  # noqa: E402  (bridge/scripts/cpp_strip.py: the one C++ tokenizer)

POINTER_KEY = re.compile(r'"([a-z0-9_]*(?:_ref|_ptr|addr|address|pointer|vtable)|va|ptr|ref)"')
READERS = re.compile(r"\b(json_get_\w+|json_int\w*|parse_u64\w*|strto\w+|std::sto\w+|ato\w+)\s*\(|"
                     r"\.Get(?:Uint64|Int64|Uint|Int|String)\s*\(")
TRANSPARENT = {"static_cast", "reinterpret_cast", "uint64_t", "uintptr_t", "int64_t",
               "parse_u64_str", "strtoull", "strtoul", "strtoll", "strtol"}
SINKS = re.compile(r"\b(is_mapped|safe_read\w*|read_ptr|read_u64|chase_ptr|process_vm_readv|"
                   r"memcpy)\s*\(\s*(?:[^,()]*?\b)?(\w+)\b")


def strip_comments(src: str) -> str:
    """Comments blanked, string and char literals and newlines kept (cpp_strip)."""
    return cpp_strip.strip(src, keep_strings=True)


def drop_debug_blocks(text: str) -> str:
    """Remove `#ifdef CK3_BRIDGE_DEBUG_VERBS ... #else/#endif` (nesting-aware);
    keep the #else arm. `#ifndef CK3_BRIDGE_DEBUG_VERBS` arms are KEPT."""
    out, stack = [], []          # stack of (is_debug, in_else)
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
        dead = any(d and not e for d, e in stack)
        out.append("\n" if dead else line)
    return "".join(out)


def body_at(text: str, brace: int) -> str:
    """The brace-matched block at text[brace] (cpp_strip.match_close)."""
    end = cpp_strip.match_close(text, brace)
    return text[brace:] if end < 0 else text[brace:end + 1]


def definitions(files: dict[str, str]) -> dict[str, tuple[str, str]]:
    """name -> (file, body) for every function definition in the release text."""
    defs: dict[str, tuple[str, str]] = {}
    pat = re.compile(r"(?m)^[\w:<>,\s\*&]*?\b(\w+)\s*\(([^;{}]*)\)\s*(?:const\s*)?\{")
    for rel, text in files.items():
        for m in pat.finditer(text):
            name = m.group(1)
            if name in ("if", "for", "while", "switch", "return", "catch", "sizeof"):
                continue
            defs.setdefault(name, (rel, body_at(text, m.end() - 1)))
    return defs


TYPE_WORDS = {"const", "volatile", "unsigned", "signed", "int", "long", "char", "auto",
              "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t",
              "int32_t", "int64_t", "uintptr_t", "intptr_t", "size_t", "void", "bool",
              "nullptr", "true", "false", "static_cast", "reinterpret_cast"}


def _operands(rhs: str) -> set[str]:
    """Identifiers in `rhs` that are variables: not calls, members, types,
    namespaced or ALL_CAPS constants, or the inside of a reader call."""
    rhs = re.sub(r'"(?:\\.|[^"\\])*"', '""', rhs)
    out = set()
    for m in re.finditer(r"(?<![\w.>:])([A-Za-z_]\w*)\b(?!\s*(?:\(|::))", rhs):
        w = m.group(1)
        if w in TYPE_WORDS or w.isupper() or re.match(r"k[A-Z]", w) or w == "params_json":
            continue
        out.add(w)
    return out


def tainted_vars(body: str) -> set[str]:
    """Variables holding a value that comes ONLY from the request: a reader
    call, or casts/arithmetic over such values and constants. A value that
    mixes in an engine variable (base + client index) or passes through any
    other call (a lookup by id) is not a client pointer."""
    tainted: set[str] = set()
    assigns = re.findall(r"\b(\w+)\s*(?:\+|\||-)?=(?!=)\s*([^;]*);", body)
    for _ in range(4):
        for var, rhs in assigns:
            if var in tainted:
                continue
            calls = set(re.findall(r"([\w:]+)\s*\(", rhs)) - TRANSPARENT
            readers = {c for c in calls if READERS.search(c + "(")}
            if readers:
                if not (calls - readers):
                    tainted.add(var)
                continue
            if calls:
                continue
            ops = _operands(rhs)
            if ops and ops <= tainted:
                tainted.add(var)
    return tainted


def scan_body(name: str, body: str) -> list[str]:
    bad = []
    for m in POINTER_KEY.finditer(body):
        before = body[max(0, m.start() - 60):m.start()]
        if re.search(r"(json_get_\w+|json_has_key|action_args|action_has|FindMember|HasMember|\[)\s*\(?[^;]*$",
                     before):
            bad.append(f"{name}: reads client key {m.group(0)}, which names a pointer")
    t = tainted_vars(body)
    for v in sorted(t):
        vv = re.escape(v)
        if re.search(r"reinterpret_cast<[^>]*\*\s*>\s*\(\s*" + vv + r"\b", body) or \
           re.search(r"\(\s*(?:const\s+)?(?:volatile\s+)?\w+\s*\*+\s*\)\s*\(?\s*" + vv + r"\b", body):
            bad.append(f"{name}: client value `{v}` is cast to a pointer")
        for s in SINKS.finditer(body):
            if s.group(2) == v:
                bad.append(f"{name}: client value `{v}` reaches {s.group(1)}()")
    return bad


def release_handlers(bridge: Path) -> set[str]:
    reg = drop_debug_blocks(strip_comments((bridge / "src/action_registry.inc").read_text()))
    names = set(re.findall(r"BRIDGE_ACTION\(\s*\"[^\"]+\"\s*,\s*(\w+)\s*\)", reg))
    router = drop_debug_blocks(strip_comments((bridge / "src/dispatch_router.cpp").read_text()))
    names |= set(re.findall(r"\b(do_\w+)\s*\(", router))
    return names


def census(bridge: Path) -> tuple[list[str], int]:
    files = {}
    unparsed = []
    for p in sorted((bridge / "src").glob("*.cpp")):
        try:
            files[p.name] = drop_debug_blocks(strip_comments(p.read_text(errors="replace")))
        except cpp_strip.CppParseError as e:      # fail OPEN
            unparsed.append(f"{p.name}: cannot be tokenized ({e}); nothing in it was checked")
    defs = definitions(files)
    handlers = release_handlers(bridge)
    if len(handlers) < 100:
        return [f"only {len(handlers)} release handlers found; the parse is broken"], 0
    bad: list[str] = list(unparsed)
    for h in sorted(handlers):
        if h not in defs:
            bad.append(f"{h}: release handler with no definition found (cannot be checked)")
            continue
        rel, body = defs[h]
        bodies = [(h, body)]
        for callee in set(re.findall(r"\b(\w+)\s*\(", body)):
            if callee != h and callee in defs and defs[callee][0] == rel \
                    and re.search(r"(?m)^\s*static\b[^;{]*\b" + re.escape(callee) + r"\s*\(",
                                  files[rel]):
                bodies.append((f"{h} -> {callee}", defs[callee][1]))
        for label, b in bodies:
            bad += [f"{rel}: {x}" for x in scan_body(label, b)]
    return sorted(set(bad)), len(handlers)


SELFTEST_BAD = {
    "pointer key": 'bool do_x(const char* p, std::string* o) { const char* s = json_get_string(p, "cb_ref"); return s; }',
    "cast": 'bool do_x(const char* p, std::string* o) { uint64_t a = json_get_int(p, "n"); auto* q = reinterpret_cast<int*>(a); return q; }',
    "two hop": 'bool do_x(const char* p, std::string* o) { const char* s = json_get_string(p, "n"); uint64_t a = strtoull(s, nullptr, 16); uintptr_t b = a + 8; return is_mapped(b); }',
    "c cast": 'bool do_x(const char* p, std::string* o) { uint64_t a = json_get_int(p, "n"); void* q = (void*)a; return q; }',
    "address key": 'bool do_x(const char* p, std::string* o) { int a = json_get_int(p, "address"); return a; }',
    # L13 FF3 (witness X4): the scan stays in phase across a raw string and a
    # comment with an apostrophe. The escaped-looking-quote row is flagged by the
    # pre-FF3 checker too (its quotes pair; non-discriminating); the bare-quote
    # row (a later "/*" then reads as a comment that swallows the handler) is
    # the one the pre-FF3 checker misses.
    "pointer key after a raw string": ('static const char* k = R"x(\\")x";\nint b = 1; // it\'s\n'
        'bool do_x(const char* p, std::string* o) { const char* s = json_get_string(p, "cb_ref"); return s; }'),
    "pointer key after a raw string with a bare quote": ('static const char* k = R"x(")x"; const char* u = "/*";\n'
        'bool do_x(const char* p, std::string* o) { const char* s = json_get_string(p, "cb_ref"); return s; }\n'
        'const char* v = "*/";'),
}
SELFTEST_GOOD = {
    "id lookup": 'bool do_x(const char* p, std::string* o) { uint32_t id = json_get_int(p, "character_id"); void* ch = get_character_by_id(id); return is_mapped(reinterpret_cast<uintptr_t>(ch)); }',
    "output key": 'bool do_x(const char* p, std::string* o) { r.key("cb_ref"); return true; }',
}


def selftest() -> int:
    fails = 0
    for k, src in SELFTEST_BAD.items():
        defs = definitions({"x.cpp": strip_comments(src)})
        b = scan_body("do_x", defs["do_x"][1]) if "do_x" in defs else []   # unseen is a miss
        print(f"{'ok  ' if b else 'FAIL'} control flags {k}")
        fails += not b
    for k, src in SELFTEST_GOOD.items():
        defs = definitions({"x.cpp": strip_comments(src)})
        b = scan_body("do_x", defs["do_x"][1])
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
    bad, n = census(Path(argv[1]))
    for b in bad:
        print("FAIL", b)
    print(f"{'FAILED' if bad else 'ok'}: {n} release handlers walked, {len(bad)} finding(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
