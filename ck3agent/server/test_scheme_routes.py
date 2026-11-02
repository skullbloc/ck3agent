"""The scheme routes a player uses (cleanup 2026.11 F5, lane l3-schemes):
discover(what="scheme_types"), discover(what="scheme_targets") and
do(intent="scheme") start/cancel, against a fake gateway that records every bridge
call. What the bridge does with them is proven live (scripts/live867/scenarios/
l3_schemes_matrix.py); these pin what the server sends and how it reads the reply."""
import asyncio
import pathlib
import re
import pytest

import redesign
import tools
import do_intent_args


def _cpp_code(text: str) -> str:
    """Comments blanked, literals kept: the one C++ tokenizer (L13 FF3,
    bridge/scripts/cpp_strip.py). A `//` inside a string is not a comment."""
    import sys as _sys
    from pathlib import Path as _Path
    _sys.path.insert(0, str(_Path(__file__).resolve().parents[2] / "bridge" / "scripts"))
    import cpp_strip
    return cpp_strip.strip(text, keep_strings=True)


OWNER = 9238
TARGET = 10447


class _Gateway:
    def __init__(self, write_reply=None, schemes_after=None, check_reply=None,
                 types_reply=None, schemes_before=None):
        """schemes_before answers the first query_schemes when given (a cancel reads
        the owner's list before it sends); schemes_after every other one."""
        self.write_reply = write_reply or {}
        self.schemes_after = schemes_after
        self.schemes_before = schemes_before
        self.check_reply = check_reply or {}
        self.types_reply = types_reply
        self.writes, self.reads, self.checks = [], [], []
        self.health = "ok"

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        if action == "scheme_action":
            # scheme_targets' check: an intent the drain evaluates (gw.read)
            self.checks.append((action, dict(params or {})))
            return self.check_reply
        self.reads.append((action, dict(params or {})))
        if action == "query_character":
            return {"id": OWNER, "alive": True,
                                  "name": "Anangapida"}
        if action == "query_schemes":
            if self.schemes_before is not None:
                before, self.schemes_before = self.schemes_before, None
                return before
            return self.schemes_after if self.schemes_after is not None else {}
        if action == "query_scheme_types":
            return self.types_reply if self.types_reply is not None else {}
        return {}

    async def read_many(self, specs):
        return [{} for _ in specs]

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return self.write_reply

    def game(self):
        return {"tick": 1, "game_date": "0867.01.02"}


def _do(gw, **args):
    return asyncio.run(redesign.do_impl(gw, intent="scheme", character_id=OWNER, args=args))


# --- start -------------------------------------------------------------------

def test_start_sends_the_scheme_the_player_named_and_no_default():
    gw = _Gateway({"success": True, "result": {"scheme_id": 77, "created": True}},
                  {"schemes": [{"scheme_id": 77, "type": "befriend", "target_id": TARGET}]})
    env = _do(gw, action="start", scheme="befriend", target_id=TARGET)
    assert gw.writes == [("scheme_action", {"actor_id": OWNER, "action": "start",
                                            "target_id": TARGET, "scheme": "befriend"})]
    assert env["verdict"] == "applied"
    assert env["result"]["scheme"]["type"] == "befriend"


def test_a_start_without_a_scheme_is_refused_before_the_bridge():
    """It used to default to murder: a player who forgot the key got a murder."""
    gw = _Gateway()
    env = _do(gw, action="start", target_id=TARGET)
    assert gw.writes == []
    assert env["blocked"]["gate"] == "missing_required_argument"
    assert env["blocked"]["fix"]["rejected_fields"] == ["scheme"]


def test_a_start_the_second_read_does_not_find_is_not_applied():
    gw = _Gateway({"success": True, "result": {"scheme_id": 77}}, {"schemes": []})
    env = _do(gw, action="start", scheme="murder", target_id=TARGET)
    assert env["verdict"] == "rejected"
    assert env["blocked"]["gate"] == "write_unconfirmed"
    assert env["blocked"]["measured"]["listed"] is False


def test_the_readback_is_a_different_read_than_the_write():
    gw = _Gateway({"success": True, "result": {"scheme_id": 5}},
                  {"schemes": [{"scheme_id": 5, "type": "sway", "target_id": TARGET}]})
    _do(gw, action="start", scheme="sway", target_id=TARGET)
    assert ("query_schemes", {"character_id": OWNER}) in gw.reads


