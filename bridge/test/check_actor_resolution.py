#!/usr/bin/env python3
"""check_actor_resolution.py — census: no code resolves an unnamed
actor outside the one resolver (bridge/include/actor_resolve.h).

v4 seats several rulers in one game, one bridge session each. Before this lane,
nine query sites, the decision eval, query_marriage_candidates and queue_intent
read the GLOBAL primary mirror (the FIRST claimer's ruler) or PLAYER_CHAR_ID
for a request that named no actor, and get_controlled_character fell back to
the mirror for a write: a duke's read answered about the king, and a write by
a session holding nothing acted as another seat's ruler. They now all call
resolve_actor_id. This census keeps it that way.

Enumerated from the code, failing OPEN:
  1. Every .cpp in bridge/src is read with comments blanked (cpp_strip). Every
     occurrence of a resolution PRIMITIVE —
        g_controlled_primary            (the global mirror)
        PLAYER_CHAR_ID / get_player_character_id(   (the human player)
        primary_character_id            (a session's primary, read directly)
     — is placed in its innermost enclosing function definition.
  2. An occurrence passes only inside a function DECLARED below with the reason
     it is not an actor resolution (it reports, guards, maintains or hooks), or
     in a whole file declared as the owner of those values (the resolver
     itself, the session registry). Anything else FAILS: a new handler that
     reads the mirror, the player or a primary to find "its" actor.
  3. An occurrence outside any function (a global definition, an offset table)
     passes only by declared file and exact count.
  4. A declared entry that no longer holds a primitive, or names a function
     that no longer exists, FAILS (a stale exemption is an unread site).
  5. The shared helpers must route through the resolver: get_controlled_character
     and controlled_character_reading call resolve_actor_id; resolve_actor and
     resolve_character call controlled_character_reading (it
     passes the key each reads, for actor_resolve.h rule 0) for an unnamed
     actor; the dispatch macro installs ActorDispatchScope. A helper that stops
     doing so FAILS.

    check_actor_resolution.py <bridge-dir>     exit 0 clean, 1 findings
    check_actor_resolution.py --selftest       controls: known-bad MUST flag
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import cpp_strip  # noqa: E402  (bridge/scripts/cpp_strip.py: the one C++ tokenizer)

PRIMITIVE = re.compile(r"\bg_controlled_primary\b|\bPLAYER_CHAR_ID\b|\bget_player_character_id\s*\("
                       r"|\bprimary_character_id\b")

#: Files that OWN these values and resolve no request's actor.
OWNER_FILES = {
    "actor_resolve.cpp": "THE resolver (actor_resolve.h)",
    "session.cpp": "the session registry: keeps each session's primary and rebuilds the mirror",
}

#: function -> why its primitive is not an actor resolution.
DECLARED = {
    # dispatch_router.cpp — the claim verbs and the bridge-state reports
    "get_player_character_id": "THE reader of PLAYER_CHAR_ID (the resolver and the player guard call it)",
    "reject_player_actor": "the player GUARD on an already-resolved actor (never picks one)",
    "do_set_controlled_character": "the claim verb: writes the session primary / N=1 mirror",
    "do_release_character": "the release verb: clears the primary it releases",
    "do_get_bridge_state": "reports the mirror and the player by name",
    "do_get_player_character": "reports the player by name",
    "do_get_controlled_roster": "reports the mirror and the player by name",
    # query_handlers.cpp
    "char_is_ai_controlled": "a property of a NAMED character (is it the human player)",
    "do_get_tick_info": "reports the global mirror by name: the tick hook's input, not 'me'",
    # bridge_init.cpp — tick-side steering and load-time wiring, no request
    "bridge_init": "installs the registry's player source at load",
    "on_tick": "tick-side: the steering hook's watched primary",
    "check_controlled_handoff": "tick-side: the succession handoff of a held ruler",
    # ipc_server.cpp / pipeline.cpp — bookkeeping on the session's OWN primary
    "handle_handshake_for_slot": "logs a resumed session's own primary",
    "emit_management_ledger_record": "records the calling session's own primary in its ledger row",
    "pipeline_note_drain_result": "attributes a drained ticket to its OWN session's primary",
    # event_handler.cpp — the human player's own event popups, tick-side
    "poll_and_notify_events": "tick-side: the human player's pending events",
    "auto_respond_pending_events": "tick-side: the human player's pending events",
    # intent_dispatch.cpp — the PLAYER_CHAR_ID swap around an already-resolved actor
    "execute_create_title_intent": "swaps PLAYER_CHAR_ID to the intent's NAMED actor and back",
    # snapshot.cpp — the tick snapshot of the controlled set
    "capture_snapshot": "tick-side: snapshots the controlled set (no request)",
    # runtime_resolver.cpp / offset_validator.cpp — offset tables
    "populate_hardcoded": "offset table: resolves the ADDRESS of PLAYER_CHAR_ID",
    "cross_validate": "offset table: cross-checks the ADDRESS of PLAYER_CHAR_ID",
}

#: file -> number of primitive occurrences outside any function (definitions, tables).
TOP_LEVEL = {
    "ai_eval_hook.cpp": 1,       # std::atomic<uint32_t> g_controlled_primary{0};
    "bridge_init.cpp": 1,        # the get_player_character_id() prototype
    "runtime_resolver.cpp": 1,   # the resolution table row naming PLAYER_CHAR_ID
    "offset_validator.cpp": 1,   # the validation table row
    "query_handlers.cpp": 1,     # the debug globals table row
    "pipeline.cpp": 1,           # ExecutionStamp: a buffered call stamped with its OWN session's primary
}

#: helper -> a regex its body must match (rule 5).
HELPERS = {
    "get_controlled_character": re.compile(r"\bresolve_actor_id\s*\("),
    "controlled_character_reading": re.compile(r"\bresolve_actor_id\s*\("),
    "resolve_actor": re.compile(r"\bcontrolled_character_reading\s*\(\s*params_json\s*,\s*bridge::kActorKeyActorId\s*\)"),
    "resolve_character": re.compile(r"\bcontrolled_character_reading\s*\(\s*params_json\s*,\s*bridge::kActorKeyCharacterId\s*\)"),
}

DEF = re.compile(r"(?m)^[ \t]*(?:[\w:<>,\*&]+[ \t\*&]+)+?(?:[\w:]+::)?(\w+)\s*\(([^;{}]*)\)\s*"
                 r"(?:const\s*)?(?:noexcept\s*)?\{")
KEYWORDS = {"if", "for", "while", "switch", "return", "catch", "sizeof", "else"}


def functions(text: str) -> list[tuple[str, int, int]]:
    """(name, start, end) of every function definition in comment-blanked text."""
    out = []
    for m in DEF.finditer(text):
        name = m.group(1)
        if name in KEYWORDS:
            continue
        brace = m.end() - 1
        end = cpp_strip.match_close(text, brace)
        if end < 0:
            end = len(text)
        out.append((name, m.start(), end))
    return out


def scan_text(rel: str, text: str) -> tuple[list[str], dict[str, int], dict[str, str], int]:
    """(findings, declared-function hits, helper bodies, top-level count) for one file."""
    code = cpp_strip.strip(text, keep_strings=True)     # structure: braces matched with strings kept
    text = cpp_strip.strip(text, keep_strings=False)    # primitives: a string literal is not a read
    if len(code) != len(text):
        return [f"{rel}: comment/string blanking changed the text length; cannot place reads"], {}, {}, 0
    fns = functions(code)
    findings, hits, helpers = [], {}, {}
    top = 0
    for name, s, e in fns:
        if name in HELPERS:
            helpers[name] = text[s:e]
    for m in PRIMITIVE.finditer(text):
        pos = m.start()
        inner = [f for f in fns if f[1] <= pos < f[2]]
        if not inner:
            if rel not in OWNER_FILES:
                top += 1
            continue
        name = min(inner, key=lambda f: f[2] - f[1])[0]
        if rel in OWNER_FILES:
            continue
        if name in DECLARED:
            hits[name] = hits.get(name, 0) + 1
            continue
        line = text.count("\n", 0, pos) + 1
        findings.append(f"{rel}:{line}: {name}() reads {m.group(0).strip('( ')} — resolve an "
                        f"unnamed actor through resolve_actor_id (actor_resolve.h), or declare "
                        f"why this is not an actor resolution")
    return findings, hits, helpers, top


def scan_tree(bridge: Path) -> list[str]:
    src = bridge / "src"
    files = sorted(src.glob("*.cpp"))
    if len(files) < 20:
        return [f"{src}: only {len(files)} .cpp files — not a bridge tree"]
    findings: list[str] = []
    hits: dict[str, int] = {}
    helpers: dict[str, str] = {}
    tops: dict[str, int] = {}
    for f in files:
        fnd, h, hl, top = scan_text(f.name, f.read_text(encoding="utf-8", errors="replace"))
        findings += fnd
        for k, v in h.items():
            hits[k] = hits.get(k, 0) + v
        helpers.update(hl)
        if top:
            tops[f.name] = top
    for name in sorted(DECLARED):
        if name not in hits:
            findings.append(f"declared {name}() holds no primitive (or no longer exists): "
                            f"drop the stale exemption")
    if tops != {k: v for k, v in TOP_LEVEL.items()}:
        findings.append(f"top-level primitive occurrences {tops} != declared {TOP_LEVEL}")
    for name, rx in HELPERS.items():
        body = helpers.get(name)
        if body is None:
            findings.append(f"helper {name}() not found: the census cannot see the resolver path")
        elif not rx.search(body):
            findings.append(f"helper {name}() does not route through the resolver ({rx.pattern})")
    router = (src / "dispatch_router.cpp").read_text(encoding="utf-8", errors="replace")
    macro = re.search(r"#define BRIDGE_ACTION\(NAME, HANDLER\)(?:.*\\\n)*.*", router)
    if not macro or "ActorDispatchScope" not in macro.group(0):
        findings.append("dispatch_router.cpp: the dispatching BRIDGE_ACTION macro does not "
                        "install ActorDispatchScope")
    return findings


def selftest() -> int:
    bad = 0
    cases = [
        ("bool do_query_new(const char* p, std::string* out) {\n"
         "    uint32_t c = g_controlled_primary.load(std::memory_order_relaxed);\n    return c;\n}\n", True),
        ("static bool do_x(const char* p, std::string* o) {\n"
         "    if (id == 0) id = safe_read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);\n}\n", True),
        ("bool do_y(const char* p, std::string* o) {\n"
         "    std::lock_guard<std::mutex> lk(s->mutex); uint32_t c = s->primary_character_id;\n}\n", True),
        ("bool do_z(const char* p, std::string* o) {\n"
         "    // g_controlled_primary in a comment is not a read\n"
         "    uint32_t c = resolve_actor_id(0, LegacyChain::MirrorOnly).id;\n}\n", False),
        ("bool do_get_tick_info(const char* p, std::string* o) {\n"
         "    uint32_t c = g_controlled_primary.load();\n}\n", False),
    ]
    for i, (text, want) in enumerate(cases):
        got = bool(scan_text("selftest.cpp", text)[0])
        ok = got == want
        print(f"selftest case {i}: {'flagged' if got else 'clean'} — {'ok' if ok else 'WRONG'}")
        bad += not ok
    return 1 if bad else 0


def main(argv: list[str]) -> int:
    if argv[1:] == ["--selftest"]:
        return selftest()
    if len(argv) != 2:
        print(__doc__)
        return 2
    findings = scan_tree(Path(argv[1]))
    for f in findings:
        print(f"FAIL {f}")
    print(f"check_actor_resolution: {len(findings)} finding(s)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
