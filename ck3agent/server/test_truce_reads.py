"""What a seated game on the test host found,
each row replayed from its recorded reply.

a seated game: Murchad, Count of Dublin (31506), 1066.09.15 -> 1096.09.15, his son Domnall (36124)
from 1082.08.21. Recorded replies are quoted in the tests as the values they held; rules read
from the game's own files are checked against the install when one is present (CK3_GAME_DIR, or
the default Steam path) and skipped otherwise.
"""
import asyncio
import json
import os
from pathlib import Path

import pytest

import declare_gates as DG
import news
import redesign as R
try:  # absent without the rule: its rows run red
    import truces as TR
except ImportError:  # pragma: no cover
    TR = None
from test_ireland_walk_replies import Gw, cbs_by_target

HERE = Path(__file__).resolve().parent
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")
DOMNALL, ATHLONE_LORD, C_ATHLONE = 36124, 14492, 469


def run(c):
    return asyncio.run(c)


def game_file(rel: str) -> str:
    p = GAME / rel
    if not p.is_file():
        pytest.skip(f"no game install at {GAME}")
    return p.read_text(encoding="utf-8-sig", errors="replace")


@pytest.fixture(autouse=True)
def _fresh(monkeypatch):
    news.reset()
    R.CB_COST_SEEN.clear()
    if TR:
        TR.forget()

    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    monkeypatch.delenv("CK3_EPISODE_HORIZON_END", raising=False)
    yield
    if TR:
        TR.forget()
    news.reset()


# ── 1: a truce the game enforces, on every war read ─────────────────────────────────────

#: a seated game 1095.05.15, cbs(target_id=14492): the bridge's read, as the toolkit received it
#: (target_state in_truce false; claim_cb valid, pressed, 46.88 prestige; gold 136.2)
ATHLONE_CBS_1095 = {
    "status": "ok", "attacker_id": DOMNALL, "prestige": 438.58, "piety": 344.1,
    "target_state": {"independent": True, "imprisoned": False, "in_alliance": False, "in_truce": False,
                     "already_at_war_opposing": False, "pressed_claim_on_target": True},
    "cbs": [{"cb_key": "claim_cb", "valid": True, "scope": "county", "target_mode": 6, "family": "claim",
             "cb_title_id": C_ATHLONE, "claimant_id": DOMNALL, "cost_prestige": 0, "cost_piety": 0,
             "affordable": True, "dispatch_supported": True, "needs_cb_title_id": True,
             "needs_claimant": True, "requirements_unmet": []}],
}
#: the game's refusal of 1094.05.15, 1095.05.15 and 1096.05.15 (declare_war, the same args)
GAME_REFUSAL = {"gate": "target_truced", "marks": ["WHY_TRUCE"], "raw_reason": "can_declare_war_false"}
#: the white peace that made the truce: end_war(enemy 14492, white_peace) applied 1092.05.15
WHITE_PEACE_DATE = "1092.05.15"


def _relations(truces):
    """query_relations as the bridge answers it (the relation slots)."""
    return {"action": "query_relations", "character_id": DOMNALL, "truces": truces, "truces_on_him": [],
            "truce_count": len(truces), "engine_list_ids": [], "engine_truce_probe": "ok"}


ATHLONE_TRUCE = {"target_id": ATHLONE_LORD, "end_date": "1097.05.15", "end_date_raw": 400160,
                 "result_code": 2, "war_name": "Dyflinner Claim on the Earldom of Athlone",
                 "source": "relation_slot"}


def _gw(relations=None, date="1095.05.15"):
    gw = Gw(cbs_by_target(ATHLONE_CBS_1095), date=date)
    gw.replies["query_character"] = lambda p: {
        "id": p.get("character_id"), "name": "Domnall", "alive": True, "age": 47, "liege_id": 0,
        "top_liege_id": p.get("character_id"), "gold": 136.2, "prestige": 438.58, "piety": 344.1,
        "faith_key": "catholic", "income_breakdown": {"total_monthly": 4.43}}
    if relations is not None:
        gw.replies["query_relations"] = relations
    return gw


