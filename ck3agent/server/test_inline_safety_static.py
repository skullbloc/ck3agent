"""The static half of the bridge's inline-safety audit, in the gate (D22).

bridge/scripts/inline_safety_static.py reads the bridge source: every handler
the IPC reader thread serves inline -- Tier 1 (snapshot.cpp
is_bridge_internal_query, on the reader thread) and Tier 2 (is_game_state_query,
on the game thread through the drain), each resolved through action_registry.inc -- reads game memory through
safe_read with no raw read (read<>, read_offset<>, or a `*reinterpret_cast<T*>`
read) in it or its callees, checks the safe_read fault flag by name before it
answers (safe_read_clear() + safe_read_had_fault(), or a FaultScope: a fault is
memory_access_fault or degraded, never a silent 0), returns early on a
character that did not resolve, and every inline dispatch in ipc_server.cpp is
inside a sigsetjmp guard whose recovery answers memory_access_fault. Since L14
the game-thread actions (Tier 3, is_game_thread_action, section G) are graded by
THE RULE in the harness docstring: no raw read, a fault bracket, a decision
before the first engine contact, every engine call under a watchdog, no sleep or
wait, no clock write, declared exceptions parsed from snapshot.cpp. The dump
half (bridge/scripts/test_inline_safety.py) is not part of any gate.

Both layouts carry bridge/src and bridge/scripts/inline_safety_static.py at
the same place (the public export ships both), so this skips only where the
bridge sources are truly absent. The mutants below run against a copy of the
tree and must red, in every tier.
"""
from __future__ import annotations

import importlib.util
import re
import shutil
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BRIDGE = REPO / "bridge"
STATIC = BRIDGE / "scripts" / "inline_safety_static.py"

if not (BRIDGE / "src" / "snapshot.cpp").is_file():
    pytest.skip(f"no bridge sources in this layout ({BRIDGE / 'src'})", allow_module_level=True)


def _module():
    assert STATIC.is_file(), f"bridge sources are here but the audit is not: {STATIC}"
    spec = importlib.util.spec_from_file_location("inline_safety_static", STATIC)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


S = _module()


def _fails(bridge_dir: Path, *sections: str) -> list:
    """The failing rows. A mutant names the section(s) whose rows its change can
    red (S.SECTIONS); the controls run every section. Selecting is not
    weakening: each mutant asserts a row of the section it runs."""
    return S.run_static(bridge_dir, echo=False, sections=sections or None).failures


def test_the_static_audit_is_green_on_this_tree():
    rep = S.run_static(BRIDGE, echo=False)
    assert not rep.failures, "\n".join(f"{n}: {d}" for _, n, d in rep.failures)


@pytest.mark.parametrize("classifier", S.TIERS + (S.GAME_THREAD_ACTIONS,))
def test_every_classified_handler_is_audited(classifier):
    """Not a discriminating control: pins that the audit found the handlers it
    claims, in each tier, the game-thread actions (Tier 3, section G) included
    (a parse that matched nothing would audit nothing and pass)."""
    b = S.Bridge(BRIDGE)
    names = b.classifier_names(classifier)
    # The fail-OPEN floor is the population pinned in the harness
    # (CLASSIFIER_FLOOR): 46 bridge-internal handlers, 87 game-state reads and
    # 22 game-thread actions since L16 R3 deleted the event handlers and
    # query_pending_events (47/90/23 before). A parse that found fewer would audit fewer and pass.
    assert len(names) >= S.CLASSIFIER_FLOOR[classifier], (len(names), names)
    rows = [n for _, n, _ in S.run_static(BRIDGE, echo=False).rows]
    assert f"{classifier}() parsed" in rows
    for name in names:
        assert any(r.startswith(f"{name}: registered") for r in rows), name


@pytest.fixture
def tree(tmp_path):
    """A copy of bridge/src and bridge/include to mutate."""
    dst = tmp_path / "bridge"
    shutil.copytree(BRIDGE / "src", dst / "src")
    shutil.copytree(BRIDGE / "include", dst / "include")
    return dst


def test_MUTANT_a_handler_safe_read_made_raw_reds(tree):
    p = tree / "src" / "query_handlers.cpp"
    s = p.read_text()
    old = "uint8_t is_ended = safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED);"
    i = s.index(old, s.index("bool do_query_wars("))
    p.write_text(s[:i] + old.replace("safe_read<", "read<") + s[i + len(old):])
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert "query_wars (do_query_wars): zero raw read<>/read_offset<> in it and its callees" in msgs, msgs


def test_MUTANT_a_callee_raw_read_reds_its_handlers(tree):
    p = tree / "src" / "dispatch_router.cpp"
    s = p.read_text()
    old = "return safe_read<uint32_t>(reinterpret_cast<uintptr_t>(character) + ck3::character::ID);"
    assert s.count(old) == 1
    p.write_text(s.replace(old, old.replace("safe_read<", "read<")))
    msgs = [m for _, m, d in _fails(tree, "A") if "char_get_id" in d]
    assert len(msgs) >= 10, msgs


def test_MUTANT_a_tier1_handler_safe_read_made_raw_reds(tree):
    """get_ai_personality is Tier 1: served inline while the game RUNS."""
    p = tree / "src" / "personality.cpp"
    s = p.read_text()
    old = "uintptr_t personality_ptr = safe_read<uintptr_t>(core_data + ck3::core_data::PERSONALITY_PTR);"
    i = s.index(old, s.index("bool do_get_ai_personality("))
    p.write_text(s[:i] + old.replace("safe_read<", "read<") + s[i + len(old):])
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert ("get_ai_personality (do_get_ai_personality): zero raw read<>/read_offset<> "
            "in it and its callees") in msgs, msgs


# L13 FF1-B retargeted the next four mutants from set_ai_personality, which is a
# game-thread ACTION now (is_game_thread_action: section A's rows do not name
# it; section G grades it since L14, test_MUTANT_G_* below), to handlers
# section A covers: get_ai_personality (a game-state read) and
# set_controlled_character (bridge-internal). On set_ai_personality they would
# pass silently (no section-A row names it), which is what the gate caught.
_GET_PERS_PTR = "uintptr_t personality_ptr = safe_read<uintptr_t>(core_data + ck3::core_data::PERSONALITY_PTR);"


def _mutate_get_ai_personality(tree, old, new):
    p = tree / "src" / "personality.cpp"
    s = p.read_text()
    i = s.index(old, s.index("bool do_get_ai_personality("))
    p.write_text(s[:i] + new + s[i + len(old):])


def test_MUTANT_a_tier1_cast_dereference_read_reds(tree):
    """A raw `*reinterpret_cast<T*>(addr)` read is a raw read too."""
    _mutate_get_ai_personality(tree, _GET_PERS_PTR, _GET_PERS_PTR.replace(
        "safe_read<uintptr_t>(core_data + ck3::core_data::PERSONALITY_PTR)",
        "*reinterpret_cast<uintptr_t*>(core_data + ck3::core_data::PERSONALITY_PTR)"))
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert ("get_ai_personality (do_get_ai_personality): zero raw read<>/read_offset<> "
            "in it and its callees") in msgs, msgs


def test_cast_dereference_writes_and_local_buffers_are_not_reads():
    """The detector's two exclusions, each beside a read it must count."""
    assert S.deref_reads("{ *reinterpret_cast<int8_t*>(p + i) = 1; }") == 0
    assert S.deref_reads("{ int v = *reinterpret_cast<int8_t*>(p + i); }") == 1
    assert S.deref_reads("{ if (*reinterpret_cast<int*>(p) == 1) {} }") == 1
    assert S.deref_reads("{ char buf[32] = {}; size_t n = *reinterpret_cast<size_t*>(buf + 16); }") == 0
    assert S.deref_reads("{ alignas(8) uint8_t cmd[0x30] = {}; f(*reinterpret_cast<uintptr_t*>(cmd + 0x18)); }") == 0


# ── ff3 N2: the five forms witness #9 found hidden or unstated, each a read ──
# (each was GREEN under the fix-8 detector, blob dbf4a91d's successor at 720e5757)

def test_N2_an_array_of_game_pointers_is_not_a_local_buffer():
    body = "{ uintptr_t ptrs[4] = {a, b, c, d}; int v = *reinterpret_cast<int*>(ptrs[0] + 8); }"
    assert S.deref_reads(body) == 1


def test_N2_a_subscript_statement_is_not_a_declaration():
    body = "{ p[0]; int v = *reinterpret_cast<int*>(p + 8); }"
    assert S.deref_reads(body) == 1


def test_N2_a_nested_template_cast_is_seen():
    body = "{ auto a = *reinterpret_cast<std::array<char, 8>*>(obj + 0x10); }"
    assert S.deref_reads(body) == 1


def test_N2_a_c_style_cast_dereference_is_seen():
    assert S.deref_reads("{ uint32_t id = *(uint32_t*)(obj + 0x10); }") == 1
    assert S.deref_reads("{ uint32_t id = *(const uint32_t*)obj; }") == 1
    assert S.deref_reads("{ *(uint32_t*)(obj + 0x10) = 7; }") == 0           # a write
    assert S.deref_reads("{ char b[8]; uint32_t x = *(uint32_t*)(b + 4); }") == 0   # local bytes


def test_N2_a_cast_then_index_is_seen():
    assert S.deref_reads("{ int v = reinterpret_cast<int32_t*>(arr)[i]; }") == 1
    assert S.deref_reads("{ reinterpret_cast<int32_t*>(arr)[i] = 0; }") == 0   # a write
    assert S.deref_reads("{ auto p = reinterpret_cast<int32_t*>(arr); }") == 0  # a cast, no read


def test_MUTANT_N2_a_c_style_read_in_a_tier1_handler_reds(tree):
    _mutate_get_ai_personality(tree, _GET_PERS_PTR, _GET_PERS_PTR.replace(
        "safe_read<uintptr_t>(core_data + ck3::core_data::PERSONALITY_PTR)",
        "*(uintptr_t*)(core_data + ck3::core_data::PERSONALITY_PTR)"))
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert ("get_ai_personality (do_get_ai_personality): zero raw read<>/read_offset<> "
            "in it and its callees") in msgs, msgs


# ── ff3 N1: every inline handler that reads through safe_read checks the flag ──

_FAULT_ROW = "{name} ({fn}): checks the safe_read fault flag before it answers"