def test_the_engine_refusal_carries_the_engine_reasons():
    # The bridge's row shape since L14 S4 (scheme_engine.cpp emit_reasons).
    reasons = [{"check": "allow", "text": ""}]
    gw = _Gateway({"success": False, "reason": "engine_refused_can_start_scheme",
                   "result": {"created": False, "engine_reasons": reasons}})
    env = _do(gw, action="start", scheme="murder", target_id=TARGET)
    b = env["blocked"]
    assert b["gate"] == "scheme_start_refused"
    assert b["measured"]["engine_reasons"] == [
        {"check": "allow", "summary": redesign.NO_BRIDGE_WORDS}]
    assert b["retry"] == "never"
    # One shape per reply (L14 FF1 m-22): the bridge's {check, text} copy is not
    # carried beside measured's {check, summary}.
    assert "engine_reasons" not in env["result"]["raw"].get("result", {}), env["result"]
    assert env["result"]["raw"]["result"] == {"created": False}


def test_an_allow_trigger_says_the_bridge_reads_no_words_not_that_the_game_has_none():
    """L14 FF1 M-11: the bridge never asks the game for an allow/valid trigger's
    words (scheme_engine.cpp wants_text 0), so "the game gives no detail" would
    claim a reading nobody made. Two distinct constants. An engine entry is sent
    only with text (emit_reasons skips an empty one); text that is markup with no
    words keeps the game's-no-detail wording (L14 FF2 N-7: the reachable case)."""
    assert redesign.NO_BRIDGE_WORDS != redesign.NO_GAME_DETAIL
    out = redesign.scheme_reasons([{"check": "valid", "text": ""},
                                   {"check": "engine_entry", "text": "All of these:"},
                                   {"check": "allow", "text": "Not adult"}])
    assert out == [{"check": "valid", "summary": redesign.NO_BRIDGE_WORDS},
                   {"check": "engine_entry", "summary": redesign.NO_GAME_DETAIL},
                   {"check": "allow", "summary": "Not adult"}]


def test_every_wordless_row_the_bridge_sends_says_what_it_is():
    """L14 FF2 N-7. The rows scheme_engine.cpp sends with text "" (emit_reasons:
    allow / valid / trigger and sink_method_not_modelled; scheme_can_start:
    owner_invalid / target_invalid) never read "the game gives no detail": the
    game was not asked. RED on b84e3634 (trigger, sink_method_not_modelled,
    owner_invalid and target_invalid all said NO_GAME_DETAIL)."""
    rows = [{"check": c, "text": ""} for c in
            ("allow", "valid", "trigger", "sink_method_not_modelled",
             "owner_invalid", "target_invalid")]
    out = {r["check"]: r["summary"] for r in redesign.scheme_reasons(rows)}
    assert all(v != redesign.NO_GAME_DETAIL for v in out.values()), out
    assert out["trigger"] == out["allow"] == out["valid"] == redesign.NO_BRIDGE_WORDS, out
    assert len({out["sink_method_not_modelled"], out["owner_invalid"],
                out["target_invalid"], redesign.NO_BRIDGE_WORDS}) == 4, out


def test_the_scheme_check_names_mirror_the_bridge():
    """The checks the server words are the checks the bridge emits: read off
    scheme_engine.cpp, so a renamed or added check reds here, not in play."""
    src_path = pathlib.Path(__file__).resolve().parents[2] / "bridge" / "src" / "scheme_engine.cpp"
    if not src_path.is_file():
        pytest.skip("the bridge source is research-tree only")
    src = src_path.read_text(encoding="utf-8")
    emitted = set(re.findall(r'"(allow|valid|trigger|engine_entry|[a-z_]+_(?:invalid|not_modelled))"', src))
    named = set(redesign.SCHEME_UNWORDED_CHECKS) | set(redesign.SCHEME_CHECK_WORDS) | {"engine_entry"}
    assert emitted == named, (emitted, named)


# --- cancel ------------------------------------------------------------------

#: The witness's run (w-l3-schemes-1, steps 123-128): learn_language 10465->10447
#: started as id 152, the cancel applied, and the AI's next scheme got id 152.
MINE = {"scheme_id": 152, "type": "learn_language", "target_id": 10447,
        "start_date": 244042142049125040}
NEXT = {"scheme_id": 152, "type": "learn_language", "target_id": 13434,
        "start_date": 244042159228994320}
DONE = {"success": True, "result": {"removed": True}}


def test_cancel_is_applied_only_when_the_scheme_is_gone():
    gone = _Gateway(DONE, {"schemes": []}, schemes_before={"schemes": [MINE]})
    env = _do(gone, action="cancel", scheme_id=152)
    assert gone.writes == [("scheme_action", {"actor_id": OWNER, "action": "cancel",
                                              "scheme_id": 152})]
    assert env["verdict"] == "applied"
    assert env["result"]["cancelled"] == MINE
    still = _Gateway(DONE, {"schemes": [MINE]}, schemes_before={"schemes": [MINE]})
    env = _do(still, action="cancel", scheme_id=152)
    assert env["blocked"]["gate"] == "write_unconfirmed"


