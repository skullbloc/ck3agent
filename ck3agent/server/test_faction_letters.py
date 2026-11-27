"""A faction's demand to the seat's ruler, before and after it arrives
(faction_letters.py, the seat_events / news wiring).

The case: a test run, 1079.11.13. A populist faction's
letter `faction_demand.1001` reached Murchad; the game's AI picked
FACTION_DEMAND_POPULIST_ACCEPT_GRANT_INDEPENDENCE and the faction's leader took every title.
The pick row's call named the first option (an acceptance), the refuse option read "" (a dynamic
name), and no read warned while the faction grew. The game-file fixture below copies the shapes
of the shipped files (common/factions/00_populist_faction.txt, 00_factions.txt,
events/factions/faction_demands.txt): option names, the dynamic refuse name, a member notice
fired from the same demand block.
"""
from __future__ import annotations

import asyncio

import pytest

import faction_letters as FL
import news
import seat_events as SE
from test_seat_events import TGw, Table, actor, pick, run  # noqa: F401
from test_ireland_walk_replies import Gw

ME = 31506
POP = ["FACTION_DEMAND_POPULIST_ACCEPT_CONVERT", "FACTION_DEMAND_POPULIST_ACCEPT_CONVERT_STATE_FAITH",
       "FACTION_DEMAND_POPULIST_ACCEPT_GRANT_INDEPENDENCE", "FACTION_DEMAND_POPULIST_REFUSE"]

FACTIONS = """﻿independence_faction = {
\tcasus_belli = independence_faction_war
\tdemand = {
\t\tevery_faction_member = { limit = { is_ai = no } trigger_event = faction_demand.0005 }
\t\tfaction_target = { trigger_event = { id = faction_demand.0001 days = 5 } }
\t}
}
"""
POPULIST = """populist_faction = {
\tdemand = {
\t\tsetup_populist_leader_effect = yes
\t\tscope:faction.faction_target = {
\t\t\ttrigger_event = faction_demand.1001
\t\t}
\t}
}
"""
EVENTS = """namespace = faction_demand
faction_demand.0001 = {
\ttype = letter_event
\toption = { name = "FACTION_DEMAND_ACCEPT" }
\toption = { name = "FACTION_DEMAND_BRIBE_CO_EMPEROR" }
\toption = { name = "FACTION_DEMAND_REFUSE" scope:faction = { faction_start_war = {} } }
}
faction_demand.0005 = {
\ttype = letter_event
\toption = { name = "FACTION_DEMAND_SEND_DEMAND_NOTIFICATION_OPT" }
}
faction_demand.1001 = {
\ttype = letter_event
\t# Option A: Convert
\toption = { name = "FACTION_DEMAND_POPULIST_ACCEPT_CONVERT" ai_chance = { base = 50 } }
\toption = { name = FACTION_DEMAND_POPULIST_ACCEPT_CONVERT_STATE_FAITH }
\toption = { name = "FACTION_DEMAND_POPULIST_ACCEPT_GRANT_INDEPENDENCE" }
\t# Option C: Refuse the demand and start a war.
\toption = {
\t\tname = {
\t\t\ttext = {
\t\t\t\tfirst_valid = {
\t\t\t\t\ttriggered_desc = { trigger = { government_allows = state_faith } desc = FACTION_DEMAND_POPULIST_REFUSE_ADMIN }
\t\t\t\t\tdesc = FACTION_DEMAND_POPULIST_REFUSE
\t\t\t\t}
\t\t\t}
\t\t}
\t\tscope:faction = { faction_start_war = { title = scope:target_title } }
\t}
}
"""


@pytest.fixture
def game(tmp_path, monkeypatch):
    (tmp_path / "common" / "factions").mkdir(parents=True)
    (tmp_path / "events" / "factions").mkdir(parents=True)
    (tmp_path / "common" / "factions" / "00_factions.txt").write_text(FACTIONS, encoding="utf-8")
    (tmp_path / "common" / "factions" / "00_populist_faction.txt").write_text(POPULIST)
    (tmp_path / "events" / "factions" / "faction_demands.txt").write_text(EVENTS)
    monkeypatch.setenv("CK3_GAME_DIR", str(tmp_path))
    for c in (SE._EVENT_CACHE, FL._FILES, FL._LETTERS):
        c.clear()
    yield tmp_path
    for c in (SE._EVENT_CACHE, FL._FILES, FL._LETTERS):
        c.clear()