def test_MUTANT_N1_a_swept_handler_with_its_fault_check_removed_reds(tree):
    """get_ai_personality (Tier 1): its safe_read_had_fault() checks deleted, so
    a fault reads gold/axes 0 again. The row names it."""
    p = tree / "src" / "personality.cpp"
    s = p.read_text()
    i = s.index("bool do_get_ai_personality(")
    j = s.index("\nint8_t personality_axis_value(", i)
    body = s[i:j]
    line = '    if (safe_read_had_fault()) return answer_memory_access_fault("get_ai_personality", out);\n'
    assert body.count(line) == 3, body.count(line)
    p.write_text(s[:i] + body.replace(line, "") + s[j:])
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert _FAULT_ROW.format(name="get_ai_personality", fn="do_get_ai_personality") in msgs, msgs


def test_MUTANT_N1_a_handler_with_its_clear_removed_reds(tree):
    """The check without the clear reads a stale flag: both names are required."""
    p = tree / "src" / "action_handlers.cpp"
    s = p.read_text()
    i = s.index("bool do_send_interaction(")
    k = s.index("    safe_read_clear();\n", i)
    p.write_text(s[:k] + s[k + len("    safe_read_clear();\n"):])
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert _FAULT_ROW.format(name="send_interaction", fn="do_send_interaction") in msgs, msgs


def test_MUTANT_N1_a_tier2_handler_with_its_FaultScope_removed_reds(tree):
    p = tree / "src" / "query_handlers.cpp"
    s = p.read_text()
    old = '    FaultScope fault_scope("query_council", out);\n'
    assert s.count(old) == 1
    p.write_text(s.replace(old, ""))
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert _FAULT_ROW.format(name="query_council", fn="do_query_council") in msgs, msgs


def test_MUTANT_N1_a_FaultScope_that_does_not_check_reds(tree):
    """A FaultScope whose destructor no longer asks the flag satisfies section A
    by name only; section F reds."""
    p = tree / "include" / "action_dispatch_internal.h"
    s = p.read_text()
    old = "        if (safe_read_had_fault()) {\n"
    assert s.count(old) == 1
    p.write_text(s.replace(old, "        if (false) {\n"))
    msgs = [n for _, n, _ in _fails(tree, "F")]
    assert ("FaultScope: the constructor calls safe_read_clear(), the destructor "
            "safe_read_had_fault()") in msgs, msgs


def test_MUTANT_N1_a_second_spelling_of_the_fault_reply_reds(tree):
    p = tree / "src" / "ipc_server.cpp"
    s = p.read_text()
    old = "response_out = MEMORY_ACCESS_FAULT_REPLY;"
    assert old in s
    p.write_text(s.replace(old, 'response_out = "{\\"error\\":\\"memory_access_fault\\"}";', 1))
    msgs = [n for _, n, _ in _fails(tree, "F")]
    assert ("ipc_server.cpp answers MEMORY_ACCESS_FAULT_REPLY (2-4 sites) and spells no "
            "second copy") in msgs, msgs


# ── witness #10 R5: the fault row checked presence only. Placement (the bracket
#    opens before the first read) and use (a check decides) are rows of their
#    own; the witness's mutants M1, M1b and M3a were green before them. ──

_PLACED_ROW = "{name} ({fn}): the fault bracket opens before its first read"
_DECIDES_ROW = "{name} ({fn}): every fault check decides (returns, says degraded, or picks a branch)"


def test_MUTANT_R5_M1_a_FaultScope_after_the_first_read_reds(tree):
    """get_ai_personality: the clear removed, a safe_read and then a FaultScope."""
    p = tree / "src" / "personality.cpp"
    s = p.read_text()
    old = ("bool do_get_ai_personality(const char* params_json, std::string* out) {\n"
           "    safe_read_clear();\n    void* char_ptr = resolve_character(params_json);")
    assert s.count(old) == 1
    p.write_text(s.replace(old, (
        "bool do_get_ai_personality(const char* params_json, std::string* out) {\n"
        "    void* char_ptr = resolve_character(params_json);\n"
        "    uint32_t early = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(char_ptr));\n"
        "    (void)early;\n    FaultScope late(\"get_ai_personality\", out);")))
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert _PLACED_ROW.format(name="get_ai_personality", fn="do_get_ai_personality") in msgs, msgs


def test_MUTANT_R5_M1b_a_read_before_query_councils_FaultScope_reds(tree):
    p = tree / "src" / "query_handlers.cpp"
    s = p.read_text()
    old = '    FaultScope fault_scope("query_council", out);\n'
    assert s.count(old) == 1
    p.write_text(s.replace(old, "    uint32_t pre = safe_read<uint32_t>(0x1000);\n    (void)pre;\n" + old))
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert _PLACED_ROW.format(name="query_council", fn="do_query_council") in msgs, msgs


def test_MUTANT_R5_a_bracket_after_a_call_that_reads_reds(tree):
    """The first read may be a callee's: send_interaction's clear moved below
    resolve_actor (which reads through safe_read two calls down)."""
    p = tree / "src" / "action_handlers.cpp"
    s = p.read_text()
    i = s.index("bool do_send_interaction(")
    k = s.index("    safe_read_clear();\n", i)
    body = s[k:]
    r = body.index("resolve_actor(")
    stmt_end = body.index(";\n", r) + 2
    moved = body[len("    safe_read_clear();\n"):stmt_end] + "    safe_read_clear();\n" + body[stmt_end:]
    p.write_text(s[:k] + moved)
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert _PLACED_ROW.format(name="send_interaction", fn="do_send_interaction") in msgs, msgs


def test_MUTANT_R5_M3a_a_check_that_logs_and_writes_anyway_reds(tree):
    old = '    if (safe_read_had_fault()) return answer_memory_access_fault("get_ai_personality", out);'
    _mutate_get_ai_personality(tree, old,
                               '    if (safe_read_had_fault()) { log_degraded_read("get_ai_personality"); }')
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert _DECIDES_ROW.format(name="get_ai_personality", fn="do_get_ai_personality") in msgs, msgs


def test_MUTANT_R5_a_check_the_parser_cannot_read_reds_by_name(tree):
    """The flag kept in a variable is not decided by this parser: a FAIL, never a pass."""
    p = tree / "src" / "dispatch_router.cpp"
    s = p.read_text()
    old = '        if (safe_read_had_fault()) emit_degraded(r, "set_controlled_character");'
    assert s.count(old) == 1
    p.write_text(s.replace(old, ('        const bool f = safe_read_had_fault();\n'
                                 '        if (f) emit_degraded(r, "set_controlled_character");')))
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert _DECIDES_ROW.format(name="set_controlled_character",
                               fn="do_set_controlled_character") in msgs, msgs


def test_the_decides_row_reads_the_three_shapes_the_tree_uses():
    """Not a discriminating control: the shapes that decide (a return, a
    degraded reply, a branch with an else) read as decided; a bare log does not."""
    assert S.undecided_fault_checks("{ if (safe_read_had_fault()) return answer_memory_access_fault(\"x\", out); }") == []
    assert S.undecided_fault_checks("{ if (safe_read_had_fault()) emit_degraded(r, \"x\"); }") == []
    assert S.undecided_fault_checks("{ if (a) { } else if (safe_read_had_fault()) { log(); } else { *p = 1; } }") == []
    assert S.undecided_fault_checks("{ if (safe_read_had_fault()) { log(); } *p = 1; }") != []
    assert S.undecided_fault_checks("{ if (!safe_read_had_fault()) *p = 1; }") != []


# ── witness #10 minors 6 and 7: a write before the fault check. generate_rng
#    advances the game's AI RNG seed (a store), so in a handler that brackets its
#    reads the fault check sits between the last read and each generate_rng();
#    game_pause / game_unpause / game_set_speed store nothing before their fault
#    comparison. Enumerated from the source (fail OPEN: too few sites reds). ──

def _reads(b, file, body):
    """Positions in body of a safe_read or a call whose own path reads through one."""
    out = [m.start() for m in S.SAFE_READ_RE.finditer(body)]
    for m in S._CALL_RE.finditer(body):
        c = m.group(1)
        if c in S._KEYWORDS or c == "generate_rng" or not b.resolve(c, file):
            continue
        if any(S.safe_reads(cb) for _, _, cb in b.closure(c, home=file, depth=S.CALL_DEPTH - 1)):
            out.append(m.start())
    return sorted(out)


def rng_before_its_check(bridge_dir):
    """(sites, unchecked): every generate_rng() call in a bracketed body (by name,
    or a FaultScope: witness #11 minor 3), and the ones with a read since the
    last safe_read_had_fault() check (or no check). A FaultScope checks only on
    the way out, after the draw, so its body needs its own check before it."""
    b = S.Bridge(bridge_dir)
    sites, bad = 0, []
    for name, defs in b.functions.items():
        for file, body, _ in defs:
            if "generate_rng" not in body or not (S.FAULT_PAIR[0].search(body)
                                                  or S.FAULT_SCOPE_RE.search(body)):
                continue
            reads = _reads(b, file, body)
            checks = [m.end() for m in S.FAULT_PAIR[1].finditer(body)]
            for m in re.finditer(r"\bgenerate_rng\s*\(", body):
                sites += 1
                last_check = max((c for c in checks if c < m.start()), default=None)
                if last_check is None or any(last_check < r < m.start() for r in reads):
                    bad.append(f"{file}:{name}")
    return sites, bad


def test_the_ai_rng_seed_is_advanced_only_after_the_fault_check():
    sites, bad = rng_before_its_check(BRIDGE)
    assert sites >= 9, f"too few generate_rng sites in bracketed handlers (fail OPEN): {sites}"
    assert bad == [], bad


def test_the_interaction_check_is_a_counted_rng_site():
    """The FaultScope body do_query_interaction_check draws the seed after
    lookup_interaction's reads; the census must see it (fail OPEN otherwise).
    Scope (witness #12): the census orders draws in BRACKETED bodies. Since L14
    every game-thread action brackets too (section G), do_create_title among
    them; test_MUTANT_G_create_titles_seed_drawn_before_its_fault_check_reds
    shows the census sees it."""
    [body] = [b for _, b, _ in S.Bridge(BRIDGE).functions["do_query_interaction_check"]]
    assert S.FAULT_SCOPE_RE.search(body) and "generate_rng" in body


