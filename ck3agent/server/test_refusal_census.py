"""Every refusal code the bridge's write paths emit
maps to a typed gate; none reaches a seat as unmapped_refusal.

THE SMOKE. probe-paused.jsonl line 35 (R2.declare_synth):
  declare_war(character_id=13902, target_id=12604, cb="county_conquest_cb", ...)
  -> blocked {"gate": "unmapped_refusal", "raw_reason": "engine_cb_validity_check_failed"}
declare_war's strict CB pre-check passes the row's cull_reason to write_blocked,
and the strict read's culls (query_handlers.cpp cb_cull_reason and the claim
branch of do_query_cb_validity_strict) had no row in RAW_REASON_TO_GATE.

THE CENSUS walks the bridge source, not a list of names: the five write-path
files and the strict CB read's two cull sites, every snake_case code in a
refusal position (a reason/error/cull_reason/block_reason/refused_by key, the
same keys in a JSON literal, the first argument of the reject/fail/refuse/
short_error helpers, an assignment to cull/reason/why/err/code). Each code maps
to a gate, or is in NOT_A_SEAT_REFUSAL with why no seat can meet it. The parser
fails OPEN: it must find at least FLOOR codes, each pattern must find some, and
every exclusion must still be emitted (a stale exclusion is a red).
"""

import asyncio
import re
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
BRIDGE = HERE.parent.parent / "bridge"
sys.path.insert(0, str(BRIDGE / "scripts"))

R = pytest.importorskip("redesign")

WRITE_PATH_FILES = ("action_handlers.cpp", "intent_dispatch.cpp", "scheme_engine.cpp",
                    "factions.cpp", "ai_eval_hook.cpp")
CODE = r'([a-z][a-z0-9_]*[a-z0-9])'
PATTERNS = {
    "keyed": re.compile(r'key\("(?:error|reason|cull_reason|block_reason|refused_by)"\)\s*;\s*\w+\s*\.\s*'
                        r'value_string\(\s*"' + CODE + '"'),
    "json": re.compile(r'\\?"(?:error|reason|cull_reason|refused_by)\\?"\s*:\s*\\?"' + CODE + r'\\?"'),
    "helper": re.compile(r'\b(?:reject|short_error|fail|refuse)\(\s*(?:\w+\s*,\s*)?"' + CODE + '"'),
    "assigned": re.compile(r'\b(?:cull|reason|why|err|code)\s*=\s*"' + CODE + '"'),
    "member": re.compile(r'\b\w+\s*(?:\.|->)\s*reason\s*=\s*"' + CODE + '"'),
}
#: below this many codes the walker is broken, not the bridge quiet
FLOOR = 150

#: Codes the walker finds that no seat's tool can receive, with why.
NOT_A_SEAT_REFUSAL = {
    "uinput_dev_create_failed": "inject_mouse_click / select_observe_default (virtual input): an operator verb no MCP tool sends",
    "uinput_dev_setup_failed": "the same virtual-input verbs",
    "uinput_set_evbit_failed": "the same virtual-input verbs",
    "uinput_set_keybit_failed": "the same virtual-input verbs",
    "uinput_set_relbit_failed": "the same virtual-input verbs",
    "x_y_required": "inject_mouse_click's argument check: an operator verb no MCP tool sends",
    "not_in_lobby_state": "select_observe_default (the observer boot): an operator verb no MCP tool sends",
    "lobby_view_not_constructed": "the observer boot's lobby capture: an operator verb no MCP tool sends",
    "unknown_rejection": ("the AI-pipeline declare's own 'I do not know why' (action_handlers.cpp, "
                          "the war count rose by none and no cause was read): unmapped_refusal is the true gate"),
}


def _strip(text):
    cpp_strip = pytest.importorskip("cpp_strip")
    return cpp_strip.strip(text, keep_strings=True)


def _body(text, signature):
    i = text.index(signature)
    return text[i:text.index("\n}\n", i)]


