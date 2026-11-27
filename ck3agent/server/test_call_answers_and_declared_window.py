"""Three readings from recorded runs, each red without its rule and green with it; the values are the runs' own.

  1  a call to war the seat sent (call_ally to 32624, war 50331777, sent 1086.01.06) had no answer row, while the
     game's acceptance letter call_ally.0100 sat in events_answered (1086.01.14), and the prestige line ruled out
     "no ally joined a war you attack in" beside -274 unexplained       -> asks_answered row, allies_joined row
  2  the same war, declared by the seat at the turn and won inside the step, read "begun and ended between two
     reads"                                                             -> the row says the seat declared it
  3  succession_plan's disinherit block gave one son's reason without naming him; the lever rows and the goal line
     spoke of another son                                               -> `why_not_of`, `allowed_for`
No game.
"""
from __future__ import annotations

import asyncio
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))

import news  # noqa: E402
import partition_forecast as PF  # noqa: E402
import redesign as R  # noqa: E402
from test_ireland_walk_replies import Gw  # noqa: E402

BRIAN, MURCHAD_DUBLIN, ALLY, WAR = 36303, 31506, 32624, 50331777

#: the war's record as the recorded wars_ended row gave it (name, sides, outcome, end date; the cb from the
#: seat's declare_war reply; c_dublin 463 its target)
REC = {"war_id": WAR, "war_name": "Irish De Jure War for the Earldom of Dubhlinn", "attacker_id": BRIAN,
       "defender_id": MURCHAD_DUBLIN, "outcome": "attacker_victory", "end_date": "1086.07.04",
       "cb_type": "individual_county_de_jure_cb", "target_titles": [463]}
#: the recorded events_answered row (1086.01.14, call_ally.0100, EXCELLENT), in the bridge's pick form
ACCEPTED = {"seq": 1, "character_id": BRIAN, "date": "1086.01.14", "event": "call_ally.0100", "options": ["EXCELLENT"],
            "ai_pick": {"index": 0, "key": "EXCELLENT"}, "applied": {"index": 0, "key": "EXCELLENT"}, "rule": None}


def run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _fresh():
    news.reset()
    yield
    news.reset()


def _hist(p):
    if p.get("defender_id") is not None or p.get("war_id") not in (None, WAR):
        return {"wars": []}
    return {"wars": [REC]}


def _gw():
    # 32624's tier: the seat's call reply priced his joining at -350 (a king)
    return Gw({"query_war_history": _hist,
               "query_character": lambda p: {"id": p["character_id"], "ruler_tier": 4, "alive": True}})


def _snap(date, prestige, titles, picks, watched=True):
    return {"actor_id": BRIAN, "date": date, "prestige": prestige, "piety": 860.52, "gold": 51.96, "wars": {},
            "laws": [], "titles": titles, "claims": {}, "event_watched": watched, "event_picks": picks}


def _turn(picks=(ACCEPTED,), declared=True, called=True, watched=True):
    """The recorded turn: read 1086.01.06 (prestige 868.94), the seat declares war 50331777 (-50 charged) and calls
    32624; the next read 1086.07.05 (571.47), c_dublin gained."""
    gw = _gw()
    run(news.block(gw, 0, _snap("1086.01.06", 868.94, {491: "c_mayo"}, [], watched), offers_served=False,
                   baseline="turn"))
    if declared:
        news.note_act(BRIAN, "1086.01.06", f"declared war {WAR} (individual_county_de_jure_cb)", cost_known=True,
                      prestige=-50.0, measured=True, war_id=WAR, war_title_id=463)
    if called:
        news.note_call(BRIAN, ALLY, WAR, "1086.01.06")
    return run(news.block(gw, 0, _snap("1086.07.05", 571.47, {491: "c_mayo", 463: "c_dublin"}, list(picks), watched),
                          offers_served=False, baseline="turn"))


# ── 1: the answer to a call to war ─────────────────────────────────────────────────────────────────────────────────

def test_1_RED_the_acceptance_letter_answers_the_call():
    (row,) = _turn()["asks_answered"]
    assert row["interaction"] == "call_ally_interaction" and row["target_id"] == ALLY and row["war_id"] == WAR
    assert row["answer"] == "joined" and "call_ally.0100" in row["how"] and "1086.01.14" in row["how"]