def test_MUTANT_the_interaction_checks_seed_drawn_before_its_fault_check_reds(tree):
    p = tree / "src" / "action_handlers.cpp"
    s = p.read_text()
    old = '    if (safe_read_had_fault()) return answer_memory_access_fault("query_interaction_check", response_out);\n'
    assert s.count(old) == 1
    p.write_text(s.replace(old, ""))
    sites, bad = rng_before_its_check(tree)
    assert any(x.endswith(":do_query_interaction_check") for x in bad), bad


_CLOCK = ("pause_primitive", "unpause_primitive", "speed_primitive")  # clock_write's (L13 D4)
_STORE_RE = re.compile(r"\.store\s*\(|\*\s*reinterpret_cast\s*<[^>]*\*\s*>\s*\([^;]*?\)\s*=(?!=)")


def clock_writes_before_the_check(bridge_dir):
    b = S.Bridge(bridge_dir)
    out = {}
    for fn in _CLOCK:
        defs = b.functions.get(fn) or []
        assert defs, f"no body for {fn} (fail OPEN)"
        body = defs[0][1]
        check = re.search(r"safe_read_fault_count\s*\(\s*\)\s*!=\s*faults", body)
        assert check, f"{fn}: no fault comparison found (fail OPEN)"
        early = [m.group(0)[:40] for m in _STORE_RE.finditer(body) if m.start() < check.start()]
        if early:
            out[fn] = early
    return out


def test_the_clock_verbs_store_nothing_before_their_fault_check():
    assert clock_writes_before_the_check(BRIDGE) == {}


def test_MUTANT_the_seed_advanced_before_queue_intents_check_reds(tree):
    p = tree / "src" / "action_handlers.cpp"
    s = p.read_text()
    old = ('    if (safe_read_had_fault()) return answer_memory_access_fault("queue_intent", response_out);\n'
           '    // Generate a unique request_id')
    assert s.count(old) == 1
    p.write_text(s.replace(old, "    // Generate a unique request_id"))
    sites, bad = rng_before_its_check(tree)
    assert any(x.endswith(":do_queue_intent") for x in bad), bad


def test_MUTANT_the_pause_primitive_storing_before_its_check_reds(tree):
    p = tree / "src" / "event_handler.cpp"
    s = p.read_text()
    old = "static void pause_primitive() {\n"
    assert s.count(old) == 1
    p.write_text(s.replace(old, old + "    g_delayed_unpause_ticks.store(0, std::memory_order_relaxed);\n"))
    assert "pause_primitive" in clock_writes_before_the_check(tree)


# ── L13 D4: section C, the one clock write ─────────────────────────────────

_C_ROW = "no pause-flag / PAUSE_SET / speed write outside clock_write"


def _inject(tree, fname, anchor, text):
    p = tree / "src" / fname
    s = p.read_text()
    assert s.count(anchor) == 1, anchor
    p.write_text(s.replace(anchor, anchor + text))


def test_the_clock_section_is_green_and_covers_every_file():
    """Not a discriminating control: section C ran, over every bridge/src file."""
    rows = [n for _, n, _ in S.run_static(BRIDGE, echo=False).rows]
    assert _C_ROW in rows and "every clock mark in bridge/src and bridge/include is inside a parsed function (fails open)" in rows
    assert "clock_write: defined in event_handler.cpp" in rows


def test_MUTANT_a_pause_byte_store_outside_clock_write_reds(tree):
    _inject(tree, "dispatch_router.cpp",
            "static bool case_set_pause(const char* params_json, std::string* response_out) {\n",
            "    { void* i = nullptr; *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(i)"
            " + ck3::idler::PAUSE_FLAG) = 1; }\n")
    assert _C_ROW in [n for _, n, _ in _fails(tree, "C")]


def test_MUTANT_a_speed_store_through_a_bound_address_reds(tree):
    _inject(tree, "pipeline.cpp", "bool action_routes_to_drain(const char* action_type) {\n",
            "    { uintptr_t gs = 0; const uintptr_t at = gs + ck3::game_state::GAME_SPEED;"
            " *reinterpret_cast<int32_t*>(at) = 4; }\n")
    assert _C_ROW in [n for _, n, _ in _fails(tree, "C")]


def test_MUTANT_a_PAUSE_SET_call_outside_clock_write_reds(tree):
    _inject(tree, "action_handlers.cpp", "bool do_pause_game(const char* params_json) {\n",
            "    reinterpret_cast<void(*)(void*,int,int)>(bridge::game_fn(ck3::funcs::PAUSE_SET))"
            "(nullptr, 1, 0);\n")
    assert _C_ROW in [n for _, n, _ in _fails(tree, "C")]


def test_MUTANT_a_primitive_called_around_clock_write_reds(tree):
    _inject(tree, "event_handler.cpp", "void check_delayed_unpause() {\n", "    pause_primitive();\n")
    assert "pause_primitive: called from clock_write alone" in [n for _, n, _ in _fails(tree, "C")]


def test_MUTANT_a_mark_the_parse_cannot_place_reds_fail_open(tree):
    p = tree / "src" / "pipeline.cpp"
    s = p.read_text()
    anchor = "namespace bridge {\n"
    assert anchor in s
    p.write_text(s.replace(anchor, anchor + "static const size_t kStray = ck3::idler::PAUSE_FLAG;\n", 1))
    assert ("every clock mark in bridge/src and bridge/include is inside a parsed function (fails open)"
            in [n for _, n, _ in _fails(tree, "C")])


def test_the_fault_check_census_covers_both_tiers():
    """Not a discriminating control: the fault-check row exists for handlers of
    each tier (a rule that matched nothing would pass)."""
    rows = [n for _, n, _ in S.run_static(BRIDGE, echo=False).rows]
    fault_rows = [r for r in rows if r.endswith("checks the safe_read fault flag before it answers")]
    assert len(fault_rows) >= 100, len(fault_rows)
    for name in ("get_ai_personality", "declare_war", "send_decision", "query_council"):
        assert any(r.startswith(name + " (") for r in fault_rows), name


def test_MUTANT_a_tier1_classified_name_with_no_registry_line_reds(tree):
    snap = tree / "src" / "snapshot.cpp"
    s = snap.read_text()
    anchor = '        "get_bridge_state",\n'
    assert s.count(anchor) == 1
    snap.write_text(s.replace(anchor, anchor + '        "ff2_tier1_unregistered",\n', 1))
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert "ff2_tier1_unregistered: registered in action_registry.inc" in msgs, msgs


def test_MUTANT_a_classified_name_with_no_body_reds(tree):
    snap = tree / "src" / "snapshot.cpp"
    s = snap.read_text()
    anchor = '        "query_trigger_slots",\n'
    assert anchor in s
    snap.write_text(s.replace(anchor, anchor + '        "query_ff_mutant",\n', 1))
    reg = tree / "src" / "action_registry.inc"
    reg.write_text(reg.read_text() + '\nBRIDGE_ACTION("query_ff_mutant", do_query_ff_mutant)\n')
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert "query_ff_mutant (do_query_ff_mutant): body found in bridge/src" in msgs, msgs


def test_MUTANT_a_classified_name_with_no_registry_line_reds(tree):
    snap = tree / "src" / "snapshot.cpp"
    s = snap.read_text()
    anchor = '        "query_trigger_slots",\n'
    snap.write_text(s.replace(anchor, anchor + '        "query_ff_unregistered",\n', 1))
    msgs = [n for _, n, _ in _fails(tree, "A")]
    assert "query_ff_unregistered: registered in action_registry.inc" in msgs, msgs


def test_MUTANT_a_tier2_dispatch_outside_the_guard_reds(tree):
    p = tree / "src" / "ipc_server.cpp"
    s = p.read_text()
    i = s.index("} else if (is_game_state_query(action_type_buf)) {")
    j = s.index("{", i) + 1
    p.write_text(s[:j] + "\n            dispatch_action_with_response(action_type_buf, "
                 "inner_json.c_str(), &response_out);" + s[j:])
    msgs = [n for _, n, _ in _fails(tree, "B")]
    assert "is_game_state_query branch: every dispatch inside a sigsetjmp guard" in msgs, msgs


def test_MUTANT_no_range_refresh_before_the_game_thread_reads_reds(tree):
    p = tree / "src" / "bridge_init.cpp"
    s = p.read_text()
    old = "        refresh_ranges_for_reads(tick);\n"
    assert s.count(old) == 1
    p.write_text(s.replace(old, ""))
    msgs = [n for _, n, _ in _fails(tree, "B")]
    assert ("drain_game_thread_work: the range table is refreshed before the game-thread "
            "reads run") in msgs, msgs


def test_CONTROL_the_copied_tree_is_green(tree):
    """The mutants above red on a copy; the unmutated copy is green, so a red
    there is the mutation's, not the copy's."""
    assert _fails(tree) == []


def test_the_registry_reading_agrees_with_the_census():
    """Two readers of action_registry.inc: this audit's (it ships in the
    export) and scripts/bridge_action_census.py's (the census; research tree
    only). A disagreement would mean one of them misreads the table, so the
    mirror is checked wherever both exist."""
    census = REPO / "scripts" / "bridge_action_census.py"
    if not census.is_file():
        pytest.skip("scripts/bridge_action_census.py is not in this layout")
    spec = importlib.util.spec_from_file_location("bridge_action_census", census)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    rows = mod.enumerate_registry((BRIDGE / "src" / "action_registry.inc").read_text())
    assert S.Bridge(BRIDGE).registry() == {r["name"]: r["handler"] for r in rows}


# ── L13 FF1-B (witness W10): section C sees every writer shape ──────────────
# Each mutant plants one clock write in a shape the first section C missed;
# every one must red the row, and the copied tree without it is green (the
# control below the others).

_C_FAIL_OPEN = "every clock mark in bridge/src and bridge/include is inside a parsed function (fails open)"
_CASE_SET_PAUSE = "static bool case_set_pause(const char* params_json, std::string* response_out) {\n"