def census():
    """{code: 'file' } and the per-pattern hit counts."""
    if not (BRIDGE / "src").is_dir():
        pytest.skip("no bridge sources in this layout")
    found, hits = {}, {k: 0 for k in PATTERNS}
    for fn in WRITE_PATH_FILES:
        t = _strip((BRIDGE / "src" / fn).read_text(errors="replace"))
        for name, p in PATTERNS.items():
            for m in p.finditer(t):
                hits[name] += 1
                found.setdefault(m.group(1), fn)
    q = _strip((BRIDGE / "src" / "query_handlers.cpp").read_text(errors="replace"))
    for m in re.finditer(r'return\s+"' + CODE + '"', _body(q, "static const char* cb_cull_reason(")):
        found.setdefault(m.group(1), "query_handlers.cpp:cb_cull_reason")
    strict = _body(q, "bool do_query_cb_validity_strict(")
    for m in re.finditer(r'\bcull\s*=\s*"' + CODE + '"', strict):
        found.setdefault(m.group(1), "query_handlers.cpp:do_query_cb_validity_strict")
    for m in re.finditer(r'value_string\(\s*\w+\s*\?\s*\w+\s*:\s*"' + CODE + '"', strict):
        found.setdefault(m.group(1), "query_handlers.cpp:do_query_cb_validity_strict")
    return found, hits


def test_the_walker_finds_the_write_paths_codes():
    found, hits = census()
    assert len(found) >= FLOOR, len(found)
    assert all(hits.values()), hits
    # the code carried is one the walker finds, at the strict read
    assert found.get("engine_cb_validity_check_failed", "").startswith("query_handlers.cpp")


def test_every_write_path_code_maps_to_a_gate():
    found, _ = census()
    unmapped = sorted(c for c in found if c not in NOT_A_SEAT_REFUSAL
                      and R.map_raw_reason(c)["gate"] == "unmapped_refusal")
    assert not unmapped, unmapped


def test_every_exclusion_is_still_emitted_and_still_unmapped():
    found, _ = census()
    for code in NOT_A_SEAT_REFUSAL:
        assert code in found, code                         # a stale exclusion
        assert R.map_raw_reason(code)["gate"] == "unmapped_refusal", code


#: Codes the walk over every bridge source finds beyond the write paths that no seat's tool receives, with why
#: (each read from its emitter)
BRIDGE_WIDE_NOT_A_SEAT_REFUSAL = {
    "army_disband": "army_orders.cpp: the action's name, the first argument of refuse(out, action, code)",
    "army_hold": "the same: an action's name, not a code",
    "army_move": "the same: an action's name, not a code",
    "army_raid": "the same: an action's name, not a code",
    "auth_refused": ("ipc_server.cpp: the socket's first-line handshake, answered to the client before any call "
                     "(bridge_client raises on it at connect)"),
    "peer_uid_refused": "ipc_server.cpp: a connection from another user, refused and closed at accept",
    "bridge_busy": "ipc_server.cpp: every client slot held; sent and the socket closed at accept, before any call",
    "same_world_epoch": ("session.cpp resume_identity_verdict: the reason a resumed session's claims were kept or "
                         "discarded (the handshake's identity_reason), not a refusal of a call"),
    "declared_same_save": "the same resume verdict's reason",
    "declared_different_save": "the same resume verdict's reason",
    "declared_same_save_after_reload": "the same resume verdict's reason",
    "world_epoch_unavailable_abstain": "the same resume verdict's reason",
    "world_changed_no_recorded_identity": "the same resume verdict's reason",
    "world_changed_no_declared_identity": "the same resume verdict's reason",
}
#: below this many codes the walk over every file is broken
WIDE_FLOOR = 300


def wide_census():
    """{code: file} over every .cpp/.h/.inc of the bridge's sources and headers, the same refusal positions."""
    if not (BRIDGE / "src").is_dir():
        pytest.skip("no bridge sources in this layout")
    found = {}
    files = [f for d in ("src", "include") for f in sorted((BRIDGE / d).iterdir())
             if f.suffix in (".cpp", ".h", ".inc")]
    for f in files:
        t = _strip(f.read_text(errors="replace"))
        for p in PATTERNS.values():
            for m in p.finditer(t):
                found.setdefault(m.group(1), f.name)
    return found


def test_the_wide_walk_finds_the_read_handlers_codes():
    found = wide_census()
    assert len(found) >= WIDE_FLOOR, len(found)
    # a read's refusal the write-path walk cannot see (a marriage listing for a ruler with no capital)
    assert found.get("actor_capital_unresolved") == "query_handlers.cpp"


