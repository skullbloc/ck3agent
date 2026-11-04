"""lane-query: the query/do defects the 2026.10.0 personas hit.

Driven through the real redesign/tools functions with a scripted gateway, so a
row reds on what the function SENDS and RETURNS, not on its source text. Where
the contract is with the bridge (a key a C++ handler reads), the row reads the
handler's own text, because a literal written only here could agree with itself.

  1. council_task forwarded no `seat`; the enumerate printed seat "chancellor"
     for task_fabricate_claim, a court-chaplain task.
  2. laws enumerate read keys the bridge never emits; mercenaries had no label
     or size; men_at_arms had no enumerate and the refusal's `next` was a
     placeholder.
  4. neighbors forwarded only title_id.
  6. cbs did not say it is the authority over war_targets.
  7. set_phase sent set_character_ai with character_id 0 (refused by the
     bridge) and reported the phase applied without reading the refusal.
"""
import asyncio
import os
import re
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

redesign = pytest.importorskip("redesign")
tools = pytest.importorskip("tools")

BRIDGE_SRC = os.path.join(os.path.dirname(__file__), "..", "..", "bridge", "src")


class ScriptedGateway:
    """Records every read/write; answers from a per-action script."""

    def __init__(self, cid=13902, replies=None):
        self.cid = cid
        self.replies = dict(replies or {})
        self.reads: list = []
        self.writes: list = []
        self.health = "ok"

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character":
            return {"id": self.cid, "alive": True, "name": "seat"}
        r = self.replies.get(action, {})
        return r(params) if callable(r) else r


    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        r = self.replies.get(action, {"success": True})
        return r(params) if callable(r) else r

    def game(self):
        return {"tick": 1, "date": "867.03.21", "paused": True}


def _run(coro):
    return asyncio.run(coro)


def _handler_text(fname, handler):
    src = open(os.path.join(BRIDGE_SRC, fname), encoding="utf-8", errors="replace").read()
    m = re.search(r"\nbool " + re.escape(handler) + r"\(", src)
    assert m, f"{handler} not found in {fname}"
    nxt = re.search(r"\nbool do_[a-z_0-9]+\(", src[m.end():])
    return src[m.start(): m.end() + (nxt.start() if nxt else len(src))]


# ---------------------------------------------------------------- item 1
def test_council_task_seat_reaches_the_wire_under_the_key_the_bridge_reads():
    gw = ScriptedGateway(replies={"set_council_task": {
        "success": True, "applied": True, "seat": "councillor_court_chaplain",
        "position_id": 2411, "councillor_id": 34189}})
    env = _run(redesign.do_impl(gw, intent="council_task", character_id=13902,
                                args={"seat": "chancellor", "task": "task_fabricate_claim",
                                      "target_title_id": 329}))
    sent = [p for a, p in gw.writes if a == "set_council_task"]
    assert sent, "no set_council_task write was issued"
    assert sent[0].get("seat") == "chancellor", sent[0]
    assert sent[0].get("target_title_id") == 329
    seg = _handler_text("action_handlers.cpp", "do_set_council_task")
    assert 'json_get_string(params_json, "seat")' in seg, \
        "the bridge handler does not read the `seat` key this surface sends"
    assert env["verdict"] == "applied"


def test_council_task_no_seat_sends_no_seat():
    gw = ScriptedGateway(replies={"set_council_task": {"success": True}})
    _run(redesign.do_impl(gw, intent="council_task", character_id=13902,
                          args={"task": "task_domestic_affairs"}))
    sent = [p for a, p in gw.writes if a == "set_council_task"][0]
    assert "seat" not in sent


