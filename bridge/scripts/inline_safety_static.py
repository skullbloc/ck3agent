"""The static half of the inline-safety audit (ck-86ue.5; rewritten in cleanup
2026.11, D22; Tier 1 added in the final fix): every handler the IPC reader
thread serves inline reads game memory only through safe_read, and
ipc_server.cpp guards every inline dispatch with sigsetjmp.

Two tiers are served from the IPC reader's dispatch: Tier 1, the names in
snapshot.cpp's is_bridge_internal_query() (on the reader thread, in either
clock state, so also while the game's workers run), and Tier 2, the names in
is_game_state_query() (handed to the game-thread drain and waited for, in
either clock state; L13 D1). A read of a freed or forged pointer there is a
fault on that thread. Two layers stand between that and a crash:

  1. per handler: the reads go through safe_read<T> (process_vm_readv; an
     unmapped address returns T{} and bumps the fault flag), never through
     the raw read<T> / read_offset<T> dereference bridge.h also defines;
  2. per dispatch: ipc_server.cpp runs the handler inside
     `if (sigsetjmp(g_recovery_point, 1) == 0)` and answers
     {"error":"memory_access_fault"} on the recovery branch.

Nothing here is a hand list. The handlers are the names in snapshot.cpp's
is_bridge_internal_query() and is_game_state_query(), each resolved to its
function through
action_registry.inc (the table dispatch is generated from), and its body
found in whichever bridge source defines it: bridge/src/*.cpp, src/os/**
(its tests excepted) and the headers under bridge/include (L14 FB m-14; the
callees a closure follows are found there too). A name added to the
classifier is audited the moment it is added. Every gap FAILS and names the
handler: no registry line, no body found anywhere, a raw read. Nothing is
skipped.

What a "raw read" is, and the stated limit of what this parser sees:
  - `read<T>(...)` / `read_offset<T>(...)`: bridge.h's unguarded dereference;
  - a dereference of a cast address that is not the left side of a plain `=`
    (a write keeps its write semantics): `*reinterpret_cast<T*>(EXPR)` (T may
    carry one level of template arguments, `std::array<char, 8>*`), the
    C-style `*(T*)EXPR`, and the cast-then-index `reinterpret_cast<T*>(EXPR)[i]`.
    The one exclusion is a local byte buffer: EXPR led by an array the same
    body declares as `char`, `unsigned char`, `int8_t`, `uint8_t` or
    `std::byte` (`alignas(8) char result_storage[32]`) -- an array of any other
    type (an array of game pointers) is game memory and counts.
  NOT seen: a dereference through a pointer variable
  (`auto* p = reinterpret_cast<T*>(x); ... *p`), a `->` through a cast game
  object, a cast whose template arguments nest two levels deep, and a read
  deeper than CALL_DEPTH calls from every handler that reaches it. Writes are
  not audited at all; each inline write site is kept under the per-dispatch
  sigsetjmp guard (section B).

What a fault check is (section A, every handler whose path reads game memory
through safe_read): safe_read answers T{} on a fault, so a handler that does not
look at the fault flag passes a 0 off as a value. Its own body either brackets
its reads by name -- `safe_read_clear()` and `safe_read_had_fault()` (it answers
memory_access_fault, or says degraded) -- or declares a `FaultScope` (bridge/
include/action_dispatch_internal.h), whose constructor is the
safe_read_clear() and whose destructor is the safe_read_had_fault() check;
section F checks that the class still makes both calls. Two more rows read
where they stand (witness #10 R5):
  - placement: the bracket opens (the first `safe_read_clear()` or the
    `FaultScope` declaration) before the first safe-reading call in the body --
    a safe_read itself, or a call to a bridge function whose own path (to
    CALL_DEPTH - 1) reads through safe_read. A clear after a read wipes that
    read's fault; a scope after it never saw it.
  - use: every `safe_read_had_fault()` in the body is the whole condition of an
    `if` whose statement returns or calls emit_degraded, or that has an `else`
    (the check picks the branch a write sits in). A check followed by a write
    with no branch decides nothing. Any other spelling (the flag in a variable,
    negated, inside a larger condition) is a FAIL by name: this parser cannot
    decide it, and an undecided row is red. Stated limits (witness #10 R5, witness #11 W1, W2, W10): an
    `else` arm that itself writes after a fault is not looked into; a return the
    check reaches through a lambda it calls, a read hidden in a macro before the
    bracket opens, and an `emit_degraded` on a builder that is not the reply each
    read as the row intends and escape. The RNG-order row (`rng_before_its_check`,
    in the test module) orders every generate_rng() draw against the reads in
    bodies that bracket by name or by `FaultScope`; an unbracketed game-thread
    body is outside it.

Tier 3, the game-thread actions (L14): the names in is_game_thread_action().
The reader queues them to g_msg_queue and the drain (bridge_init.cpp
drain_game_thread_work_at) runs each queued message on the game thread, holding
the session's command mutex, inside ONE sigsetjmp of its own. THE RULE -- when
is the drain's guard enough, and when must the handler guard itself:

  The drain's sigsetjmp is outside the handler's frame, and siglongjmp does not
  unwind. A fault the drain catches skips every destructor and every restore
  between the faulting read and the drain: an armed fake random scope stays
  published in the engine's RandomScopeManager slot, an armed watchdog stays
  armed, a held RAII lock is left locked with no owner (the next taker blocks
  the game thread for good), and the handler's reply is never written (the
  drain records game_thread_crash_during_drain for a write; a game-thread read's
  caller gets nothing and times out). So the drain's guard is a CRASH NET, never
  an answer. And it covers a handler only UNTIL the handler's first guard of its
  own (L14 FB m-11): that sigsetjmp overwrites the thread's g_recovery_point,
  and the guard's end sets g_in_protected_region = false, so from then on a
  fault outside a guard is not recovered at all (the SIGSEGV handler recovers
  only inside a protected region); nothing restores the drain's jmp_buf. The
  net is therefore not relied on; these rows make a fault outside a handler's
  own armed span impossible by construction:
    - every read of game memory goes through safe_read, so a fault is a value
      the handler answers (row G-raw; never declarable: a raw read's fault is
      the drain's, with whatever is armed at that moment);
    - every engine call runs inside the handler's OWN armed span (row G-bound,
      L14 FB M-8): after a sigsetjmp that a watchdog_arm in the same body
      precedes, and before that guard's first watchdog_disarm() /
      `g_in_protected_region = false` outside its crash block (a disarm in a
      nested block that ends in `return` is an early exit and does not end the
      span); or every call of the helper that makes it lies in its caller's
      span. The handler's recovery branch, in the frame that armed the scope and
      the watchdog, restores both and answers;
    - the handler decides on its reads before it commits (row G-commit, writes
      only): an `if (safe_read_had_fault()) return` precedes the first engine
      contact (a scope arm, a watchdog arm, an engine call, an intent enqueue, a
      store into game memory, or a callee that makes one), and no read is
      reachable between the LAST such decision and that contact (L14 FB M-7);
      a fault after it marks the reply degraded (FaultScope). The
      is_game_thread_read() names change nothing and bracket as a Tier-2 read
      does.
  The other rows are the Tier-1/2 rows (fault bracket, placement, decide, the
  early return on a character that did not resolve -- after the resolve, with
  no engine contact between the two: L14 FB m-14), no sleep or wait in the
  closure (the drain holds the session mutex; as A2), and no clock write: no
  clock_write_sites() and no call of clock_write in the closure (only the
  session moves the clock, L13 D4).
  A handler whose body is a guard in front of `return other(params_json, ...)`
  and reads nothing itself is graded by that function's body (a forwarder).
  DECLARED EXCEPTIONS live in ONE place, snapshot.cpp's is_game_thread_action,
  as `game-thread-ok <name>: <row>: <reason>` comment lines, <row> one of
  DECLARABLE_ROWS. A declaration excuses that one row of that one handler; one
  whose row does not red, whose handler is not a game-thread action, that does
  not parse, or that sits anywhere else in snapshot.cpp (L14 FB m-12), is
  itself a FAIL.

Sections:
  A  the handlers, per tier: registry line, body, zero raw reads in the body AND in
     every bridge function it reaches (call depth CALL_DEPTH), reads that go
     through safe_read somewhere on that path when the path reads game
     memory at all, a fault check in the handler's own body when that path
     reads through safe_read, and an early return on a character that did not
     resolve for every handler that resolves one
  B  the dispatch: each Tier-1 / Tier-2 branch of ipc_server.cpp calls the
     handler only inside a sigsetjmp guard whose recovery answers
     memory_access_fault
  C  the clock: every pause-flag / speed store and every clock-setter call in
     bridge/src and bridge/include sits in clock_write or its static
     primitives (event_handler.cpp). clock_write_sites() states the shapes it
     sees and the ones out of its reach.
  G  the game-thread actions (Tier 3), by THE RULE above; and three rows over
     the whole tree: G-scope (L14 S4), every body that arms the fake random
     scope under a sigsetjmp of its own arms it BEFORE that guard and restores
     it in the crash branch (siglongjmp does not unwind); G-nest (L14 S4), no
     function reachable from a registered handler (whole closure, every one of
     the REGISTRY_FLOOR registry lines) calls safe_read_clear() but the handler
     itself or stores the fault counter other than putting back a value it
     saved (L14 FB m-14), and FaultScope saves and restores the record it finds
     (so a nested handler's bracket wipes nothing); G-al (L14 FB m-1), no call
     through a function pointer whose return type is wider than a byte is
     converted to bool where it stands without `& 0xFF` (an engine yes/no is a
     C++ bool in AL; engine_bool.h)
  F  file-wide: query_handlers.cpp has zero raw reads; the memory_access_fault
     reply is one string (bridge.h MEMORY_ACCESS_FAULT_REPLY), answered in
     ipc_server.cpp at a stable few sites; FaultScope makes both calls

Used by bridge/scripts/test_inline_safety.py (the CLI, which adds the
core-dump sections) and ck3agent/server/test_inline_safety_static.py (the
`mise run test` gate). Ships in the public export: no host path here.
"""
from __future__ import annotations

import functools
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cpp_strip  # noqa: E402  (bridge/scripts/cpp_strip.py: the one C++ tokenizer)

#: How deep the audit follows bridge-defined callees from a handler.
CALL_DEPTH = 3

#: The raw read primitives bridge.h defines: a plain dereference of the
#: address, with no mapping check. safe_read<T> / safe_read_offset<T> are the
#: guarded forms.
RAW_READ_RE = re.compile(r"(?<![A-Za-z0-9_])(?:read|read_offset)\s*<")
#: A dereference of a cast address: `*reinterpret_cast<T*>(` (T with at most one
#: level of template arguments). deref_reads() decides whether the match is a
#: read (see the module docstring).
_CAST_T = r"<(?:[^;{}<>()]|<[^;{}<>()]*>)*\*\s*>"
DEREF_CAST_RE = re.compile(r"\*\s*reinterpret_cast\s*" + _CAST_T + r"\s*\(")
#: The C-style form: `*(T*)EXPR` / `*(const T*)(EXPR)`.
DEREF_CSTYLE_RE = re.compile(
    r"\*\s*\(\s*(?:const\s+|volatile\s+)*[A-Za-z_][\w:]*(?:\s*<[^;{}<>()]*>)?\s*\*+\s*\)\s*")