def test_1_the_recorded_reads_said_no_truce():
    """What a seated game held: the cbs read said in_truce false; the game refused for has_truce; the
    truce was 1825 days from the white peace of 1092.05.15 (until 1097.05.15, after the horizon)."""
    assert ATHLONE_CBS_1095["target_state"]["in_truce"] is False
    assert GAME_REFUSAL["marks"] == ["WHY_TRUCE"]
    from cycle_cadence import ordinal, date_from_ordinal
    assert date_from_ordinal(ordinal(WHITE_PEACE_DATE) + 1825) == "1097.05.15"


def test_1_cbs_shows_the_truce_with_its_end_and_does_not_offer_the_war(monkeypatch):
    monkeypatch.setenv("CK3_EPISODE_HORIZON_END", "1096.09.15")
    env = run(R.cbs_impl(_gw(_relations([ATHLONE_TRUCE])), target_id=ATHLONE_LORD, character_id=DOMNALL))
    res = env["result"]
    ts = res["target_state"]
    assert ts.get("in_truce") is True, ts
    assert ts["truce"]["until"] == "1097.05.15" and ts["truce"]["days_left"] == 730
    assert ts["truce"]["outlasts_episode"] is True
    assert res["best"] is None and res["valid_options"] == []
    (b,) = res["blocked_now"]
    (cond,) = b["blocked_by"]
    assert cond["condition"] == "truce" and "1097.05.15" in cond["fix"] and "episode" in cond["fix"], cond


def test_1_CONTROL_no_truce_in_the_slots_offers_the_war():
    env = run(R.cbs_impl(_gw(_relations([])), target_id=ATHLONE_LORD, character_id=DOMNALL))
    assert env["result"]["best"] == "claim_cb"
    assert env["result"]["target_state"]["in_truce"] is False


def test_1_CONTROL_an_older_bridge_leaves_the_read_as_it_was():
    """A bridge without the slot read (no truces_on_him): the overlay does nothing."""
    env = run(R.cbs_impl(_gw({"truces": [], "truce_count": 0}), target_id=ATHLONE_LORD, character_id=DOMNALL))
    assert env["result"]["best"] == "claim_cb"
    assert "truce" not in env["result"]["target_state"]


def test_1_declarable_now_is_the_one_answer_and_declare_war_refuses_before_sending():
    gw = _gw(_relations([ATHLONE_TRUCE]))
    opts = run(R.declarable_now(gw, DOMNALL, [ATHLONE_LORD]))[ATHLONE_LORD]
    (o,) = [o for o in opts if o["cb"] == "claim_cb"]
    assert not R.cb_option_declarable(o)
    assert o["blocked_by"][0]["truce"]["until"] == "1097.05.15"
    blk = R.declare_gate_blocked(o)
    assert blk["gate"] == "target_truced" and "1097.05.15" in blk["summary"]


def test_1_the_peace_prices_state_the_truce_they_make(monkeypatch):
    monkeypatch.setenv("CK3_EPISODE_HORIZON_END", "1096.09.15")
    p = R.peace_prices("claim_cb", "attacker", 3.9, ["white_peace", "we_yield"], today=WHITE_PEACE_DATE)
    for t in ("white_peace", "we_yield"):
        tr = p[t]["truce"]
        assert tr["until"] == "1097.05.15" and tr["outlasts_episode"] is True, tr
        assert tr["who"] == "you cannot declare on him"
    d = R.peace_prices("claim_cb", "defender", 3.9, ["white_peace"], today=WHITE_PEACE_DATE)
    assert d["white_peace"]["truce"]["who"].startswith("he cannot declare on you")