def test_a_freed_id_given_to_a_new_scheme_is_not_the_cancelled_scheme():
    """The game reuses a freed scheme id: the read after the cancel lists id 152
    again, a different scheme. The cancel applied; the reply says so and names
    the new scheme, so the player does not cancel the AI's one by sending it again."""
    gw = _Gateway(DONE, {"schemes": [NEXT]}, schemes_before={"schemes": [MINE]})
    env = _do(gw, action="cancel", scheme_id=152)
    assert env["verdict"] == "applied"
    assert env["result"]["cancelled"] == MINE
    assert env["result"]["id_reused_by"] == NEXT
    assert "new scheme" in env["result"]["note"]


def test_a_cancel_of_a_scheme_the_owner_does_not_hold_sends_nothing():
    gw = _Gateway(DONE, schemes_before={"schemes": [NEXT | {"scheme_id": 9}]})
    env = _do(gw, action="cancel", scheme_id=152)
    assert gw.writes == []
    assert env["blocked"]["gate"] == "name_not_found"


def test_a_cancel_whose_list_cannot_be_read_first_sends_nothing():
    gw = _Gateway(DONE, schemes_before={"error": "read_timeout"})
    env = _do(gw, action="cancel", scheme_id=152)
    assert gw.writes == []
    assert env["blocked"]["gate"] == "read_timeout"


def test_a_start_whose_id_holds_another_scheme_is_not_applied():
    """The start's id read back with another type or target is not the new scheme."""
    gw = _Gateway({"success": True, "result": {"scheme_id": 152}}, {"schemes": [NEXT]})
    env = _do(gw, action="start", scheme="learn_language", target_id=10447)
    assert env["verdict"] == "rejected"
    assert env["blocked"]["gate"] == "write_unconfirmed"


def test_a_cancel_without_scheme_id_is_refused_before_the_bridge():
    gw = _Gateway()
    env = _do(gw, action="cancel")
    assert gw.writes == []
    assert env["blocked"]["fix"]["rejected_fields"] == ["scheme_id"]


def test_an_unknown_scheme_action_names_the_read_that_answers_it():
    gw = _Gateway()
    env = _do(gw, action="check", scheme="murder", target_id=TARGET)
    assert gw.writes == []
    assert env["blocked"]["gate"] == "unknown_verb"
    assert "scheme_targets" in env["blocked"]["summary"]


def test_a_timed_out_start_is_pending_not_applied():
    gw = _Gateway({"success": False, "reason": "intent_timeout_no_result"})
    env = _do(gw, action="start", scheme="murder", target_id=TARGET)
    assert env["verdict"] == "pending"


# --- discover ------------------------------------------------------------------

def test_scheme_targets_asks_the_bridge_check():
    result = {"scheme": "befriend", "valid_count": 1, "targets": [{"id": TARGET}]}
    gw = _Gateway(check_reply={"success": True, "result": result})
    env = asyncio.run(redesign.scheme_targets_impl(gw, "befriend", character_id=OWNER,
                                                   candidates=[TARGET, 0], max_results=3))
    assert gw.checks == [("scheme_action", {"actor_id": OWNER, "action": "check",
                                              "scheme": "befriend", "candidates": [TARGET],
                                              "max_results": 3})]
    assert env["ok"] is True and env["result"] == result


def test_scheme_targets_without_a_scheme_is_refused():
    gw = _Gateway()
    env = asyncio.run(redesign.scheme_targets_impl(gw, "", character_id=OWNER))
    assert gw.checks == []
    assert env["blocked"]["gate"] == "missing_required_argument"


def test_scheme_targets_refusal_is_typed():
    gw = _Gateway(check_reply={"success": False, "reason": "scheme_type_not_found:murdr"})
    env = asyncio.run(redesign.scheme_targets_impl(gw, "murdr", character_id=OWNER))
    assert env["blocked"]["gate"] == "name_not_found"


def test_scheme_types_passes_the_database_through():
    rows = {"scheme_types": [{"scheme": "murder", "target_type": "character"}], "count": 1}
    gw = _Gateway(types_reply=rows)
    env = asyncio.run(redesign.scheme_types_impl(gw))
    assert env["result"] == rows
    assert gw.reads == [("query_scheme_types", {})]


