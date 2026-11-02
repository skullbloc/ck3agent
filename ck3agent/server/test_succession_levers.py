"""Lane l4 (cleanup 2026.11, F6/F7): the succession levers a CK3 player relies on.

Driven through the real redesign/tools functions with a scripted gateway, so a row
reds on what do() SENDS (the bridge action and its parameters) and RETURNS, never on
source text. Where the contract is with the bridge (a key its C++ handler reads), the
row reads the handler's own text.

  1. The disinherit KIN GUARD: do() refuses a target that is not the actor's own
     child, grandchild or great-grandchild, and sends nothing to the bridge; a
     grandchild passes and is sent.
  2. Send options: do(intent='interact', args={'send_options': [...]}) reaches the
     bridge as the comma list send_interaction reads, with the secondaries and the
     title target.
  3. The levers grant_titles / disinherit are no longer refused by the server.
  4. A title law the engine refuses comes back with the engine's text, gate
     engine_refused; a queued one is pending.
  5. succession_plan reports the levers this ruler has: heir ages, succession laws,
     and designate_heir only under a law that allows it.
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

RULER = 10465
SON, GRANDSON, NEPHEW, STRANGER = 12829, 20001, 30001, 40001

#: The family the fake bridge answers query_character with: children lists by id.
FAMILY = {RULER: [SON, 13463], SON: [GRANDSON], 13463: [], GRANDSON: [],
          NEPHEW: [], STRANGER: []}


class FamilyGateway:
    """Records every read/write; query_character answers from FAMILY."""

    def __init__(self, replies=None, ages=None, laws=None, head=RULER):
        self.head = head
        self.dynasty_reads: list = []
        self.replies = dict(replies or {})
        self.ages = dict(ages or {})
        self.laws = list(laws or [])
        self.reads: list = []
        self.writes: list = []
        self.health = "ok"

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        p = dict(params or {})
        self.reads.append((action, p))
        if action == "query_character":
            cid = int(p.get("character_id") or RULER)
            return {"id": cid, "alive": True,
                    "name": f"c{cid}", "age": self.ages.get(cid, 30),
                    "is_female": False, "children": FAMILY.get(cid, []),
                    "dynasty_id": 555,
                    "primary_title_id": 77 if cid == RULER else 0}
        if action == "query_realm_laws":
            return {"active_laws": self.laws}
        if action == "query_dynasty":
            self.dynasty_reads.append(p)
            return {"dynasty_id": p.get("dynasty_id"), "head_character_id": self.head}
        r = self.replies.get(action, {})
        return r(p) if callable(r) else r

    async def clock(self):
        """Paused: a days lever with a readback (the grant, L14) reads once and waits."""
        return {"paused": True, "date": "867.1.1"}

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        r = self.replies.get(action, {"success": True, "status": "queued",
                                      "intent_request_id": "si_1"})
        return r(params) if callable(r) else r

    def game(self):
        return {"tick": 1, "date": "867.01.03", "paused": False}


def _run(coro):
    return asyncio.run(coro)


def _handler_text(fname, handler):
    src = open(os.path.join(BRIDGE_SRC, fname), encoding="utf-8", errors="replace").read()
    m = re.search(r"\nbool " + re.escape(handler) + r"\(", src)
    assert m, f"{handler} not found in {fname}"
    nxt = re.search(r"\nbool do_[a-z_0-9]+\(", src[m.end():])
    return src[m.start(): m.end() + (nxt.start() if nxt else len(src))]


def _disinherit(gw, target):
    return _run(redesign.do_impl(gw, intent="interact", character_id=RULER,
                                 args={"interaction": "disinherit_interaction",
                                       "target_id": target}))


# ---------------------------------------------------------------- 1 kin guard
@pytest.mark.parametrize("target", [NEPHEW, STRANGER, RULER])
def test_kin_guard_refuses_a_target_outside_the_actors_line(target):
    gw = FamilyGateway()
    env = _disinherit(gw, target)
    assert env["ok"] is False and env["verdict"] == "rejected"
    assert env["blocked"]["gate"] == "target_not_own_descendant"
    assert sorted(env["blocked"]["measured"]["own_descendants"]) == sorted([SON, 13463, GRANDSON])
    assert gw.writes == [], "the guard must refuse BEFORE anything reaches the bridge"


@pytest.mark.parametrize("target", [SON, GRANDSON])
def test_kin_guard_passes_the_actors_own_line_and_sends_it(target):
    gw = FamilyGateway()
    env = _disinherit(gw, target)
    assert env["verdict"] == "pending", env.get("blocked")
    (action, params), = gw.writes
    assert action == "send_interaction"
    assert params["interaction_type"] == "disinherit_interaction"
    assert params["target_id"] == target and params["actor_id"] == RULER


def test_kin_guard_refuses_a_missing_target():
    gw = FamilyGateway()
    env = _disinherit(gw, 0)
    assert env["blocked"]["gate"] == "missing_required_argument"
    assert gw.writes == []


def test_kin_guard_is_keyed_on_the_engine_name_only():
    # A guard that matched a substring would also catch disinherit_children_interaction.
    assert redesign.KIN_GUARDED_INTERACTIONS == frozenset({"disinherit_interaction"})


# ---------------------------------------------------------------- 2 send options
def test_send_options_secondaries_and_title_reach_the_wire():
    gw = FamilyGateway()
    env = _run(redesign.do_impl(gw, intent="interact", character_id=RULER, args={
        "interaction": "arrange_marriage_interaction", "target_id": RULER,
        "secondary_actor_id": SON, "secondary_recipient_id": NEPHEW,
        "send_options": ["matrilineal", "hook", "matrilineal", " "]}))
    assert env["verdict"] == "pending"
    (action, params), = gw.writes
    assert action == "send_interaction"
    assert params["send_options"] == "matrilineal,hook"
    assert params["secondary_actor_id"] == SON and params["secondary_recipient_id"] == NEPHEW


def test_own_court_marriage_may_name_the_actor_as_recipient_only_with_secondaries():
    gw = FamilyGateway()
    env = _run(redesign.do_impl(gw, intent="interact", character_id=RULER, args={
        "interaction": "arrange_marriage_interaction", "target_id": RULER}))
    assert env["blocked"]["raw_reason"] == "self_target" and gw.writes == []
    env = _run(redesign.do_impl(gw, intent="interact", character_id=RULER, args={
        "interaction": "gift_interaction", "target_id": RULER, "secondary_actor_id": SON}))
    assert env["blocked"]["raw_reason"] == "self_target" and gw.writes == []


def test_send_options_accepts_a_comma_string():
    assert redesign._send_options_list("matrilineal, hook,,") == ["matrilineal", "hook"]
    assert redesign._send_options_list(None) == []


def test_no_send_options_sends_an_empty_list():
    gw = FamilyGateway()
    _run(redesign.do_impl(gw, intent="interact", character_id=RULER, args={
        "interaction": "grant_titles_interaction", "target_id": SON, "title_id": 4242}))
    (action, params), = gw.writes
    assert params["send_options"] == "" and params["title_id"] == 4242


def test_the_bridge_reads_the_keys_python_sends():
    body = _handler_text("action_handlers.cpp", "do_send_interaction")
    for key in ("send_options", "secondary_actor_id", "secondary_recipient_id", "title_id"):
        assert f'"{key}"' in body, key
    assert "intent.send_options = send_options" in body


def test_the_pipeline_applies_the_window_choices_after_set_target():
    src = open(os.path.join(BRIDGE_SRC, "ai_eval_hook.cpp"), encoding="utf-8").read()
    body = src[src.index("static bool execute_tls_pipeline_dispatch("):]
    body = body[:body.index("\nstatic bool is_direct_dispatch_interaction(")]
    i_target = body.index("ctx_set_target(")
    i_choices = body.index("apply_interaction_window_choices(intent, tls_ctx, interaction)")
    i_validate = body.index("ctx_validate(")
    assert i_target < i_choices < i_validate


# ---------------------------------------------------------------- 3 no server refusal
@pytest.mark.parametrize("itype", ["grant_titles_interaction", "revoke_title_interaction",
                                   "designate_heir_interaction"])
def test_succession_interactions_are_sent_not_refused(itype):
    gw = FamilyGateway()
    env = _run(redesign.do_impl(gw, intent="interact", character_id=RULER,
                                args={"interaction": itype, "target_id": SON, "title_id": 9}))
    assert env["verdict"] == "pending", env.get("blocked")
    assert [a for a, _ in gw.writes] == ["send_interaction"]


def test_the_retired_self_disadvantage_gate_is_gone():
    assert "ai_refuses_self_disadvantaging" not in redesign.GATE_TABLE
    assert not hasattr(redesign, "SELF_DISADVANTAGE_INTERACTIONS")


def test_the_succession_levers_take_the_direct_path():
    src = open(os.path.join(BRIDGE_SRC, "ai_eval_hook.cpp"), encoding="utf-8").read()
    lst = src[src.index("static const char* direct_list[] = {"):]
    lst = lst[:lst.index("nullptr")]
    for name in ("grant_titles_interaction", "designate_heir_interaction",
                 "disinherit_interaction", "revoke_title_interaction",
                 "arrange_marriage_interaction"):
        assert f'"{name}"' in lst, name


# ---------------------------------------------------------------- 4 title law
def test_title_law_engine_refusal_carries_the_engine_text():
    gw = FamilyGateway(replies={"set_title_law": {
        "success": False, "action": "set_title_law", "reason": "engine_refused",
        "engine_reason": "Requires Crown Authority"}})
    env = _run(redesign.do_impl(gw, intent="change_law", character_id=RULER, args={
        "law": "feudal_elective_succession_law", "scope": "title", "title_id": 77}))
    (action, params), = gw.writes
    assert action == "set_title_law"
    assert params == {"actor_id": RULER, "title_id": 77, "law_name": "feudal_elective_succession_law"}
    assert env["verdict"] == "rejected"
    assert env["blocked"]["gate"] == "engine_refused"
    assert "Requires Crown Authority" in env["blocked"]["summary"]


def test_title_law_queued_is_pending_not_applied():
    gw = FamilyGateway(replies={"set_title_law": {
        "success": True, "action": "set_title_law", "status": "queued"}})
    env = _run(redesign.do_impl(gw, intent="change_law", character_id=RULER, args={
        "law": "feudal_elective_succession_law", "scope": "title", "title_id": 77}))
    assert env["verdict"] == "pending"


def test_title_law_names_its_intent_and_reads_back_as_applied():
    """L13 FF2 (witness m-15) and the FF3 ruling (X1): set_title_law's reply names
    its verdict's intent, so the do() tail reads the drain's verdict back, and a
    title law is a stage-1 lever (LEVERS: the command runs on the next pass) whose
    readback is the title's own laws: the drain's success with the law listed is
    applied; the law not listed after the pass is write_unconfirmed; a drain
    refusal is rejected with the engine's reason."""
    law = "feudal_elective_succession_law"
    for status, listed, verdict in (("success", [law], "applied"),
                                    ("success", [], "rejected"),
                                    ("rejected", [], "rejected")):
        gw = FamilyGateway(replies={
            "set_title_law": {"success": True, "action": "set_title_law", "status": "queued",
                              "intent_request_id": "stlaw_1_2_0", "dispatch_id": "stlaw_1_2_0"},
            "query_dispatch_result": {"status": status, "dispatch_id": "stlaw_1_2_0",
                                      "rejection_reason": "engine_refused"},
            "query_title_succession": {"title_id": 77, "title_laws": listed}})
        env = _run(redesign.do_impl(gw, intent="change_law", character_id=RULER, args={
            "law": law, "scope": "title", "title_id": 77}))
        assert env["verdict"] == verdict, (status, listed, env)
        assert ("query_dispatch_result", {"dispatch_id": "stlaw_1_2_0"}) in gw.reads
        if verdict == "applied":
            assert env["result"]["readback"]["title_laws"] == [law]
        elif status == "success":
            assert env["blocked"]["gate"] == "write_unconfirmed", env["blocked"]
        else:
            assert env["blocked"]["gate"] == "engine_refused", env["blocked"]