def test_1_the_peace_prices_state_each_casus_belli_s_own_truce(monkeypatch):
    """A refused liege demand war's victory and white peace make a truce both ways of 1825 days, its defeat none
    (00_civil_war.txt); a claim war's every ending the one-way truce; the end_war reply says the same, and a casus
    belli whose truce is not read says so."""
    monkeypatch.delenv("CK3_EPISODE_HORIZON_END", raising=False)
    for side, won, lost in (("attacker", "we_win", "we_yield"), ("defender", "we_yield", "we_win")):
        p = R.peace_prices("refused_liege_demand_war", side, 3.9, ["we_win", "white_peace", "we_yield"],
                           today=WHITE_PEACE_DATE)
        for t in (won, "white_peace"):
            assert p[t]["truce_days"] == 1825 and p[t]["truce"]["until"] == "1097.05.15", (side, t, p[t])
            assert p[t]["truce"]["who"] == "neither of you can declare on the other"
        assert p[lost]["truce_days"] == 0 and "until" not in p[lost]["truce"], (side, p[lost])
        assert p[lost]["truce"]["who"].startswith("no truce")
    c = R.peace_prices("claim_cb", "attacker", 3.9, ["we_win", "white_peace", "we_yield"], today=WHITE_PEACE_DATE)
    assert all(c[t]["truce_days"] == 1825 and c[t]["truce"]["who"] == "you cannot declare on him" for t in c)
    gw = Gw(date=WHITE_PEACE_DATE)
    ended = {"end_date": WHITE_PEACE_DATE, "attacker_id": 7, "cb_type": "refused_liege_demand_war"}
    assert R._peace_truce(dict(ended, outcome="defender_victory"), 7, gw)["days"] == 0
    both = R._peace_truce(dict(ended, outcome="white_peace"), 7, gw)
    assert both["days"] == 1825 and both["who"] == "neither of you can declare on the other"
    one = R._peace_truce(dict(ended, outcome="attacker_victory", cb_type="claim_cb"), 8, gw)
    assert one["days"] == 1825 and one["who"].startswith("he cannot declare on you")
    other = R._peace_truce(dict(ended, outcome="white_peace", cb_type="fp3_seljuk_invasion_cb"), 7, gw)
    assert other["days"] == "not read" and other["who"].startswith("not read") and "until" not in other


def test_1_no_horizon_says_so():
    f = TR.facts("1097.05.15", "1095.05.15")
    assert f["days_left"] == 730 and f["episode_end"].startswith("not registered")
    assert "outlasts_episode" not in f


def test_1_truce_rule_matches_the_game_files():
    vals = game_file("common/script_values/00_war_values.txt")
    assert "standard_truce_duration_days = {\n\tvalue = 1825" in vals.replace("\r", "")
    fx = game_file("common/scripted_effects/00_war_effects.txt")
    assert "break_truce_prestige_loss" in fx and "add_truce_one_way" in fx
    basic = game_file("common/script_values/00_basic_values.txt")
    assert "break_truce_prestige_loss = -250" in basic


# ── 5: a war against a rebel who holds no title, at +100 ─────────────────────────────────

def test_5_end_war_on_a_titleless_rebel_matches_his_war_by_side(monkeypatch):
    """A recorded game, 1092.01.06: end_war(enemy 8782, we_win) -> target_ineligible "holds no title";
    the populist war 201326628 was at +100 for Domnall and ended by itself on 1092.01.15."""
    import test_end_war as TE
    from test_script_actions import ran
    REBEL, ME = 8782, TE.ME

    def reply(p):
        if p.get("enemy_landless"):
            return dict(ran("end_war"), marks=["LEADS", "OPEN_WE_WIN", "OPEN_WE_YIELD", "SCORE_GE_100"])
        return {"success": False, "action": "script_action", "reason": "target_not_script_addressable",
                "detail": "target_id"}

    def ended(st, p):
        if p.get("enemy_landless"):
            st["war_list"] = []
            st["ended"] = [{"war_id": 201326628, "outcome": "defender_victory", "end_date": "1092.01.06"}]
    war = {"war_id": 201326628, "name": "Irish Insular Uprising", "primary_attacker": REBEL,
           "primary_defender": ME, "attackers": [REBEL], "defenders": [ME]}
    monkeypatch.setattr(R, "_SLEEP", TE.R._SLEEP)
    gw = TE.WarGw(reply, state={"war_list": [war]}, on_write=ended)
    env = TE.end(gw, enemy_id=REBEL, terms="we_win")
    sent = [p for a, p in gw.writes if a == "script_action"]
    assert len(sent) == 2 and sent[1]["enemy_landless"] is True and sent[1]["terms"] == "we_win"
    assert env.get("verdict") == "applied", env
    assert "holds no title" in env["result"]["enemy_matched_as"]