# ── the game's files ──────────────────────────────────────────────────────────
def test_each_type_maps_to_its_letter_not_the_members_notice(game):
    lmap, why = FL.letters()
    assert why == "" and lmap == {"independence_faction": "faction_demand.0001",
                                  "populist_faction": "faction_demand.1001"}


def test_the_populist_letters_dynamic_refuse_name_reads_its_fallback(game):
    opts, why = FL.letter_options("faction_demand.1001")
    assert why == "" and [o["key"] for o in opts] == POP
    assert [o["index"] for o in opts if o["refuses"]] == [3]


def test_unreadable_files_say_why_and_name_no_letter(monkeypatch, tmp_path):
    monkeypatch.setenv("CK3_GAME_DIR", str(tmp_path / "nothing"))
    monkeypatch.setattr(SE.Path, "home", lambda: tmp_path)
    FL._LETTERS.clear()
    SE._EVENT_CACHE.clear()
    lmap, why = FL.letters()
    assert lmap is None and "not readable" in why
    FL._LETTERS.clear()


# ── the pick row and event_rule ───────────────────────────────────────────────
def recorded_pick():
    """The pick a test run's news row carried on 1079.11.13 (the refuse name read "" then)."""
    return {"seq": 9, "date": "1079.11.13", "character_id": ME, "event": "faction_demand.1001",
            "options": POP, "ai_pick": {"index": 2, "key": POP[2]}, "applied": {"index": 2, "key": POP[2]},
            "rule": None, "gold": -157.97}


def test_a_demand_pick_names_its_refuse_option_not_the_first():
    row = SE.pick_row(recorded_pick())
    assert row["next"]["args"]["args"]["option"] == "FACTION_DEMAND_POPULIST_REFUSE"
    assert "war" in row["refusing"]


def test_a_two_option_pick_still_names_the_first():
    row = SE.pick_row(pick(1, 0, 0))
    assert row["next"]["args"]["args"]["option"] == "court_chaplain_task.0301.a" and "refusing" not in row