_C_W10_MUTANTS = {
    "typed_pointer_store": ("src/dispatch_router.cpp", _CASE_SET_PAUSE,
        "    { uint8_t* p = reinterpret_cast<uint8_t*>(0x10 + ck3::idler::PAUSE_FLAG); *p = 1; }\n"),
    "compound_or_store": ("src/dispatch_router.cpp", _CASE_SET_PAUSE,
        "    { uintptr_t i = 0; *reinterpret_cast<uint8_t*>(i + ck3::idler::PAUSE_FLAG) |= 1; }\n"),
    "increment_store": ("src/dispatch_router.cpp", _CASE_SET_PAUSE,
        "    { uintptr_t gs = 0; ++*reinterpret_cast<int32_t*>(gs + ck3::game_state::DATE_PAUSE_COUNT); }\n"),
    "namespace_alias": ("src/dispatch_router.cpp", _CASE_SET_PAUSE,
        "    { namespace I = ck3::idler; uintptr_t i = 0; *reinterpret_cast<uint8_t*>(i + I::PAUSE_FLAG) = 1; }\n"),
    "using_declaration": ("src/dispatch_router.cpp", _CASE_SET_PAUSE,
        "    { using ck3::idler::PAUSE_FLAG; uintptr_t i = 0; *reinterpret_cast<uint8_t*>(i + PAUSE_FLAG) = 1; }\n"),
    "event_pause_count_store": ("src/dispatch_router.cpp", _CASE_SET_PAUSE,
        "    { uintptr_t gs = 0; const uintptr_t at = gs + ck3::game_state::EVENT_PAUSE_COUNT;"
        " *reinterpret_cast<int32_t*>(at) = 0; }\n"),
    "pause_command_ctor_call": ("src/dispatch_router.cpp", _CASE_SET_PAUSE,
        "    reinterpret_cast<void(*)(void*,int,int)>(bridge::game_fn(ck3::funcs::PAUSE_GAME_CMD_CTOR))(nullptr, 0, 0);\n"),
    "speed_increment_call": ("src/dispatch_router.cpp", _CASE_SET_PAUSE,
        "    reinterpret_cast<void(*)()>(g_base_addr + ck3::globals::SPEED_INCREMENT_FN)();\n"),
    "a_header_inline_function": ("include/bridge.h", "namespace bridge {\n",
        "inline void l13_mutant_pause() { uintptr_t i = 0;"
        " *reinterpret_cast<uint8_t*>(i + ck3::idler::PAUSE_FLAG) = 1; }\n"),
    "an_inc_file": ("src/action_registry.inc", "",
        "\ninline void l13_mutant_inc() { uintptr_t i = 0;"
        " *reinterpret_cast<uint8_t*>(i + ck3::idler::PAUSE_FLAG) = 1; }\n"),
    "the_os_tree": ("src/os/os_common.cpp", "",
        "\nvoid l13_mutant_os() { uintptr_t i = 0;"
        " *reinterpret_cast<uint8_t*>(i + ck3::idler::PAUSE_FLAG) = 1; }\n"),
}


# L13 FF2 (witness m-6): four more shapes, one mutant each.
_C_W10_MUTANTS.update({
    "c_style_cast_store": ("src/dispatch_router.cpp", _CASE_SET_PAUSE,
        "    { uintptr_t i = 0; *(uint8_t*)(i + ck3::idler::PAUSE_FLAG) = 1; }\n"),
    "atomic_store_n_on_engine_memory": ("src/dispatch_router.cpp", _CASE_SET_PAUSE,
        "    { uintptr_t i = 0; __atomic_store_n(reinterpret_cast<uint8_t*>(i + ck3::idler::PAUSE_FLAG),"
        " 1, __ATOMIC_RELEASE); }\n"),
    "reference_alias_store": ("src/dispatch_router.cpp", _CASE_SET_PAUSE,
        "    { uintptr_t i = 0; auto& r = *reinterpret_cast<uint8_t*>(i + ck3::idler::PAUSE_FLAG); r = 1; }\n"),
    "setter_offset_in_a_variable": ("src/dispatch_router.cpp", _CASE_SET_PAUSE,
        "    { const uintptr_t off = ck3::funcs::PAUSE_SET;"
        " reinterpret_cast<void(*)(void*,int)>(bridge::game_fn(off))(nullptr, 1); }\n"),
})


def test_CONTROL_m6_an_atomic_LOAD_of_the_pause_flag_is_not_a_write(tree):
    """The atomic row keys on the store builtins: a load of the same byte, in the
    same spelling, stays green (so the atomic mutant reds on the store, not on
    the name __atomic_ or the field)."""
    p = tree / "src" / "dispatch_router.cpp"
    s = p.read_text()
    p.write_text(s.replace(_CASE_SET_PAUSE, _CASE_SET_PAUSE
        + "    { uintptr_t i = 0; (void)__atomic_load_n(reinterpret_cast<uint8_t*>(i + ck3::idler::PAUSE_FLAG),"
          " __ATOMIC_ACQUIRE); }\n", 1))
    assert _C_ROW not in [n for _, n, _ in _fails(tree, "C")]


@pytest.mark.parametrize("name", sorted(_C_W10_MUTANTS))
def test_MUTANT_W10_a_clock_write_in_a_shape_section_C_missed_reds(tree, name):
    rel, anchor, text = _C_W10_MUTANTS[name]
    p = tree / rel
    s = p.read_text()
    if anchor:
        assert s.count(anchor) >= 1, (name, anchor)
        s = s.replace(anchor, anchor + text, 1)
    else:
        s = s + text
    p.write_text(s)
    msgs = [n for _, n, _ in _fails(tree, "C")]
    assert _C_ROW in msgs, (name, msgs)


def test_MUTANT_W10_an_unreadable_source_reds_fail_open(tree):
    (tree / "src" / "l13_unreadable.cpp").write_bytes(b"\xff\xfe\xfa not utf-8 \x80\x81")
    msgs = [n for _, n, _ in _fails(tree, "C")]
    assert "every bridge source file parsed (fails open)" in msgs, msgs


def test_CONTROL_W10_the_copied_tree_with_os_and_headers_is_green(tree):
    assert _fails(tree) == []


# ── L13 FF1-B (witness W2/W6): row A0, no undeclared game-object handler on the
# IPC thread. On the tree before FF1-B (6edf11f4) the row reds naming
# set_ai_personality, get_ai_personality, the CB submitters, get_active_wars,
# pre_validate_war and query_council_tasks.

_A0_ROW = ("every IPC-thread handler that resolves a character or calls the engine "
           "is a declared exception")


def test_MUTANT_A0_query_council_tasks_back_on_the_ipc_thread_reds(tree):
    snap = tree / "src" / "snapshot.cpp"
    s = snap.read_text()
    anchor = '        "get_bridge_state",\n'
    assert s.count(anchor) == 1
    snap.write_text(s.replace(anchor, anchor + '        "query_council_tasks",\n', 1))
    fails = {n: d for _, n, d in _fails(tree, "A0")}
    assert _A0_ROW in fails and "query_council_tasks" in fails[_A0_ROW], fails


def test_A0_the_declared_exceptions_are_all_ipc_thread_handlers():
    """Not a discriminating control: the one exception list (snapshot.cpp's
    `ipc-exception` lines, which the harness reads) names only IPC-thread
    handlers, each with a reason."""
    b = S.Bridge(BRIDGE)
    declared = S.ipc_exceptions(b)
    names = set(b.classifier_names("is_bridge_internal_query"))
    assert len(declared) >= 10 and set(declared) <= names, set(declared) - names
    assert all(r for r in declared.values()), declared


# ── L13 FF2 (witness R4/R5): A0 checks the property, from ONE list ─────────
# On 81255a4c the row was green with query_war_score_breakdown (a WAR_DB walk)
# on the IPC thread: its touch regex could not see a g_base_addr + globals walk
# or resolve_war_by_id, and the harness kept its own copy of the exception list,
# which disagreed with snapshot.cpp's (query_cb_catalog).

_A0_STALE = "every declared ipc-exception is an IPC-thread handler that still touches game memory"


def test_MUTANT_A0_war_score_breakdown_back_on_the_ipc_thread_reds(tree):
    snap = tree / "src" / "snapshot.cpp"
    s = snap.read_text()
    anchor = '        "get_bridge_state",\n'
    assert s.count(anchor) == 1
    snap.write_text(s.replace(anchor, anchor + '        "query_war_score_breakdown",\n', 1))
    fails = {n: d for _, n, d in _fails(tree, "A0")}
    assert _A0_ROW in fails and "query_war_score_breakdown" in fails[_A0_ROW], fails


def test_MUTANT_A0_an_exception_deleted_from_the_one_list_reds(tree):
    snap = tree / "src" / "snapshot.cpp"
    s = snap.read_text()
    line = [ln for ln in s.splitlines(True) if "ipc-exception query_cb_catalog:" in ln]
    assert len(line) == 1
    snap.write_text(s.replace(line[0], "", 1))
    fails = {n: d for _, n, d in _fails(tree, "A0")}
    assert _A0_ROW in fails and "query_cb_catalog" in fails[_A0_ROW], fails


def test_MUTANT_A0_a_stale_exception_reds(tree):
    snap = tree / "src" / "snapshot.cpp"
    s = snap.read_text()
    line = [ln for ln in s.splitlines(True) if "ipc-exception query_cb_catalog:" in ln][0]
    snap.write_text(s.replace(line, line + "    //   ipc-exception query_intents: excuses nothing\n", 1))
    fails = {n: d for _, n, d in _fails(tree, "A0")}
    assert _A0_STALE in fails and "query_intents" in fails[_A0_STALE], fails


# ── L13 FF1-B (witness W3): row A2, no game-thread read sleeps. On the tree
# before FF1-B (6edf11f4) the row reds naming tick_diff (its 3 s wait for a
# tick) and query_conquest_targets (the strict CB poll's 50 ms sleeps).

_A2_ROW = "no game-state handler reaches a sleep (the drain holds the session mutex)"


def test_MUTANT_A2_tick_diff_waiting_for_a_tick_again_reds(tree):
    p = tree / "src" / "phase3_probes.cpp"
    s = p.read_text()
    anchor = "    TickSnapshot after = capture_snapshot();\n"
    assert s.count(anchor) == 1
    p.write_text(s.replace(anchor, "    std::this_thread::sleep_for(std::chrono::milliseconds(50));\n"
                                   + anchor, 1))
    fails = {n: d for _, n, d in _fails(tree, "A2")}
    assert _A2_ROW in fails and "tick_diff" in fails[_A2_ROW], fails


