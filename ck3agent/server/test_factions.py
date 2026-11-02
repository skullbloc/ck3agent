"""Faction management from both seats (F16): the rows a player reads and the
writes he sends, against a recording gateway. The replies are shaped like the
bridge's (factions.cpp) as seen live in runs l10-scout1 and l10-dev2."""
from __future__ import annotations

import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

import factions  # noqa: E402
import redesign as R  # noqa: E402


def _cpp_code(text: str) -> str:
    """Comments blanked, literals kept: the one C++ tokenizer (L13 FF3,
    bridge/scripts/cpp_strip.py). A `//` inside a string is not a comment."""
    import sys as _sys
    from pathlib import Path as _Path
    _sys.path.insert(0, str(_Path(__file__).resolve().parents[2] / "bridge" / "scripts"))
    import cpp_strip
    return cpp_strip.strip(text, keep_strings=True)


LIEGE, A, B = 12197, 14842, 14900

OPTIONS = {
    "action": "query_joinable_factions", "actor_id": A, "liege_id": LIEGE, "liege_name": "Eraq",
    "faction_types": [
        {"faction_type": "independence_faction", "can_create": True, "war_if_refused": "independence_faction_war"},
        {"faction_type": "liberty_faction", "can_create": False,
         "reason": "Khwarezmshah Eraq already has the lowest possible Crown Authority"},
    ],
    "factions": [
        {"faction_id": 69, "faction_type": "independence_faction", "leader_id": A, "leader_name": "X",
         "member_count": 1, "is_member": True, "is_leader": True,
         "leave": {"can_leave": True},
         "demand": {"discontent": 40.0, "discontent_threshold": 100.0, "war_if_refused": "independence_faction_war",
                    "can_press_now": False, "reason": "Discontent is below 100"}},
        {"faction_id": 70, "faction_type": "liberty_faction", "leader_id": B, "member_count": 2,
         "is_member": False, "join": {"can_join": False, "reason": "You are at war"}},
    ],
}


class Gw:
    health = "ok"

    def __init__(self, reads=None, write_reply=None):
        self.reads_map = reads or {}
        self.write_reply = write_reply or {}
        self.writes = []
        self.reads = []

    async def actor_gate(self):
        return None

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character":
            return {"id": A, "alive": True, "liege_id": LIEGE}
        r = self.reads_map.get(action, {})
        return r(params) if callable(r) else r

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return self.write_reply

    def game(self):
        return {"tick": 1, "date": "0868.03.01"}


def run(c):
    return asyncio.run(c)


def test_enumerate_carries_the_games_verdict_and_reason():
    env = run(R.do_impl(Gw({"query_joinable_factions": OPTIONS}), action_class="factions_to_join"))
    res = env["result"]
    assert res["liege_id"] == LIEGE
    avail = {r["label"]: r for r in res["available"]}
    unav = {r["label"]: r for r in res["unavailable"]}
    assert "create independence_faction against Eraq" in avail
    lib = unav["create liberty_faction against Eraq"]
    assert lib["blocked"]["gate"] == "faction_refused"
    assert "Crown Authority" in lib["blocked"]["summary"]
    press = next(r for r in res["unavailable"] if r["do_call"]["args"]["action"] == "press_demand")
    assert press["blocked"]["summary"] == "Discontent is below 100"
    assert press["demand"]["war_if_refused"] == "independence_faction_war"
    join = next(r for r in res["unavailable"] if r["do_call"]["args"]["action"] == "join")
    assert join["blocked"]["summary"] == "You are at war"
    assert any(r["do_call"]["args"] == {"action": "leave", "faction_id": 69} for r in res["available"])


def test_an_independent_ruler_is_told_why_there_is_nothing():
    env = run(R.do_impl(Gw({"query_joinable_factions": {"independent": True, "note": "no liege"}}),
                        action_class="factions_to_join"))
    assert env["result"]["available"] == [] and env["warnings"][0]["code"] == "no_liege"


def _members(*ids):
    return {"query_factions": {"factions": [{"faction_id": 69, "members": list(ids)}]}}


def test_join_is_applied_only_when_the_member_list_shows_it():
    reply = {"success": True, "result": {"faction_id": 69, "target_id": LIEGE, "is_member": True}}
    ok = run(R.do_impl(Gw(_members(A, B), reply), intent="faction", character_id=B,
                       args={"action": "join", "faction_id": 69}))
    assert ok["verdict"] == "applied" and ok["result"]["readback"]["member"] is True
    bad = run(R.do_impl(Gw(_members(A), reply), intent="faction", character_id=B,
                        args={"action": "join", "faction_id": 69}))
    assert bad["verdict"] == "rejected" and bad["blocked"]["gate"] == "faction_write_not_applied"


def test_the_write_sends_the_actor_and_the_faction():
    gw = Gw(_members(A), {"success": True, "result": {"faction_id": 69, "target_id": LIEGE}})
    run(R.do_impl(gw, intent="faction", character_id=B, args={"action": "leave", "faction_id": 69}))
    assert gw.writes == [("faction_action", {"actor_id": B, "faction_action": "leave", "faction_id": 69})]