def test_1_RED_his_joining_is_priced_and_not_ruled_out():
    out = _turn()
    (j,) = out["allies_joined"]
    assert (j["ally_id"], j["war_id"], j["prestige"]) == (ALLY, WAR, -350) and j["called_by"].startswith("you")
    p = out["purse"]["prestige"]
    assert "no ally joined a war you attack in" not in p["ruled_out"]
    assert any(r.get("expected") == -350 for r in p["in_this_step"])


def test_1_said_once():
    _turn()
    gw = _gw()
    out = run(news.block(gw, 0, _snap("1086.09.01", 575.0, {491: "c_mayo", 463: "c_dublin"}, [ACCEPTED]),
                         offers_served=False, baseline="turn"))
    assert "asks_answered" not in out


def test_1_a_refusal_letter_is_declined():
    (row,) = _turn(picks=[dict(ACCEPTED, event="call_ally.0101", options=["call_ally.0101.a"])])["asks_answered"]
    assert row["answer"] == "declined" and "call_ally.0101" in row["how"]


def test_1_a_war_over_with_no_letter_is_said():
    out = _turn(picks=[])
    (row,) = out["asks_answered"]
    assert row["answer"] == "war_ended" and "attacker_victory" in row["how"] and "did not join" in row["how"]
    # read: no letter came, so nothing joined; the rule-out holds
    assert "no ally joined a war you attack in" in out["purse"]["prestige"]["ruled_out"]


def test_1_without_the_letters_a_join_is_not_ruled_out():
    out = _turn(picks=[], watched=False)
    (row,) = out["asks_answered"]
    assert row["answer"] == "not_read"
    assert "no ally joined a war you attack in" not in out["purse"]["prestige"]["ruled_out"]


def test_1_CONTROL_no_call_no_row_and_the_rule_out_stands():
    out = _turn(picks=[], called=False)
    assert "asks_answered" not in out
    assert "no ally joined a war you attack in" in out["purse"]["prestige"]["ruled_out"]


def test_1_a_running_war_waits_then_shows_the_join():
    me_war = {WAR: {"war_id": WAR, "name": REC["war_name"], "attacker_id": BRIAN, "defender_id": MURCHAD_DUBLIN,
                    "is_attacker": True, "cb_type": "individual_county_de_jure_cb", "attackers": [BRIAN]}}
    gw = _gw()
    s0 = dict(_snap("1086.01.06", 868.94, {}, []), wars=me_war)
    run(news.block(gw, 0, s0, offers_served=False, baseline="turn"))
    news.note_call(BRIAN, ALLY, WAR, "1086.01.06")
    out = run(news.block(gw, 0, dict(_snap("1086.01.10", 818.94, {}, []), wars=me_war), offers_served=False,
                         baseline="turn"))
    assert out["asks_answered"][0]["answer"] == "waiting"
    joined = {WAR: dict(me_war[WAR], attackers=[BRIAN, ALLY])}
    out = run(news.block(gw, 0, dict(_snap("1086.02.10", 468.94, {}, []), wars=joined), offers_served=False,
                         baseline="turn"))
    (row,) = out["asks_answered"]
    assert row["answer"] == "joined" and "attackers" in row["how"]
    assert len(out["allies_joined"]) == 1                  # counted once, from the war list


def test_1_the_call_reply_records_the_ask():
    ME, ULSTER = 31762, 32427
    gw = Gw({"query_wars": {"wars": [{"war_id": 91, "primary_attacker": ME, "primary_defender": 29585,
                                      "attackers": [ME], "defenders": [29585]}]},
             "query_character": lambda p: {"id": p["character_id"], "ruler_tier": 2}})

    async def cw(action, params, readback=None, deadline_s=0, max_polls=0):
        return {"sent": True, "raw": {"success": True, "status": "queued"}, "resolved": None,
                "drains_when": "clock_advances"}
    gw.confirmed_write = cw
    run(R._do_war_intent(gw, "call_ally", {"war_id": 91, "target_id": ULSTER}, ME, {"id": ME}))
    (a,) = news._ASKS[ME]
    assert (a["interaction"], a["target_id"], a["war_id"]) == ("call_ally_interaction", ULSTER, 91)


# ── 2: a war the seat declared and won inside one step ─────────────────────────────────────────────────────────────

def test_2_RED_the_row_says_the_seat_declared_it():
    (w,) = _turn()["wars_ended"]
    assert w["declared_by"] == "you" and w["declared"] == "1086.01.06"
    assert w["seen"].startswith("declared by you on 1086.01.06") and "attacker_victory" in w["seen"]
    assert "begun and ended between two reads" not in w["seen"]