#: The cast-then-index form: `reinterpret_cast<T*>(EXPR)[`.
CAST_INDEX_RE = re.compile(r"(?<![*\w])reinterpret_cast\s*" + _CAST_T + r"\s*\(")
#: A local byte buffer: the one array a cast dereference may read (not game memory).
LOCAL_BYTE_BUFFER = r"(?:\bchar|\bunsigned\s+char|\bu?int8_t|\bstd::byte)\s+{name}\s*\["
#: The fault check a handler makes by name (see the module docstring).
FAULT_PAIR = (re.compile(r"\bsafe_read_clear\s*\(\s*\)"), re.compile(r"\bsafe_read_had_fault\s*\(\s*\)"))
FAULT_SCOPE_RE = re.compile(r"\bFaultScope\s+[A-Za-z_]\w*\s*[({]")
SAFE_READ_RE = re.compile(r"(?<![A-Za-z0-9_])(?:safe_read|safe_read_offset|safe_read_bytes)\s*[<(]")
#: Resolving a character from the request; a handler that does must return
#: early when it does not resolve.
CHAR_RESOLVE_RE = re.compile(r"\b(?:resolve_character|lookup_character)\s*\(")
CHAR_EARLY_RETURN_RE = re.compile(r"\bis_sentinel\s*\(|\bactor_refusal\s*\(|character_not_found|!\s*character\b")

_DEF_RE = re.compile(
    r"^[ \t]*(?:[A-Za-z_][\w:<>,\*&\s]*?[\s\*&])?([A-Za-z_]\w*)\s*"
    r"\(([^;{}()]*(?:\([^()]*\)[^;{}()]*)*)\)\s*(?:const\s*)?(?:noexcept\s*)?\{",
    re.MULTILINE)
_KEYWORDS = {"if", "for", "while", "switch", "return", "catch", "sizeof", "defined", "else"}
_CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


# ── source text ──────────────────────────────────────────────────────────────

def strip_code(src: str, keep_strings: bool = False) -> str:
    """The C++ source, same length and line for line, with every comment
    blanked and every string / char literal's contents blanked, or kept
    verbatim with keep_strings: cpp_strip.strip, the one tokenizer (L13 FF3).
    A source it cannot place raises cpp_strip.CppParseError: a red, never a
    pass. Two strippings of one file have the same offsets, so a block found in
    the blanked text can be read in the kept one."""
    return cpp_strip.strip(src, keep_strings=keep_strings)


def _span_at(src: str, i: int) -> tuple[int, int] | None:
    """The [start, end) span of the brace-matched block whose `{` is at src[i]."""
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return i, j + 1
    return None


def _block_at(src: str, i: int) -> str | None:
    """The brace-matched block whose `{` is at src[i]."""
    sp = _span_at(src, i)
    return src[sp[0]:sp[1]] if sp else None


@functools.lru_cache(maxsize=4096)
def _definitions(text: str) -> tuple[tuple[str, str, bool, int], ...]:
    """Every function definition in one stripped file: (name, body, is_static,
    body start), at any indentation (L14: a definition inside an indented
    `namespace {` block -- action_handlers.cpp's evaluate_build -- was not
    matched, so every call into it was dropped from the closure, unaudited).
    Cached by content: a mutant re-parses only the file it changed (the suite
    builds many trees that differ in one file)."""
    out = []
    for m in _DEF_RE.finditer(text):
        name = m.group(1)
        if name in _KEYWORDS:
            continue
        body = _block_at(text, m.end() - 1)
        if body:
            is_static = bool(re.match(r"\s*(?:inline\s+)?static\b", m.group(0)))
            out.append((name, body, is_static, m.end() - 1))
    return tuple(out)


class Bridge:
    """The bridge source tree, read once: stripped text per file and every
    top-level function body by name."""

    def __init__(self, bridge_dir: Path):
        self.dir = Path(bridge_dir)
        self.src_dir = self.dir / "src"
        #: every bridge source a handler's path can reach (L14 FB m-14): the
        #: top-level src/*.cpp by name, src/os/** (not its tests) as
        #: "os/<path>", and the headers as "../include/<path>" (src_dir /
        #: key is the file either way). A function defined inline in a header
        #: or in an os adapter was in no closure before.
        self.files = {}
        for p in sorted(self.src_dir.glob("*.cpp")):
            self.files[p.name] = strip_code(p.read_text(errors="replace"))
        for p in sorted(self.src_dir.glob("os/**/*.cpp")) + sorted(self.dir.glob("include/**/*.h")):
            rel = p.relative_to(self.src_dir) if p.is_relative_to(self.src_dir) else Path("..") / p.relative_to(self.dir)
            if rel.parts[:2] == ("os", "test"):
                continue
            self.files[str(rel)] = strip_code(p.read_text(errors="replace"))
        #: name -> [(file, body, is_static)], every top-level definition
        self.functions: dict[str, list[tuple[str, str, bool]]] = {}
        #: (file, name) -> the [start, end) of its body, the same in every stripping
        self.spans: dict[tuple[str, str], tuple[int, int]] = {}
        for fname, text in self.files.items():
            for name, body, is_static, start in _definitions(text):
                self.functions.setdefault(name, []).append((fname, body, is_static))
                self.spans.setdefault((fname, name), (start, start + len(body)))

    def resolve(self, name: str, from_file: str | None) -> list[tuple[str, str]]:
        """The definitions a call to `name` from `from_file` can reach: the
        caller's own file's definition when it has one (a static helper shadows
        a same-named function elsewhere), else every non-static definition."""
        defs = self.functions.get(name, [])
        local = [(f, b) for f, b, _ in defs if f == from_file]
        return local or [(f, b) for f, b, st in defs if not st]

    def body_with_strings(self, name: str, from_file: str | None = None) -> str | None:
        """The body of `name` with its string literals kept (comments still
        blanked), for checks on the keys a handler writes."""
        for f, _ in self.resolve(name, from_file):
            sp = self.spans.get((f, name))
            if sp:
                return self.text(f, keep_strings=True)[sp[0]:sp[1]]
        return None

    def text(self, name: str, keep_strings: bool = False) -> str:
        p = self.src_dir / name
        return strip_code(p.read_text(errors="replace"), keep_strings) if p.exists() else ""

    def classifier_names(self, classifier: str = "is_game_state_query") -> list[str]:
        """The names `classifier` (one of TIERS) lists in snapshot.cpp
        (comments stripped first: a quoted phrase in a comment is not a
        name)."""
        snap = self.text("snapshot.cpp", keep_strings=True)
        m = re.search(r"bool\s+" + classifier + r"\s*\([^)]*\)\s*\{", snap)
        block = (_block_at(snap, m.end() - 1) or "") if m else ""
        return re.findall(r'"([^"]+)"', block)

    def registry(self) -> dict[str, str]:
        """action name -> handler function, from action_registry.inc (every
        line, whatever #ifdef it sits under)."""
        text = self.text("action_registry.inc", keep_strings=True)
        return dict(re.findall(r'BRIDGE_ACTION\(\s*"([^"]+)"\s*,\s*(\w+)\s*\)', text))

    def closure(self, fn: str, home: str | None = "dispatch_router.cpp",
                depth: int = CALL_DEPTH) -> list[tuple[str, str, str]]:
        """(file, name, body) for the handler `fn` (as the dispatch chain in
        `home` reaches it) and every bridge-defined function it calls, to
        `depth` calls."""
        start = [(f, fn, b) for f, b in self.resolve(fn, home)]
        seen = {(f, n) for f, n, _ in start}
        out = list(start)
        frontier = start
        for _ in range(depth):
            nxt = []
            for f, _, body in frontier:
                for c in _CALL_RE.findall(body):
                    for cf, cb in self.resolve(c, f):
                        if (cf, c) not in seen:
                            seen.add((cf, c))
                            nxt.append((cf, c, cb))
            out.extend(nxt)
            frontier = nxt
        return out


def _paren_end(src: str, i: int) -> int:
    """The index just past the `)` matching the `(` at src[i] (len(src) when
    unmatched)."""
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "(":
            depth += 1
        elif src[j] == ")":
            depth -= 1
            if depth == 0:
                return j + 1
    return len(src)


def _local_byte_buffer(body: str, expr: str) -> bool:
    """EXPR is led by an array the body declares with a byte element type."""
    lead = re.match(r"\s*\(*\s*([A-Za-z_]\w*)", expr)
    return bool(lead) and bool(re.search(LOCAL_BYTE_BUFFER.format(name=re.escape(lead.group(1))), body))


def _is_write(body: str, end: int) -> bool:
    return bool(re.match(r"\s*=(?!=)", body[end:]))


def deref_reads(body: str) -> int:
    """Cast-dereference reads in body (the three forms in the module
    docstring): not the target of a plain `=`, and not of a local byte buffer."""
    n = 0
    for m in DEREF_CAST_RE.finditer(body):
        end = _paren_end(body, m.end() - 1)
        if _is_write(body, end) or _local_byte_buffer(body, body[m.end():end]):
            continue
        n += 1
    for m in DEREF_CSTYLE_RE.finditer(body):
        if body[m.end():m.end() + 1] == "(":
            end = _paren_end(body, m.end())
            expr = body[m.end() + 1:end]
        else:
            t = re.match(r"[A-Za-z_][\w:.\->]*(?:\s*\+\s*[\w:]+)*", body[m.end():])
            if not t:
                continue
            end, expr = m.end() + t.end(), t.group(0)
        if _is_write(body, end) or _local_byte_buffer(body, expr):
            continue
        n += 1
    for m in CAST_INDEX_RE.finditer(body):
        end = _paren_end(body, m.end() - 1)
        if not re.match(r"\s*\[", body[end:]):
            continue                       # not indexed (a plain cast, or `*` handled above)
        close = body.find("]", end)
        if close >= 0 and _is_write(body, close + 1):
            continue
        if _local_byte_buffer(body, body[m.end():end]):
            continue
        n += 1
    return n


def _first(regexes, body: str) -> int | None:
    pos = [m.start() for r in regexes for m in [r.search(body)] if m]
    return min(pos) if pos else None


def fault_bracket_misplaced(b: "Bridge", file: str, body: str) -> str | None:
    """None when the body's fault bracket (the first safe_read_clear() or the
    FaultScope declaration) opens before its first safe-reading call; else the
    call it opens after (see the module docstring)."""
    opens = _first((FAULT_PAIR[0], FAULT_SCOPE_RE), body)
    if opens is None:
        return "no bracket"
    first = SAFE_READ_RE.search(body)
    if first and first.start() < opens:
        return body[first.start():first.end()].strip()
    for m in _CALL_RE.finditer(body[:opens]):
        c = m.group(1)
        if c in _KEYWORDS or not b.resolve(c, file):
            continue
        if any(safe_reads(cb) for _, _, cb in b.closure(c, home=file, depth=CALL_DEPTH - 1)):
            return c + "()"
    return None


_IF_TAIL_RE = re.compile(r"\bif\s*\(\s*$")
_HAD_FAULT_RE = re.compile(r"\bsafe_read_had_fault\s*\(\s*\)")


def undecided_fault_checks(body: str) -> list[str]:
    """Every safe_read_had_fault() in body that does not decide (see the module
    docstring), each as the text of its statement."""
    out = []
    for m in _HAD_FAULT_RE.finditer(body):
        close = re.match(r"\s*\)", body[m.end():])
        if not _IF_TAIL_RE.search(body[:m.start()]) or not close:
            out.append(body[max(0, m.start() - 40):m.end() + 20].strip())
            continue
        i = m.end() + close.end()
        j = len(body[i:]) - len(body[i:].lstrip())
        i += j
        if body[i:i + 1] == "{":
            stmt = _block_at(body, i) or ""
        else:
            depth, k = 0, i
            while k < len(body):
                ch = body[k]
                depth += ch in "({[" ; depth -= ch in ")}]"
                if ch == ";" and depth <= 0:
                    break
                k += 1
            stmt = body[i:k + 1]
        rest = body[i + len(stmt):]
        if (re.search(r"\breturn\b|\bemit_degraded\s*\(", stmt)
                or re.match(r"\s*else\b", rest)):
            continue
        out.append(("if (safe_read_had_fault()) " + stmt.strip())[:120])
    return out


def has_fault_check(body: str) -> bool:
    """The handler's own body brackets its reads by name: safe_read_clear() and
    safe_read_had_fault(), or a FaultScope."""
    return all(r.search(body) for r in FAULT_PAIR) or bool(FAULT_SCOPE_RE.search(body))