def test_leave_that_disbands_reads_back_as_left():
    gw = Gw({"query_factions": {"factions": []}},
            {"success": True, "result": {"faction_id": 69, "target_id": LIEGE, "faction_disbanded": True}})
    env = run(R.do_impl(gw, intent="faction", character_id=A, args={"action": "leave", "faction_id": 69}))
    assert env["verdict"] == "applied" and env["result"]["readback"]["faction_disbanded"] is True


def _leave(reads, result):
    gw = Gw(reads, {"success": True, "result": {"faction_id": 69, **result}})
    return gw, run(R.do_impl(gw, intent="faction", character_id=A, args={"action": "leave", "faction_id": 69}))


def _unread(env):
    """A write that ran whose second read could not be made (FF5 ruling)."""
    assert env["ok"] is True and env["verdict"] == "pending", env
    assert env["pending"]["drains_when"] == "readback", env["pending"]
    assert env["pending"]["raw_reason"] == "write_unconfirmed"
    assert env["result"]["sent"] is True


def test_a_leave_the_second_read_cannot_see_is_never_applied():
    """w-l10-factions-1 review: the leave read the actor's own liege when the reply
    named no target, and a missing faction or a failed read passed as left."""
    # no target named: the actor's liege is not read in its place
    # FF5 ruling: the bridge reported the leave done, so a second read that
    # could not be made is pending readback (ok:true, sent), never rejected.
    gw, env = _leave({"query_factions": {"factions": []}}, {"faction_disbanded": True})
    _unread(env)
    assert not any(a == "query_factions" for a, _ in gw.reads)
    # the read failed
    _, env = _leave({"query_factions": {"error": "character_not_found"}},
                    {"target_id": LIEGE, "faction_disbanded": True})
    _unread(env)
    # gone from the list, and the engine did not say it disbanded the faction
    _, env = _leave({"query_factions": {"factions": []}}, {"target_id": LIEGE, "faction_disbanded": False})
    _unread(env)
    # still listed as a member
    _, env = _leave(_members(A, B), {"target_id": LIEGE})
    assert env["blocked"]["gate"] == "faction_write_not_applied"
    # listed without him: left
    _, env = _leave(_members(B), {"target_id": LIEGE})
    assert env["verdict"] == "applied" and env["result"]["readback"]["member"] is False


def _press(options_reply):
    gw = Gw({"query_joinable_factions": options_reply},
            {"success": True, "result": {"faction_id": 69, "target_id": LIEGE, "demand_sent": True,
                                         "state_after": {"demand_cooldown_until": "0869.09.21"}}})
    return gw, run(R.do_impl(gw, intent="faction", character_id=A,
                             args={"action": "press_demand", "faction_id": 69}))


def _with_demand(**demand):
    f = dict(OPTIONS["factions"][0], demand=dict(OPTIONS["factions"][0]["demand"], **demand))
    return dict(OPTIONS, factions=[f])


def test_a_pressed_demand_is_read_back_from_the_leaders_own_read():
    """w-l10-factions-1 review: press_demand's applied rested on the bridge's own
    demand_sent, and its readback copied the same reply's state_after."""
    gw, env = _press(_with_demand(can_press_now=False, demand_cooldown_until="0869.09.21",
                                  reason="You have to wait until 21 September, 869"))
    assert env["verdict"] == "applied"
    rb = env["result"]["readback"]
    assert rb["via"] == "query_joinable_factions" and rb["demand"]["can_press_now"] is False
    assert rb["demand"]["demand_cooldown_until"] == "0869.09.21"
    assert ("query_joinable_factions", {"character_id": A}) in gw.reads
    assert "demands_answered" in env["result"]["next"]
    # the game still lets him press: the bridge's demand_sent does not carry it
    _, env = _press(_with_demand(can_press_now=True))
    assert env["blocked"]["gate"] == "faction_write_not_applied"
    # the read failed, or he is no longer in the faction: pending readback
    _, env = _press({"error": "timeout_waiting_for_game_thread"})
    _unread(env)
    _, env = _press(dict(OPTIONS, factions=[]))
    _unread(env)


def test_the_games_refusal_reaches_the_seat_as_its_sentence():
    gw = Gw(write_reply={"success": False, "reason": "engine_refused_create",
                         "result": {"engine_reason": "You are not a powerful vassal"}})
    env = run(R.do_impl(gw, intent="faction", character_id=A,
                        args={"action": "create", "faction_type": "liberty_faction"}))
    assert env["blocked"]["gate"] == "faction_refused"
    assert env["blocked"]["summary"] == "You are not a powerful vassal"


def test_arguments_are_checked_before_the_bridge():
    for args, field in (({}, "action"), ({"action": "create"}, "faction_type"),
                        ({"action": "join"}, "faction_id"), ({"action": "respond_demand"}, "response")):
        gw = Gw()
        env = run(R.do_impl(gw, intent="faction", character_id=A, args=args))
        assert env["blocked"]["gate"] == "missing_required_argument", args
        assert gw.writes == [], args
    env = run(R.do_impl(Gw(), intent="faction", character_id=A, args={"action": "rebel"}))
    assert env["blocked"]["gate"] == "invalid_argument"