def test_the_discover_vocabulary_names_both_scheme_reads():
    assert {"scheme_types", "scheme_targets"} <= set(tools.DISCOVER_WHATS)


def test_the_do_argument_table_lists_the_scheme_keys():
    table = do_intent_args.extract(pathlib.Path(redesign.__file__).read_text())
    assert table["scheme"] == ["action", "target_id", "scheme", "scheme_id"]


# --- the bridge side the routes rely on (read from the source) -----------------

BRIDGE = pathlib.Path(__file__).resolve().parents[2] / "bridge"


def test_every_scheme_category_the_bridge_names_is_the_script_word():
    src = (BRIDGE / "src" / "scheme_engine.cpp").read_text()
    body = src[src.index("const char* scheme_category_name"):]
    body = body[:body.index("}\n}")]
    assert re.findall(r'return "([a-z]+)"', body) == ["hostile", "contract", "personal",
                                                      "political"]


def test_the_start_runs_the_engine_check_before_creating():
    src = (BRIDGE / "src" / "intent_dispatch.cpp").read_text()
    body = src[src.index("bool execute_scheme_intent(Intent* intent) {"):]
    assert body.index("scheme_can_start(") < body.index("create_scheme(mgr")


def test_the_bridge_no_longer_refuses_self_targets_itself():
    """expand_power_base targets its owner (1097 dump: owner == target); whether a
    scheme may target its owner is the game's answer."""
    src = (BRIDGE / "src" / "action_handlers.cpp").read_text()
    assert "cannot_scheme_against_self" not in src


def test_scheme_targets_with_no_list_checks_the_rulers_own_circle():
    """The bridge checks a list, never the whole character database (a full scan
    tripped the scope latch live, l3m-c1); with no target and no candidates the
    server builds the list: court and vassals, liege and his court, claimed
    titles' holders and their courts."""
    class _Circle(_Gateway):
        async def read(self, action, params=None):
            if action == "search_characters":
                self.reads.append((action, dict(params or {})))
                court = {OWNER: [1, 2], 77: [3], 88: [4, 1]}
                return {"characters": [{"id": x} for x in court.get(params["court_of"], [])]}
            if action == "query_character":
                return {"id": OWNER, "alive": True, "liege_id": 77}
            if action == "query_claims":
                return {"claims": [{"holder_id": 88}, {"holder_id": OWNER}]}
            return await super().read(action, params)

    gw = _Circle(check_reply={"success": True, "result": {"targets": []}})
    env = asyncio.run(redesign.scheme_targets_impl(gw, "befriend", character_id=OWNER))
    assert gw.checks[0][1]["candidates"] == [1, 2, 77, 3, 88, 4]
    assert [s["from"] for s in env["result"]["candidates_from"]] == [
        f"court_of:{OWNER}", "liege", "court_of:77", "court_of:88", "claim_holders"]


def test_the_bridge_check_refuses_a_scan_without_a_list():
    src = (BRIDGE / "src" / "scheme_engine.cpp").read_text()
    body = src[src.index("bool execute_scheme_check_intent(Intent* intent) {"):]
    assert 'reject("candidates_required")' in body
    assert "CHARACTER_DB" not in body


# --- one literal per field in the scheme namespaces of the offset header -------

HEADER = pathlib.Path(__file__).resolve().parents[2] / "re" / "offsets" / "ck3_offsets.h"
SCHEME_NAMESPACES = ("active_scheme", "scheme_type", "scheme_agent_slot",
                     "scheme_agent_slot_type", "scheme_type_db", "token_table",
                     "scheme_fail_entry")


def _literals(ns: str) -> dict:
    """NAME -> value for every literal constant of namespace ns (comments stripped);
    the namespace must exist."""
    src = HEADER.read_text()
    m = re.search(r"^namespace\s+" + ns + r"\s*\{(.*?)^\}", src, re.M | re.S)
    assert m, ns
    body = _cpp_code(m.group(1))
    return {n: int(v, 0) for n, v in
            re.findall(r"constexpr\s+\w+\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;", body)}


def test_no_scheme_field_has_two_literals():
    """A second literal for one field drifts: the Windows generator maps each by
    its own rule (the witness's run: CAP_TYPE by the probe, CATEGORY by a slot run).
    A second name is defined from the first."""
    for ns in SCHEME_NAMESPACES:
        by_value: dict = {}
        for name, v in _literals(ns).items():
            by_value.setdefault(v, []).append(name)
        twins = {hex(v): names for v, names in by_value.items() if len(names) > 1}
        assert not twins, (ns, twins)