def raw_reads(body: str) -> int:
    return len(RAW_READ_RE.findall(body)) + deref_reads(body)


def safe_reads(body: str) -> int:
    return len(SAFE_READ_RE.findall(body))


# ── reporting ────────────────────────────────────────────────────────────────

class Report:
    """Rows as (ok, name, detail); printed as [OK] / [FAIL] lines."""

    def __init__(self, echo: bool = True):
        self.rows: list[tuple[bool, str, str]] = []
        self.echo = echo

    def section(self, title: str) -> None:
        if self.echo:
            print(f"\n## {title}")

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.rows.append((bool(ok), name, detail))
        if self.echo:
            suffix = f"  -- {detail}" if detail and not ok else ""
            print(f"  [{'OK' if ok else 'FAIL'}] {name}{suffix}")
        return bool(ok)

    @property
    def failures(self) -> list[tuple[bool, str, str]]:
        return [r for r in self.rows if not r[0]]


# ── A: the handlers ──────────────────────────────────────────────────────────

#: The classifiers whose names the reader's dispatch serves: Tier 1 (on the
#: reader thread) and Tier 2 (on the game thread, through the drain). The
#: game-thread ACTIONS (is_game_thread_action, queued) are Tier 3, section G.
TIERS = ("is_bridge_internal_query", "is_game_state_query")
GAME_THREAD_ACTIONS = "is_game_thread_action"
#: FAIL-OPEN FLOORS: each classifier's population when the floor was last
#: pinned. A parse that matched less would audit less and pass, so fewer names
#: than this reds. Re-pin, with the reason, when handlers move: 47 / 90 is the
#: tree after L13 FF1-B moved 15 handlers out of is_bridge_internal_query
#: (B1/B3: the deferred-slot submitters, get_active_wars, get_ai_personality,
#: pre_validate_war, query_council_tasks, query_debug_flags to Tier 2;
#: set_ai_personality to the game-thread actions) and FF2 moved two more
#: (R4: query_war_score_breakdown to Tier 2; R5: respond_event to the
#: game-thread actions).
#: L14: is_game_thread_action's 22 (the 20 of census D.3, set_ai_personality
#: from L13 FF1-B and respond_event from FF2); 23 since L14 S3 added
#: query_create_title_cost, a game-thread READ (snapshot.cpp is_game_thread_read:
#: the engine's IsValid runs scripted triggers). 47 / 90 did not move: S3 added
#: no bridge-internal or Tier-2 handler.
#: L16 (ruling R3): 46 / 88 / 22. The event handlers nothing called went with
#: their registry lines: set_auto_respond_events (bridge-internal),
#: query_event_probabilities and its alias event_probabilities (Tier 2) and
#: respond_event (game-thread action). Then 87 Tier-2: query_pending_events,
#: kept only by the polarity table and parity_plan, went the same way.
#: Cleanup 2026.11 L17: 43 / 87 / 22. The shared board went (owner ruling
#: 2026-11-02): board_post, board_read and board_admin were the three
#: bridge-internal names in snapshot.cpp's safe[] list; no Tier-2 read and no
#: game-thread action was the board's.
CLASSIFIER_FLOOR = {"is_bridge_internal_query": 43, "is_game_state_query": 87,
                    "is_game_thread_action": 22}


#: L13 D1: what "touches game memory" means for row A0: resolving a character,
#: title or war, calling into the engine, or reading or writing an image
#: address (g_base_addr + anything — a globals:: name or an offset held in a
#: table — safe_read_offset, resolve(globals::)). Followed to depth A0_DEPTH,
#: deeper than the raw-read audit, because the clock verbs reach the engine four
#: calls down (do_pause_game -> clock_verb_pause -> clock_write -> pause_primitive).
_GAME_TOUCH_RE = re.compile(
    r"\b(?:resolve_character|lookup_character|get_controlled_character|lookup_title_obj|"
    r"resolve_title_realm|game_fn|resolve_\w+_by_id|safe_read_offset\s*<[^>]*>)\s*\("
    r"|\bg_base_addr\s*\+|\bresolve\s*\(\s*(?:ck3::)?globals::")
A0_DEPTH = 6
#: The declared IPC-thread exceptions live in ONE place, snapshot.cpp's
#: is_bridge_internal_query, as `ipc-exception <name>: <reason>` comment lines.
_IPC_EXCEPTION_RE = re.compile(r"//\s*ipc-exception\s+(\w+):\s*(\S.*)")


def ipc_exceptions(b: Bridge) -> dict[str, str]:
    """name -> reason, read from snapshot.cpp (comments kept)."""
    p = b.src_dir / "snapshot.cpp"
    text = p.read_text(errors="replace") if p.is_file() else ""
    return {m.group(1): m.group(2).strip() for m in _IPC_EXCEPTION_RE.finditer(text)}


def section_a0(b: Bridge, rep: Report) -> None:
    rep.section("(A0) D1 — no undeclared game-object handler on the IPC thread")
    reg = b.registry()
    names = b.classifier_names("is_bridge_internal_query")
    if not rep.check("is_bridge_internal_query() parsed for A0",
                     len(names) >= CLASSIFIER_FLOOR["is_bridge_internal_query"],
                     f"found {len(names)} names"):
        return
    declared = ipc_exceptions(b)
    if not rep.check("snapshot.cpp's ipc-exception list parsed (fails open)", len(declared) >= 10,
                     f"found {len(declared)} lines"):
        return
    touching = {}
    for name in names:
        fn = reg.get(name)
        path = b.closure(fn, depth=A0_DEPTH) if fn else []
        hits = sorted({n for _, n, body in path if _GAME_TOUCH_RE.search(body)})
        if hits:
            touching[name] = hits
    wrong = [f"{n} (via {', '.join(h[:3])})" for n, h in touching.items() if n not in declared]
    rep.check("every IPC-thread handler that resolves a character or calls the engine is a declared exception",
              not wrong, "; ".join(wrong))
    stale = [n for n in declared if n not in names or n not in touching]
    rep.check("every declared ipc-exception is an IPC-thread handler that still touches game memory",
              not stale, "stale: " + ", ".join(stale))


#: L13 (witness W3): a game-thread read runs inside the drain, holding the
#: session's command mutex, so it must not sleep: every engine command waits
#: behind it and no tick can come. The only sleeps its closure may reach are
#: these, each a no-op on the game thread.
#:
#: What A2 cannot see (witness m-5): it matches the sleep calls and (L14) a
#: condition-variable wait or timed lock by its member name (.wait / .wait_for /
#: .wait_until / .try_lock_for / .try_lock_until), to CALL_DEPTH. A blocking
#: lock() on a mutex another thread holds, a spin loop on an atomic, a blocking
#: read or poll on a descriptor, and a long engine evaluation with no sleep at
#: all each stall the drain just as a sleep does and pass this row. Row A3
#: covers the long evaluations it names, and nothing else covers the rest.
GAME_THREAD_SLEEP_OK = {
    "slot_poll_wait": "returns at once on the game thread",
}
#: L14: a sleep, and (by member name) a condition-variable wait or a timed lock.
_SLEEP_RE = re.compile(r"\b(?:sleep_for|sleep_until|usleep|nanosleep|sleep)\s*\("
                       r"|(?:\.|->)\s*(?:wait|wait_for|wait_until|try_lock_for|try_lock_until)\s*\(")
#: A loop that does not run on the game thread: its condition says so.
_OFF_GAME_THREAD_COND_RE = re.compile(r"!\s*(?:bridge::)?on_game_thread\s*\(\s*\)")


def _loops(body: str) -> list[tuple[str, int, int]]:
    """(condition, body start, body end) for every braced for / while loop."""
    out = []
    for m in re.finditer(r"\b(?:for|while)\s*\(", body):
        i = body.index("(", m.start())
        j = _paren_end(body, i)
        k = j + len(body[j:]) - len(body[j:].lstrip())
        if body[k:k + 1] == "{":
            sp = _span_at(body, k)
            if sp:
                out.append((body[i:j], sp[0], sp[1]))
    return out


def drain_sleeps(body: str) -> list[str]:
    """The sleeps / waits in one body that can run on the game thread: every
    _SLEEP_RE match except one inside a loop whose condition is false there
    (`for (...; poll < N && !bridge::on_game_thread(); ...)`). The other no-op
    form, a function that returns first on the game thread, is an entry in
    GAME_THREAD_SLEEP_OK."""
    guarded = [(a, z) for cond, a, z in _loops(body) if _OFF_GAME_THREAD_COND_RE.search(cond)]
    return [body[m.start():m.end()] for m in _SLEEP_RE.finditer(body)
            if not any(a <= m.start() < z for a, z in guarded)]


def section_a2(b: Bridge, rep: Report) -> None:
    rep.section("(A2) no game-thread read sleeps (W3)")
    reg = b.registry()
    names = b.classifier_names("is_game_state_query")
    if not rep.check("is_game_state_query() parsed for A2",
                     len(names) >= CLASSIFIER_FLOOR["is_game_state_query"],
                     f"found {len(names)} names"):
        return
    wrong = set()
    for name in names:
        fn = reg.get(name)
        for _, n, body in (b.closure(fn) if fn else []):
            if n not in GAME_THREAD_SLEEP_OK and drain_sleeps(body):
                wrong.add(f"{name} (in {n})")
    rep.check("no game-state handler reaches a sleep (the drain holds the session mutex)",
              not wrong, "; ".join(sorted(wrong)))


#: L13 FF2 (witness R6): the long engine evaluations that run on the game thread
#: holding the session mutex, each of which must check the game-thread time
#: budget between engine calls (and return partial results past it).
BUDGETED_EVALUATORS = {
    "do_query_conquest_targets": "the strict CB check per holder",
    "do_query_cbs_for_targets": "one strict CB probe per target",
    "build_faction_options": "the engine's faction verdicts",
    "enumerate_decisions_annotated": "the decision triggers",
}
_BUDGET_RE = re.compile(r"\bgame_thread_budget_spent\s*\(|\bkGameThreadEvalBudgetMs\b")


def section_a3(b: Bridge, rep: Report) -> None:
    rep.section("(A3) every long game-thread evaluation checks the time budget (R6)")
    for fn, what in BUDGETED_EVALUATORS.items():
        defs = b.functions.get(fn) or []
        body = defs[0][1] if defs else ""
        rep.check(f"{fn}: defined", bool(defs), f"no body for {fn} ({what})")
        inside = False
        for m in re.finditer(r"\b(?:for|while)\s*\(", body):
            i = body.index("(", m.start())
            j = _paren_end(body, i)
            k = len(body[j:]) - len(body[j:].lstrip())
            if body[j + k:j + k + 1] != "{":
                continue
            loop_body = _block_at(body, j + k) or ""
            if _BUDGET_RE.search(loop_body):
                inside = True
                break
        rep.check(f"{fn}: checks the game-thread budget inside its loop ({what})", inside,
                  "no loop body in it checks game_thread_budget_spent / kGameThreadEvalBudgetMs")


# ── G: the game-thread actions (Tier 3, L14) ─────────────────────────────────

#: The rows a `game-thread-ok <name>: <row>: <reason>` line may excuse (see
#: THE RULE in the module docstring). G-raw and G-clock are not among them.
DECLARABLE_ROWS = ("fault_check", "commit", "char", "time_bound", "sleep")
_GT_OK_RE = re.compile(r"//\s*game-thread-ok\s+(\w+):\s*(\w+):\s*(\S.*)")
#: A comment that LEADS with the token is a declaration, parsed or a FAIL.
_GT_OK_MARK_RE = re.compile(r"//\s*game-thread-ok\b")