def test_wrong_seat_refusal_is_typed_and_names_the_task_seat():
    warning = ("'task_fabricate_claim' is a councillor_court_chaplain task; the game "
               "assigns it only to that seat.")
    gw = ScriptedGateway(replies={"set_council_task": {
        "success": False, "reason": "seat_cannot_take_task",
        "requested_seat": "chancellor", "task_seat": "councillor_court_chaplain",
        "warning": warning}})
    env = _run(redesign.do_impl(gw, intent="council_task", character_id=13902,
                                args={"seat": "chancellor", "task": "task_fabricate_claim",
                                      "target_id": 329}))
    assert env["ok"] is False and env["verdict"] == "rejected"
    assert env["blocked"]["gate"] == "target_ineligible", env["blocked"]
    assert "councillor_court_chaplain" in env["blocked"]["summary"]
    assert env["result"]["raw"]["task_seat"] == "councillor_court_chaplain"


def test_fabricate_enumerate_prints_the_chaplain_and_the_bridge_accepts_it():
    gw = ScriptedGateway(replies={"query_fabricate_targets": {
        "targets": [{"title_id": 329, "title": "c_x", "assignable": True}]}})
    env = _run(redesign.do_impl(gw, action_class="council_tasks", character_id=13902))
    row = env["result"]["available"][0]
    assert row["do_call"]["args"]["seat"] == "court_chaplain"
    # The bridge compares after stripping "councillor_" and lower-casing; the
    # task's seat key for task_fabricate_claim is councillor_court_chaplain.
    # L20-A: the comparison moved into council_task_family.h (norm_seat, run by
    # bridge/test/test_council_task_family.cpp on "court_chaplain" and
    # "councillor_court_chaplain"), and do_set_council_task resolves through it.
    seg = _handler_text("action_handlers.cpp", "do_set_council_task")
    assert "council_task_family::resolve(" in seg
    hdr = open(os.path.join(BRIDGE_SRC, "..", "include", "council_task_family.h"),
               encoding="utf-8").read()
    assert 'const std::string pre = "councillor_";' in hdr


# ---------------------------------------------------------------- item 2
def test_laws_enumerate_reads_the_groups_the_bridge_emits():
    gw = ScriptedGateway(replies={"query_available_law_changes": {
        "character_id": 13902,
        "groups": [{"group_name": "crown_authority", "current_law": "crown_authority_0",
                    "options": [{"name": "crown_authority_1", "law_id": 5, "level": 1}]}]}})
    env = _run(redesign.do_impl(gw, action_class="laws", character_id=13902))
    rows = env["result"]["available"]
    assert [r["do_call"]["args"]["law"] for r in rows] == ["crown_authority_1"]
    assert "crown_authority_0" in rows[0]["label"]
    seg = _handler_text("query_handlers.cpp", "do_query_available_law_changes")
    assert 'r.key("groups")' in seg and 'r.key("options")' in seg


def test_laws_enumerate_empty_says_why():
    gw = ScriptedGateway(replies={"query_available_law_changes": {"error": "not_a_ruler"}})
    env = _run(redesign.do_impl(gw, action_class="laws", character_id=13902))
    codes = {w["code"]: w for w in env["warnings"]}
    assert "no_law_options" in codes and "not_a_ruler" in codes["no_law_options"]["detail"]


def test_mercenary_rows_carry_label_troops_and_the_actor():
    gw = ScriptedGateway(replies={"query_hireable_mercenaries": {
        "companies": [{"company_id": 7, "label": "Mercenary company from Fante, 1451 men",
                       "troops": 1451, "home_county_name": "Fante", "cost": None,
                       "regiments": [{"maa_type": "levies", "men": 829}], "distance_px": 40}],
        "count": 1, "listed_before_cap": 551, "order": "nearest_first",
        "cost_note": "cost is null"}})
    env = _run(redesign.do_impl(gw, action_class="mercenaries", character_id=13902))
    (action, params), = [r for r in gw.reads if r[0] == "query_hireable_mercenaries"]
    assert params == {"actor_id": 13902, "max_results": 25}
    row = env["result"]["available"][0]
    assert row["label"] and row["troops"] == 1451 and row["distance_px"] == 40
    codes = {w["code"] for w in env["warnings"]}
    assert {"merc_cost_not_read", "truncated"} <= codes