def test_2_CONTROL_a_war_the_seat_did_not_declare_keeps_the_window_words():
    (w,) = _turn(declared=False, called=False)["wars_ended"]
    assert w["seen"] == news.WINDOW_SEEN and "declared_by" not in w


# ── 3: the disinherit block names the son its reason is for ─────────────────────────────────────────────────────────
#
# A run's turn 48 (1089.06.30), succession_plan: levers.disinherit {candidates [36303, 70282], why_not "you cannot pay its
# price: Missing 621"} and levers.take_out_of_split.disinherit[0] {son_id 70282, game_check.refused.code
# interaction_not_valid_for_target, why_not the culture-line sentence}: two sons, two reasons, the block naming none.

SON_A, SON_B, HEAD = 36303, 70282, 31762
CHECK_A = {"target_id": SON_A, "sendable": False, "refused_by": "send_cost_unaffordable", "engine_reason": "Missing 621"}
CHECK_B = {"target_id": SON_B, "sendable": False, "refused_by": "interaction_not_valid_for_target",
           "engine_reason": "Culture does not have the Mystical Ancestors Tradition."}


class _FamilyGw:
    def __init__(self, checks):
        self.checks = checks

    def game(self):
        return {"date": "1089.06.30"}

    async def read(self, action, params=None):
        p = params or {}
        if action == "query_character":
            return {"id": p.get("character_id"), "age": 30, "alive": True, "is_female": False, "dynasty_id": 2044}
        if action == "query_realm_laws":
            return {"active_laws": [{"name": "confederate_partition_succession_law",
                                     "group_name": "succession_order_laws"}]}
        if action == "query_dynasty":
            return {"dynasty_id": 2044, "head_character_id": HEAD}
        if action == "query_interaction_check":
            return {"result": {"results": [c for c in self.checks if c["target_id"] in p.get("candidates", [])]}}
        return {}


def _block(checks):
    ruler = {"id": HEAD, "dynasty_id": 2044, "children": [SON_A, SON_B], "primary_title_id": 518}
    return run(R._succession_levers(_FamilyGw(checks), HEAD, [{"id": SON_A}, {"id": SON_B}], ruler))["disinherit"]


def test_3_RED_the_reason_names_its_son():
    d = _block([CHECK_A, CHECK_B])
    assert d["candidates"] == [SON_A, SON_B]
    assert d["why_not"] == "you cannot pay its price: Missing 621" and d["why_not_of"] == SON_A
    # the other son's own verdict, the lever row's words in that run
    assert PF.disinherit_allowed(HEAD, HEAD, PF._game_check(CHECK_B, {})) == (False, [PF.MYSTICAL_ANCESTORS_WHY])


def test_3_RED_allowed_names_the_sons_it_is_allowed_for():
    d = _block([CHECK_A, dict(CHECK_B, sendable=True, refused_by=None, engine_reason=None)])
    assert d["available"] is True and d["allowed_for"] == [SON_B] and "why_not_of" not in d


def test_3_the_one_verdict_per_son_is_unchanged():
    gca, gcb = PF._game_check(CHECK_A, {}), PF._game_check(CHECK_B, {})
    assert PF.disinherit_summary(HEAD, HEAD, [gca, gcb]) == (False, "you cannot pay its price: Missing 621")
    assert PF.disinherit_by_son(HEAD, HEAD, [(SON_A, gca), (SON_B, gcb)])[2] == SON_A


def test_3_the_recorded_numbers_are_a_landed_sons_price():
    """One full run: succession_plan's block at turn 28/turn 38/turn 40 read "Missing" prestige and renown; held + missing
    is 900 prestige and 450 renown each time, the price for a landed son (150 + 750; (75 + 150) x 2), while the goal
    line's son 63985 was a courtier then (take_vows, which needs is_courtier_of, sendable at turn 38 and turn 40) and was
    allowed at turn 44 with 306.98 renown. The block's reason was another son's, not his."""
    held = {"T28": (227.49, 216.1, 673, 234), "T38": (343.59, 255.28, 557, 195), "T40": (367.24, 277.44, 533, 173)}
    for prestige, renown, miss_p, miss_r in held.values():
        assert abs(prestige + miss_p - 900) < 1 and abs(renown + miss_r - 450) < 1   # the game rounds "Missing"
    landless_heir_renown = (75 + 75) * PF.ERA_FACTOR_EARLY
    assert landless_heir_renown == 300 < 306.98 < 450