def test_set_title_law_stores_its_verdict_under_the_id_it_replies_with():
    """The bridge half of m-15: the handler stores the engine's verdict in the
    completed ring (store_completed) under request_id, and replies with that
    same id in both keys, on every branch that reached the engine."""
    body = _handler_text("action_handlers.cpp", "do_set_title_law")
    assert re.search(r"rec\.request_id\s*=\s*request_id\s*;", body)
    assert re.search(r"g_intent_queue\.store_completed\s*\(\s*rec\s*\)", body)
    for key in ("intent_request_id", "dispatch_id"):
        assert re.search(r'r\.key\("' + key + r'"\);\s*r\.value_string\(request_id\.c_str\(\)\)', body), key
    store = body.index("store_completed")
    reply = body.index('r.key("intent_request_id")')
    assert store < reply, "the verdict must be in the ring before the reply names it"


def test_set_title_law_uses_the_window_command_not_the_console():
    body = _handler_text("action_handlers.cpp", "do_set_title_law")
    assert "ADD_TITLE_LAW_COMMAND_CTOR" in body and "VT_IS_VALID" in body
    assert "CMD_ADD_TITLE_LAW" not in body
    assert '"engine_reason"' in body and '"queued"' in body


# ---------------------------------------------------------------- 5 succession_plan
def _plan(gw):
    # The bridge's shape (do_query_succession_simulation): `heirs`, the player heir
    # first, each with character_id and the titles it would take.
    gw.replies.setdefault("query_succession_simulation", {
        "heirs": [{"character_id": SON, "is_player_heir": True, "title_count": 1,
                   "titles": [{"title_id": 998}]},
                  {"character_id": 13463, "is_player_heir": False, "title_count": 1,
                   "titles": [{"title_id": 808}]}]})
    return _run(redesign.succession_plan_impl(gw, character_id=RULER))