def test_5_two_landless_wars_are_refused_and_nothing_ends():
    import test_end_war as TE
    from test_script_actions import ran

    def reply(p):
        if p.get("enemy_landless"):
            return dict(ran("end_war", guard="refused"), marks=["AMBIGUOUS"])
        return {"success": False, "action": "script_action", "reason": "target_not_script_addressable",
                "detail": "target_id"}
    gw = TE.WarGw(reply, state={"war_list": []})
    env = TE.end(gw, enemy_id=8782, terms="we_win")
    assert env["blocked"]["raw_reason"] == "two_wars_against_landless_leaders", env


# ── 2: the three prestige drops, named with their amounts ───────────────────────────────

def _purse(prev, snap, out_events):
    return news._resource_change(prev, snap, "prestige", out_events)


def _p(date, prestige, **kw):
    return dict({"actor_id": DOMNALL, "date": date, "prestige": prestige, "monthly_prestige": 1.3,
                 "laws": [], "titles": {}}, **kw)


def test_2_the_recorded_purse_read_partly_unknown():
    """A recorded game, 7.4: unexplained -299.92 (1071.05.17) and -1007.63 (1088.01.19), -280.12 (1092.05.15)."""
    out = _purse(_p("1087.01.19", 1825.25), _p("1088.01.19", 833.21), {})
    assert out["unexplained"] < -990 and out["cause"].startswith("partly unknown")


def test_2_a_culture_change_is_the_decision_and_its_price():
    game_file("common/decisions/10_culture_conversion_decisions.txt")
    out = _purse(_p("1087.01.19", 1825.25, culture="irish"), _p("1088.01.19", 833.21, culture="norse"), {})
    (row,) = [e for e in out["in_this_step"] if "culture" in e["what"]]
    assert row["expected"] == -1000 and "convert_to_local_culture_decision" in row["what"], row
    assert abs(out.get("unexplained", 0)) < 30, out           # the rest is the year's income


def test_2_CONTROL_the_same_culture_names_no_decision():
    out = _purse(_p("1087.01.19", 1825.25, culture="norse"), _p("1088.01.19", 833.21, culture="norse"), {})
    assert not [e for e in out.get("in_this_step") or [] if "culture" in e["what"]]


def test_2_events_the_ai_answered_are_priced_from_their_options():
    game_file("events/travel_events/travel_danger_events_oltner.txt")
    ev = {"events_answered": [
        {"date": "1070.10.02", "event": "travel_danger_events.9010", "applied": "travel_danger_events.9010.b"},
        {"date": "1070.10.20", "event": "travel_danger_events.6000", "applied": "travel_danger_events.6000.c"}]}
    out = _purse(_p("1070.05.17", 468.95, actor_id=31506), _p("1071.05.17", 180.54, actor_id=31506), ev)
    rows = [e for e in out["in_this_step"] if e["what"].startswith("event ")]
    assert [r["expected"] for r in rows] == [-75, -75], rows