def test_every_bridge_code_maps_to_a_gate():
    """RED before the read handlers' codes had rows: actor_capital_unresolved reached a seat as unmapped_refusal."""
    found = wide_census()
    skip = set(NOT_A_SEAT_REFUSAL) | set(BRIDGE_WIDE_NOT_A_SEAT_REFUSAL)
    unmapped = sorted(c for c in found if c not in skip and R.map_raw_reason(c)["gate"] == "unmapped_refusal")
    assert not unmapped, unmapped


def test_every_wide_exclusion_is_still_emitted_and_still_unmapped():
    found = wide_census()
    for code in BRIDGE_WIDE_NOT_A_SEAT_REFUSAL:
        assert code in found, code
        assert R.map_raw_reason(code)["gate"] == "unmapped_refusal", code


def test_a_marriage_read_for_a_ruler_with_no_capital_says_what_to_send():
    """The bridge's reply shape (query_handlers.cpp do_query_marriage_candidates) through the envelope every read
    reply passes: a typed gate and a note naming the argument to change, never unmapped_refusal."""
    env = R.envelope("query", result={"action": "query_marriage_candidates", "error": "actor_capital_unresolved",
                                      "hint": "pass require_in_range=0 to browse without a range filter"})
    b = env["blocked"]
    assert env["ok"] is False and b["gate"] == "invalid_argument", b
    assert b["raw_reason"] == "actor_capital_unresolved"
    assert "require_in_range=false" in b["note"] and "no capital" in b["note"]


def test_a_mapped_code_with_no_note_carries_none():
    b = R.map_raw_reason("bad_cb_list")
    assert b["gate"] == "invalid_argument" and "note" not in b


def test_every_mapped_gate_is_a_declared_gate():
    for code, gate in R.RAW_REASON_TO_GATE.items():
        assert gate in R.GATE_TABLE, (code, gate)


def test_the_r2_code_is_cb_invalid_for_target_and_never_retried():
    b = R.map_raw_reason("engine_cb_validity_check_failed")
    assert b["gate"] == "cb_invalid_for_target" and b["retryable"] is False
    assert "cbs(" in b["next"] and "plan_war(" in b["next"]


# ── the declare_war reply itself ─────────────────────────────────────────────

class GW:
    health = "ok"

    def __init__(self, cull):
        self.cull, self.sent = cull, []

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        if action == "query_character":
            return {"id": 13902, "alive": True, "name": "Æthelred", "age": 30}
        if action == "get_active_wars":
            return {"wars": []}
        if action == "query_cb_validity_strict":
            # do_query_cb_validity_strict results[] keys, as R2's row would carry them
            return {"status": "ok", "results": [
                {"cb_index": 5, "cb_name": "county_conquest_cb", "target_mode": "title",
                 "outer_status": "pass", "inner_status": "fail", "declarable": False,
                 "cull_reason": self.cull}], "declarable_count": 0}
        return {}

    async def write(self, action, params=None):
        self.sent.append(action)
        return {}

    def game(self):
        return {"tick": 50, "date": "0867.02.20", "paused": True}


def test_declare_war_refused_by_the_cb_check_names_the_doors_that_list_what_works():
    gw = GW("engine_cb_validity_check_failed")
    env = asyncio.run(R.declare_war_impl(gw, 13902, target_id=12604, cb="county_conquest_cb",
                                         confirm_straddle=True))
    assert env["ok"] is False and env["verdict"] == "rejected"
    b = env["blocked"]
    assert b["gate"] == "cb_invalid_for_target", b
    assert b["raw_reason"] == "engine_cb_validity_check_failed"
    assert b["fix"]["tool"] == "cbs" and b["fix"]["args"]["target_id"] == 12604
    assert b["fix"]["or"]["tool"] == "plan_war"
    assert gw.sent == []                                   # refused before any dispatch


def test_control_a_truce_cull_keeps_its_own_gate():
    env = asyncio.run(R.declare_war_impl(GW("target_in_truce"), 13902, target_id=12604,
                                         cb="county_conquest_cb", confirm_straddle=True))
    assert env["blocked"]["gate"] == "target_truced" and "fix" not in env["blocked"]