def test_respond_demand_is_read_back_from_the_bridge():
    gw = Gw({"query_factions": {"demand_response": "refuse"}},
            {"success": True, "character_id": LIEGE, "response": "refuse"})
    env = run(R.do_impl(gw, intent="faction", character_id=LIEGE,
                        args={"action": "respond_demand", "response": "refuse"}))
    assert env["verdict"] == "applied"
    assert gw.writes == [("set_faction_demand_response", {"character_id": LIEGE, "response": "refuse"})]
    gw2 = Gw({"query_factions": {"demand_response": "ai"}}, {"success": True})
    env2 = run(R.do_impl(gw2, intent="faction", character_id=LIEGE,
                         args={"action": "respond_demand", "response": "accept"}))
    assert env2["blocked"]["gate"] == "faction_write_not_applied"


def test_the_liege_reads_members_threat_and_levers():
    fac = {"character_id": LIEGE, "faction_count": 1, "demand_response": "accept",
           "demands_answered": [{"event": "faction_demand.0001", "answered": "FACTION_DEMAND_ACCEPT"}],
           "factions": [{"faction_id": 69, "type": "independence_faction", "leader_character_id": A,
                         "leader_name": "X", "member_rows": [{"id": A, "name": "X", "military_strength": 900,
                                                              "is_leader": True}]}]}
    thr = {"discontent_threshold_pct": 100.0,
           "factions": [{"faction_id": 69, "discontent": 62.0, "power_pct_of_liege": 58.5,
                         "threat_level": "medium"}]}
    council = {"council": [{"position_id": 1, "position_key": "councillor_chancellor", "holder_id": 5},
                           {"position_id": 2, "vacant": True, "holder_id": 16777215}]}
    gw = Gw({"query_factions": fac, "query_faction_threat": thr, "query_council": council,
             "query_imprisonment_reasons": {"has_imprisonment_reason": False},
             "query_interaction_check": _check(_NO_GROUNDS(A))})
    env = run(R.inspect_impl(gw, subject="factions", character_id=LIEGE))
    f = env["result"]["factions"][0]
    assert f["members"][0]["name"] == "X" and f["discontent"] == 62.0 and f["power_pct_of_liege"] == 58.5
    assert env["result"]["demand_response"] == "accept"
    lev = f["levers"]
    assert lev["gift"]["do_call"] == {"intent": "interact",
                                      "args": {"interaction": "gift_interaction", "target_id": A}}
    # The council is a read: the seats the leader holds, never an appoint call
    # (the game's seat swap cannot seat a leader who has no seat).
    assert lev["on_council"] == [] and "council_seat" not in lev
    assert lev["imprison"]["blocked"]["gate"] == "interaction_invalid"
    assert lev["imprison"]["blocked"]["summary"].startswith("the game will not let you send it")
    assert lev["imprison"]["blocked"]["summary"].endswith(": You have no reason to imprison them")


def test_the_liege_reads_the_ultimatum_as_a_chance_never_a_due_date():
    """The engine keeps no countdown (re_factions.md section 7): a faction's demand is
    the AI leader's chance to press, or none, as the bridge classified it
    (classify_faction_demand); a faction with no threat row is no reading."""
    fac = {"character_id": LIEGE, "factions": [{"faction_id": i, "type": "liberty_faction"} for i in (1, 2, 3)]}
    thr = {"factions": [{"faction_id": 1, "ultimatum": "ai_may_press", "ai_press_chance_pct": 12.5},
                        {"faction_id": 2, "ultimatum": "none", "demand_cooldown_until": "0869.09.15"}]}
    env = run(R.inspect_impl(Gw({"query_factions": fac, "query_faction_threat": thr, "query_council": {}}),
                             subject="factions", character_id=LIEGE))
    rows = {f["faction_id"]: f for f in env["result"]["factions"]}
    assert rows[1]["ultimatum"] == "ai_may_press" and rows[1]["ai_press_chance_pct"] == 12.5
    assert rows[2]["ultimatum"] == "none" and "ai_press_chance_pct" not in rows[2]
    assert rows[2]["demand_cooldown_until"] == "0869.09.15"
    assert "ultimatum" not in rows[3]
    for f in rows.values():
        assert not {"days_to_ultimatum", "ultimatum_due", "ultimatum_countdown_running"} & set(f)