def test_succession_plan_reports_heir_ages_and_laws():
    gw = FamilyGateway(ages={SON: 24, 13463: 9}, laws=[
        {"name": "confederate_partition_succession_law", "group_name": "succession_order_laws"},
        {"name": "male_preference_law", "group_name": "succession_gender_laws"},
        {"name": "crown_authority_1", "group_name": "crown_authority"}])
    lev = _plan(gw)["result"]["levers"]
    assert [(h["id"], h["age"], h["adult"], h["own_child"]) for h in lev["heirs"]] == [
        (SON, 24, True, True), (13463, 9, False, True)]
    assert lev["succession_laws"] == ["confederate_partition_succession_law", "male_preference_law"]
    assert lev["partition"] == {"eligible_heirs": 2, "adult_heirs": 1}
    assert lev["designate_heir"]["available"] is False and lev["designate_heir"]["why_not"]
    assert lev["disinherit"]["candidates"] == [SON, 13463]
    assert lev["title_law"]["primary_title_id"] == 77


def test_designate_heir_exists_only_under_a_law_that_allows_it():
    gw = FamilyGateway(laws=[{"name": "crown_authority_3", "group_name": "crown_authority"}])
    lev = _plan(gw)["result"]["levers"]
    assert lev["designate_heir"]["available"] is True
    assert lev["designate_heir"]["granted_by"] == ["crown_authority_3"]