def test_2_a_war_declared_inside_a_truce_is_the_truce_broken():
    prev = _p("1092.01.06", 837.45, truces={ATHLONE_LORD: "1092.01.19"})
    snap = _p("1092.05.15", 475.58, truces={})
    ev = {"wars_started": [{"war_id": 218103844, "attacker_id": DOMNALL, "defender_id": ATHLONE_LORD,
                            "my_side": "attacker", "name": "Dyflinner Claim on the Earldom of Athlone"}]}
    out = _purse(prev, snap, ev)
    (row,) = [e for e in out["in_this_step"] if "truce" in e["what"]]
    assert row["expected"] == -250 and "1092.01.19" in row["what"]


# ── 2(b) and 3: the prices before paying, and the cost of losing beside the odds ─────────

def test_3_a_claim_war_says_each_ending_in_prestige():
    opt = {"cb": "claim_cb", "valid": True, "cb_title_id": 479, "scope": "county"}
    DG.annotate([opt], {"gold": 78.0, "prestige": 189.2, "piety": 719.6, "monthly_prestige": 0.96,
                        "armies": {"raised": False, "men": 0, "armies": 0}}, {"in_truce": False},
                title_keys={479: "c_oriel"})
    p = opt["prestige_if"]
    # a win gives prestige experience (0 prestige); a white peace and a defeat are -5 and -10 x the war's factor
    assert p["win"] == 0 and "-5 x" in p["white_peace"] and "-10 x" in p["defeat"]


def test_3_outnumbered_the_loss_and_the_months_until_the_next_declaration():
    """A recorded game, 1072.02.11: Oriel declared at 189.2 prestige for 125 against ~6,800 vs 551; lost; then
    eight years below every war's price."""
    pi = {"have": 189.2, "declare": -125.0, "defeat": -15.0, "monthly": 0.96}
    la = DG.loss_after(pi, "claim_cb")
    assert la["prestige_after_loss"] == 49.2 and la["months_to_afford_again"] == 79 and la["claim"] == "lost"
    assert DG.loss_after({"have": 59.49, "declare": "unread", "defeat": -150}, "ducal_conquest_cb") is None


def test_3_war_targets_rows_carry_his_strength_with_likely_allies():
    gw = _gw(_relations([]))
    gw.replies["query_valid_war_targets"] = {"targets": [{"target_char_id": ATHLONE_LORD, "cb_type": "claim",
                                                          "title_id": C_ATHLONE, "target_name": "Athlone"}]}
    gw.replies["query_alliances"] = lambda p: ({"own_military_strength": 656, "total_callable_strength": 656,
                                                "alliances": []} if p["character_id"] == ATHLONE_LORD else
                                               {"own_military_strength": 1259, "alliances": []})
    env = run(R.war_targets_impl(gw, actor_id=DOMNALL))
    (row,) = env["result"]["targets"]
    assert row["his_own"] == 656 and row["his_likely"] == 656 and row["verdict"] == "stronger_than_all_his_allies"
    assert env["result"]["your_strength"] == 1259


# ── 4: a ruled option the game does not offer ─────────────────────────────────────────────

POP = "faction_demand.1001"
POP_OPTS = ["FACTION_DEMAND_POPULIST_ACCEPT_CONVERT", "FACTION_DEMAND_POPULIST_ACCEPT_CONVERT_STATE_FAITH",
            "FACTION_DEMAND_POPULIST_ACCEPT_GRANT_INDEPENDENCE", "FACTION_DEMAND_POPULIST_REFUSE"]