#: A cast whose argument names the image (game_fn, g_base_addr) or a vtable slot.
_IMAGE_ARG_RE = re.compile(r"\bgame_fn\s*\(|\bg_base_addr\b|\bvt\s*\[")
_ANY_CAST = re.compile(r"reinterpret_cast\s*<([^;{}]*?)>\s*\(")
_FNPTR_TYPE_RE = re.compile(r"\(\s*\*\s*\)")
_DATA_TYPE_RE = re.compile(r"\buintptr_t\b|\bvoid\s*\*|\bu?int\d+_t\b|\bchar\b")
_WATCHDOG_RE = re.compile(r"\bwatchdog_arm\s*\(")
_TEMPLATE_ARGS_RE = re.compile(r"<[^<>]*>")


def outer_type(t: str) -> str:
    """A cast's type with its template arguments removed (innermost first), so
    the data-type test reads the type itself and not its arguments: L14 FB M-6,
    `bridge::EngineBoolFn<void*, std::string*>` is a function-pointer alias whose
    `void*` ARGUMENT had dropped five engine calls from the census."""
    prev = None
    while prev != t:
        prev, t = t, _TEMPLATE_ARGS_RE.sub("", t)
    return t


def engine_calls(body: str) -> list[tuple[int, str]]:
    """(position, label) of every call into the engine in one body: a call
    through a variable bound from a cast to a function-pointer type, or from a
    cast of an image address to a named (typedef) type
    (`auto f = reinterpret_cast<fn_t>(bridge::game_fn(X)); ... f(..)`), and
    such a cast called where it stands (`reinterpret_cast<R(*)(A)>(p)(obj)`).
    Stated limits: a pointer bound in another function and passed in, a member
    function pointer, a std::function, and a call through a bridge wrapper
    that hides the cast deeper than CALL_DEPTH."""
    out, bound = [], {}
    for m in _ANY_CAST.finditer(body):
        end = _paren_end(body, m.end() - 1)
        t, arg = m.group(1), body[m.end():end - 1]
        if not (_FNPTR_TYPE_RE.search(t) or (_IMAGE_ARG_RE.search(arg) and not _DATA_TYPE_RE.search(outer_type(t)))):
            continue
        lhs = re.search(r"\b([A-Za-z_]\w*)\s*=\s*$", body[max(0, m.start() - 80):m.start()])
        if lhs:
            bound.setdefault(lhs.group(1), end)
        elif re.match(r"\s*\(", body[end:]):
            out.append((m.start(), "cast-call"))
    for name, after in bound.items():
        for c in re.finditer(r"(?<![\w.>:])" + re.escape(name) + r"\s*\(", body):
            if c.start() >= after:
                out.append((c.start(), name))
    return sorted(out)


_DISARM_RE = re.compile(r"\bwatchdog_disarm\s*\(|\bg_in_protected_region\s*=\s*false\b")


def guarded_spans(body: str) -> list[tuple[int, int]]:
    """The [start, end) spans of one body where its OWN guard is armed (L14 FB
    M-8): from a sigsetjmp whose last arm/disarm event before it is a
    watchdog_arm (L14 FB2 N-5: a disarm after the arm leaves a later sigsetjmp
    unarmed), to the first watchdog_disarm() / `g_in_protected_region = false`
    after it that is not inside that sigsetjmp's crash block (the end of the
    body when there is none). A disarm inside a nested block that returns is an
    early exit and ends neither (in_guarded_span still puts the calls after it
    in that block out); a disarm inside a lambda body is top-level, since the
    lambda may run anywhere. A guard whose crash block this parser cannot place
    counts its crash path as unknown and ends the span at its first disarm of
    any kind."""
    spans = []
    for j in _SETJMP_RE.finditer(body):
        events = sorted([(m.start(), "arm") for m in _WATCHDOG_RE.finditer(body, 0, j.start())]
                        + [(m.start(), "disarm") for m in _DISARM_RE.finditer(body, 0, j.start())
                           if not _early_exit(body, m.start())])
        if not events or events[-1][1] != "arm":
            continue
        crash = _crash_span(body, j.start())
        end = len(body)
        for d in _DISARM_RE.finditer(body, _paren_end(body, body.index("(", j.start()))):
            if crash and crash[0] <= d.start() < crash[1]:
                continue
            if _early_exit(body, d.start()):
                continue
            end = d.start()
            break
        spans.append((j.start(), end))
    return spans


_LAMBDA_HEAD_RE = re.compile(r"\]\s*(?:\([^()]*\))?\s*(?:mutable\s*)?(?:->\s*[\w:<>*&\s]+?)?\s*$")


def _open_brace(body: str, pos: int) -> int:
    """The index of the `{` of the innermost block enclosing body[pos] (0 when
    none is found)."""
    depth, i = 0, pos
    while i > 0:
        i -= 1
        if body[i] == "}":
            depth += 1
        elif body[i] == "{":
            if depth == 0:
                return i
            depth -= 1
    return 0


def _is_lambda_body(body: str, brace: int) -> bool:
    return bool(_LAMBDA_HEAD_RE.search(body[max(0, brace - 200):brace]))


def _early_exit(body: str, pos: int) -> bool:
    """A disarm at body[pos] that is an early exit (its block returns) and not
    inside a lambda body (L14 FB2 N-5: a lambda's disarm counts as top-level)."""
    return _exits_its_block(body, pos) and not _is_lambda_body(body, _open_brace(body, pos))


def _exits_its_block(body: str, pos: int) -> bool:
    """True when the block enclosing body[pos] is nested (not the function's
    own body) and its last statement is a `return`: a disarm there is an early
    exit, and the code after that block is reached only without it."""
    depth, i = 0, pos
    while i > 0:
        i -= 1
        if body[i] == "}":
            depth += 1
        elif body[i] == "{":
            if depth == 0:
                break
            depth -= 1
    if i == 0:
        return False
    sp = _span_at(body, i)
    if not sp:
        return False
    inner = body[sp[0] + 1:sp[1] - 1].rstrip()
    if not inner.endswith(";"):
        return False
    head = inner[:-1]
    start = max(head.rfind(";"), head.rfind("{"), head.rfind("}")) + 1
    return bool(re.match(r"\s*return\b", head[start:]))


def in_guarded_span(body: str, pos: int) -> bool:
    """pos lies in a guarded span and no disarm stands before it in any block
    that encloses it inside that span (L14 FB2 N-5: `if (x) { watchdog_disarm();
    call(); return; }` is an early exit for the code after the block, and the
    call after the disarm in it runs unarmed)."""
    for a, e in guarded_spans(body):
        if not a <= pos < e:
            continue
        brace, inside = pos, True
        while True:
            brace = _open_brace(body, brace)
            if brace <= a:
                break
            if any(_open_brace(body, d.start()) == brace
                   for d in _DISARM_RE.finditer(body, brace, pos)):
                inside = False
                break
        if inside:
            return True
    return False


def unbounded_engine_calls(body: str) -> list[str]:
    """The engine calls in one body that its own armed guard does not hold:
    not between a sigsetjmp (after a watchdog_arm) and that guard's first
    disarm outside its crash block (guarded_spans). A body that checks the
    game-thread budget is bounded by it."""
    if _BUDGET_RE.search(body):
        return []
    return [lab for pos, lab in engine_calls(body) if not in_guarded_span(body, pos)]


#: L14 FB m-1 (G-al): an engine yes/no is a C++ bool, returned in AL; the rest
#: of RAX is garbage (engine_bool.h). A function-pointer cast whose return type
#: is wider than a byte, tested for truth without `& 0xFF`, reads the garbage.
_WIDE_RET = (r"(?:const\s+)?(?:u?int(?:64|32|16)_t|uintptr_t|int|long(?:\s+long)?"
             r"|unsigned(?:\s+int|\s+long(?:\s+long)?)?|size_t)")
_WIDE_FNPTR_RE = re.compile(r"^\s*" + _WIDE_RET + r"\s*\(\s*\*\s*\)")
_FNPTR_ALIAS_RE = re.compile(r"\busing\s+([A-Za-z_]\w*)\s*=\s*([^;=]*?\(\s*\*\s*\)[^;]*);")
_FNPTR_TYPEDEF_RE = re.compile(r"\btypedef\s+([^;(]*?)\(\s*\*\s*([A-Za-z_]\w*)\s*\)[^;]*;")
_AL_MASK_RE = re.compile(r"&\s*0x[fF][fF]\b")


def fnptr_aliases(text: str) -> dict[str, str]:
    """name -> function-pointer type, for every `using X = R (*)(..)` and
    `typedef R (*X)(..)` in one file's text."""
    out = {m.group(1): m.group(2) for m in _FNPTR_ALIAS_RE.finditer(text)}
    out.update({m.group(2): m.group(1) + "(*)" for m in _FNPTR_TYPEDEF_RE.finditer(text)})
    return out


def _statement(body: str, pos: int) -> tuple[int, int]:
    start = max(body.rfind(";", 0, pos), body.rfind("{", 0, pos), body.rfind("}", 0, pos)) + 1
    end = body.find(";", pos)
    return start, (end if end != -1 else len(body))


def _in_condition(body: str, stmt_start: int, pos: int) -> bool:
    """The call at pos lies inside the parenthesized condition of the if/while
    its statement starts with."""
    m = re.compile(r"\s*(?:\}\s*else\s+)?(?:if|while)\s*\(").match(body, stmt_start)
    return bool(m) and pos < _paren_end(body, m.end() - 1)


def wide_engine_truth_tests(body: str, aliases: dict[str, str]) -> tuple[list[str], int]:
    """([`label: statement`] for each call through a function pointer whose
    return type is wider than a byte, converted to bool where it stands without
    `& 0xFF`, the number of function-pointer casts examined). Where it stands:
    inside an if/while condition, under `!`, initializing or assigned to a
    bool, or an operand of && / ||. Stated limits: a result stored in a wider
    variable and tested later (a pointer or count return is tested so
    legitimately, and the type does not say which it is), passed on, or
    returned."""
    out, seen = [], 0
    bools = set(re.findall(r"\bbool\s+([A-Za-z_]\w*)\s*[=;,)]", body))
    for m in _ANY_CAST.finditer(body):
        t = m.group(1).strip()
        t = aliases.get(t, t)
        if not _FNPTR_TYPE_RE.search(t):
            continue
        seen += 1
        if not _WIDE_FNPTR_RE.search(t):
            continue
        end = _paren_end(body, m.end() - 1)
        lhs = re.search(r"\b([A-Za-z_]\w*)\s*=\s*$", body[max(0, m.start() - 80):m.start()])
        if lhs:
            name = lhs.group(1)
            calls = [c.start() for c in re.finditer(r"(?<![\w.>:])" + re.escape(name) + r"\s*\(", body)
                     if c.start() >= end]
        elif re.match(r"\s*\(", body[end:]):
            name, calls = "cast-call", [m.start()]
        else:
            continue
        for pos in calls:
            a, z = _statement(body, pos)
            stmt = body[a:z]
            if _AL_MASK_RE.search(stmt):
                continue
            pre = body[a:pos]
            var = re.search(r"\b([A-Za-z_]\w*)\s*=\s*$", pre)
            if (_in_condition(body, a, pos) or re.search(r"!\s*$", pre)
                    or re.search(r"\bbool\s+[A-Za-z_]\w*\s*=\s*$", pre)
                    or (var and var.group(1) in bools)
                    or re.search(r"(?:&&|\|\|)\s*$", pre)
                    or re.match(r"\s*(?:&&|\|\|)", body[_paren_end(body, body.index("(", pos)):])):
                out.append(f"{name}: {' '.join(stmt.split())[:100]}")
    return out, seen


#: Fail-open floor for G-al: the function-pointer casts the census examines
#: across the parsed tree (L14 FB m-1). Fewer = a parse that grades less and
#: passes. 218 on FB's tree; 217 since the int4 merge of FC's m-16, which
#: deleted the Linux bridge_session_pass wrapper and with it its
#: reinterpret_cast<session_loop_fn> in bridge_init.cpp (the shared cell's one
#: LoopFn cast, src/os/session_pass.cpp, was already counted at os/windows/).
#: 216 since L16 R3 deleted select_event_option (event_handler.cpp) with
#: respond_event, and with it its GameCommand__Dispatch cast.
FNPTR_CAST_FLOOR = 216