def test_men_at_arms_is_an_action_class_with_rows():
    assert "men_at_arms" in tools.ACTION_CLASSES and "men_at_arms" in redesign.ACTION_CLASSES
    gw = ScriptedGateway(replies={"query_maa_types": {
        "gold": 100.0,
        "types": [{"maa_type": "huscarl", "recruit_cost": 115.2, "upkeep_per_stack": 1.5,
                   "affordable": False, "fielded_regiments": 0},
                  {"maa_type": "light_footmen", "recruit_cost": 45.0, "upkeep_per_stack": 0.45,
                   "affordable": True, "fielded_regiments": 2}]}})
    env = _run(redesign.do_impl(gw, action_class="men_at_arms", character_id=13902))
    assert [r["do_call"]["args"]["maa_type"] for r in env["result"]["available"]] == ["light_footmen"]
    blocked = env["result"]["unavailable"][0]["blocked"]
    assert blocked["gate"] == "insufficient_gold"
    reg = open(os.path.join(BRIDGE_SRC, "action_registry.inc")).read()
    assert 'BRIDGE_ACTION("query_maa_types", do_query_maa_types)' in reg


def test_missing_argument_next_names_a_real_class():
    blocked = redesign.why_blocked("missing_required_argument", summary="maa_type_required")
    assert "<this intent's class>" in blocked["next"], "precondition: the placeholder exists"
    out = redesign.concrete_next(dict(blocked), "men_at_arms")
    assert "<this intent's class>" not in out["next"]
    assert "'men_at_arms'" in out["next"]
    assert redesign.INTENT_ENUMERATE_CLASS["men_at_arms"] in redesign.ACTION_CLASSES
    for intent, cls in redesign.INTENT_ENUMERATE_CLASS.items():
        assert intent in redesign.INTENTS and cls in redesign.ACTION_CLASSES, (intent, cls)
    out2 = redesign.concrete_next(dict(blocked), "move_capital")
    assert "no enumerate mode" in out2["next"]


# ---------------------------------------------------------------- item 4
def test_neighbors_forwards_character_id():
    action, params = tools.resolve_query("neighbors", {"character_id": 13902, "title_id": 0})
    assert action == "query_county_neighbors"
    assert params == {"title_id": 0, "character_id": 13902}
    seg = _handler_text("query_handlers.cpp", "do_query_county_neighbors")
    assert "resolve_character(params_json)" in seg and 'r.key("bordering_realms")' in seg


# ---------------------------------------------------------------- item 6
def test_cbs_says_it_is_the_authority():
    gw = ScriptedGateway(replies={"query_cbs_for_target": {"cbs": []}})
    env = _run(redesign.cbs_impl(gw, target_id=12145, character_id=13902))
    assert env["result"]["authority"] == "engine"
    seg = _handler_text("query_handlers.cpp", "do_query_valid_war_targets")
    assert 'r.key("declarable_authoritative"); r.value_bool(false);\n    r.key("authority")' in seg


# ---------------------------------------------------------------- item 7
def test_set_phase_reasserts_the_ai_gate_on_the_RESOLVED_actor():
    gw = ScriptedGateway(cid=30853, replies={"set_character_ai": {"success": True,
                                                                  "ai_enabled": True}})
    env = _run(redesign.set_phase_impl(gw, phase="consolidate", character_id=0))
    sent = [p for a, p in gw.writes if a == "set_character_ai"]
    assert sent == [{"character_id": 30853, "enabled": True}], sent
    assert env["result"]["ai_gate"]["reasserted"] is True
    assert "ai_gate_reassert_failed" not in {w["code"] for w in env["warnings"]}