def test_4_the_answer_is_named_the_ais_with_the_option_wanted():
    """A recorded game, 1091.07.16: answered REFUSE, by: event_rule, the rule wanting ACCEPT_CONVERT."""
    import faction_letters as FL
    pick = {"seq": 7, "date": "1091.07.16", "event": POP, "options": POP_OPTS,
            "ai_pick": {"index": 3, "key": POP_OPTS[3]}, "applied": {"index": 3, "key": POP_OPTS[3]},
            "rule": "option option=" + POP_OPTS[0], "why": "option_not_available",
            "wanted": {"index": 0, "key": POP_OPTS[0]}, "answered_by": "ai", "else": "ai"}
    ans = {"seq": 3, "date": "1091.07.16", "event": POP, "answered": POP_OPTS[3], "ai_would_have": POP_OPTS[3],
           "by": "ai", "refused": True, "options": POP_OPTS}
    snap = {"actor_id": DOMNALL, "factions_against": {}, "demands_answered": [ans], "event_picks": [pick]}
    (row,) = FL.news_part({"actor_id": DOMNALL, "factions_against": {}, "demands_answered": []},
                          snap)["faction_demands_answered"]
    assert row["by"] == "ai" and row["rule_wanted"] == POP_OPTS[0]
    assert "did not offer" in row["rule_why"]
    import seat_events as SE
    r = SE.pick_row(pick)
    assert r["rule_wanted"] == POP_OPTS[0] and r["applied_by"].startswith("the game's AI")


def test_4_the_demand_rows_say_which_options_are_offered():
    game_file("events/factions/faction_demands.txt")
    import faction_letters as FL
    off = {k: FL.option_offered(POP, k)["offered"] for k in POP_OPTS}
    assert off[POP_OPTS[3]] == "always" and off[POP_OPTS[0]] == "unknown"
    assert "fervor" in FL.option_offered(POP, POP_OPTS[0])["when"] or \
        FL.option_offered(POP, POP_OPTS[0])["when"].endswith("...")


def test_4_event_rule_takes_an_else_and_refuses_one_not_in_the_event():
    game_file("events/factions/faction_demands.txt")
    import seat_events as SE
    gw = Gw({"set_event_rule": None}, date="1091.04.09")
    sent = []

    async def write(action, params=None):
        sent.append((action, dict(params or {})))
        return {"success": True, "rules": []}
    gw.write = write
    gw.replies["query_event_rules"] = {"success": True, "seats": [{"character_id": DOMNALL, "rules": [
        {"event": POP, "answer": "option", "option": POP_OPTS[0], "else": POP_OPTS[2]}]}], "picks": []}
    env = run(SE.do_event_rule(gw, {"op": "set", "event": POP, "answer": "option", "option": POP_OPTS[0],
                                    "else": POP_OPTS[2]}, DOMNALL, {"id": DOMNALL}))
    assert sent and sent[-1][1]["else"] == POP_OPTS[2], sent
    assert env["result"]["event_check"]["option_offered"]["offered"] == "unknown"
    bad = run(SE.do_event_rule(gw, {"op": "set", "event": POP, "answer": "option", "option": POP_OPTS[0],
                                    "else": "NOT_AN_OPTION"}, DOMNALL, {"id": DOMNALL}))
    assert bad["blocked"]["raw_reason"] == "bad_else_option_key"


# ── 6: the succession before the death (a seated game, turn 31) ───────────────────────────────