def test_MUTANT_A2_a_slot_poll_that_sleeps_on_the_game_thread_reds(tree):
    p = tree / "src" / "ai_eval_hook.cpp"
    s = p.read_text()
    old = "    if (bridge::on_game_thread()) return;\n    std::this_thread::sleep_for"
    assert s.count(old) == 1
    # Renaming the helper takes it off the allow-list: its sleep is now a plain one.
    p.write_text(s.replace("void slot_poll_wait() {", "void slot_poll_wait() { l13_poll_sleep(); }\n"
                           "void l13_poll_sleep() {", 1))
    fails = {n: d for _, n, d in _fails(tree, "A2")}
    assert _A2_ROW in fails, fails


# ── L13 FF1-B (witness W9): row F2, no reply advises running the clock. On the
# tree before FF1-B (6edf11f4) it reds on the nine sites the witness listed
# ("Game must be unpaused for … one tick", pause_state_required).

_F2_ROW = "no string literal in bridge/src tells the caller to unpause or wait for a tick"


@pytest.mark.parametrize("literal", [
    '"Game must be unpaused for at least one tick."',
    'R"({"hint":"the game must run for one tick"})"',
    '"pause_state_required"',
])
def test_MUTANT_F2_clock_advice_in_a_reply_reds(tree, literal):
    p = tree / "src" / "dispatch_router.cpp"
    s = p.read_text()
    anchor = "static bool case_set_pause(const char* params_json, std::string* response_out) {\n"
    assert s.count(anchor) == 1
    p.write_text(s.replace(anchor, anchor + f"    (void){literal};\n", 1))
    assert _F2_ROW in [n for _, n, _ in _fails(tree, "F2")]


# ── L13 FF2 (witness R6): row A3, every long game-thread evaluation checks the
# time budget inside its loop. Before FF2 (81255a4c) none of the four did (the
# decision eval had its own 4 s budget, not the game-thread one).

@pytest.mark.parametrize("fn,rel,old,new", [
    ("do_query_conquest_targets", "src/query_handlers.cpp",
     "if (budget_stopped || game_thread_budget_spent(budget_start)) {", "if (budget_stopped) {"),
    ("do_query_cbs_for_targets", "src/query_handlers.cpp",
     "if (!unprocessed.empty() || game_thread_budget_spent(budget_start)) {", "if (!unprocessed.empty()) {"),
    ("build_faction_options", "src/factions.cpp",
     "if (!unevaluated_types.empty() || game_thread_budget_spent(budget_start)) {",
     "if (!unevaluated_types.empty()) {"),
    ("enumerate_decisions_annotated", "src/query_handlers.cpp",
     "if (elapsed > kGameThreadEvalBudgetMs) { budget_hit = true; break; }",
     "if (elapsed > 4000) { budget_hit = true; break; }"),
])
def test_MUTANT_A3_an_evaluation_without_its_budget_check_reds(tree, fn, rel, old, new):
    p = tree / rel
    s = p.read_text()
    assert s.count(old) == 1, old
    s = s.replace(old, new)
    if fn == "build_faction_options":   # its second loop checks the budget too
        o2 = "if (!unevaluated_factions.empty() || game_thread_budget_spent(budget_start)) {"
        assert s.count(o2) == 1
        s = s.replace(o2, "if (!unevaluated_factions.empty()) {")
    p.write_text(s)
    row = [n for _, n, _ in _fails(tree, "A3") if n.startswith(fn + ": checks the game-thread budget")]
    assert row, fn


def test_declare_war_fits_the_clients_8_s_deadline():
    """Not a discriminating control: the two waits are named and their sum is
    checked by a static_assert the compiler runs (action_handlers.cpp)."""
    src = (BRIDGE / "src" / "action_handlers.cpp").read_text()
    assert "kDeclareWarPrevalWaitMs + kDeclareWarIntentWaitMs < 8000" in src
    i = src.index("bool do_declare_war(")
    body = src[i:src.index("\n}\n", i)]
    assert "kDeclareWarPrevalWaitMs, &preval_result" in body
    assert "poll < kDeclareWarIntentWaitMs / 50" in body


# ── L13 FF3 (witness X5): an odd digit separator keeps the audit in phase ────
# strip_code is cpp_strip.strip. Before FF3 its own scan paired the `'` of
# `1'0` with the next `'` in the file and read code as a char literal; the
# tree's separators come two per literal and paired by luck.

def test_an_odd_digit_separator_changes_no_row(tree):
    before = [(ok, n) for ok, n, _ in S.run_static(tree, echo=False).rows]
    p = tree / "src" / "war_score_hook.cpp"
    s = p.read_text()
    first_include = s.index("#include")
    eol = s.index("\n", first_include) + 1
    p.write_text(s[:eol] + "constexpr int kOdd = 1'0;\n" + s[eol:])
    after = [(ok, n) for ok, n, _ in S.run_static(tree, echo=False).rows]
    assert after == before
    assert "kOdd = 1'0;" in S.strip_code(p.read_text())


# ── L14: section G, the game-thread actions (Tier 3) ─────────────────────────
# On the phase-3 base (2308181b) section G reds 58 rows (18 G-raw, 19 fault
# bracket, 18 G-commit, 3 G-char): the L13 count of 39 plus G-commit, which is
# new, plus construction_check's raw read, which the definition parser could
# not see (evaluate_build sits in an indented `namespace {`). Each mutant below
# plants one defect class back into a game-thread action on the tip, in a body
# the dispatch reaches, and expects that action's row by name.

def _g_fails(tree) -> dict:
    return {n: d for _, n, d in _fails(tree, "G")}


def _in_body(tree, rel, sig, old, new, count=1):
    """Replace `old` (exactly `count` times) inside the function whose
    definition starts at `sig` in bridge/<rel>."""
    p = tree / rel
    s = p.read_text()
    i = s.index(sig)
    j = s.index("\n}\n", i)
    body = s[i:j]
    assert body.count(old) == count, (sig, old, body.count(old))
    p.write_text(s[:i] + body.replace(old, new) + s[j:])


_AH = "src/action_handlers.cpp"


def test_MUTANT_G_raw_the_scope_arms_measurement_read_raw_again_reds_every_arming_action(tree):
    """The base's defect: arm_fake_random_scope read the displaced node with
    read<> after publishing the fake scope. Every action that arms reds."""
    _in_body(tree, "src/intent_dispatch.cpp", "bool arm_fake_random_scope(",
             "const uint64_t node0 = safe_read<uint64_t>(", "const uint64_t node0 = read<uint64_t>(")
    fails = _g_fails(tree)
    red = {n.split(":")[0] for n in fails if n.endswith("(G-raw)")}
    for name in ("set_lifestyle_focus", "set_council_task", "move_capital", "maa_station",
                 "set_title_law", "construction_check", "construct_building"):
        assert name in red, (name, sorted(red))
    assert "arm_fake_random_scope" in fails["move_capital: zero raw reads in it and its callees (G-raw)"]


def test_MUTANT_G_raw_a_cast_dereference_in_a_handler_body_reds(tree):
    _in_body(tree, _AH, "bool do_set_obligation_level(",
             "uint8_t old_level = safe_read<uint8_t>(level_addr);",
             "uint8_t old_level = *reinterpret_cast<volatile uint8_t*>(level_addr);")
    assert "set_obligation_level: zero raw reads in it and its callees (G-raw)" in _g_fails(tree)


def test_MUTANT_G_raw_in_a_helper_defined_inside_an_indented_namespace_reds(tree):
    """evaluate_build is defined inside `namespace {`, indented; before L14 the
    parser found no such definition, so a raw read there was in no closure."""
    assert "evaluate_build" in S.Bridge(tree).functions       # not discriminating: the parse
    _in_body(tree, _AH, "    BuildEval evaluate_build(",
             "safe_read<uintptr_t>(t.building + ck3::funcs::BUILDING_COST_SCALE_OFF)",
             "read<uintptr_t>(t.building + ck3::funcs::BUILDING_COST_SCALE_OFF)")
    fails = _g_fails(tree)
    for name in ("construction_check", "construct_building"):
        assert "evaluate_build" in fails[f"{name}: zero raw reads in it and its callees (G-raw)"]


def test_MUTANT_G_fault_check_a_write_without_its_FaultScope_reds(tree):
    _in_body(tree, _AH, "bool do_hire_mercenary(",
             '    FaultScope fault_scope("hire_mercenary", response_out);', "")
    assert "hire_mercenary: checks the safe_read fault flag before it answers" in _g_fails(tree)


def test_MUTANT_G_fault_check_a_game_thread_read_without_its_FaultScope_reds(tree):
    _in_body(tree, _AH, "bool do_construction_check(",
             '    FaultScope fault_scope("construction_check", response_out);', "")
    assert "construction_check: checks the safe_read fault flag before it answers" in _g_fails(tree)


_COMMIT = "move_capital: decides on its reads before its first engine contact (G-commit)"
_MC_DECIDE = '    if (safe_read_had_fault()) return answer_memory_access_fault("move_capital", response_out);\n'


def test_MUTANT_G_commit_a_write_without_its_decision_reds(tree):
    _in_body(tree, _AH, "bool do_move_capital(", _MC_DECIDE, "")
    fails = _g_fails(tree)
    assert _COMMIT in fails, fails
    # FaultScope still brackets it, so this is the commit row and not the bracket row.
    assert "move_capital: checks the safe_read fault flag before it answers" not in fails


def test_MUTANT_G_commit_a_decision_after_the_arm_reds(tree):
    _in_body(tree, _AH, "bool do_move_capital(", _MC_DECIDE, "")
    _in_body(tree, _AH, "bool do_move_capital(", "    watchdog_arm(4);\n",
             "    watchdog_arm(4);\n" + _MC_DECIDE)
    assert _COMMIT in _g_fails(tree)


def test_MUTANT_G_commit_an_intent_enqueued_before_the_decision_reds(tree):
    """create_title's first engine contact is generate_rng (a seed store) and the
    enqueue; the decision moved below both reds."""
    _in_body(tree, _AH, "bool do_create_title(",
             '    if (safe_read_had_fault()) return answer_memory_access_fault("create_title", response_out);\n', "")
    _in_body(tree, _AH, "bool do_create_title(", '    ADLOG("create_title: queued',
             '    if (safe_read_had_fault()) return answer_memory_access_fault("create_title", response_out);\n'
             '    ADLOG("create_title: queued')
    assert "create_title: decides on its reads before its first engine contact (G-commit)" in _g_fails(tree)