def test_set_phase_reports_a_refused_reassert():
    gw = ScriptedGateway(cid=30853, replies={"set_character_ai": {
        "success": False, "ai_enabled": None, "reason": "no_ai_controller"}})
    env = _run(redesign.set_phase_impl(gw, phase="consolidate", character_id=30853))
    assert env["result"]["ai_gate"] == {"character_id": 30853, "reasserted": False,
                                        "ai_enabled_read_back": None,
                                        "reason": "no_ai_controller"}
    assert "ai_gate_reassert_failed" in {w["code"] for w in env["warnings"]}


def test_set_phase_reads_the_ai_byte_back_not_the_write_return():
    # F11 (cleanup 2026.11, lane l6): the bridge answers success with the byte
    # as it reads after the store; a store that did not take is not reasserted.
    gw = ScriptedGateway(cid=30853, replies={"set_character_ai": {
        "success": True, "ai_enabled": False, "requested": True, "applied": False}})
    env = _run(redesign.set_phase_impl(gw, phase="consolidate", character_id=30853))
    assert env["result"]["ai_gate"]["reasserted"] is False
    assert env["result"]["ai_gate"]["ai_enabled_read_back"] is False
    assert "ai_gate_reassert_failed" in {w["code"] for w in env["warnings"]}
    ok = ScriptedGateway(cid=30853, replies={"set_character_ai": {
        "success": True, "ai_enabled": True, "requested": True, "applied": True}})
    env = _run(redesign.set_phase_impl(ok, phase="consolidate", character_id=30853))
    assert env["result"]["ai_gate"]["reasserted"] is True
    assert env["result"]["ai_gate"]["ai_enabled_read_back"] is True


def test_bridge_set_character_ai_success_reads_the_byte_back():
    src = open(os.path.join(BRIDGE_SRC, "dispatch_router.cpp")).read()
    m = re.search(r"static bool case_set_character_ai\(.*?\n}\n", src, re.S)
    assert m
    body = m.group(0)
    assert "ck3::ai_ctrl::AI_ENABLED" in body and 'rb.key("applied")' in body
    assert 'rb.key("ai_enabled"); rb.value_bool(enabled);' not in body


def test_bridge_set_character_ai_failure_does_not_claim_ai_enabled():
    src = open(os.path.join(BRIDGE_SRC, "dispatch_router.cpp")).read()
    m = re.search(r"static bool case_set_character_ai\(.*?\n}\n", src, re.S)
    assert m
    body = m.group(0)
    assert "ok ? enabled : true" not in body
    assert 'rb.key("ai_enabled"); rb.value_null();' in body


def test_men_at_arms_enumerate_is_capped_and_candidates_filter():
    types = [{"maa_type": f"t{i}", "recruit_cost": float(i), "upkeep_per_stack": 0.1,
              "affordable": True, "fielded_regiments": 0} for i in range(109)]
    gw = ScriptedGateway(replies={"query_maa_types": {"gold": 1e6, "types": types}})
    env = _run(redesign.do_impl(gw, action_class="men_at_arms", character_id=13902))
    rows = env["result"]["available"]
    assert len(rows) == redesign.MAA_ENUMERATE_CAP
    assert rows[0]["do_call"]["args"]["maa_type"] == "t1", "zero-cost type must sort last"
    assert "truncated" in {w["code"] for w in env["warnings"]}
    env = _run(redesign.do_impl(gw, action_class="men_at_arms", character_id=13902,
                                candidates=["t0", "t50"]))
    assert sorted(r["do_call"]["args"]["maa_type"] for r in env["result"]["available"]) == ["t0", "t50"]