def test_python_relays_the_bridges_demand_state_and_has_no_rule_of_its_own():
    """MAJOR 4 (witness w-l10-factions-3): ai_may_press was defined three times. The
    bridge's classify_faction_demand is the one rule; Python copies its fields.

    Proof by disagreement: rows whose older fields (ultimatum_active,
    can_fire_ultimatum, ultimatum_progress_pct, cooldown_active) contradict the
    classification read exactly as the classification says. A second rule in
    Python would follow the older fields somewhere."""
    fac = {"character_id": LIEGE, "factions": [{"faction_id": i, "type": "liberty_faction"} for i in (1, 2)]}
    thr = {"factions": [
        {"faction_id": 1, "ultimatum": "none", "ultimatum_active": True, "can_fire_ultimatum": True,
         "ultimatum_progress_pct": 40.0, "cooldown_active": False},
        {"faction_id": 2, "ultimatum": "ai_may_press", "ai_press_chance_pct": 7.0,
         "ultimatum_active": False, "can_fire_ultimatum": False, "ultimatum_progress_pct": 0.0,
         "cooldown_active": True}]}
    env = run(R.inspect_impl(Gw({"query_factions": fac, "query_faction_threat": thr, "query_council": {}}),
                             subject="factions", character_id=LIEGE))
    rows = {f["faction_id"]: f for f in env["result"]["factions"]}
    assert rows[1]["ultimatum"] == "none" and "ai_press_chance_pct" not in rows[1]
    assert rows[2]["ultimatum"] == "ai_may_press" and rows[2]["ai_press_chance_pct"] == 7.0
    # and the source names none of the older fields: nothing to re-derive from
    import ast
    with open(os.path.join(os.path.dirname(__file__), "factions.py")) as fh:
        consts = {n.value for n in ast.walk(ast.parse(fh.read()))
                  if isinstance(n, ast.Constant) and isinstance(n.value, str)}
    assert not {"ultimatum_active", "can_fire_ultimatum", "ultimatum_progress_pct",
                "cooldown_active", "ai_may_press"} & consts
    assert factions.DEMAND_STATE_KEYS == ("ultimatum", "ai_press_chance_pct", "demand_cooldown_until")


def _code_only(text: str) -> str:
    """C++ source with // comments removed (a comment naming a field is not a read)."""
    import re
    return _cpp_code(text)


def test_the_bridge_classifies_the_demand_in_one_function():
    """The bridge side of the one rule: classify_faction_demand is defined once
    (factions.cpp), every faction read calls it, and no code outside it reads the
    tick's demand fields (the armed date and the chance)."""
    import re
    from pathlib import Path
    src = Path(__file__).resolve().parents[2] / "bridge" / "src"
    fac = _code_only((src / "factions.cpp").read_text())
    qh = _code_only((src / "query_handlers.cpp").read_text())
    assert len(re.findall(r"^FactionDemandState classify_faction_demand\(", fac, re.M)) == 1
    assert qh.count("classify_faction_demand(") == 2          # query_factions, query_faction_threat
    assert "classify_faction_demand(f)" in fac                # emit_demand_state
    for field in ("faction_v2::ULTIMATUM_PROGRESS", "faction_v2::ULTIMATUM_FIRE_DATE"):
        assert field not in qh, field
        start = fac.index("FactionDemandState classify_faction_demand(")
        body = fac[start:fac.index("\n}\n", start)]
        assert body.count(field) == 1 and fac.count(field) == 1, field


def test_a_bridge_precheck_is_said_in_plain_words_not_as_the_games_rule():
    """MAJOR 2 (witness w-l10-factions-3): not_the_faction_leader, not_a_member and
    actor_has_no_liege are the bridge's own checks, made before the game is asked.
    They were faction_refused, whose description says the summary is the game's
    reason, with the raw code as the summary."""
    cases = (("press_demand", {"faction_id": 69}, "not_the_faction_leader",
              {"leader_id": B, "leader_name": "Y"}, f"the leader is Y ({B})"),
             ("leave", {"faction_id": 69}, "not_a_member", {}, "not a member"),
             ("create", {"faction_type": "liberty_faction"}, "actor_has_no_liege", {}, "no liege"))
    for action, args, code, extra, said in cases:
        gw = Gw(write_reply={"success": False, "reason": code,
                             "result": {"faction_action": action, "actor_id": A, **extra}})
        env = run(R.do_impl(gw, intent="faction", character_id=A, args={"action": action, **args}))
        b = env["blocked"]
        assert b["gate"] == "faction_precondition", (code, b)
        assert b["raw_reason"] == code and said in b["summary"] and code not in b["summary"], b
    assert R.GATE_WHY["faction_precondition"].startswith("the bridge's own check")
    # the vassal's option row for a faction he is in and does not lead
    opts = dict(OPTIONS, factions=[dict(OPTIONS["factions"][0], is_leader=False, leader_id=B,
                                        leader_name="Y")])
    env = run(R.do_impl(Gw({"query_joinable_factions": opts}), action_class="factions_to_join",
                        character_id=A))
    press = [r for r in env["result"]["unavailable"] if r["do_call"]["args"]["action"] == "press_demand"][0]
    assert press["blocked"]["gate"] == "faction_precondition"
    assert press["blocked"]["summary"].startswith(f"you are not this faction's leader; the leader is Y ({B})")


def test_the_games_refusal_is_faction_refused_with_the_games_text_only():
    """faction_refused is kept for the game's refusals (engine_refused_*); its
    summary is the game's sentence, or says the game gave none, never the code."""
    for code in factions.ENGINE_REFUSALS:
        gw = Gw(write_reply={"success": False, "reason": code,
                             "result": {"engine_reason": "The faction discontent must be over 100%"}})
        env = run(R.do_impl(gw, intent="faction", character_id=A,
                            args={"action": "press_demand", "faction_id": 69}))
        assert env["blocked"]["gate"] == "faction_refused"
        assert env["blocked"]["summary"] == "The faction discontent must be over 100%"
        gw = Gw(write_reply={"success": False, "reason": code, "result": {}})
        env = run(R.do_impl(gw, intent="faction", character_id=A,
                            args={"action": "press_demand", "faction_id": 69}))
        assert env["blocked"]["summary"] == R.NO_GAME_DETAIL
    # and no other code reaches faction_refused
    for code, gate in R.RAW_REASON_TO_GATE.items():
        if gate == "faction_refused":
            assert code in factions.ENGINE_REFUSALS, code