def test_MUTANT_G_create_titles_seed_drawn_before_its_fault_check_reds(tree):
    """The RNG-order census (witness #12 minor 4) sees create_title now that it
    brackets: the check moved past the draw reds there too."""
    _in_body(tree, _AH, "bool do_create_title(",
             '    if (safe_read_had_fault()) return answer_memory_access_fault("create_title", response_out);\n', "")
    _in_body(tree, _AH, "bool do_create_title(", "    Intent intent = {};\n",
             '    if (safe_read_had_fault()) return answer_memory_access_fault("create_title", response_out);\n'
             "    Intent intent = {};\n")
    _, bad = rng_before_its_check(tree)
    assert any(b.endswith(":do_create_title") for b in bad), bad


def test_MUTANT_G_char_a_null_only_actor_check_reds(tree):
    _in_body(tree, _AH, "bool do_appoint_councillor(",
             "    if (const char* why = actor_refusal(actor)) { short_error(why); return false; }\n",
             '    if (!actor) { short_error("actor_not_found"); return false; }\n')
    assert "appoint_councillor: returns early when the character does not resolve" in _g_fails(tree)


_BOUND = "move_capital: every engine call is under a watchdog its body armed first (G-bound)"


def test_MUTANT_G_bound_an_engine_call_before_the_watchdog_reds(tree):
    _in_body(tree, _AH, "bool do_move_capital(", _MC_DECIDE,
             _MC_DECIDE + "    (void)validate(actor, county, 0, 0);\n")
    fails = _g_fails(tree)
    assert _BOUND in fails and "validate" in fails[_BOUND], fails


def test_MUTANT_G_bound_a_helper_called_before_its_callers_watchdog_reds(tree):
    """engine_virtual_dtor runs under its callers' watchdogs; one call before
    the arm takes that away (the one-level caller rule)."""
    _in_body(tree, _AH, "bool do_set_title_law(", "    alignas(16) uint8_t cmd[0x40];\n",
             "    engine_virtual_dtor(reinterpret_cast<void*>(tobj));\n    alignas(16) uint8_t cmd[0x40];\n")
    fails = _g_fails(tree)
    row = "set_title_law: every engine call is under a watchdog its body armed first (G-bound)"
    assert row in fails and "engine_virtual_dtor" in fails[row], fails


@pytest.mark.parametrize("stmt", [
    "    std::this_thread::sleep_for(std::chrono::milliseconds(50));\n",
    "    { std::mutex m; std::condition_variable cv; std::unique_lock<std::mutex> lk(m);"
    " cv.wait_for(lk, std::chrono::milliseconds(50)); }\n",
])
def test_MUTANT_G_sleep_a_wait_in_a_game_thread_action_reds(tree, stmt):
    _in_body(tree, _AH, "bool do_construct_building(",
             "    std::string bname = ad_read_std_string(t.building + ck3::db_object::NAME_KEY);\n",
             stmt + "    std::string bname = ad_read_std_string(t.building + ck3::db_object::NAME_KEY);\n")
    assert ("construct_building: no sleep or wait on the game thread (the drain holds the session mutex)"
            in _g_fails(tree))


def test_MUTANT_G_sleep_a_poll_loop_that_runs_on_the_game_thread_reds(tree):
    """call_war_create_core's poll sleeps only off the game thread (its loop
    condition: declare_war's force_player_path forwards to it from the IPC
    thread); the condition dropped, the drain would sleep 5 s holding the
    session mutex. (create_title's poll, the earlier subject here, is deleted:
    L14 FB m-5, nothing reaches that handler off the game thread.)"""
    _in_body(tree, _AH, "bool do_call_war_create_core(",
             "for (int poll = 0; poll < 100 && !bridge::on_game_thread(); poll++) {",
             "for (int poll = 0; poll < 100; poll++) {")
    assert ("call_war_create_core: no sleep or wait on the game thread (the drain holds the session mutex)"
            in _g_fails(tree))


@pytest.mark.parametrize("stmt", [
    "    *reinterpret_cast<uint8_t*>(g_base_addr + ck3::idler::PAUSE_FLAG) = 1;\n",
    "    clock_write(0);\n",
])
def test_MUTANT_G_clock_a_game_thread_action_that_moves_the_clock_reds(tree, stmt):
    _in_body(tree, _AH, "bool do_set_doctrine(", "    FaultScope fault_scope(", stmt + "    FaultScope fault_scope(")
    assert ("set_doctrine: no clock write and no call of clock_write in its path (G-clock)"
            in _g_fails(tree))


def test_MUTANT_G_forwarder_that_reads_before_forwarding_is_graded_by_its_own_body(tree):
    """set_education_focus is graded in do_set_lifestyle_focus because it reads
    nothing before it forwards; a read in front makes its own body the one graded."""
    _in_body(tree, _AH, "bool do_set_education_focus(",
             "    return do_set_lifestyle_focus(params_json, response_out);",
             "    (void)safe_read<uint32_t>(g_base_addr);\n"
             "    return do_set_lifestyle_focus(params_json, response_out);")
    assert "set_education_focus: checks the safe_read fault flag before it answers" in _g_fails(tree)


_SNAP = "src/snapshot.cpp"
#: L16: the declarations' place in is_game_thread_action. None stands since
#: respond_event's went with the handler (ruling R3); the mutants plant theirs
#: after this line.
_DECL_ANCHOR = "    // respond_event's, went with the handler).\n"


def test_no_declared_exception_stands():
    """L16: is_game_thread_action carries no game-thread-ok declaration, and the
    harness parses that as none (not as a parse that found nothing to read)."""
    decl, bad = S.game_thread_ok(S.Bridge(BRIDGE))
    assert decl == {} and bad == [], (decl, bad)
    assert (BRIDGE / _SNAP).read_text().count(_DECL_ANCHOR) == 1


def test_MUTANT_G_a_declaration_excuses_its_row(tree):
    """The mechanism still excuses one row of one handler: plant a sleep in a
    game-thread action (row sleep reds), then declare that row for that handler,
    and the row passes as declared with no stale declaration."""
    _SLEEP = "    std::this_thread::sleep_for(std::chrono::milliseconds(1));\n"
    _in_body(tree, _AH, "bool do_set_doctrine(", "    FaultScope fault_scope(",
             _SLEEP + "    FaultScope fault_scope(")
    row = "set_doctrine: no sleep or wait on the game thread (the drain holds the session mutex)"
    assert row in _g_fails(tree)
    p = tree / _SNAP
    s = p.read_text()
    p.write_text(s.replace(_DECL_ANCHOR, _DECL_ANCHOR +
                           "    //   game-thread-ok set_doctrine: sleep: a planted sleep, declared\n"))
    fails = _g_fails(tree)
    assert row not in fails, fails
    assert not any(n.startswith("every game-thread-ok") for n in fails), fails


@pytest.mark.parametrize("decl,row", [
    ("game-thread-ok move_capital: char: excuses nothing", "every game-thread-ok declaration excuses"),
    ("game-thread-ok query_wars: commit: not a game-thread action", "every game-thread-ok declaration excuses"),
    ("game-thread-ok set_title_law: raw_read: a raw read is never declarable", "every game-thread-ok line parses"),
    ("game-thread-ok set_title_law commit no colons", "every game-thread-ok line parses"),
])
def test_MUTANT_G_a_stale_or_malformed_declaration_reds(tree, decl, row):
    p = tree / _SNAP
    s = p.read_text()
    assert s.count(_DECL_ANCHOR) == 1
    p.write_text(s.replace(_DECL_ANCHOR, _DECL_ANCHOR + f"    //   {decl}\n"))
    assert any(n.startswith(row) for n in _g_fails(tree)), _g_fails(tree)


def test_MUTANT_G_a_classifier_parse_that_finds_fewer_names_reds(tree):
    """Fails OPEN: one name gone from the list is one handler unaudited."""
    p = tree / _SNAP
    s = p.read_text()
    assert s.count('        "change_realm_law",\n') == 1
    p.write_text(s.replace('        "change_realm_law",\n', ""))
    assert "is_game_thread_action() parsed" in _g_fails(tree)


# ── L14 S4: G-scope (audit 3(ii)) and G-nest (audit 3(iii)) ──────────────────
# On a0a2fff3 G-scope reds appoint_councillor, maa_recruit, maa_origin_dispatch
# and hire_mercenary (armed after their own sigsetjmp), and G-nest reds
# declare_war and query (a callee's safe_read_clear() wiped the handler's record)
# and FaultScope (no save/restore). The mutants plant each defect back.

_ARM_LINE = "    uint64_t saved_rng = 0; const bool rng_armed = arm_fake_random_scope(&saved_rng);\n"
_HM_CRASH = '        if (response_out) *response_out = R"({"success":false,"action":"hire_mercenary","reason":"apply_crashed"})";\n        return false;\n    }\n'


def test_MUTANT_G_scope_an_arm_after_the_guard_reds(tree):
    _in_body(tree, _AH, "bool do_hire_mercenary(", _ARM_LINE, "")
    _in_body(tree, _AH, "bool do_hire_mercenary(", _HM_CRASH, _HM_CRASH + _ARM_LINE)
    fails = _g_fails(tree)
    row = ("do_hire_mercenary (action_handlers.cpp): arms the scope before its own sigsetjmp, "
           "and its crash branch restores it (G-scope)")
    assert row in fails and "no sigsetjmp between it and its restore" in fails[row], fails


def test_MUTANT_G_scope_a_crash_branch_that_returns_without_restoring_reds(tree):
    _in_body(tree, _AH, "bool do_move_capital(",
             "        if (rng_armed) restore_fake_random_scope(saved_rng);\n        g_in_protected_region = false;\n",
             "        g_in_protected_region = false;\n")
    row = ("do_move_capital (action_handlers.cpp): arms the scope before its own sigsetjmp, "
           "and its crash branch restores it (G-scope)")
    fails = _g_fails(tree)
    assert row in fails and "returns without restore_fake_random_scope" in fails[row], fails


def test_G_scope_reads_the_guard_shapes_the_tree_uses():
    """Not discriminating: the crash-branch parser on the three shapes."""
    assert S.scope_arm_defects("{ arm_fake_random_scope(&s); int sig = sigsetjmp(j, 1); "
                               "if (sig != 0) { restore_fake_random_scope(s); return false; } "
                               "f(); restore_fake_random_scope(s); }") == []
    assert S.scope_arm_defects("{ arm_fake_random_scope(&s); int sig = sigsetjmp(j, 1); "
                               "if (sig == 0) { f(); } restore_fake_random_scope(s); }") == []
    assert S.scope_arm_defects("{ arm_fake_random_scope(&s); if (sigsetjmp(j, 1) == 0) { f(); } "
                               "else { return false; } restore_fake_random_scope(s); }") != []