def test_building_rows_come_from_the_bridge_reply_and_name_the_holding_they_need():
    # cleanup 2026.11, lane l6: the class read keys the bridge never emits and
    # listed nothing; its do_call carried location_id 0, which build refuses.
    types = [{"name": f"b_{i:02d}", "level": 1} for i in range(45)]
    gw = ScriptedGateway(cid=13902, replies={"query_building_types": {"building_types": types}})
    env = _run(redesign.do_impl(gw, action_class="buildings", character_id=13902))
    rows = env["result"]["available"]
    assert len(rows) == redesign.BUILDING_ENUMERATE_CAP
    assert rows[0]["do_call"] == {"intent": "build", "args": {"building": "b_00"}}
    assert "location_id" in rows[0]["needs"]
    assert "truncated" in {w["code"] for w in env["warnings"]}
    gw = ScriptedGateway(cid=13902, replies={"query_building_types": {"building_types": types}})
    env = _run(redesign.do_impl(gw, action_class="buildings", character_id=13902,
                                candidates=["b_44"]))
    assert [r["do_call"]["args"]["building"] for r in env["result"]["available"]] == ["b_44"]
    # args.location_id: each row is the game's verdict there (construction_check)
    # and its do_call is complete (no `needs`).
    checks = {
        "b_01": {"success": True, "buildable": True, "price": {"gold": 50.0}, "gold": 200.0,
                 "engine_reasons": []},
        "b_02": {"success": True, "buildable": False, "price": {"gold": 50.0}, "gold": 200.0,
                 "engine_reasons": [_HAS_HOLDING]},
        "b_03": {"success": True, "buildable": False, "price": {"gold": 550.0}, "gold": 200.0,
                 "engine_reasons": [_MISSING_GOLD]},
    }
    gw = ScriptedGateway(cid=13902, replies={
        "query_building_types": {"building_types": types},
        "construction_check": lambda p: {
            "success": True, "gold": 200.0,
            "rows": [{"building": b, **checks[b]} for b in p["buildings"]]}})
    env = _run(redesign.do_impl(gw, action_class="buildings", character_id=13902,
                                candidates=["b_01", "b_02", "b_03"], args={"location_id": 1268}))
    av = {r["do_call"]["args"]["building"]: r for r in env["result"]["available"]}
    un = {r["do_call"]["args"]["building"]: r for r in env["result"]["unavailable"]}
    assert set(av) == {"b_01"} and set(un) == {"b_02", "b_03"}
    row = av["b_01"]
    assert row["do_call"] == {"intent": "build", "args": {"building": "b_01", "location_id": 1268}}
    assert "needs" not in row
    assert un["b_02"]["blocked"]["raw_reason"] == "construction_refused"
    assert un["b_02"]["blocked"]["summary"] == "the game refuses: This Barony already contains a Holding"
    assert un["b_03"]["blocked"]["summary"] == "the game refuses: Missing gold 318"
    # One call for the whole list (witness #2: one inline engine call per row, on the
    # IPC thread, tripped the scope latch); none per building.
    assert [r for r in gw.reads if r[0] == "construction_check"] == [
        ("construction_check", {"actor_id": 13902, "location_id": 1268,
                                "buildings": ["b_01", "b_02", "b_03"]})]
    # A list the bridge could not check leaves every row blocked with its reason.
    gw = ScriptedGateway(cid=13902, replies={
        "query_building_types": {"building_types": types},
        "construction_check": {"success": False, "reason": "rng_scope_latched"}})
    env = _run(redesign.do_impl(gw, action_class="buildings", character_id=13902,
                                candidates=["b_01", "b_02"], args={"location_id": 1268}))
    assert env["result"]["available"] == []
    assert [r["blocked"]["raw_reason"] for r in env["result"]["unavailable"]] == ["rng_scope_latched"] * 2


# The construction validity's lines as the game wrote them (live l6fix2-live-1).
_HAS_HOLDING = ("\x16warning_icon! \x15X This Barony already contains a \x15E; "
                "\x15TOOLTIP:GAME_CONCEPT,holding Holding\x15!\x15!\x15!")
_MISSING_GOLD = ("\x16warning_icon! \x15X Missing \x15TOOLTIP:GAME_CONCEPT,gold_i "
                 "\x16gold_icon!\x15! 318\x15!\x15indent_newline:0")