def unbounded_on_path(path: list[tuple[str, str, str]]) -> list[str]:
    """`callee: label` for each engine call on a handler's path that no armed
    guard holds: not one in its own body (guarded_spans), nor one that holds
    every call of it from its callers on the path (a helper that runs under its
    caller's guard). One level of callers; the path's root has none."""
    out = set()
    for f, n, body in path:
        labs = unbounded_engine_calls(body)
        if not labs:
            continue
        sites = []
        for _, cn, cb in path:
            for m in re.finditer(r"(?<![\w.>:])" + re.escape(n) + r"\s*\(", cb):
                if cn != n:
                    sites.append((cb, m.start()))
        covered = bool(sites) and all(in_guarded_span(cb, pos) for cb, pos in sites)
        if not covered:
            out |= {f"{n}: {lab}" for lab in labs}
    return sorted(out)


def _store_sites(body: str) -> list[int]:
    """Stores into game memory in one body: `*reinterpret_cast<T*>(EXPR) =` (not
    a local byte buffer), write_to_readonly / safe_write*."""
    out = []
    for m in DEREF_CAST_RE.finditer(body):
        end = _paren_end(body, m.end() - 1)
        if _is_write(body, end) and not _local_byte_buffer(body, body[m.end():end]):
            out.append(m.start())
    out += [m.start() for m in re.finditer(r"\b(?:write_to_readonly|safe_write\w*)\s*\(", body)]
    return out


_DIRECT_COMMIT_RE = re.compile(r"\barm_fake_random_scope\s*\(|\bwatchdog_arm\s*\(|\.\s*enqueue\s*\(")


def _commits_directly(body: str) -> list[int]:
    return sorted([m.start() for m in _DIRECT_COMMIT_RE.finditer(body)]
                  + [p for p, _ in engine_calls(body)] + _store_sites(body))


def first_commit(b: Bridge, file: str, body: str) -> tuple[int, str] | None:
    """(position, what) of the first engine contact in a handler's own body: a
    scope or watchdog arm, an engine call, an intent enqueue, a store into game
    memory, or a call of a bridge function whose path (to CALL_DEPTH - 1) makes
    one (see THE RULE)."""
    cands = [(p, "a direct commit") for p in _commits_directly(body)[:1]]
    for m in _CALL_RE.finditer(body):
        c = m.group(1)
        if c in _KEYWORDS or not b.resolve(c, file):
            continue
        if any(_commits_directly(cb) for _, _, cb in b.closure(c, home=file, depth=CALL_DEPTH - 1)):
            cands.append((m.start(), c + "()"))
            break
    return min(cands) if cands else None


_DECIDE_RE = re.compile(r"\bif\s*\(\s*safe_read_had_fault\s*\(\s*\)\s*\)\s*return\b")


def commit_gap(b: Bridge, file: str, body: str, commit: tuple[int, str] | None) -> str | None:
    """None when a body decides on its reads before its first engine contact
    (G-commit): an `if (safe_read_had_fault()) return` precedes the commit in a
    block that contains the commit (L14 FB2 N-5b: a decision nested in an
    unrelated `if` does not dominate it), and no read is reachable between the
    LAST such decision and the commit (L14 FB M-7: do_change_realm_law decided,
    then read the war count, and a fault there skipped the at-war refusal). A
    body with no engine contact found is a red by name (L14 FB2 N-5c: the
    caller grades only game-thread writes that read through safe_read, and a
    write that touches nothing this parser can see is a parse that grades less,
    never a pass). Else what is missing."""
    if commit is None:
        return "no engine contact found"
    decisions = [m for m in _DECIDE_RE.finditer(body)
                 if m.start() < commit[0] and _contains(body, _open_brace(body, m.start()), commit[0])]
    if not decisions:
        return "`if (safe_read_had_fault()) return` before it, in a block that contains it: no"
    last = decisions[-1]
    stmt_end = body.find(";", last.end())
    gap = body[stmt_end + 1:commit[0]] if stmt_end != -1 else ""
    if _reads_before(b, file, gap):
        return "a read lies between the last `if (safe_read_had_fault()) return` and it"
    return None


def _contains(body: str, brace: int, pos: int) -> bool:
    """The block whose `{` is at body[brace] contains body[pos] (brace 0 with
    no `{` there: the whole body)."""
    if body[brace:brace + 1] != "{":
        return True
    sp = _span_at(body, brace)
    return bool(sp) and sp[0] <= pos < sp[1]


def _reads_before(b: Bridge, file: str, text: str) -> bool:
    if SAFE_READ_RE.search(text) or raw_reads(text):
        return True
    for m in _CALL_RE.finditer(text):
        c = m.group(1)
        if c in _KEYWORDS or not b.resolve(c, file):
            continue
        if any(safe_reads(cb) or raw_reads(cb) for _, _, cb in b.closure(c, home=file, depth=CALL_DEPTH - 1)):
            return True
    return False


def graded_body(b: Bridge, file: str, fn: str, body: str) -> tuple[str, str, str]:
    """(file, name, body) the per-body rows grade for handler `fn`: its own,
    or, for a forwarder (a guard in front of `return other(params_json, ...)`
    that reads nothing itself), the body it forwards to (two hops at most)."""
    for _ in range(2):
        m = re.search(r"\breturn\s+([A-Za-z_]\w*)\s*\(\s*params_json\b", body)
        if not m or _reads_before(b, file, body[:m.start()]):
            break
        defs = b.resolve(m.group(1), file)
        if len(defs) != 1:
            break
        file, fn, body = defs[0][0], m.group(1), defs[0][1]
    return file, fn, body


def game_thread_ok(b: Bridge) -> tuple[dict[tuple[str, str], str], list[str]]:
    """({(name, row): reason}, malformed lines), read from snapshot.cpp's
    is_game_thread_action (comments kept). A `game-thread-ok` line anywhere
    else in snapshot.cpp is malformed (L14 FB m-12): the declarations live in
    ONE place, and a line outside it would excuse a row from where no reader of
    the classifier looks."""
    p = b.src_dir / "snapshot.cpp"
    text = p.read_text(errors="replace") if p.is_file() else ""
    stripped = strip_code(text, keep_strings=True)
    m = re.search(r"bool\s+" + GAME_THREAD_ACTIONS + r"\s*\([^)]*\)\s*\{", stripped)
    sp = _span_at(stripped, m.end() - 1) if m else None
    decl, bad = {}, []
    if not sp:
        bad.append(f"no {GAME_THREAD_ACTIONS} body in snapshot.cpp")
    pos = 0
    for line in text.splitlines(keepends=True):
        at, pos = pos, pos + len(line)
        if not _GT_OK_MARK_RE.search(line):
            continue
        mm = _GT_OK_RE.search(line)
        inside = bool(sp) and sp[0] <= at < sp[1]
        if not inside or not mm or mm.group(2) not in DECLARABLE_ROWS:
            bad.append(line.strip() + ("" if inside else f"  (outside {GAME_THREAD_ACTIONS})"))
            continue
        decl[(mm.group(1), mm.group(2))] = mm.group(3).strip()
    return decl, bad


#: L14 S4 (audit 3(ii)): the fake random scope and a body's own guard.
_ARM_RE = re.compile(r"\barm_fake_random_scope\s*\(")
_RESTORE_RE = re.compile(r"\brestore_fake_random_scope\s*\(")
_SETJMP_RE = re.compile(r"\bsigsetjmp\s*\(")
#: Fail-open floor: the bodies that arm the scope under a guard of their own
#: (L14 S4: 22 on a0a2fff3; 23 after round 2 merged S3's
#: do_query_create_title_cost, 378222e6). Fewer found = a parse that grades
#: less and passes.
SCOPE_ARM_FLOOR = 23
#: Fail-open floor for G-nest (L14 FB m-13): the BRIDGE_ACTION lines in
#: action_registry.inc (160 at a1fb8c6b; 156 since L16 R3 deleted the four
#: event lines: query_event_probabilities, event_probabilities, respond_event,
#: set_auto_respond_events; 155 with query_pending_events, which had no caller
#: either; 152 since cleanup 2026.11 L17 deleted the shared board's three
#: lines: board_post, board_read, board_admin). A parse that found fewer would
#: walk fewer closures and pass.
REGISTRY_FLOOR = 152


def _crash_span(body: str, jmp: int) -> tuple[int, int] | None:
    """The [start, end) of the block a sigsetjmp's crash path runs, or None when
    the shape is not one this parser reads: `int sig = sigsetjmp(..); if (sig != 0) {..}` /
    `if (sig) {..}`, the inline `if (sigsetjmp(..) != 0) {..}`, or the `== 0`
    forms' `else {..}`. A `== 0` form with no else has no crash block: its
    crash path falls through to the code after the if (an empty span, (e, e))."""
    end = _paren_end(body, body.index("(", jmp))
    var = re.search(r"\b([A-Za-z_]\w*)\s*=\s*$", body[max(0, jmp - 60):jmp])
    if var:
        m = re.compile(r"\s*;\s*if\s*\(\s*" + re.escape(var.group(1)) + r"\s*(!=\s*0|==\s*0)?\s*\)\s*\{").match(body, end)
    else:
        m = re.compile(r"\s*(!=\s*0|==\s*0)?\s*\)\s*\{").match(body, end)
    if not m:
        return None
    blk = _span_at(body, m.end() - 1)
    if not blk:
        return None
    if (m.group(1) or "").replace(" ", "") != "==0":
        return blk
    e = re.compile(r"\s*else\s*\{").match(body, blk[1])
    if not e:
        return (blk[1], blk[1])
    return _span_at(body, e.end() - 1)


def _crash_branch(body: str, jmp: int) -> str | None:
    """The text of _crash_span ("" for a fall-through crash path)."""
    sp = _crash_span(body, jmp)
    return body[sp[0]:sp[1]] if sp else None


def scope_arm_defects(body: str) -> list[str]:
    """For a body that arms the fake random scope and sets a guard of its own:
    each arm is followed by a sigsetjmp before the arm's first restore (an arm
    after the guard leaves the crash branch, which siglongjmp reaches without
    unwinding, unable to restore the scope it never saw armed), and that
    guard's crash branch, when it returns, restores the scope. A guard whose
    crash path this parser cannot place is a defect by name (fail OPEN)."""
    out = []
    for a in _ARM_RE.finditer(body):
        r = _RESTORE_RE.search(body, a.end())
        stop = r.start() if r else len(body)
        guards = [m.start() for m in _SETJMP_RE.finditer(body, a.end(), stop)]
        if not guards:
            out.append(f"arm at +{a.start()}: no sigsetjmp between it and its restore "
                       "(armed after the guard, or unguarded)")
            continue
        crash = _crash_branch(body, guards[-1])
        if crash is None:
            out.append(f"arm at +{a.start()}: the guard's crash path is in a shape this parser cannot read")
        elif re.search(r"\breturn\b", crash) and not _RESTORE_RE.search(crash):
            out.append(f"arm at +{a.start()}: the crash branch returns without restore_fake_random_scope")
    return out


def char_return_misplaced(b: Bridge, file: str, body: str) -> str | None:
    """None when the first early return on an unresolved character comes after
    the body's first character resolve, with no engine contact between the two
    (L14 FB m-14: the row had checked that such a return exists anywhere)."""
    r = CHAR_RESOLVE_RE.search(body)
    e = CHAR_EARLY_RETURN_RE.search(body, r.end()) if r else None
    if not e:
        return "resolves a character with no is_sentinel / actor_refusal / character_not_found return after it"
    commit = first_commit(b, file, body[r.end():])
    if commit and commit[0] < e.start() - r.end():
        return f"an engine contact ({commit[1]}) lies between the resolve and its early return"
    return None