def test_succession_plan_no_longer_teaches_the_retired_refusal():
    gw = FamilyGateway()
    res = _plan(gw)["result"]
    assert "fix" not in res
    assert "ai_refuses_self_disadvantaging" not in repr(res)


def test_disinherit_lever_names_the_dynasty_head_read_by_dynasty_id():
    gw = FamilyGateway(head=RULER)
    lev = _plan(gw)["result"]["levers"]["disinherit"]
    assert lev["available"] is True and lev["dynasty_head_id"] == RULER
    assert gw.dynasty_reads and all(r.get("dynasty_id") == 555 for r in gw.dynasty_reads)
    gw = FamilyGateway(head=99)
    lev = _plan(gw)["result"]["levers"]["disinherit"]
    assert lev["available"] is False and "dynasty's head" in lev["why_not"]


def test_succession_plan_falls_back_to_the_per_title_map_when_the_simulation_is_empty():
    # Measured on the 867 save (live867 l4-live-a-t1, ruler 10465): an empty simulation
    # read passes_intact=true while the per-title map split the realm between two sons.
    gw = FamilyGateway(ages={SON: 24, 13463: 19}, replies={
        "query_succession_simulation": {"heirs": []},
        "query_title_succession_map": {
            "primary_title_id": 998, "primary_heir_id": SON, "any_splits": True,
            "titles": [
                {"title_id": 998, "tier": 4, "projected_heir_id": SON, "title_division": "partition"},
                {"title_id": 1141, "tier": 2, "projected_heir_id": SON, "title_division": "partition"},
                {"title_id": 808, "tier": 4, "projected_heir_id": 13463, "title_division": "partition"},
            ]}})
    res = _run(redesign.succession_plan_impl(gw, character_id=RULER))["result"]
    assert res["passes_intact"] is False
    assert res["heirs_source"] == "per_title_map"
    assert res["eligible_heir_count"] == 2
    assert res["primary_heir"]["id"] == SON
    assert res["breaker"]["id"] == 13463
    assert [h["id"] for h in res["levers"]["heirs"]] == [SON, 13463]