_INNOVATION = ("\x15!\x15indent_newline:6 \x16trigger_fail! The \x15ONCLICK:CULTURE,40 "
               "\x15TOOLTIP:CULTURE,40 \x15L; Swabian\x15!\x15!\x15! \x15E; "
               "\x15TOOLTIP:GAME_CONCEPT,culture Culture\x15!\x15! has the "
               "\x15TOOLTIP:CULTURE_INNOVATION,40,14 \x15L; Battlements\x15!\x15! \x15E; "
               "\x15TOOLTIP:GAME_CONCEPT,innovation Innovation\x15!\x15!\x15!")


_WORD_STYLE = ("\x15X Requires that \x15high Serene Doge\x15! \x15high Orso\x15!'s "
               "\x15E; \x15TOOLTIP:GAME_CONCEPT,government Government\x15!\x15! Enables Bartering\x15!")


def test_the_games_tooltip_text_reads_plain():
    assert redesign.engine_text(_HAS_HOLDING) == "This Barony already contains a Holding"
    assert redesign.engine_text(_MISSING_GOLD) == "Missing gold 318"
    assert redesign.engine_text(_INNOVATION) == \
        "not met: The Swabian Culture has the Battlements Innovation"
    assert redesign.engine_text("\x15!\x15indent_newline:0") == ""
    # A style named by a word (l6fin-1: "Requires that igh Serene Doge igh Orso's
    # Government ..." while only the style's first letter was removed).
    assert redesign.engine_text(_WORD_STYLE) == \
        "Requires that Serene Doge Orso's Government Enables Bartering"


def test_build_refusal_carries_the_games_reasons_and_a_build_sends_no_execute_flag():
    # cleanup 2026.11, lane l6: build runs the player's construction path; a
    # refusal names the game's reasons, as its tooltip shows them.
    refused = {"success": False, "action": "construct_building", "reason": "construction_refused",
               "engine_reasons": [_MISSING_GOLD, _INNOVATION], "price": {"gold": 522.5},
               "gold_before": 204.77}
    gw = ScriptedGateway(cid=13902, replies={"construct_building": refused})
    env = _run(redesign.do_impl(gw, intent="build", character_id=13902,
                                args={"building": "castle_02", "location_id": 1142}))
    assert env["ok"] is False and env["blocked"]["raw_reason"] == "construction_refused"
    assert env["blocked"]["summary"] == ("the game refuses: Missing gold 318; not met: The Swabian "
                                         "Culture has the Battlements Innovation")
    assert env["result"]["raw"]["engine_reasons"] == [_MISSING_GOLD, _INNOVATION]
    assert gw.writes == [("construct_building", {"actor_id": 13902, "building": "castle_02",
                                                 "location_id": 1142})]


def test_an_obligation_raise_is_refused_with_a_named_gate_and_the_reason():
    # lane l6, witness #2: the raise refusal read as gate unmapped_refusal (l6fin4-latch-1).
    gw = ScriptedGateway(cid=10465, replies={"set_obligation_level": {
        "success": False, "action": "set_obligation_level", "reason": "raise_requires_negotiation",
        "old_level": 2, "requested_level": 3}})
    env = _run(redesign.do_impl(gw, intent="set_obligation", character_id=10465,
                                args={"vassal_id": 10749, "obligation": "feudal_government_taxes",
                                      "level": 3}))
    assert env["ok"] is False and env["verdict"] == "rejected"
    b = env["blocked"]
    assert b["gate"] == "target_ineligible" and b["raw_reason"] == "raise_requires_negotiation"
    assert "consent" in b["summary"] and "2 -> 3" in b["summary"]
    assert gw.writes == [("set_obligation_level", {"actor_id": 10465, "vassal_id": 10749,
                                                   "obligation": "feudal_government_taxes",
                                                   "level": 3})]