def callee_clears(b: Bridge, fn: str) -> tuple[list[str], bool]:
    """([`callee (file)`], parsed): every function reachable from handler `fn`
    (the whole closure, to a fixpoint), other than `fn` itself, whose body calls
    safe_read_clear() by name. A FaultScope there is not a clear: it saves the
    record it finds and puts it back (section F checks the class does).
    parsed is False when the handler's body is not found: red, never green."""
    path = b.closure(fn, depth=64)
    if not path:
        return [], False
    return sorted({f"{n} ({f})" for f, n, body in path[1:]
                   if n != fn and (FAULT_PAIR[0].search(body)
                                   or (n != "safe_read_clear" and counter_wipes(body)))}), True


_COUNTER_STORE_RE = re.compile(r"\bg_safe_read_fault_count\s*=(?!=)\s*([^;]*);")


def counter_wipes(body: str) -> list[str]:
    """Direct stores to the fault counter that are a clear by another spelling
    (L14 FB m-14): `g_safe_read_fault_count = <expr>` unless <expr> is a local
    the same body saved from the counter first (`int n = g_safe_read_fault_count;
    ... g_safe_read_fault_count = n;`, a put-back). Increments (`++`, `+=`) add a
    fault and are not stores here. bridge.h's safe_read_clear is the clear by
    name, graded at its callers."""
    out = []
    for m in _COUNTER_STORE_RE.finditer(body):
        rhs = m.group(1).strip()
        saved = re.fullmatch(r"[A-Za-z_]\w*", rhs) and re.search(
            r"\b" + re.escape(rhs) + r"\s*=\s*g_safe_read_fault_count\s*;", body[:m.start()])
        if not saved:
            out.append(body[m.start():m.end()].strip())
    return out


#: G-session (L14 FB2 N-12): the one raw read the session-pass wrapper may
#: make, on every session pass and outside any sigsetjmp. It is the execute byte
#: the engine's own pass has just used, on the session object in the image's
#: .data, at an offset install probed first (os/session_pass.cpp); a second raw
#: read there has none of those reasons and reds.
SESSION_PASS_FILE, SESSION_PASS_FN = "os/session_pass.cpp", "session_pass_wrapper"
SESSION_PASS_RAW = "*reinterpret_cast<volatile std::uint8_t*>(s + g_execute_off)"


def session_pass_raw_reads(b: Bridge) -> str | None:
    """None when the session-pass wrapper is found once and its only raw read is
    SESSION_PASS_RAW, once; else what differs (a wrapper not found is a red)."""
    defs = [body for f, body, _ in b.functions.get(SESSION_PASS_FN, []) if f == SESSION_PASS_FILE]
    if len(defs) != 1:
        return f"{len(defs)} definitions of {SESSION_PASS_FN} in {SESSION_PASS_FILE}"
    flat = " ".join(defs[0].split())
    n = flat.count(SESSION_PASS_RAW)
    if n != 1:
        return f"the execute-byte read appears {n} times"
    rest = raw_reads(flat.replace(SESSION_PASS_RAW, "0", 1))
    if rest:
        return f"{rest} raw read(s) besides the execute byte"
    return None


def section_g(b: Bridge, rep: Report) -> None:
    rep.section("(G) TIER 3 — every is_game_thread_action() name, by THE RULE")
    reg = b.registry()
    names = b.classifier_names(GAME_THREAD_ACTIONS)
    if not rep.check(f"{GAME_THREAD_ACTIONS}() parsed", len(names) >= CLASSIFIER_FLOOR[GAME_THREAD_ACTIONS],
                     f"found {len(names)} names, floor {CLASSIFIER_FLOOR[GAME_THREAD_ACTIONS]}"):
        return
    reads = set(b.classifier_names("is_game_thread_read"))
    rep.check("is_game_thread_read() parsed (its names are game-thread actions)",
              bool(reads) and reads <= set(names), f"found {sorted(reads)}")
    decl, bad = game_thread_ok(b)
    rep.check("every game-thread-ok line parses as <name>: <row>: <reason> with a declarable row",
              not bad, "; ".join(bad))
    used: set[tuple[str, str]] = set()

    def row(name: str, key: str | None, label: str, ok: bool, detail: str) -> None:
        if not ok and key and (name, key) in decl:
            used.add((name, key))
            ok, detail = True, f"declared: {decl[(name, key)]}"
        rep.check(f"{name}: {label}", ok, detail)

    for name in names:
        fn = reg.get(name)
        if not rep.check(f"{name}: registered in action_registry.inc", fn is not None,
                         "a game-thread action with no BRIDGE_ACTION line: nothing serves it"):
            continue
        path = b.closure(fn)
        if not rep.check(f"{name} ({fn}): body found in bridge/src", bool(path),
                         f"no definition of {fn} in any bridge/src/*.cpp"):
            continue
        raw = {f"{n} ({f})": raw_reads(body) for f, n, body in path if raw_reads(body)}
        row(name, None, "zero raw reads in it and its callees (G-raw)", not raw,
            "raw reads: " + ", ".join(f"{k}: {v}" for k, v in raw.items()))
        gfile, gname, own = graded_body(b, path[0][0], fn, path[0][2])
        via = "" if gname == fn else f" (graded in {gname})"
        if any(safe_reads(body) for _, _, body in path):
            if has_fault_check(own):
                late = fault_bracket_misplaced(b, gfile, own)
                row(name, "fault_check", "the fault bracket opens before its first read" + via, late is None,
                    f"the clear / FaultScope comes after {late}")
                undecided = undecided_fault_checks(own)
                row(name, "fault_check", "every fault check decides" + via, not undecided, "; ".join(undecided))
            else:
                row(name, "fault_check", "checks the safe_read fault flag before it answers" + via, False,
                    "its path reads through safe_read and its body has neither safe_read_clear()+"
                    "safe_read_had_fault() nor a FaultScope: a fault answers a silent 0")
            if name not in reads:
                commit = first_commit(b, gfile, own)
                gap = commit_gap(b, gfile, own, commit)
                row(name, "commit", "decides on its reads before its first engine contact (G-commit)" + via,
                    gap is None, f"first engine contact: {commit[1] if commit else '-'}; {gap}")
        if CHAR_RESOLVE_RE.search(own):
            miss = char_return_misplaced(b, gfile, own)
            row(name, "char", "returns early when the character does not resolve" + via, miss is None, miss)
        unb = unbounded_on_path(path)
        row(name, "time_bound", "every engine call is under a watchdog its body armed first (G-bound)",
            not unb, "unbounded: " + ", ".join(unb))
        slp = sorted({f"{n}: {s}" for _, n, body in path if n not in GAME_THREAD_SLEEP_OK
                      for s in drain_sleeps(body)})
        row(name, "sleep", "no sleep or wait on the game thread (the drain holds the session mutex)",
            not slp, "; ".join(slp))
        clk = sorted({f"{n}" for f, n, body in b.closure(fn, depth=A0_DEPTH)
                      if (f, n) not in {(CLOCK_HOME, a) for a in CLOCK_ALLOWED}
                      and (clock_write_sites(body, b.files.get(f, ""))
                           or re.search(r"\bclock_write\s*\(", body))})
        row(name, None, "no clock write and no call of clock_write in its path (G-clock)", not clk,
            "in " + ", ".join(clk))
    stale = sorted(f"{n}: {r}" for (n, r) in decl if (n, r) not in used)
    rep.check("every game-thread-ok declaration excuses a row that reds without it", not stale,
              "stale: " + "; ".join(stale))

    # G-al (L14 FB m-1): every engine yes/no in the parsed tree reads AL.
    seen, wide = 0, []
    for fname, text in b.files.items():
        aliases = fnptr_aliases(text)
        for n, body, _, _ in _definitions(text):
            bad, k = wide_engine_truth_tests(body, aliases)
            seen += k
            wide += [f"{n} ({fname}): {x}" for x in bad]
    if rep.check("the function-pointer casts were found (G-al)", seen >= FNPTR_CAST_FLOOR,
                 f"found {seen}, floor {FNPTR_CAST_FLOOR}"):
        rep.check("no wide-return engine call is tested for truth without & 0xFF (G-al)", not wide,
                  "; ".join(wide))

    # G-scope (L14 S4, audit 3(ii)): every body in bridge/src that arms the fake
    # random scope and sets a sigsetjmp of its own, enumerated from the code.
    armed = sorted((n, f, body) for n, defs in b.functions.items() for f, body, _ in defs
                   if _ARM_RE.search(body) and _SETJMP_RE.search(body))
    if rep.check("the bodies that arm the scope under their own guard were found (G-scope)",
                 len(armed) >= SCOPE_ARM_FLOOR, f"found {len(armed)}, floor {SCOPE_ARM_FLOOR}"):
        for n, f, body in armed:
            bad = scope_arm_defects(body)
            rep.check(f"{n} ({f}): arms the scope before its own sigsetjmp, and its crash branch "
                      "restores it (G-scope)", not bad, "; ".join(bad))

    # G-nest (L14 S4, audit 3(iii)): a callee that clears the fault flag wipes the
    # record its handler made before the call. Every registered handler, its
    # whole closure.
    reg_all = b.registry()
    rep.check("action_registry.inc parsed (G-nest)", len(reg_all) >= REGISTRY_FLOOR,
              f"found {len(reg_all)}, floor {REGISTRY_FLOOR}")
    for action, fn in sorted(reg_all.items()):
        clears, parsed = callee_clears(b, fn)
        rep.check(f"{action} ({fn}): no function it reaches calls safe_read_clear() but itself (G-nest)",
                  parsed and not clears,
                  "clears: " + ", ".join(clears) if parsed else f"no body found for {fn}")


    # G-session (L14 FB2 N-12): the session-pass wrapper runs on every pass,
    # outside any guard; its raw reads are pinned to the one execute byte.
    bad = session_pass_raw_reads(b)
    rep.check(f"{SESSION_PASS_FN} ({SESSION_PASS_FILE}): its one raw read is the execute byte the "
              "engine's pass just used, at an offset install probed (G-session)", bad is None, bad or "")

def section_a(b: Bridge, rep: Report) -> None:
    reg = b.registry()
    rep.check("action_registry.inc parsed", len(reg) >= 50, f"found {len(reg)} lines")
    audited: set[str] = set()
    for tier, classifier in enumerate(TIERS, start=1):
        rep.section(f"(A{tier}) HANDLERS — every {classifier}() name reads through safe_read")
        names = b.classifier_names(classifier)
        if rep.check(f"{classifier}() parsed", len(names) >= CLASSIFIER_FLOOR[classifier],
                     f"found {len(names)} names, floor {CLASSIFIER_FLOOR[classifier]}"):
            _audit_handlers(b, rep, reg, names, tier, audited)


def _audit_handlers(b: Bridge, rep: Report, reg: dict[str, str], names: list[str],
                    tier: int, audited: set[str]) -> None:
    for name in names:
        fn = reg.get(name)
        if not rep.check(f"{name}: registered in action_registry.inc", fn is not None,
                         f"classified Tier-{tier} but no BRIDGE_ACTION line: nothing serves it"):
            continue
        path = b.closure(fn)
        if not rep.check(f"{name} ({fn}): body found in bridge/src", bool(path),
                         f"no definition of {fn} in any bridge/src/*.cpp"):
            continue
        if fn in audited:
            continue                       # an alias of a handler already audited
        audited.add(fn)
        raw = {f"{n} ({f})": raw_reads(body) for f, n, body in path if raw_reads(body)}
        rep.check(f"{name} ({fn}): zero raw read<>/read_offset<> in it and its callees", not raw,
                  "raw reads: " + ", ".join(f"{k}: {v}" for k, v in raw.items()))
        safe = sum(safe_reads(body) for _, _, body in path)
        # An address formed from the image (not `x - g_base_addr`, an RVA for a
        # log line: bridge.h log_degraded_read, reached once headers are parsed).
        touches = safe or raw or any(
            re.search(r"(?<!-)(?<!-\s)\bg_base_addr\b|reinterpret_cast<uintptr_t>", body)
            for _, _, body in path)
        if touches:
            rep.check(f"{name} ({fn}): reads game memory through safe_read", safe >= 1,
                      "reads game memory and no safe_read on its path")
        own_file, own = path[0][0], path[0][2]
        if safe:
            if rep.check(f"{name} ({fn}): checks the safe_read fault flag before it answers",
                         has_fault_check(own),
                         "its path reads through safe_read and its body has neither "
                         "safe_read_clear()+safe_read_had_fault() nor a FaultScope: a fault "
                         "answers a silent 0"):
                late = fault_bracket_misplaced(b, own_file, own)
                rep.check(f"{name} ({fn}): the fault bracket opens before its first read", late is None,
                          f"the clear / FaultScope comes after {late}: that read's fault is wiped or unseen")
                undecided = undecided_fault_checks(own)
                rep.check(f"{name} ({fn}): every fault check decides (returns, says degraded, or picks a branch)",
                          not undecided, "; ".join(undecided))
        if CHAR_RESOLVE_RE.search(own):
            rep.check(f"{name} ({fn}): returns early when the character does not resolve",
                      bool(CHAR_EARLY_RETURN_RE.search(own)),
                      "resolves a character with no is_sentinel / null / character_not_found return")