def test_succession_plan_keeps_the_simulation_when_it_names_heirs():
    gw = FamilyGateway()
    res = _plan(gw)["result"]
    assert res["heirs_source"] == "simulation" and res["eligible_heir_count"] == 2


def test_engine_text_reads_the_tooltip_as_words():
    raw = ("\x16warning_icon! \x15X Missing \x15TOOLTIP:GAME_CONCEPT,prestige_i "
           "\x16prestige_icon!\x15! 948\x15!")
    assert redesign.engine_text(raw) == "Missing prestige 948"
    assert redesign.engine_text("plain words") == "plain words"
    # Measured on the 867 save (live867 l4fix-live-2): a grant refused by CTX_VALIDATE,
    # and a title law refused for two reasons on two lines.
    grant = ("\x15indent_newline:0 \x15!\x15indent_newline:6 \x16warning_icon! \x15X "
             "\x15ONCLICK:CHARACTER,33217 \x15TOOLTIP:CHARACTER,33217 \x15L \x15high "
             "Sigeræd\x15!\x15!\x15!\x15! is a \x15TOOLTIP:GAME_FAITH_DOCTRINE,"
             "doctrine_theocracy_temporal \x15L; Theocratic\x15!\x15! Bishop\x15!\x15!")
    assert redesign.engine_text(grant) == "Sigeræd is a Theocratic Bishop"
    law = ("\x15indent_newline:0 \x15!\x15indent_newline:6 \x16warning_icon! \x15X "
           "\x15ONCLICK:CHARACTER,10465 \x15TOOLTIP:CHARACTER,10465 \x15L \x15high King\x15! "
           "\x15high Ludwig II\x15!\x15!\x15!\x15! is at \x15E; \x15TOOLTIP:GAME_CONCEPT,war "
           "War\x15!\x15!\x15!\x15!\n\x16warning_icon! \x15X Missing \x15TOOLTIP:GAME_CONCEPT,"
           "prestige_i \x16prestige_icon!\x15! 954\x15!")
    assert redesign.engine_text(law) == "King Ludwig II is at War; Missing prestige 954"
    gw = FamilyGateway(replies={"set_title_law": {
        "success": False, "reason": "engine_refused", "engine_reason": raw}})
    env = _run(redesign.do_impl(gw, intent="change_law", character_id=RULER, args={
        "law": "feudal_elective_succession_law", "scope": "title", "title_id": 77}))
    assert env["blocked"]["summary"] == "engine_refused: Missing prestige 948"