def test_the_bridge_names_of_aliased_fields_are_defined_from_the_literal():
    src = _cpp_code(HEADER.read_text())
    for alias, base in (("CATEGORY", "CAP_TYPE"), ("IS_BASIC", "PASSIVE_FLAG"),
                        ("BREACHES", "AGENT_CHARGES + 0x4")):
        assert re.search(r"constexpr size_t " + alias + r"\s*=\s*" + re.escape(base)
                         + r"\s*;", src), alias


def test_every_scheme_success_chance_comes_from_one_reader():
    """inspect and the observation feed give success_chance the same way: the
    percent scheme_success_chance returns, null when it cannot be read."""
    for name in ("observation.cpp", "scheme_engine.cpp"):
        code = _cpp_code((BRIDGE / "src" / name).read_text())
        i = code.index('key("success_chance")')
        # up to the next field (success_chance_max is a stored fixed-point value)
        end = min(j for j in (code.find('key("success_chance_max")', i),
                              code.find('key("secrecy")', i)) if j > 0)
        window = code[max(0, i - 300):end]
        assert "scheme_success_chance(" in window, name
        assert "value_null()" in window, name
        assert "100000" not in window, name


# ── L14 (surface-eval minor 12; S4): a scheme refusal is words, and nothing else ──

def _bridge_refused_row(debug=False):
    """A refused row in the bridge's shape (scheme_engine.cpp emit_reasons, L14 S4).
    debug=True is the a0a2fff3 shape (the script place under debug)."""
    trig = {"check": "allow", "text": ""}
    entry = {"check": "engine_entry", "text": "You already have a Scheme against them"}
    if debug:
        trig["debug"] = {"block": "befriend:allow", "file": "common/schemes/scheme_types/befriend_scheme.txt",
                         "line": 25}
        entry["debug"] = {"key": "can_target_character_with_scheme"}
    return {"id": 4242, "reasons": [trig, entry]}


def _refused_via_server(row):
    import asyncio
    import redesign as R

    class Gw:
        health = "ok"

        async def actor_gate(self):
            return None

        async def read(self, action, params=None):
            if action == "query_character":
                return {"id": 9238, "name": "X", "alive": True}
            if action == "scheme_action":
                return {"success": True, "result": {"scheme": "befriend", "targets": [],
                                                    "refused": [row]}}
            return {}

        def game(self):
            return {"tick": 1, "date": "867.1.1", "paused": True, "speed": 1}

    env = asyncio.run(R.scheme_targets_impl(Gw(), "befriend", character_id=9238, candidates=[4242]))
    (out,) = env["result"]["refused"]
    return out


@pytest.mark.parametrize("debug", [False, True])
def test_a_refused_scheme_row_says_words_and_nothing_else(debug):
    """Each reason is {check, summary}: the game's words for an engine entry, that
    the bridge does not read the words for a trigger, and no script place even when a bridge
    still sends one (debug=True: the a0a2fff3 bridge's shape)."""
    import redesign as R
    row = _refused_via_server(_bridge_refused_row(debug))
    trig, entry = row["reasons"]
    assert trig == {"check": "allow", "summary": R.NO_BRIDGE_WORDS}
    assert entry == {"check": "engine_entry", "summary": "You already have a Scheme against them"}


def test_the_bridge_reports_no_script_place():
    """The bridge side, read from its source: every reason row emit_reasons and
    scheme_can_start write is {check, text}; no debug object, no file or line."""
    import os
    src = open(os.path.join(os.path.dirname(__file__), "..", "..", "bridge", "src",
                            "scheme_engine.cpp"), encoding="utf-8").read()
    body = src[src.index("void emit_reasons("):src.index("// the check intent")]
    rows = body.count('r.key("check")')
    assert rows >= 4, rows
    assert body.count('r.key("text")') == rows, "every reason row carries text"
    for k in ('r.key("debug")', 'r.key("file")', 'r.key("line")', 'r.key("block")'):
        assert k not in body, k
    assert "uintptr_t sink_wants_text(Sink*) { return 0; }" in src, "one sink definition: no description built"


def test_the_discover_description_promises_no_debug_on_a_scheme_row():
    """L14 int round 3: S4 deleted the scheme row's `debug` end to end, and the
    discover description S2 wrote still sent the player to it. The taught words
    for a refused scheme row name only {check, summary}."""
    import os
    src = open(os.path.join(os.path.dirname(__file__), "mcp_server.py"), encoding="utf-8").read()
    body = src[src.index("scheme_targets: whom the actor"):src.index("scheme: For what=scheme_targets")]
    assert "{check, summary}" in body
    for word in ("debug", "file", "line", "block"):
        assert word not in body, word