#: query_interaction_check results as the bridge gives them (ai_eval_hook.cpp
#: execute_interaction_check_intent). The texts are the game's (run l10fx5-live).
def _NO_GROUNDS(t):
    return {"target_id": t, "sendable": False, "refused_by": "send_conditions_not_met",
            "engine_reason": "You have no reason to imprison them", "answer": "maybe", "accept_score": 12.0}


def _MAYBE(t, score):
    return {"target_id": t, "sendable": True, "answer": "maybe", "accept_score": score,
            "answer_text": "Might succeed"}


def _check(*rows):
    return {"success": True, "action": "query_interaction_check",
            "result": {"interaction": "imprison_interaction", "results": list(rows)}}


def test_the_imprison_lever_is_the_imprison_targets_row():
    """One rule: the leader's imprison lever and the imprison_targets enumeration
    are the same row from redesign.imprison_rows, blocked or not alike, judged by
    the game's own send check (query_interaction_check)."""
    fac = {"character_id": LIEGE, "factions": [{"faction_id": 69, "type": "independence_faction",
                                                "leader_character_id": A}]}
    for sendable in (False, True):
        ann = {"has_imprisonment_reason": sendable,
               "reasons": [{"type": "imprisonment_reason", "modifier": "x"}] if sendable else []}
        gw = Gw({"query_factions": fac, "query_faction_threat": {}, "query_council": {},
                 "query_imprisonment_reasons": ann,
                 "query_interaction_check": _check(_MAYBE(A, 41.5) if sendable else _NO_GROUNDS(A)),
                 "query_realm": {"vassals": [{"id": A, "name": "X"}]}})
        lever = run(R.inspect_impl(gw, subject="factions", character_id=LIEGE))[
            "result"]["factions"][0]["levers"]["imprison"]
        env = run(R.do_impl(gw, action_class="imprison_targets", character_id=LIEGE))
        rows = env["result"]["available"] + env["result"]["unavailable"]
        assert len(rows) == 1
        row = {k: v for k, v in rows[0].items() if k != "label"}
        assert row == lever
        assert row["do_call"]["args"] == {"interaction": "imprison_interaction", "target_id": A}
        assert (row["blocked"] is None) is sendable
        assert row["game_check"]["sendable"] is sendable
        if sendable:
            assert row["chance_pct"] == 41.5
        else:
            assert "chance_pct" not in row
            assert row["blocked"]["raw_reason"] == "send_conditions_not_met"
            assert "You have no reason to imprison them" in row["blocked"]["summary"]
        assert ("query_imprisonment_reasons", {"actor_id": LIEGE, "target_id": A}) in gw.reads
        assert ("query_interaction_check", {"actor_id": LIEGE, "interaction_type": "imprison_interaction",
                                            "candidates": [A]}) in gw.reads


def test_grounds_without_the_games_send_check_are_not_offered():
    """Witness 4: grounds (query_imprisonment_reasons) are not the game's verdict on
    the send. A target with grounds is offered only when the game's check says the
    send goes and the recipient may accept; an unread check, a refusal and a "no"
    are blocked with the game's words."""
    grounds = {"has_imprisonment_reason": True, "reasons": [{"type": "imprisonment_reason", "modifier": "x"}]}
    # The gates are the ones a direct send of the same refusal gets (lane l5's
    # interaction_invalid / recipient_refuses; RAW_REASON_TO_GATE), one mapping.
    cases = [
        ({}, "read_timeout", "send_check_unread"),
        (_check({"target_id": A, "sendable": False, "refused_by": "interaction_not_valid_for_target",
                 "engine_reason": "Is not in your realm"}), "interaction_invalid",
         "interaction_not_valid_for_target"),
        # the game's tooltip header alone is no reason: the bridge drops it
        # (engine_text.h), so engine_reason is absent and the reply says the game
        # gives no detail (L12)
        (_check({"target_id": A, "sendable": False, "refused_by": "send_conditions_not_met"}),
         "interaction_invalid", "send_conditions_not_met"),
        (_check({"target_id": A, "sendable": False, "refused_by": "target_would_reject",
                 "engine_reason": "Will not succeed"}), "recipient_refuses", "target_would_reject"),
        (_check({"target_id": A, "sendable": False, "refused_by": "send_cost_unaffordable",
                 "engine_reason": "does not have 225 gold"}), "cannot_afford", "send_cost_unaffordable"),
        (_check({"target_id": A, "sendable": True, "answer": "no", "accept_score": -40.0,
                 "answer_text": "Cannot succeed"}), "recipient_refuses", "answer_no"),
    ]
    for reply, gate, raw in cases:
        gw = Gw({"query_imprisonment_reasons": grounds, "query_interaction_check": reply,
                 "query_realm": {"vassals": [{"id": A, "name": "X"}]}})
        res = run(R.do_impl(gw, action_class="imprison_targets", character_id=LIEGE))["result"]
        assert res["available"] == [], reply
        b = res["unavailable"][0]["blocked"]
        assert (b["gate"], b["raw_reason"]) == (gate, raw)
        assert b["summary"] and b["summary"] != raw and not b["summary"].endswith(":")
        if raw == "send_conditions_not_met":
            assert b["summary"].endswith(": " + R.NO_GAME_DETAIL), b["summary"]