class _DesignateGateway(FamilyGateway):
    """query_character adds prestige, house and traits, as the bridge's reply carries them."""

    def __init__(self, prestige, houses=None, traits=None, designated=None, **kw):
        super().__init__(**kw)
        self.prestige, self.houses = prestige, dict(houses or {})
        self.traits, self.designated = dict(traits or {}), designated

    async def read(self, action, params=None):
        r = await super().read(action, params)
        if action == "query_character":
            c = r
            c["house_id"] = self.houses.get(c["id"], 7)
            c["traits"] = [{"name": t} for t in self.traits.get(c["id"], [])]
            if c["id"] == RULER:
                c["prestige"] = self.prestige
                c["designated_heir_id"] = self.designated
        return r



def test_designate_heir_is_not_available_below_its_prestige_cost():
    # Witness run w-l4-succession-1: the lever said available=true and the engine
    # refused every child. The prestige the interaction costs is checked from the read.
    gw = _DesignateGateway(prestige=601.11, laws=[{"name": "crown_authority_3",
                                                   "group_name": "crown_authority"}])
    lev = _plan(gw)["result"]["levers"]["designate_heir"]
    assert lev["available"] is False
    assert lev["prestige_cost"] == redesign.DESIGNATE_HEIR_PRESTIGE_COST
    assert "prestige" in lev["why_not"] and "601" in lev["why_not"]


def test_designate_heir_names_only_children_the_engine_takes():
    gw = _DesignateGateway(prestige=5000.0, houses={13463: 9}, traits={SON: ["bastard"]},
                           laws=[{"name": "crown_authority_3", "group_name": "crown_authority"}])
    lev = _plan(gw)["result"]["levers"]["designate_heir"]
    assert lev["candidates"] == [] and lev["available"] is False
    assert {b["id"]: b["why"] for b in lev["barred"]} == {SON: ["bastard"],
                                                          13463: ["another house"]}
    gw = _DesignateGateway(prestige=5000.0, designated=SON,
                           laws=[{"name": "crown_authority_3", "group_name": "crown_authority"}])
    lev = _plan(gw)["result"]["levers"]["designate_heir"]
    assert lev["candidates"] == [13463] and lev["available"] is True and lev["why_not"] is None


def test_designate_heir_in_an_administrative_realm_does_not_claim_the_unread_influence():
    # Live867 l4fix-live-3: 10357 (japanese_bureaucracy_1, japanese_regency_succession_law)
    # was refused "does not have influence 150"; no read carries influence.
    gw = _DesignateGateway(prestige=2919.0, laws=[
        {"name": "japanese_bureaucracy_1", "group_name": "japanese_bureaucracy"},
        {"name": "japanese_regency_succession_law", "group_name": "succession_order_laws"}])
    lev = _plan(gw)["result"]["levers"]["designate_heir"]
    assert lev["available"] is None and "150 influence" in lev["unread"]
    assert lev["prestige_cost"] == 500 and lev["influence_cost"] == 150
    gw = _DesignateGateway(prestige=400.0, laws=[
        {"name": "imperial_bureaucracy_1", "group_name": "imperial_bureaucracy"},
        {"name": "acclamation_succession_law", "group_name": "succession_order_laws"}])
    lev = _plan(gw)["result"]["levers"]["designate_heir"]
    assert lev["available"] is False and "500 prestige" in lev["why_not"]