def test_MUTANT_G_scope_a_census_that_finds_fewer_bodies_reds(tree):
    """Fails OPEN: the floor is the population (23)."""
    n = len([1 for defs in S.Bridge(tree).functions.values() for _, b, _ in defs
             if S._ARM_RE.search(b) and S._SETJMP_RE.search(b)])
    assert n == S.SCOPE_ARM_FLOOR, n      # the floor pins the whole population
    _in_body(tree, _AH, "bool do_move_capital(", "arm_fake_random_scope(&saved_rng)", "false")
    assert "the bodies that arm the scope under their own guard were found (G-scope)" in _g_fails(tree)


def test_MUTANT_G_nest_a_callee_that_clears_by_name_reds(tree):
    _in_body(tree, "src/query_handlers.cpp", "bool do_query_realm(",
             '    FaultScope fault_scope("query_realm", out);', "    safe_read_clear();")
    fails = _g_fails(tree)
    row = "query (case_query_meta): no function it reaches calls safe_read_clear() but itself (G-nest)"
    assert row in fails and "do_query_realm" in fails[row], fails


def test_MUTANT_G_nest_a_FaultScope_that_does_not_restore_reds(tree):
    p = tree / "include" / "action_dispatch_internal.h"
    s = p.read_text()
    assert s.count("        safe_read_restore(outer_);\n") == 1
    p.write_text(s.replace("        safe_read_restore(outer_);\n", ""))
    assert any(n.startswith("FaultScope nests") for _, n, _ in _fails(tree, "F"))


# ── L14 FB: the witness's section-G gaps (M-6, M-7, M-8, m-1, m-12..m-14) ──────
# Each mutant below was GREEN against the a1fb8c6b harness (the witness ran them
# in memory); each must red here.

_QCTC = "bool do_query_create_title_cost("
_QCTC_BOUND = "query_create_title_cost: every engine call is under a watchdog its body armed first (G-bound)"
_ISV_ALIAS = "reinterpret_cast<bridge::EngineBoolFn<void*, std::string*>>("


def test_MUTANT_E1_an_unguarded_call_through_an_EngineBoolFn_reds(tree):
    """M-6: `bridge::EngineBoolFn<void*, std::string*>` carries `void*` in its
    template ARGUMENTS; engine_calls read that as a data type and dropped the
    call. A call outside the guard reds now."""
    _in_body(tree, _AH, _QCTC, "    std::string why;\n", "    std::string why;\n    (void)is_valid(cmd, &why);\n")
    fails = _g_fails(tree)
    assert _QCTC_BOUND in fails and "is_valid" in fails[_QCTC_BOUND], fails


def test_CONTROL_E1_the_same_call_spelled_as_a_plain_function_pointer_reds(tree):
    """The control: the identical mutant with the cast spelled `bool (*)(..)`.
    This red does not depend on the M-6 fix (it is the spelling the old
    harness saw); E1 above is the one that does."""
    _in_body(tree, _AH, _QCTC, _ISV_ALIAS, "reinterpret_cast<bool (*)(void*, std::string*)>(")
    _in_body(tree, _AH, _QCTC, "    std::string why;\n", "    std::string why;\n    (void)is_valid(cmd, &why);\n")
    fails = _g_fails(tree)
    assert _QCTC_BOUND in fails and "is_valid" in fails[_QCTC_BOUND], fails


def test_the_census_sees_every_EngineBoolFn_engine_call():
    """M-6, the regression's five calls: each EngineBoolFn-bound name is an
    engine call of its body (not discriminating on a tree where they are all
    guarded; it pins the census)."""
    b = S.Bridge(BRIDGE)
    want = {"do_query_create_title_cost": {"is_valid"}, "execute_create_title_intent": {"is_valid"},
            "dispatch_costed_law_command": {"can_exec"}, "create_title_price": {"has_flag", "treasury"}}
    for fn, names in want.items():
        (_, body, _), = b.functions[fn]
        got = {lab for _, lab in S.engine_calls(body)}
        assert names <= got, (fn, sorted(got))


def test_outer_type_strips_template_arguments():
    assert S.outer_type("bridge::EngineBoolFn<void*, std::string*>") == "bridge::EngineBoolFn"
    assert S.outer_type("std::array<std::pair<int, char>, 4>*") == "std::array*"


_GIT_BASE = "2308181b"


def test_CONTROL_pinned_base_the_uint64_spelling_was_seen(tmp_path):
    """The pinned base control (L14 FB M-6): on 2308181b (the phase's base)
    dispatch_costed_law_command bound can_exec through `uint64_t (*)(..)`, and
    the census saw it; the phase respelled it EngineBoolFn and the old census
    lost it. Read by `git archive` of that sha; skips where the history is not
    there (the public export)."""
    import subprocess
    if subprocess.run(["git", "-C", str(REPO), "cat-file", "-e", _GIT_BASE + "^{commit}"],
                      capture_output=True).returncode != 0:
        pytest.skip(f"no git history with {_GIT_BASE} in this layout")
    arc = subprocess.run(["git", "-C", str(REPO), "archive", _GIT_BASE, "bridge/src", "bridge/include"],
                         capture_output=True, check=True).stdout
    subprocess.run(["tar", "-x", "-C", str(tmp_path)], input=arc, check=True)
    base = S.Bridge(tmp_path / "bridge")
    (_, body, _), = base.functions["dispatch_costed_law_command"]
    assert "uint64_t" in body[body.index("can_exec"):body.index("can_exec") + 120]
    assert "can_exec" in {lab for _, lab in S.engine_calls(body)}
    tip = S.Bridge(BRIDGE)
    (_, tbody, _), = tip.functions["dispatch_costed_law_command"]
    t = "bridge::EngineBoolFn<void*, void*>"
    assert t in tbody
    # the a1fb8c6b test (the whole type text) against the tip's spelling: blind
    assert S._DATA_TYPE_RE.search(t) and not S._DATA_TYPE_RE.search(S.outer_type(t))
    assert "can_exec" in {lab for _, lab in S.engine_calls(tbody)}


def test_MUTANT_E2_a_read_between_the_last_decision_and_the_commit_reds(tree):
    """M-7: G-commit was satisfied by a decision anywhere before the first
    engine contact."""
    _in_body(tree, _AH, "bool do_move_capital(", _MC_DECIDE,
             _MC_DECIDE + "    (void)safe_read<uintptr_t>(county_addr);\n")
    fails = _g_fails(tree)
    assert _COMMIT in fails and "a read lies between" in fails[_COMMIT], fails


_CRL_DECIDE = ('        if (safe_read_had_fault()) return answer_memory_access_fault("change_realm_law", '
               'response_out);\n        bool crashed = false;\n')


def test_MUTANT_E2_change_realm_law_without_its_decision_after_the_war_read_reds(tree):
    """M-7, the live instance: the war count read after the first decision. The
    decision added before the costed dispatch deleted reds."""
    _in_body(tree, _AH, "bool do_change_realm_law(", _CRL_DECIDE, "        bool crashed = false;\n")
    row = "change_realm_law: decides on its reads before its first engine contact (G-commit)"
    fails = _g_fails(tree)
    assert row in fails and "a read lies between" in fails[row], fails


def test_MUTANT_E3A_a_call_after_the_guard_ended_reds(tree):
    """M-8: G-bound checked "a watchdog was armed earlier in the text"."""
    _in_body(tree, _AH, _QCTC, "watchdog_disarm();\n",
             "watchdog_disarm();\n    price = create_title_price(actor, title_obj);\n")
    fails = _g_fails(tree)
    assert _QCTC_BOUND in fails and "create_title_price" in fails[_QCTC_BOUND], fails


def test_MUTANT_E3B_a_disarm_before_the_engine_call_reds(tree):
    _in_body(tree, _AH, "bool do_move_capital(", "    valid = validate(",
             "    watchdog_disarm(); g_in_protected_region = false;\n    valid = validate(")
    fails = _g_fails(tree)
    assert _BOUND in fails and "validate" in fails[_BOUND], fails


def test_guarded_spans_read_the_shapes_the_tree_uses():
    """Not discriminating: the span parser on the crash-block and early-exit
    shapes (a disarm in a crash block or in a nested block that returns does
    not end the span; one at the body's own level does)."""
    body = ("{ watchdog_arm(4); int sig = sigsetjmp(j, 1); if (sig != 0) { watchdog_disarm(); return false; } "
            "if (dead) { watchdog_disarm(); return false; } A(); watchdog_disarm(); B(); return true; }")
    (a, e), = S.guarded_spans(body)
    assert a < body.index("A()") < e <= body.index("B()")
    assert S.guarded_spans("{ int sig = sigsetjmp(j, 1); A(); }") == []    # no watchdog armed first


_ID_VALID = ("reinterpret_cast<bridge::EngineBoolFn<void*, void*>>(\n"
             "            bridge::game_fn(ck3::funcs::CREATE_TITLE_CMD_VALIDATE));\n        const bool ok")


def test_MUTANT_m1_an_engine_bool_read_as_uint64_reds(tree):
    """m-1 (run 6's mutant): IsValid bound as `uint64_t (*)(..)` and its result
    taken as a bool: the garbage above AL decides."""
    _in_body(tree, "src/intent_dispatch.cpp", "bool execute_create_title_intent(", _ID_VALID,
             _ID_VALID.replace("bridge::EngineBoolFn<void*, void*>", "uint64_t(*)(void*, void*)"))
    fails = _g_fails(tree)
    row = "no wide-return engine call is tested for truth without & 0xFF (G-al)"
    assert row in fails and "execute_create_title_intent" in fails[row], fails


def test_CONTROL_m1_the_same_read_masked_to_AL_is_green(tree):
    _in_body(tree, "src/intent_dispatch.cpp", "bool execute_create_title_intent(", _ID_VALID,
             _ID_VALID.replace("bridge::EngineBoolFn<void*, void*>", "uint64_t(*)(void*, void*)"))
    _in_body(tree, "src/intent_dispatch.cpp", "bool execute_create_title_intent(",
             "const bool ok = is_valid(cmd, nullptr);", "const bool ok = (is_valid(cmd, nullptr) & 0xFF) != 0;")
    assert not any("G-al" in n for n in _g_fails(tree))