def test_imprison_targets_offers_the_likeliest_first():
    """The available rows are ordered by the recipient's score, highest first."""
    grounds = {"has_imprisonment_reason": True, "reasons": []}
    gw = Gw({"query_imprisonment_reasons": grounds,
             "query_interaction_check": _check(_MAYBE(A, 12.0), _MAYBE(B, 71.0),
                                               {"target_id": 7, "sendable": True, "answer": "yes",
                                                "accept_score": 153.5}),
             "query_realm": {"vassals": [{"id": A, "name": "a"}, {"id": B, "name": "b"},
                                         {"id": 7, "name": "c"}]}})
    res = run(R.do_impl(gw, action_class="imprison_targets", character_id=LIEGE))["result"]
    assert [(r["do_call"]["args"]["target_id"], r["answer"], r["accept_score"]) for r in res["available"]] == \
        [(7, "yes", 153.5), (B, "maybe", 71.0), (A, "maybe", 12.0)]
    checks = [p for a, p in gw.reads if a == "query_interaction_check"]
    assert checks == [{"actor_id": LIEGE, "interaction_type": "imprison_interaction", "candidates": [A, B, 7]}]


def test_a_yes_is_the_games_answer_not_a_certainty():
    """MAJOR 3 (witness #5): "yes" was shipped as chance_pct 100 while targets read
    "yes" stayed free in 2 of 3 runs (13327: yes, score 239, sent, never imprisoned).
    A row carries the game's answer and the raw score; a chance only where the game
    defines one: a "maybe" in the maybe band (above 0, below 100), the score being
    that chance. A "yes" and a "no" carry none, and no row ever reads 100 percent."""
    grounds = {"has_imprisonment_reason": False, "reasons": []}
    rows_in = [{"target_id": 1, "sendable": True, "answer": "yes", "accept_score": 239.0,
                "answer_text": "Will succeed"},
               {"target_id": 2, "sendable": True, "answer": "yes", "accept_score": 100.0},
               _MAYBE(3, 99.5), _MAYBE(4, 0.5),
               # "maybe" whatever the score (the type's other flag): no band, no chance
               {"target_id": 5, "sendable": True, "answer": "maybe", "accept_score": 140.0},
               {"target_id": 6, "sendable": True, "answer": "maybe"}]
    gw = Gw({"query_imprisonment_reasons": grounds, "query_interaction_check": _check(*rows_in)})
    rows = {r["do_call"]["args"]["target_id"]: r
            for r in run(R.imprison_rows(gw, LIEGE, [1, 2, 3, 4, 5, 6]))}
    assert rows[1]["answer"] == "yes" and rows[1]["accept_score"] == 239.0 and "chance_pct" not in rows[1]
    assert rows[1]["game_check"]["answer_text"] == "Will succeed"
    assert "chance_pct" not in rows[2]
    assert rows[3]["chance_pct"] == 99.5 and rows[4]["chance_pct"] == 0.5
    assert "chance_pct" not in rows[5] and "chance_pct" not in rows[6]
    assert all(r.get("chance_pct") != 100.0 for r in rows.values())
    assert all(r["blocked"] is None for r in rows.values())


def test_imprison_targets_checks_the_named_targets():
    """candidates names whom to check (a check right before a send): the game's
    check runs for those ids only, and no realm read is needed."""
    gw = Gw({"query_imprisonment_reasons": {"reasons": []},
             "query_interaction_check": _check(_MAYBE(B, 30.0))})
    res = run(R.do_impl(gw, action_class="imprison_targets", character_id=LIEGE, candidates=[B]))["result"]
    assert [r["do_call"]["args"]["target_id"] for r in res["available"]] == [B]
    assert [a for a, _ in gw.reads if a == "query_realm"] == []
    assert [p for a, p in gw.reads if a == "query_interaction_check"] == [
        {"actor_id": LIEGE, "interaction_type": "imprison_interaction", "candidates": [B]}]


def test_a_declined_realm_read_is_a_refusal_not_no_vassals():
    """Nobody looked is not "no target": a realm read the bridge declined, or one
    that did not come back, refuses the enumeration with its gate."""
    for reply, gate in (({"success": False, "reason": "character_not_found"}, "character_not_found"),
                        ({}, "read_timeout")):
        env = run(R.do_impl(Gw({"query_realm": reply}), action_class="imprison_targets", character_id=LIEGE))
        assert env["ok"] is False and env["blocked"]["gate"] == gate, env