def test_revoke_names_the_law_that_allows_it():
    gw = FamilyGateway(laws=[{"name": "crown_authority_0", "group_name": "crown_authority"}])
    lev = _plan(gw)["result"]["levers"]["grant_title"]
    assert lev["revoke_allowed_by"] == [] and "crown_authority_1" in lev["revoke_why_not"]
    gw = FamilyGateway(laws=[{"name": "crown_authority_2", "group_name": "crown_authority"}])
    lev = _plan(gw)["result"]["levers"]["grant_title"]
    assert lev["revoke_allowed_by"] == ["crown_authority_2"] and lev["revoke_why_not"] is None


def test_a_refusal_detail_reaches_the_player_as_the_engines_words():
    raw = "\x16warning_icon! \x15X Missing \x15TOOLTIP:GAME_CONCEPT,prestige_i \x15! 948\x15!"
    gw = FamilyGateway(replies={"query_intents": {"pending": []}, "query_intent_results": {
        "results": [{"intent_id": "si_1", "status": "rejected",
                     "rejection_reason": "interaction_not_valid_for_target", "detail": raw}]}})
    res = _run(redesign.inspect_impl(gw, subject="pending"))["result"]
    assert res["completed"][0]["detail"] == "Missing prestige 948"
    assert res["raw"]["query_intent_results"]["results"][0]["detail"] == raw


def test_a_renamed_field_keeps_its_windows_probe_agreement(tmp_path):
    # character::IS_AI_FLAG became IS_FEMALE (re_interactions.md §4); the probe JSONs
    # key on the old name. The generator matches the probed offset, so 0x199 stays
    # `agreed` (40138/40138 pairs) instead of falling back to the unverified linux rule.
    import json as _json
    import subprocess
    root = os.path.join(os.path.dirname(__file__), "..", "..")
    gen = os.path.join(root, "re/scripts/windows/gen_win_offsets.py")
    if not os.path.isfile(gen):
        pytest.skip("the public export carries no re/ tree (the generator runs at export time)")
    r = subprocess.run([sys.executable, "-I", gen,
                        "--linux", os.path.join(root, "re/offsets/ck3_offsets.h"),
                        "--windows", os.path.join(root, "re/offsets/windows/ck3_offsets_win.h"),
                        "--probe", os.path.join(root, "re/findings/windows/p3_probe_867_pair.json"),
                        "--probe", os.path.join(root, "re/findings/windows/p3_probe_867_07_09_pair.json"),
                        "--recipes", os.path.join(root, "bridge/src/os/windows/recipes.inc"),
                        "--out", str(tmp_path / "h"), "--report", str(tmp_path / "r.json")],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    rows = {(x["ns"], x["name"]): x for x in _json.loads((tmp_path / "r.json").read_text())["rows"]}
    assert rows[("character", "IS_FEMALE")]["rule"] == "agreed"
    assert "IS_AI_FLAG" in rows[("character", "IS_FEMALE")]["source"]


def test_an_engine_refusal_carries_the_engines_text_from_the_bridge():
    # The bridge half: a CTX_VALIDATE refusal and a send-cascade refusal both fill the
    # completed record's detail (console_output) with the engine's tooltip text.
    src = open(os.path.join(BRIDGE_SRC, "ai_eval_hook.cpp"), encoding="utf-8").read()
    i = src.index('"interaction_not_valid_for_target");\n            intent->console_output')
    assert "INTERACTION_CTX_VALIDATE_WITH_TEXT" in src[i - 900:i]
    j = src.index('why = "send_conditions_not_met";')
    assert "intent->console_output = cascade_text" in src[j:j + 200]
    assert "reinterpret_cast<intptr_t>(&cascade_text)" in src[j - 300:j]