def character_keyed_handlers(b: Bridge) -> list[tuple[str, str]]:
    """(name, function) for every Tier-2 handler whose own body resolves a
    character from the request (the ones a forged character_id reaches)."""
    reg = b.registry()
    out = []
    for name in b.classifier_names():
        fn = reg.get(name)
        path = b.closure(fn, depth=0) if fn else []
        if path and CHAR_RESOLVE_RE.search(path[0][2]):
            out.append((name, fn))
    return out


# ── B: the dispatch ──────────────────────────────────────────────────────────

GUARD_RE = re.compile(r"if\s*\(\s*sigsetjmp\s*\(\s*g_recovery_point\s*,\s*1\s*\)\s*==\s*0\s*\)\s*\{")


def tier_branch(ipc: str, classifier: str) -> tuple[int, int] | None:
    """The span of the `else if (<classifier>(action_type_buf)) { ... }`
    branch of the reader's dispatch, in the blanked text."""
    m = re.search(r"\}\s*else\s+if\s*\(\s*" + classifier + r"\s*\(\s*action_type_buf\s*\)\s*\)\s*\{", ipc)
    return _span_at(ipc, m.end() - 1) if m else None


def unguarded_dispatches(branch: str) -> int:
    """dispatch_action_with_response calls in `branch` that are not the first
    statement of a sigsetjmp guard's taken arm."""
    guarded = set()
    for m in GUARD_RE.finditer(branch):
        arm = _block_at(branch, m.end() - 1) or ""
        for d in re.finditer(r"\bdispatch_action_with_response\s*\(", arm):
            guarded.add(m.end() - 1 + d.start())
    return sum(1 for d in re.finditer(r"\bdispatch_action_with_response\s*\(", branch)
               if d.start() not in guarded)


def section_b(b: Bridge, rep: Report) -> None:
    rep.section("(B) DISPATCH — each inline tier runs its handler under sigsetjmp")
    ipc = b.text("ipc_server.cpp")
    ipc_kept = b.text("ipc_server.cpp", keep_strings=True)
    for tier in ("is_bridge_internal_query", "is_game_state_query"):
        span = tier_branch(ipc, tier)
        if not rep.check(f"{tier} branch found in ipc_server.cpp", span is not None):
            continue
        branch = ipc[span[0]:span[1]]
        # L13 D1: a branch that hands the handler to the game thread does so
        # through serve_on_game_thread; its body is the dispatch site then.
        if re.search(r"\bserve_on_game_thread\s*\(", branch):
            hm = re.search(r"\bstatic\s+void\s+serve_on_game_thread\s*\([^)]*\)\s*\{", ipc)
            helper = (_block_at(ipc, hm.end() - 1) or "") if hm else ""
            rep.check(f"{tier} branch: serve_on_game_thread is defined in ipc_server.cpp", bool(helper))
            branch = branch + "\n" + helper
        calls = len(re.findall(r"\bdispatch_action_with_response\s*\(", branch))
        rep.check(f"{tier} branch dispatches a handler", calls >= 1, f"{calls} dispatch calls")
        n = unguarded_dispatches(branch)
        rep.check(f"{tier} branch: every dispatch inside a sigsetjmp guard", n == 0,
                  f"{n} dispatch call(s) outside a guard")
        rep.check(f"{tier} branch: the recovery answers memory_access_fault",
                  "MEMORY_ACCESS_FAULT_REPLY" in branch)
    # is_mapped() reads a range table refreshed only every 100th tick; the drain
    # must refresh it before it runs the Tier-2 reads (refresh_ranges_for_reads),
    # or storage the engine allocated since reads as unmapped.
    init = b.text("bridge_init.cpp")
    # The drain's body is drain_game_thread_work_at (the contract's
    # drain_game_thread_work() is its session-pass wrapper).
    m = re.search(r"\bvoid\s+drain_game_thread_work_at\s*\([^)]*\)\s*\{", init)
    drain = (_block_at(init, m.end() - 1) or "") if m else ""
    h = re.search(r"\bstatic\s+void\s+refresh_ranges_for_reads\s*\([^)]*\)\s*\{", init)
    helper = (_block_at(init, h.end() - 1) or "") if h else ""
    refresh = re.search(r"\brefresh_ranges_for_reads\s*\(", drain)
    jobs = re.search(r"\brun_game_thread_jobs\s*\(", drain)
    rep.check("drain_game_thread_work: the range table is refreshed before the game-thread reads run",
              bool(refresh and jobs and refresh.start() < jobs.start()
                   and re.search(r"\brefresh_memory_map\s*\(\s*\)", helper)),
              f"drain found: {bool(drain)}; refresh call: {bool(refresh)}; jobs call: {bool(jobs)}; "
              f"helper refreshes: {bool(re.search(r'refresh_memory_map', helper))}")



# ── C: the one clock write (L13 D4) ──────────────────────────────────────────

#: Where the clock may be written: clock_write, the three static primitives it
#: calls, and check_delayed_unpause (the deferred PAUSE_SET(0) repeat that is
#: part of an unpause clock_write made). All in event_handler.cpp.
CLOCK_HOME = "event_handler.cpp"
CLOCK_PRIMITIVES = ("pause_primitive", "unpause_primitive", "speed_primitive")
CLOCK_ALLOWED = ("clock_write",) + CLOCK_PRIMITIVES + ("check_delayed_unpause",)
#: The clock's state words (stores) and the engine's clock setters (calls). Any
#: qualifier counts (ck3::idler::, game_state::, a namespace alias), and a
#: using-declaration makes the bare name a mark in that file.
CLOCK_FIELDS = ("PAUSE_FLAG", "GAME_SPEED", "EVENT_PAUSE_COUNT", "DATE_PAUSE_COUNT")
CLOCK_SETTERS = ("PAUSE_SET", "PAUSE_TOGGLE", "PAUSE_GAME_CMD_CTOR",
                 "SPEED_INCREMENT_FN", "SPEED_DECREMENT_FN", "CMD_PAUSE_ON")
_STORE_OP = r"\s*(?:=(?!=)|\|=|&=|\^=|\+=|-=|<<=|>>=|\+\+|--)"


@functools.lru_cache(maxsize=None)
def _mark_names(text: str) -> tuple[str, ...]:
    """The bare names a using-declaration in `text` brings into scope."""
    return tuple(m.group(1) for m in re.finditer(
        r"\busing\s+(?:\w+::)+(" + "|".join(CLOCK_FIELDS + CLOCK_SETTERS) + r")\s*;", text))


@functools.lru_cache(maxsize=None)
def _field_re(text: str) -> str:
    bare = [n for n in _mark_names(text) if n in CLOCK_FIELDS]
    alts = [r"\b\w+::(?:" + "|".join(CLOCK_FIELDS) + r")\b"]
    if bare:
        alts.append(r"(?<![\w:])(?:" + "|".join(bare) + r")\b")
    return "(?:" + "|".join(alts) + ")"


@functools.lru_cache(maxsize=None)
def _setter_re(text: str) -> str:
    bare = [n for n in _mark_names(text) if n in CLOCK_SETTERS]
    q = r"(?:\w+::)*(?:" + "|".join(CLOCK_SETTERS) + r")\b"
    if bare:
        q = r"(?:(?:\w+::)*(?:" + "|".join(CLOCK_SETTERS) + r")|(?<![\w:])(?:" + "|".join(bare) + r"))\b"
    return q


@functools.lru_cache(maxsize=None)
def clock_mark_re(text: str) -> "re.Pattern[str]":
    bare = _mark_names(text)
    alts = [r"\b\w+::(?:" + "|".join(CLOCK_FIELDS + CLOCK_SETTERS) + r")\b"]
    if bare:
        alts.append(r"(?<![\w:])(?:" + "|".join(bare) + r")\b")
    return re.compile("|".join(alts))


#: The GCC/Clang atomic builtins that store (every __atomic_* / __sync_* but
#: the loads and the fences).
_ATOMIC_STORE_RE = re.compile(
    r"\b(?!__atomic_load|__atomic_thread_fence|__atomic_signal_fence|__atomic_always_lock_free"
    r"|__atomic_is_lock_free)(?:__atomic_\w+|__sync_\w+)\s*\(")
#: A C-style pointer cast: `(T*)`, `(const volatile T**)`, `(ns::T<…>*)`.
_CSTYLE_CAST = r"\(\s*(?:const\s+|volatile\s+)*[A-Za-z_][\w:]*(?:\s*<[^;{}<>()]*>)?\s*\*+\s*\)\s*"