def test_decline_on_the_four_option_letter_is_refused_with_the_refuse_option(game):
    gw = TGw(Table())
    env = run(SE.do_event_rule(gw, {"event": "faction_demand.1001", "answer": "decline"}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["raw_reason"] == "needs_two_options"
    assert "FACTION_DEMAND_POPULIST_REFUSE" in env["blocked"]["summary"] and gw.writes == []


def test_the_refuse_option_by_key_is_set_before_the_event_exists(game):
    gw = TGw(Table())
    env = run(SE.do_event_rule(gw, {"event": "faction_demand.1001", "answer": "option",
                                    "option": "FACTION_DEMAND_POPULIST_REFUSE"}, 0, actor()))
    assert env["verdict"] == "applied"
    assert gw.writes == [("set_event_rule", {"character_id": ME, "op": "set", "event": "faction_demand.1001",
                                             "answer": "option", "option": "FACTION_DEMAND_POPULIST_REFUSE"})]


# ── news ──────────────────────────────────────────────────────────────────────
def threat(factions, answered=(), response="ai"):
    return {"success": True, "character_id": ME, "factions": list(factions), "faction_count": len(factions),
            "demand_response": response, "demands_answered": list(answered)}


def fac(fid=7, level="medium", discontent=40.0, power=60.0, **kw):
    # The default was 120 % (stronger than the ruler), which is now graded
    # high and repeated every read; a quiet faction is one under the power threshold
    return dict({"faction_id": fid, "type": "populist_faction", "leader_id": 55606, "discontent": discontent,
                 "power_ratio": power, "threat_level": level, "ultimatum": "none", "ai_press_chance_pct": 0.0,
                 "at_war": False, "county_count": 5}, **kw)


def answered(seq, applied, by="ai", refused=False):
    return {"seq": seq, "date": "1079.11.13", "event": "faction_demand.1001", "options": POP,
            "answered": POP[applied], "ai_would_have": POP[2], "refuse_option": POP[3], "refused": refused,
            "by": by, "overridden": applied != 2}


def snap(t, rules=None, actor_id=ME):
    ev = {"success": True, "seats": [{"character_id": ME, "rules": rules or []}], "picks": []}
    return {"actor_id": actor_id, **SE.snapshot_part(actor_id, ev, None), **FL.snapshot_part(t)}


def test_a_new_faction_against_the_ruler_is_news_with_its_letter_and_the_refuse_call(game):
    out = FL.news_part(snap(threat([])), snap(threat([fac()])))
    (row,) = out["factions_against_you"]
    assert row["shown_because"] == "new" and row["power_pct"] == 60.0
    assert row["demand"]["event"] == "faction_demand.1001"
    assert [o["key"] for o in row["demand"]["options"]] == POP and row["demand"]["options"][3]["refuses"]
    assert row["answered_now_by"].startswith("the game's AI")
    assert row["next"]["args"] == {"intent": "event_rule", "args": {
        "op": "set", "event": "faction_demand.1001", "answer": "option", "option": "FACTION_DEMAND_POPULIST_REFUSE"}}


def test_a_quiet_faction_is_not_repeated_a_critical_one_is(game):
    s1 = snap(threat([fac()]))
    assert FL.news_part(s1, s1) == {}
    s2 = snap(threat([fac(level="critical", ultimatum="ai_may_press", ai_press_chance_pct=35.0)]))
    (row,) = FL.news_part(s1, s2)["factions_against_you"]
    assert row["shown_because"] == "threat medium -> critical" and row["ai_press_chance_pct"] == 35.0
    (again,) = FL.news_part(s2, s2)["factions_against_you"]
    assert again["shown_because"] == "threat critical"


def test_a_rule_for_the_letter_answers_it_and_no_call_is_offered(game):
    rule = [{"event": "faction_demand.1001", "answer": "option", "option": "FACTION_DEMAND_POPULIST_REFUSE"}]
    (row,) = FL.news_part(snap(threat([])), snap(threat([fac()]), rules=rule))["factions_against_you"]
    assert row["answered_now_by"] == "your event_rule for faction_demand.1001" and "next" not in row


def test_the_standing_refuse_answers_it(game):
    (row,) = FL.news_part(snap(threat([])), snap(threat([fac()], response="refuse")))["factions_against_you"]
    assert row["answered_now_by"].startswith("your standing answer: refuse") and "next" not in row


def test_a_demand_answered_by_the_ai_is_news_once_with_the_call_for_next_time(game):
    s1 = snap(threat([fac()]))
    s2 = snap(threat([], answered=[answered(1, 2)]))
    rows = FL.news_part(s1, s2)["faction_demands_answered"]
    assert rows[0]["answered"] == POP[2] and rows[0]["by"] == "ai" and rows[0]["then"].startswith("granted")
    assert rows[0]["next"]["args"]["args"]["option"] == "FACTION_DEMAND_POPULIST_REFUSE"
    assert "faction_demands_answered" not in FL.news_part(s2, s2)


def test_a_refused_demand_says_the_war_follows(game):
    rows = FL.news_part(snap(threat([])), snap(threat([], answered=[answered(1, 3, by="event_rule", refused=True)])))
    (r,) = rows["faction_demands_answered"]
    assert r["refused"] is True and "declared its war" in r["then"] and "next" not in r
    # run f2: a war that starts and ends between two reads is in no wars_started row; the row does
    # not send the seat to one
    assert "wars_started lists it" not in r["then"]


def test_an_unread_threat_is_unknown_not_none(game):
    s1 = snap(threat([fac()]))
    s2 = snap(None)
    assert "factions_unread" in FL.news_part(s1, s2)


def test_the_news_block_carries_the_faction_rows(game):
    news.reset()
    base = dict(actor_id=ME, chr_sect={"gold": -157.0}, wars={}, claims={}, council=None, heir=None)
    gw = Gw()
    run(news.block(gw, 0, news.snapshot(date="1079.05.13", faction_threat=threat([]), **base), offers_served=False))
    out = run(news.block(gw, 0, news.snapshot(date="1079.07.13", faction_threat=threat([fac(level="high")]), **base),
                         offers_served=False))
    assert out["factions_against_you"][0]["faction_id"] == 7
    out = run(news.block(gw, 0, news.snapshot(date="1079.11.16", faction_threat=threat([], answered=[answered(1, 2)]),
                                              **base), offers_served=False))
    assert out["faction_demands_answered"][0]["event"] == "faction_demand.1001"
    news.reset()