def test_6_turn_31_names_the_son_who_takes_leinster_and_the_ready_calls():
    """A recorded game, turn 31 (1082.02.10): Murchad 56, Dublin + Leinster, three sons; the observation's
    succession block said only age 56 / near_trough false, and on 1082.08.21 confederate partition gave
    Leinster to Enna 37111. With the tree the forecast names him, and the news row fires."""
    import news as N
    import test_succession_study_fixes as T5
    ruler = dict(T5.RULER_A, age=56, health=4.0, prestige=-10.7, piety=1108.6)
    sim = {"primary_title_id": 463, "partition_heir_basis": "children_in_line",
           "partition_heirs": [36124, 37111, 37528], "line_not_partition_heirs": [],
           "heirs": [{"character_id": 36124, "name": "Domnall", "is_player_heir": True,
                      "titles": [T5.county(463, "c_dublin")]},
                     {"character_id": 37111, "name": "Enna", "is_player_heir": False,
                      "titles": [T5.county(508, "c_leinster")]},
                     {"character_id": 37528, "name": "Donnchad", "is_player_heir": False, "titles": []}]}
    kids = {36124: T5.kid(36124, "Domnall", age=36), 37111: T5.kid(37111, "Enna", age=30),
            37528: T5.kid(37528, "Donnchad", age=27)}
    gw = T5.Gw(ruler, kids, sim)
    out = T5.plan(gw)
    assert out["outcome"] == "splits" and out["breaker"]["id"] == 37111, out["breaker"]
    assert out["breaker"]["takes"] == [508]
    take_out = (out.get("levers") or {}).get("take_out_of_split")
    assert take_out, out.get("levers")
    calls = json.dumps(take_out)
    assert '"tool": "do"' in calls and "37111" in calls
    brief = R._forecast_brief(out["partition_forecast"]) if hasattr(R, "_forecast_brief") else None
    split = brief or {"takers": out["partition_forecast"]["takers"], "intact": False,
                      "leaves_realm": [37111]}
    snap = N.snapshot(actor_id=31506, date="1082.02.10", chr_sect={"age": 56, "health": 4.0}, wars={}, claims={},
                      council=None, heir=None, split=split, held={"held_titles": []})
    prev = N.snapshot(actor_id=31506, date="1081.02.10", chr_sect={"age": 55, "health": 4.1}, wars={}, claims={},
                      council=None, heir=None, split=split, held={"held_titles": []})
    rows = N.split_rows(prev, snap, {})
    assert 37111 in rows["heir_split_ahead"]["takers"] and rows["heir_split_ahead"]["next"]["tool"] == "succession_plan"


def test_1_live_every_truce_refusal_names_its_end():
    """A live read, 1067.12.03, after the white peace: war_targets, inspect title's next_call, plan_war's exclusion
    and declare_war said only "target_in_truce" (the game's own casus-belli check culls a truced target)."""
    culled = dict(ATHLONE_CBS_1095, cbs=[dict(ATHLONE_CBS_1095["cbs"][0], valid=False,
                                              requirements_unmet=["target_in_truce"])])
    gw = Gw(cbs_by_target(culled), date="1095.05.15")
    gw.replies["query_relations"] = _relations([ATHLONE_TRUCE])
    opts = run(R.declarable_now(gw, DOMNALL, [ATHLONE_LORD]))[ATHLONE_LORD]
    (o,) = opts
    blk = R.unmet_gate(o)
    assert blk["gate"] == "target_truced" and "1097.05.15" in blk["summary"], blk
    assert TR.facts("1097.05.15", "1095.05.15", "\u0015L; Dubhlinner\u0015!")["from_war"] == "Dubhlinner"


def test_2b_the_end_war_price_carries_the_prestige_live_measured():
    """A live read, 1067.12.03: white peace of a county claim war as attacker, prestige 287.89 -> 280.39."""
    p = R.peace_prices("claim_cb", "attacker", 2.3, ["white_peace", "we_win", "we_yield"], today="1067.12.03")
    # the white peace's -7.5 is -5 x that war's factor (1.5); the factor is the engine's for each war, so the rows
    # carry the rule, not a number
    assert "-5 x" in p["white_peace"]["prestige"] and p["we_win"]["prestige"] == 0
    assert "-10 x" in p["we_yield"]["prestige"]
    assert round(287.89 - 280.39, 2) == 7.5
    assert "prestige" not in R.peace_prices("claim_cb", "defender", 2.3, ["white_peace"])["white_peace"]


# ── 9: one title, one taker (a recorded game 1071.01.12) ─────────────────────────────────────────