def clock_write_sites(body: str, file_text: str = "") -> list[str]:
    """The clock writes in one function body:
      - a call of a clock setter through the image: game_fn / resolve /
        g_base_addr + …, naming the setter or a variable this body set from it
        (`auto off = ck3::funcs::PAUSE_SET; game_fn(off)`);
      - a store, plain or compound (|=, +=, ++ …), through
        `*reinterpret_cast<T*>(EXPR)` or the C-style `*(T*)EXPR`, whose EXPR is,
        or uses a variable computed in this body from, a clock field;
      - a store through such a variable: `*p =`, `p[k] =`, `p->x =`, and a
        plain `r =` when the variable is a reference
        (`auto& r = *reinterpret_cast<uint8_t*>(… PAUSE_FLAG); r = 1`);
      - a store builtin (write_to_readonly, memcpy, memset, memmove,
        ScopedWritable, safe_write*, every __atomic_* / __sync_* but the loads
        and fences) whose arguments use a clock field or such a variable.
    Stated limits (out of reach, and why): a clock address or setter passed
    INTO a callee as a parameter and written there (the binding is per body,
    not across calls); one computed in another function, a global, a lambda
    capture used after the lambda's body, or a macro; a store through
    std::atomic_ref / std::atomic<T>* views, or a type-punning union; a
    template argument carrying the offset; and arithmetic that reaches the
    field's address without naming it (PAUSE_FLAG - 1 + 1 names it; a literal
    0x… equal to it does not)."""
    ctx = file_text or body
    field = _field_re(ctx)
    setter = _setter_re(ctx)
    # Variables set from a setter, transitively.
    s_bound: set[str] = set()
    while True:
        refs = setter + ("|" + "|".join(r"\b" + re.escape(v) + r"\b" for v in s_bound) if s_bound else "")
        new = set(re.findall(r"\b([A-Za-z_]\w*)\s*=(?!=)\s*(?:\([^;()]*\)\s*)?(?:static_cast\s*<[^;>]*>\s*\()?"
                             r"\s*(?:" + refs + r")(?!\s*\()", body)) - s_bound
        if not new:
            break
        s_bound |= new
    target = "(?:" + setter + "".join(r"|\b" + re.escape(v) + r"\b" for v in s_bound) + ")"
    sites = [m.group(0)[:60] for m in re.finditer(
        r"\b(?:game_fn|resolve)\s*(?:<[^;()]*>)?\s*\(\s*" + target + r"|\bg_base_addr\s*\+\s*" + target, body)]
    # Variables computed from a field (an address, a typed pointer, a reference
    # to the byte), transitively within the body.
    bound: set[str] = set()
    while True:
        refs = field + ("|" + "|".join(r"\b" + re.escape(v) + r"\b" for v in bound) if bound else "")
        new = set(re.findall(r"\b([A-Za-z_]\w*)\s*=(?!=)[^;]*?(?:" + refs + ")", body))
        new |= set(re.findall(r"&\s*([A-Za-z_]\w*)\s*[({][^;]*?(?:" + refs + ")", body))
        new -= bound
        if not new:
            break
        bound |= new
    refs_set = {v for v in bound if re.search(r"&\s*" + re.escape(v) + r"\s*[=({]", body)}
    def refers(expr: str) -> bool:
        return bool(re.search(field, expr)) or any(re.search(r"\b" + re.escape(v) + r"\b", expr) for v in bound)
    def stored(m_start: int, j: int) -> bool:
        before = body[max(0, m_start - 3):m_start]
        return bool(re.match(_STORE_OP, body[j:j + 4]) or re.search(r"(?:\+\+|--)\s*$", before))
    # *reinterpret_cast<T*>(EXPR) OP   and   ++/-- before it
    for m in re.finditer(r"\*\s*reinterpret_cast\s*<[^>]*\*\s*>\s*\(", body):
        i = body.index("(", m.end() - 1)
        j = _paren_end(body, i)                  # just past the matching ')'
        if stored(m.start(), j) and refers(body[i + 1:j - 1]):
            sites.append(body[m.start():j][:60])
    # *(T*)(EXPR) OP  /  *(T*)name OP
    for m in re.finditer(r"\*\s*" + _CSTYLE_CAST + r"(?=[(A-Za-z_])", body):
        k = m.end()
        if body[k] == "(":
            j = _paren_end(body, k)
            expr = body[k + 1:j - 1]
        else:
            n = re.match(r"[A-Za-z_][\w:]*", body[k:])
            j, expr = k + n.end(), n.group(0)
        if stored(m.start(), j) and refers(expr):
            sites.append(body[m.start():j][:60])
    # *p OP, p[k] OP, p->x OP for a bound pointer p; r OP for a bound reference r
    for v in bound:
        for m in re.finditer(r"(?:\*\s*\(?\s*" + re.escape(v) + r"\b\s*\)?|(?<![\w.>])" + re.escape(v)
                             + r"\s*(?:\[[^\]]*\]|->\s*\w+))" + _STORE_OP, body):
            sites.append(m.group(0)[:60])
    for v in refs_set:
        for m in re.finditer(r"(?<![\w.>:&*])" + re.escape(v) + r"\b(?=" + _STORE_OP + r")"
                             r"|(?:\+\+|--)\s*" + re.escape(v) + r"\b", body):
            if re.search(r"&\s*$", body[max(0, m.start() - 4):m.start()]):
                continue                         # the declaration itself
            sites.append(body[m.start():m.end() + 8][:60])
    for rx in (re.compile(r"\b(?:write_to_readonly|memcpy|memset|memmove|ScopedWritable|safe_write\w*)\s*\("),
               _ATOMIC_STORE_RE):
        for m in rx.finditer(body):
            i = body.index("(", m.end() - 1)
            args = body[i + 1:_paren_end(body, i) - 1]
            if refers(args):
                sites.append(args[:60])
    return sites


def _clock_sources(b: Bridge) -> dict[str, str]:
    """Every bridge source a clock write could hide in: src/ (with .inc and the
    os/ tree) and include/, stripped. FAILS OPEN: an unreadable file is listed
    with text None so the row reds."""
    out: dict[str, str] = {}
    pats = ("src/**/*.cpp", "src/**/*.c", "src/**/*.h", "src/**/*.inc", "include/**/*.h", "include/**/*.inc")
    for pat in pats:
        for p in sorted(b.dir.glob(pat)):
            rel = str(p.relative_to(b.dir / "src")) if p.is_relative_to(b.dir / "src") else str(p.relative_to(b.dir))
            try:
                out[rel] = strip_code(p.read_text(errors="strict"))
            except (OSError, UnicodeDecodeError, cpp_strip.CppParseError):
                out[rel] = None
    return out


def section_c(b: Bridge, rep: Report) -> None:
    rep.section("(C) CLOCK — every pause/speed write is clock_write's (L13 D4)")
    sources = _clock_sources(b)
    unreadable = [f for f, t in sources.items() if t is None]
    rep.check("every bridge source file parsed (fails open)", not unreadable,
              "unreadable: " + ", ".join(unreadable))
    funcs: dict[str, list[tuple[str, str, bool]]] = {}
    for fname, text in sources.items():
        if text is None:
            continue
        for name, body, is_static, _ in _definitions(text):
            funcs.setdefault(name, []).append((fname, body, is_static))
    home = sources.get(CLOCK_HOME) or ""
    rep.check(f"{CLOCK_HOME} present", bool(home))
    for fn in CLOCK_ALLOWED:
        defs = [d for d in funcs.get(fn, []) if d[0] == CLOCK_HOME]
        rep.check(f"{fn}: defined in {CLOCK_HOME}", bool(defs),
                  "the clock's one writer is missing, so no other site can be judged against it")
    for fn in CLOCK_PRIMITIVES:
        defs = [d for d in funcs.get(fn, []) if d[0] == CLOCK_HOME]
        rep.check(f"{fn}: static to {CLOCK_HOME}", bool(defs) and all(st for _, _, st in defs),
                  "a non-static primitive is a second door to the clock")
        callers = sorted({f"{f}:{n}" for n, ds in funcs.items() for f, body, _ in ds
                          if n != "clock_write" and re.search(r"\b" + fn + r"\s*\(", body)})
        rep.check(f"{fn}: called from clock_write alone", not callers,
                  "also called from " + ", ".join(callers))
    allowed = {(CLOCK_HOME, a) for a in CLOCK_ALLOWED}
    outside, unplaced = [], []
    for fname, text in sources.items():
        if text is None:
            continue
        mark = clock_mark_re(text)
        in_bodies = 0
        for name, ds in funcs.items():
            for f, body, _ in ds:
                if f != fname:
                    continue
                in_bodies += len(mark.findall(body))
                if (fname, name) in allowed:
                    continue
                for site in clock_write_sites(body, text):
                    outside.append(f"{fname}:{name}: {site}")
        # FAILS OPEN: a mark the function parse did not place in any body is a
        # site this row cannot judge, so it reds rather than passing unseen.
        if len(mark.findall(text)) > in_bodies:
            unplaced.append(fname)
    rep.check("no pause-flag / PAUSE_SET / speed write outside clock_write", not outside,
              "; ".join(outside))
    rep.check("every clock mark in bridge/src and bridge/include is inside a parsed function (fails open)",
              not unplaced, "unplaced marks in " + ", ".join(unplaced))

# ── F: file-wide ─────────────────────────────────────────────────────────────

#: L13 (witness W9): no reply the bridge builds tells the agent to run the
#: clock. Matched in the string literals of bridge/src (comments blanked).
_CLOCK_ADVICE_RE = re.compile(
    r"must be unpaused|unpause (?:it|the game|first|one tick)|for (?:at least )?one tick|"
    r"must run for one tick|pause_state_required|Call unpause_game", re.IGNORECASE)


def section_f2(b: Bridge, rep: Report) -> None:
    rep.section("(F2) no reply advises running the clock (W9)")
    hits = []
    for p in sorted((b.dir / "src").glob("**/*.cpp")) + sorted((b.dir / "src").glob("**/*.inc")):
        text = strip_code(p.read_text(errors="replace"), keep_strings=True)
        # Raw strings first (R"(...)"), then ordinary literals.
        for m in re.finditer(r'R"\((?:.|\n)*?\)"|"(?:[^"\\\n]|\\.)*"', text):
            if _CLOCK_ADVICE_RE.search(m.group(0)):
                line = text.count("\n", 0, m.start()) + 1
                hits.append(f"{p.relative_to(b.dir)}:{line}: {m.group(0)[:70]}")
    rep.check("no string literal in bridge/src tells the caller to unpause or wait for a tick",
              not hits, "; ".join(hits[:12]))


def section_f(b: Bridge, rep: Report) -> None:
    rep.section("(F) FILE-WIDE — query_handlers.cpp and the recovery string")
    qh = b.files.get("query_handlers.cpp", "")
    rep.check("query_handlers.cpp present", bool(qh))
    n = raw_reads(qh)
    rep.check("query_handlers.cpp: zero raw read<>/read_offset<>", n == 0, f"{n} raw reads")
    hdr = b.dir / "include" / "bridge.h"
    bridge_h = strip_code(hdr.read_text(errors="replace"), keep_strings=True) if hdr.is_file() else ""
    rep.check("bridge.h defines MEMORY_ACCESS_FAULT_REPLY as the memory_access_fault reply",
              bool(re.search(r'MEMORY_ACCESS_FAULT_REPLY\[\]\s*=\s*"\{\\"error\\":\\"memory_access_fault\\"\}"',
                             bridge_h)))
    ipc_kept = b.text("ipc_server.cpp", keep_strings=True)
    sites = len(re.findall(r"\bMEMORY_ACCESS_FAULT_REPLY\b", b.text("ipc_server.cpp")))
    rep.check("ipc_server.cpp answers MEMORY_ACCESS_FAULT_REPLY (2-4 sites) and spells no second copy",
              2 <= sites <= 4 and "memory_access_fault" not in ipc_kept,
              f"{sites} sites; literal present: {'memory_access_fault' in ipc_kept}")
    adi = b.dir / "include" / "action_dispatch_internal.h"
    adi_text = strip_code(adi.read_text(errors="replace")) if adi.is_file() else ""
    m = re.search(r"\bclass\s+FaultScope\b[^{]*\{", adi_text)
    cls = (_block_at(adi_text, m.end() - 1) or "") if m else ""
    ctor = re.search(r"FaultScope\s*\([^)]*\)\s*:[^{]*\{", cls)
    dtor = re.search(r"~FaultScope\s*\(\s*\)\s*\{", cls)
    ctor_body = (_block_at(cls, ctor.end() - 1) or "") if ctor else ""
    dtor_body = (_block_at(cls, dtor.end() - 1) or "") if dtor else ""
    rep.check("FaultScope: the constructor calls safe_read_clear(), the destructor safe_read_had_fault()",
              bool(FAULT_PAIR[0].search(ctor_body) and FAULT_PAIR[1].search(dtor_body)),
              "a FaultScope that does not make both calls satisfies section A by name only")
    rep.check("FaultScope nests: the constructor saves the fault record before its clear, "
              "the destructor restores it (G-nest)",
              bool(re.search(r"\bsafe_read_save\s*\(", ctor.group(0) + ctor_body if ctor else "")
                   and re.search(r"\bsafe_read_restore\s*\(", dtor_body)),
              "a FaultScope in a callee then wipes its caller's record")


#: The sections, by the name a caller can select them with.
SECTIONS = ("A0", "A2", "A3", "A", "B", "C", "F", "F2", "G")


def run_static(bridge_dir: Path, echo: bool = True, sections=None) -> Report:
    """The sections (all of them by default; a caller that knows which rows a
    change can red may name them) over the bridge tree at bridge_dir."""
    rep = Report(echo)
    b = Bridge(bridge_dir)
    want = set(SECTIONS if sections is None else sections)
    unknown = want - set(SECTIONS)
    if unknown:
        raise ValueError(f"unknown section(s) {sorted(unknown)}; have {SECTIONS}")
    for name, fn in (("A0", section_a0), ("A2", section_a2), ("A3", section_a3),
                     ("A", section_a), ("B", section_b),
                     ("C", section_c), ("F", section_f), ("F2", section_f2), ("G", section_g)):
        if name in want:
            fn(b, rep)
    return rep