def test_the_imprison_interaction_key_is_written_once():
    """The literal lives in redesign.py alone (IMPRISON_INTERACTION); factions.py
    neither names it nor reads the grounds itself."""
    import ast
    here = os.path.dirname(__file__)
    count = {}
    for name in ("redesign.py", "factions.py"):
        with open(os.path.join(here, name)) as fh:
            tree = ast.parse(fh.read())
        count[name] = sum(1 for n in ast.walk(tree)
                          if isinstance(n, ast.Constant) and n.value in ("imprison_interaction",
                                                                         "query_imprisonment_reasons"))
    assert count == {"redesign.py": 2, "factions.py": 0}, count


def test_every_faction_refusal_code_has_a_gate():
    for code in ("engine_refused_create", "engine_refused_join", "engine_refused_leave",
                 "engine_refused_demand", "not_the_faction_leader", "faction_call_crashed",
                 "actor_not_member_after_call", "still_a_member_after_leave",
                 "demand_still_pressable_after_press", "not_a_controlled_character"):
        assert R.map_raw_reason(code)["gate"] != "unmapped_refusal", code
    assert factions.FACTION_ACTIONS == ("create", "join", "leave", "press_demand", "respond_demand")


#: Every way factions.cpp puts a code into a reply (witness #5: the
#: `r.key("error"); r.value_string(code)` form was not collected).
_CODE_FORMS = (
    r'(?:reject\(intent,|short_error\(|fail\()\s*"([a-z_]+)"',            # the refusal helpers
    r'rejection_reason,\s*"([a-z_]+)"',                                     # an intent's rejection
    r'\\?"(?:error|reason)\\?":\\?"([a-z_]+)',                               # a literal JSON reply
    r'key\("(?:error|reason|verdict_error)"\);\s*\w+(?:\.|->)value_string\("([a-z_]+)"\)',  # a builder
    r'snprintf\(\s*[\w.>-]*error\w*\s*,[^,]+,\s*"([a-z_]+)"',               # an error buffer
)


def _faction_codes_in(src: str, pol: str = "") -> set:
    import re
    codes: set = set()
    for form in _CODE_FORMS:
        codes |= set(re.findall(form, src))
    if "scope_latched_reason()" in src:
        m = re.search(r'scope_latched_reason\(\)\s*\{\s*return\s*"([a-z_]+)"', pol)
        assert m, "scope_latched_reason's code not found"
        codes.add(m.group(1))
    return codes


def _bridge_faction_codes() -> set:
    """Every reason code factions.cpp can put in a reply, read from the source: the
    codes passed to reject(), short_error() and fail(), written into an intent's
    rejection_reason, given as a reply's "error", "reason" or "verdict_error" (a
    literal reply, a ResponseBuilder value, an error buffer), and the scope latch's
    code (its function's return, read from scope_slot_policy.h)."""
    from pathlib import Path
    root = Path(__file__).resolve().parents[2] / "bridge"
    return _faction_codes_in((root / "src" / "factions.cpp").read_text(),
                             (root / "include" / "scope_slot_policy.h").read_text())


def test_the_code_collector_catches_every_emission_form():
    """Mutants: a fake code written in each form factions.cpp uses must be collected
    and must red the gate test (it maps to no gate)."""
    from pathlib import Path
    src = (Path(__file__).resolve().parents[2] / "bridge" / "src" / "factions.cpp").read_text()
    base = _faction_codes_in(src, 'scope_latched_reason() { return "rng_scope_latched"; }')
    forms = ('r.key("error"); r.value_string("zz_fake_code");',
             'r->key("reason"); r->value_string("zz_fake_code");',
             'r.key("verdict_error"); r.value_string("zz_fake_code");',
             'snprintf(g_faction_query.error, sizeof g_faction_query.error, "zz_fake_code");',
             'reject(intent, "zz_fake_code", "", &r);',
             'short_error("zz_fake_code");',
             'snprintf(intent->rejection_reason, sizeof intent->rejection_reason, "zz_fake_code");',
             '*out = R"({"action":"query_joinable_factions","error":"zz_fake_code"})";')
    for f in forms:
        got = _faction_codes_in(src + "\n" + f + "\n", 'scope_latched_reason() { return "rng_scope_latched"; }')
        assert got - base == {"zz_fake_code"}, f
        assert R.map_raw_reason("zz_fake_code")["gate"] == "unmapped_refusal"
    # the forms the source holds today are collected (the ones witness #5 named)
    assert {"actor_not_found", "faction_query_exception", "engine_call_faulted"} <= base


def test_every_refusal_the_bridge_code_can_emit_has_a_gate():
    """Read from factions.cpp: every code it can emit must map to a named gate (a
    new spelling reds here, not at the seat)."""
    codes = _bridge_faction_codes()
    assert len(codes) >= 25, sorted(codes)
    unmapped = [c for c in sorted(codes) if R.map_raw_reason(c)["gate"] == "unmapped_refusal"]
    assert not unmapped, unmapped