def test_the_G_al_census_floor_is_the_population():
    """Fails OPEN: the floor pins the function-pointer casts examined."""
    rep = S.run_static(BRIDGE, echo=False, sections=["G"])
    row, = [d for _, n, d in rep.rows if n == "the function-pointer casts were found (G-al)"]
    assert row == f"found {S.FNPTR_CAST_FLOOR}, floor {S.FNPTR_CAST_FLOOR}", row


_DECL_ROW = "every game-thread-ok line parses as <name>: <row>: <reason> with a declarable row"


def test_MUTANT_m12_a_declaration_outside_is_game_thread_action_reds(tree):
    p = tree / _SNAP
    s = p.read_text()
    i = s.index("bool is_game_state_query(")
    p.write_text(s[:i] + "// game-thread-ok move_capital: commit: declared outside the classifier\n" + s[i:])
    fails = _g_fails(tree)
    assert _DECL_ROW in fails and "outside is_game_thread_action" in fails[_DECL_ROW], fails


def test_MUTANT_m13_a_registry_parse_that_finds_fewer_lines_reds(tree):
    reg = tree / "src" / "action_registry.inc"
    s = reg.read_text()
    assert len(S.Bridge(tree).registry()) == S.REGISTRY_FLOOR     # the floor pins the population
    line = next(l for l in s.splitlines(keepends=True) if 'BRIDGE_ACTION("get_tick_info"' in l)
    reg.write_text(s.replace(line, "", 1))
    assert "action_registry.inc parsed (G-nest)" in _g_fails(tree)


def test_MUTANT_m14_an_engine_contact_before_the_actor_check_reds(tree):
    _in_body(tree, _AH, "bool do_move_capital(", "    void* actor = lookup_character(actor_id);\n",
             "    void* actor = lookup_character(actor_id);\n    watchdog_arm(1);\n")
    fails = _g_fails(tree)
    row = "move_capital: returns early when the character does not resolve"
    assert row in fails and "between the resolve and its early return" in fails[row], fails


def test_MUTANT_m14_a_callee_that_zeroes_the_counter_directly_reds(tree):
    _in_body(tree, "src/query_handlers.cpp", "bool do_query_realm(",
             '    FaultScope fault_scope("query_realm", out);',
             '    FaultScope fault_scope("query_realm", out);\n    g_safe_read_fault_count = 0;')
    fails = _g_fails(tree)
    row = "query (case_query_meta): no function it reaches calls safe_read_clear() but itself (G-nest)"
    assert row in fails and "do_query_realm" in fails[row], fails


def test_m14_a_counter_put_back_is_not_a_wipe():
    """Not discriminating: arm_fake_random_scope saves the counter and puts it
    back (intent_dispatch.cpp); an increment adds a fault."""
    assert S.counter_wipes("{ const int faults = g_safe_read_fault_count; f(); g_safe_read_fault_count = faults; }") == []
    assert S.counter_wipes("{ ++g_safe_read_fault_count; g_safe_read_fault_count += 2; }") == []
    assert S.counter_wipes("{ g_safe_read_fault_count = 0; }") != []


def test_MUTANT_m14_a_raw_read_in_a_header_helper_reds(tree):
    """m-14: the closure follows into bridge/include. log_degraded_read
    (bridge.h) is on every answer_memory_access_fault path."""
    p = tree / "include" / "bridge.h"
    s = p.read_text()
    old = "inline void log_degraded_read(const char* action) {\n    if (!g_logfile) return;\n"
    assert s.count(old) == 1
    p.write_text(s.replace(old, old + "    (void)read<uint8_t>(g_base_addr);\n"))
    fails = _g_fails(tree)
    row = "move_capital: zero raw reads in it and its callees (G-raw)"
    assert row in fails and "log_degraded_read" in fails[row], fails


# ── L14 FB2: the witness's round-2 section-G holes (N-5, N-5b, N-5c, N-12) ─────
# Each mutant below is GREEN against the b84e3634 harness (the gap; the witness
# ran P1, P2, P4 in memory, and this lane re-ran all of them with that harness
# over the same mutated trees) and must red here.

_MC_VALIDATE = "    valid = validate(actor, county, 0, 0);\n"
_MC_LAST_DISARM = ("    if (rng_armed) restore_fake_random_scope(saved_rng);\n"
                   "    g_in_protected_region = false;\n    watchdog_disarm();\n\n    if (response_out) {")


def test_MUTANT_N5_P1_a_second_guard_after_the_final_disarm_reds(tree):
    """P1: a sigsetjmp after the body's last disarm. The b84e3634 span began at
    any sigsetjmp that SOME watchdog_arm preceded in the text, so this one was
    armed by the arm the disarm had ended. Now the last arm/disarm event before
    the sigsetjmp must be an arm."""
    _in_body(tree, _AH, "bool do_move_capital(", _MC_LAST_DISARM,
             _MC_LAST_DISARM.replace("    if (response_out) {",
                                     "    if (sigsetjmp(g_recovery_point, 1) == 0) { (void)validate(actor, county, 0, 0); }\n"
                                     "    if (response_out) {"))
    fails = _g_fails(tree)
    assert _BOUND in fails and "validate" in fails[_BOUND], fails


def test_MUTANT_N5_P2_a_call_after_a_disarm_in_an_early_exit_block_reds(tree):
    """P2: an early-exit block disarms, then calls the engine, then returns.
    The disarm rightly does not end the span for the code after the block; it
    does for the call after it in its own block."""
    _in_body(tree, _AH, "bool do_move_capital(", _MC_VALIDATE,
             "    if (!actor) { watchdog_disarm(); g_in_protected_region = false; "
             "(void)validate(actor, county, 0, 0); return false; }\n" + _MC_VALIDATE)
    fails = _g_fails(tree)
    assert _BOUND in fails and "validate" in fails[_BOUND], fails


def test_MUTANT_N5_P3_a_disarm_inside_a_lambda_before_the_call_reds(tree):
    """P3: a lambda that disarms and returns was read as an early-exit block;
    the lambda may run anywhere, so its disarm counts as top-level."""
    _in_body(tree, _AH, "bool do_move_capital(", _MC_VALIDATE,
             "    auto drop = [&]() { watchdog_disarm(); g_in_protected_region = false; return false; };\n"
             "    (void)drop();\n" + _MC_VALIDATE)
    fails = _g_fails(tree)
    assert _BOUND in fails and "validate" in fails[_BOUND], fails


def test_MUTANT_N5b_P4_a_decision_nested_in_an_unrelated_if_reds(tree):
    """P4: the decision moved inside `if (actor_id == 0) { .. }` decides only
    when that branch runs; its block must contain the commit (dominance)."""
    _in_body(tree, _AH, "bool do_move_capital(", _MC_DECIDE,
             "    if (actor_id == 0) {\n    " + _MC_DECIDE + "    }\n")
    fails = _g_fails(tree)
    assert _COMMIT in fails and "in a block that contains it" in fails[_COMMIT], fails


def test_MUTANT_N5c_a_write_with_no_engine_contact_found_reds(tree):
    """N-5c: a game-thread write that reads through safe_read and in which the
    parser finds no engine contact was green (nothing to decide before). Here
    set_obligation_level's one store is respelled as a memcpy, which no store
    pattern sees: a write that touches nothing visible is a parse gap, red by
    name."""
    _in_body(tree, _AH, "bool do_set_obligation_level(",
             "    *reinterpret_cast<volatile uint8_t*>(level_addr) = static_cast<uint8_t>(level);\n",
             "    const uint8_t lv = static_cast<uint8_t>(level);\n"
             "    std::memcpy(reinterpret_cast<void*>(level_addr), &lv, 1);\n")
    fails = _g_fails(tree)
    row = "set_obligation_level: decides on its reads before its first engine contact (G-commit)"
    assert fails.get(row, "").endswith("no engine contact found"), fails


def test_guarded_spans_N5_shapes():
    """Not discriminating: the three N-5 shapes on literal bodies."""
    arm = "{ watchdog_arm(4); int sig = sigsetjmp(j, 1); if (sig != 0) { watchdog_disarm(); return false; } "
    p1 = arm + "A(); watchdog_disarm(); if (sigsetjmp(j, 1) == 0) { B(); } return true; }"
    assert len(S.guarded_spans(p1)) == 1 and not S.in_guarded_span(p1, p1.index("B()"))
    p2 = arm + "if (x) { watchdog_disarm(); C(); return false; } A(); watchdog_disarm(); }"
    assert not S.in_guarded_span(p2, p2.index("C()")) and S.in_guarded_span(p2, p2.index("A()"))
    p3 = arm + "auto f = [&]() { watchdog_disarm(); return false; }; A(); }"
    assert not S.in_guarded_span(p3, p3.index("A()"))


_SP = "src/os/session_pass.cpp"
_SP_ROW = ("session_pass_wrapper (os/session_pass.cpp): its one raw read is the execute byte the "
           "engine's pass just used, at an offset install probed (G-session)")


def test_the_session_pass_wrapper_row_is_present_and_green():
    rep = S.run_static(BRIDGE, echo=False, sections=["G"])
    assert _SP_ROW in [n for _, n, _ in rep.rows]
    assert _SP_ROW not in [n for _, n, _ in rep.failures]


def test_MUTANT_N12_a_second_raw_read_in_the_session_pass_wrapper_reds(tree):
    """N-12: the wrapper's one raw read was parsed (raw_reads == 1) and no row
    consumed it; a second one went unseen."""
    _in_body(tree, _SP, "bool session_pass_wrapper(", "        const std::uintptr_t lock = s + g_lock_off;\n",
             "        const std::uintptr_t lock = s + g_lock_off;\n"
             "        if (*reinterpret_cast<volatile std::uint8_t*>(lock) == 0) return r;\n")
    fails = _g_fails(tree)
    assert _SP_ROW in fails and "1 raw read(s) besides the execute byte" in fails[_SP_ROW], fails


def test_MUTANT_N12_the_wrapper_not_found_reds(tree):
    _in_body(tree, _SP, "bool session_pass_wrapper(", "bool session_pass_wrapper(void* session) {",
             "bool session_pass_wrapper_moved(void* session) {")
    assert _SP_ROW in _g_fails(tree)