def test_9_two_younger_sons_do_not_share_the_same_titles():
    """A recorded game, 1071.01.12: younger_shares Énna [469, 508] and Donnchad [469, 508]; the per-title map named
    Donnchad for both, the simulation Énna for both."""
    import partition_forecast as PF
    import test_succession_study_fixes as T5
    ruler = dict(T5.RULER_B, age=47)
    sim = {"primary_title_id": 463, "partition_heir_basis": "children_in_line",
           "partition_heirs": [36124, 37111, 37528], "line_not_partition_heirs": [],
           "heirs": [{"character_id": 36124, "is_player_heir": True, "titles": [T5.county(463, "c_dublin")]},
                     {"character_id": 37111, "is_player_heir": False,
                      "titles": [T5.county(469, "c_athlone"), T5.county(508, "c_leinster")]},
                     {"character_id": 37528, "is_player_heir": False, "titles": []}]}
    kids = [T5.kid(36124, "Domnall"), T5.kid(37111, "Enna"), T5.kid(37528, "Donnchad")]
    map_titles = [{"title_id": 463, "heir_id": 36124}, {"title_id": 469, "heir_id": 37528},
                  {"title_id": 508, "heir_id": 37528}]
    fc = PF.forecast_from_reads(ruler=ruler, law_names=["confederate_partition_succession_law", "male_preference_law"],
                                children=kids, sim=sim, primary_heir_id=36124, map_titles=map_titles)
    takes = {s["id"]: s["takes"] for s in fc["younger_shares"]}
    assert takes[37528] == [469, 508] and takes[37111] == [], takes
    assert fc["titles_disagree"]["titles"] == [469, 508]
    seen = [t for v in takes.values() for t in v]
    assert len(seen) == len(set(seen))


# ── 7: a refusal inside the casus belli's part, narrowed by the game's own probes ──────────

def test_7_the_recorded_refusal_named_nothing():
    """A recorded game, 1094.11.02 (one of 36): marks [] -> "none of those conditions failed"."""
    says = R.declare_game_says([])
    assert says["failed"] == [] and "none_failed" in says and "cb_part" not in says


def test_7_the_probes_name_the_part_that_refuses():
    assert R.declare_game_says(["PROBED", "PROBE_BARE_OK"])["cb_part"] == R.CB_PART_TITLE
    assert R.declare_game_says(["PROBED", "PROBE_BARE_OK", "PROBE_TITLE_OK"])["cb_part"] == R.CB_PART_CLAIMANT
    assert R.declare_game_says(["PROBED"])["cb_part"] == R.CB_PART_CB
    assert R.declare_game_says(["PROBED", "WHY_NO_CB_ON"])["failed"][0]["condition"] == "no_cb_on"


def test_7_cbs_rows_carry_the_last_refusal():
    R.LAST_REFUSAL[(ATHLONE_LORD, "claim_cb")] = {"date": "1094.11.02", "game_says": R.CB_PART_CLAIMANT}
    try:
        env = run(R.cbs_impl(_gw(_relations([])), target_id=ATHLONE_LORD, character_id=DOMNALL))
        (o,) = env["result"]["valid_options"]
        assert o["game_refused_last"]["game_says"] == R.CB_PART_CLAIMANT
    finally:
        R.LAST_REFUSAL.clear()


# ── 8: the ~1,000 drop of both runs, said before it comes ─────────────────────────────────

def test_8_culture_conversion_ahead_names_the_decision_and_its_price():
    game_file("common/culture/cultures/00_goidelic.txt")
    gw = Gw({"query_map_graph": {"text": "# NODES\n@c_dublin +d_meath/k_ireland ~catholic *norse !36124\n# EDGES\n"}})
    snap = {"actor_id": DOMNALL, "culture": "irish", "primary_title_key": "c_dublin", "prestige": 1212.96}
    row = run(news.culture_conversion_ahead(gw, snap))
    assert row["capital_culture"] == "norse" and row["price"] == -1000 and row["prestige"] == 1212.96
    assert run(news.culture_conversion_ahead(gw, snap)) is None           # once per ruler and culture
    news.reset()
    same = Gw({"query_map_graph": {"text": "# NODES\n@c_dublin +d_meath/k_ireland *irish\n"}})
    assert run(news.culture_conversion_ahead(same, snap)) is None