def test_every_refusal_the_bridge_code_can_emit_is_said_in_words():
    """Witness 4: already_a_member, faction_type_not_found and actor_not_alive came
    back with the code as the summary. Every code factions.cpp can emit that is not
    the game's own refusal (which carries the game's sentence) has a plain sentence,
    and the blocked block a player reads carries it, never the code."""
    codes = _bridge_faction_codes()
    missing = sorted(c for c in codes if c not in factions.ENGINE_REFUSALS
                     and c not in factions.REFUSAL_SENTENCES)
    assert not missing, missing
    for c in sorted(codes):
        b = factions._refusal(c, leader="Adanis (11949)")
        assert b["summary"] and b["summary"] != c and "_" not in b["summary"].split(";")[0].split()[0], (c, b)
        assert b["raw_reason"] == c
    # the pre-checks are the bridge's own: gate faction_precondition, never the game's
    for c in factions.PRECHECK_CODES:
        assert factions._refusal(c)["gate"] == "faction_precondition"


def test_at_war_and_war_id_are_the_bridges_one_reading():
    """MAJOR 2/5 (witness #5): the bridge read faction+0xC1 (not the war) and Python
    patched it (at_war or a war id not marked ended). The bridge now reads the war
    itself (faction_war.h), so Python relays at_war as given, and the war id is the
    full id the war reads give (33554451 = generation 2, slot 19), never masked."""
    fac = {"character_id": LIEGE, "factions": [
        {"faction_id": 95, "type": "independence_faction", "at_war": True, "war_id": 33554451},
        # a row the bridge says is not at war stays so, whatever else it carries
        {"faction_id": 96, "type": "liberty_faction", "at_war": False, "war_id": 19, "war_inactive": False}]}
    env = run(R.inspect_impl(Gw({"query_factions": fac, "query_faction_threat": {}, "query_council": {}}),
                             subject="factions", character_id=LIEGE))
    rows = {f["faction_id"]: f for f in env["result"]["factions"]}
    assert rows[95]["at_war"] is True and rows[95]["war_id"] == 33554451
    assert rows[96]["at_war"] is False
    import ast
    with open(os.path.join(os.path.dirname(__file__), "factions.py")) as fh:
        consts = {n.value for n in ast.walk(ast.parse(fh.read()))
                  if isinstance(n, ast.Constant) and isinstance(n.value, str)}
    assert "war_inactive" not in consts


def test_the_bridge_reads_a_factions_war_in_one_function():
    """The bridge side: faction_war (factions.cpp) is the one reading of WAR_REF;
    every faction reply emits at_war / war_id through emit_faction_war; nothing reads
    AT_WAR_FLAG (+0xC1, not the war) or masks the war id."""
    import re
    from pathlib import Path
    root = Path(__file__).resolve().parents[2] / "bridge"
    fac = _code_only((root / "src" / "factions.cpp").read_text())
    qh = _code_only((root / "src" / "query_handlers.cpp").read_text())
    assert len(re.findall(r"^FactionWar faction_war\(", fac, re.M)) == 1
    assert fac.count("faction_v2::WAR_REF") == 1 and "faction_v2::WAR_REF" not in qh
    for src in (fac, qh):
        assert "AT_WAR_FLAG" not in src and "WAR_INACTIVE_FLAG" not in src and "SHOULD_PRESS_DEMAND" not in src
    assert fac.count('key("at_war")') == 1                # emit_faction_war's own
    assert qh.count("emit_faction_war(r, fo)") == 2       # query_factions, query_faction_threat
    assert "emit_faction_war(r, f)" in fac                # the vassal's demand rows
    assert "ultimatum_delay_days" not in qh


# ── L13 FF4 (witness r4 m3): the drain-timeout path follows the lever table ────

TIMED_OUT = {"success": False, "reason": "intent_timeout_no_result",
             "intent_request_id": "fa_1_2_3", "dispatch_id": "fa_1_2_3"}


def test_a_faction_write_the_drain_ran_with_no_reply_is_PENDING_READBACK_not_rejected():
    """A faction write is an "on_execute" lever (LEVERS ("faction", "*")): a write
    that ran is read back in the call. When the handler's reply never came, the
    drain's verdict is read (success) and the read-back cannot be made, so the
    reply is pending readback with result.sent, ok:true (FF5 ruling). FF2/FF3
    answered pending clock_advances (a days-wait), FF4 rejected write_unconfirmed."""
    gw = Gw({"query_dispatch_result": {"status": "success", "dispatch_id": "fa_1_2_3"}},
            write_reply=TIMED_OUT)
    env = run(R.do_impl(gw, intent="faction", character_id=A,
                        args={"action": "join", "faction_id": 70}))
    _unread(env)
    assert R.lever_of("faction", sub="join")[1]["shows"] == "on_execute"


def test_a_faction_write_whose_verdict_is_never_read_is_NEXT_PASS():
    gw = Gw({"query_dispatch_result": {"status": "pending", "dispatch_id": "fa_1_2_3"}},
            write_reply=TIMED_OUT)
    env = run(R.do_impl(gw, intent="faction", character_id=A,
                        args={"action": "join", "faction_id": 70}))
    assert env["verdict"] == "pending", env
    assert env["pending"]["drains_when"] == "next_pass", env["pending"]
