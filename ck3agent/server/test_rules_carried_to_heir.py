"""Rows from the fourth seated Munster run, replayed from its recorded replies.

  1  the claim-letter rules died with Murchad (1072.07.16); vetoes carried    the rules carried as the vetoes are; news names both
  2a the AI's pilgrimage spent gold under no veto                             activity: a veto kind, in the spending preset
  2b the purse pinned activity gold and the seat's mint on unrelated events   reads in date order; own acts first; no-gold options
  3  no news when the forecast first had a son leave with d_ulster            split_leaves_realm, from the forecast every read
  4  plan_war dejure_gain 0 for c_breifne under toward=k_ireland              the goal's de jure counties, the metric's read
  5  disband by maa_type refused regiment_id_required                         the type's regiments found and disbanded
  6  the turn-one note did not name decisions (nor activities)                named

No game; the game's files are read where a row says so (skipped when they are not on this host).
"""
from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))

import activities as AC  # noqa: E402
import event_prices as EP  # noqa: E402
import news as N  # noqa: E402
import notes as NT  # noqa: E402
import redesign as R  # noqa: E402
import seat_events as SE  # noqa: E402

MURCHAD, BRIAN, ERNAN = 31762, 36303, 5613
FILES = SE.game_dir() is not None
need_files = pytest.mark.skipif(not FILES, reason="the game's files are not on this host")


def run(c):
    return asyncio.run(c)


LETTER_RULES = [
    {"event": "court_chaplain_task.0301", "answer": "pay_if", "option": "court_chaplain_task.0301.a",
     "currency": "gold", "amount": 91, "else": "ai"},
    {"event": "court_chaplain_task.0302", "answer": "pay_if", "option": "court_chaplain_task.0302.a",
     "currency": "gold", "amount": 91, "else": "court_chaplain_task.0301.b"},
]


class RulesGw:
    """query_event_rules / set_event_rule as the bridge answers them (seat_levers.cpp): one table per character."""

    def __init__(self, tables):
        self.tables = {k: [dict(r) for r in v] for k, v in tables.items()}
        self.writes = []

    def game(self):
        return {"date": "1072.07.16"}

    health = "ok"

    async def write(self, action, params=None):
        p = dict(params or {})
        self.writes.append((action, p))
        if action == "set_event_rule":
            rule = {k: v for k, v in p.items() if k not in ("character_id", "op")}
            t = self.tables.setdefault(p["character_id"], [])
            t[:] = [r for r in t if r["event"] != rule["event"]] + [rule]
            return {"success": True}
        return {"success": False, "reason": "unknown"}

    async def read(self, action, params=None):
        if action == "query_event_rules":
            cid = (params or {}).get("character_id")
            seats = [{"character_id": c, "rules": list(r)} for c, r in self.tables.items() if c == cid]
            return {"success": True, "seats": seats, "picks": []}
        return {}

    async def read_many(self, calls):
        return [await self.read(a, p) for a, p in calls]


# ── 1. event rules carried with the vetoes ──────────────────────────────────────────────────────────────────

def test_1_the_dead_rulers_rules_are_set_for_the_heir_and_read_back():
    """RED before: carry_event_rules did not exist; the step-12 ruler_changed row had ai_veto_carried only."""
    N.reset()
    gw = RulesGw({MURCHAD: LETTER_RULES, BRIAN: []})
    prev = {"actor_id": MURCHAD, "event_rule_events": ["court_chaplain_task.0301", "court_chaplain_task.0302"]}
    snap = {"actor_id": BRIAN}
    row = run(N.carry_event_rules(gw, 0, prev, snap))
    assert row["carried"] == ["court_chaplain_task.0301", "court_chaplain_task.0302"] and "not_carried" not in row
    sent = [p for a, p in gw.writes if a == "set_event_rule"]
    assert [p["event"] for p in sent] == ["court_chaplain_task.0301", "court_chaplain_task.0302"]
    assert sent[1]["else"] == "court_chaplain_task.0301.b" and sent[1]["amount"] == 91 and sent[1]["character_id"] == BRIAN
    assert snap["event_rule_events"] == ["court_chaplain_task.0301", "court_chaplain_task.0302"]


def test_1_rules_the_bridge_carried_at_the_handoff_are_read_not_sent_again():
    N.reset()
    gw = RulesGw({MURCHAD: LETTER_RULES, BRIAN: LETTER_RULES})
    prev = {"actor_id": MURCHAD, "event_rule_events": ["court_chaplain_task.0301"]}
    row = run(N.carry_event_rules(gw, 0, prev, {"actor_id": BRIAN}))
    assert row["carried"] == ["court_chaplain_task.0301", "court_chaplain_task.0302"] and not gw.writes


def test_1_CONTROL_no_rule_no_row_and_a_refused_set_is_named():
    N.reset()
    assert run(N.carry_event_rules(RulesGw({MURCHAD: [], BRIAN: []}), 0, {"actor_id": MURCHAD,
                                                                         "event_rule_events": []},
                                   {"actor_id": BRIAN})) is None

    class Refuses(RulesGw):
        async def write(self, action, params=None):
            self.writes.append((action, dict(params or {})))
            return {"success": False, "reason": "not_a_controlled_character"}
    N.reset()
    row = run(N.carry_event_rules(Refuses({MURCHAD: LETTER_RULES}), 0,
                                  {"actor_id": MURCHAD, "event_rule_events": ["court_chaplain_task.0301"]},
                                  {"actor_id": BRIAN}))
    assert row["carried"] == [] and row["not_carried"]["court_chaplain_task.0302"] == "not_a_controlled_character"


# ── 2a. activity: a veto kind in the spending preset ────────────────────────────────────────────────────────

class VetoGw:
    def __init__(self, vetoes=(), spares=()):
        self.vetoes, self.spares, self.writes = set(vetoes), list(spares), []

    def game(self):
        return {"date": "1075.10.11"}

    health = "ok"

    async def write(self, action, params=None):
        p = dict(params or {})
        self.writes.append((action, p))
        if action == "set_ai_veto":
            (self.vetoes.add if p["op"] == "set" else self.vetoes.discard)(p["what"])
            if p["what"] == "activity":
                self.spares = [k for k in str(p.get("spare_keys") or "").split(",") if k]
        return {"success": True}

    async def read(self, action, params=None):
        if action == "query_ai_veto":
            return {"success": True, "rulers": [{"character_id": BRIAN, "vetoes": sorted(self.vetoes),
                                                 "decision_keys": [], "decision_gold_cost": True,
                                                 "activity_spare_keys": list(self.spares)}]}
        return {}


def test_2a_the_preset_sets_activity_and_keeps_the_seats_own_spared_type():
    gw = VetoGw(spares=["activity_feast"])
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "spending"}, BRIAN, {"id": BRIAN}))
    assert env["ok"] and "activity" in env["result"]["kinds"] and "activity" in env["result"]["vetoes"]
    act = [p for a, p in gw.writes if a == "set_ai_veto" and p["what"] == "activity"]
    assert act and act[0]["spare_keys"] == "activity_feast"
    assert "activity" in SE.AI_VETO_WHATS and "activity" not in SE.AI_VETO_LEFT_OUT


def test_2a_host_activity_under_the_veto_spares_its_type():
    gw = VetoGw(vetoes=["activity"])
    got = run(AC._spare_under_veto(gw, BRIAN, "activity_pilgrimage"))
    assert got["spared"] == ["activity_pilgrimage"] and "lets activity_pilgrimage through" in got["note"]
    assert run(AC._spare_under_veto(VetoGw(), BRIAN, "activity_feast")) is None       # CONTROL: no veto, nothing set


def test_2a_a_dropped_activity_reads_with_its_call_and_a_passed_one_with_the_veto():
    rec = {"seq": 1, "date": "1075.10.20", "character_id": BRIAN, "what": "activity", "dropped": True,
           "key": "activity_pilgrimage", "gold": 222.51, "prestige": 10.0, "priced": "no",
           "unpriced_why": "activity_cost_by_purse", "origin": "game"}
    out = SE.veto_news({"ai_veto_records": []}, {"actor_id": BRIAN, "ai_veto_records": [rec], "ai_vetoes": ["activity"]})
    row = out["ai_vetoed"][0]
    assert row["what"] == "activity" and row["key"] == "activity_pilgrimage" and row["gold_at_attempt"] == 222.51
    assert row["next"]["args"] == {"intent": "host_activity", "args": {"activity": "pilgrimage"}}
    out = SE.veto_news({"ai_veto_records": []}, {"actor_id": BRIAN, "ai_veto_records": [dict(rec, dropped=False,
                                                                                         key="activity_tour")],
                                                 "ai_vetoes": []})
    a = out["ai_acted"][0]
    assert a["veto"]["args"]["args"]["what"] == "activity" and "next" not in a
    out = SE.veto_news({"ai_veto_records": []}, {"actor_id": BRIAN, "ai_veto_records": [dict(rec, dropped=False)],
                                                 "ai_vetoes": ["activity"]})
    a = out["ai_acted"][0]                       # live act 1075.01.04: passed under the veto = spared, no veto offered
    assert "lets activity_pilgrimage through" in a["sent_by"] and "veto" not in a


# ── 2b. the purse: reads in date order, the seat's own acts first, no fall on a no-gold option ───────────────

def _no_files(monkeypatch, answer=None):
    monkeypatch.setattr(EP, "option_gold", lambda *a: answer)


def test_2b_the_seats_own_mint_is_not_read_into_an_event(monkeypatch):
    """Step 45 (1096.01.22 -> 1096.02.16): coming_of_age.1002 answered by the AI at 426.0 gold, the mint (-425,
    your_acts) after it, gold 4.0 at the read: the run read "expected -424.43" on the event, "unexplained +423.53"."""
    _no_files(monkeypatch)               # even when the files cannot say, the mint comes out of the window
    N.reset()
    N.note_act(BRIAN, "1096.02.16", "mint_title k_ireland", cost_known=True, gold=-425.0)
    prev, snap = {"date": "1096.01.22", "monthly_income": 5.0, "actor_id": BRIAN}, \
        {"date": "1096.02.16", "gold": 4.0, "actor_id": BRIAN}
    ans = [{"event": "coming_of_age.1002", "applied": "coming_of_age.1001.a", "date": "1096.01.25", "gold_at_pick": 426.0}]
    old = N.ai_pick_gold(prev, snap, ans)
    assert old and old[0]["expected"] < -420                       # the base's reading, without the own act
    assert N.ai_pick_gold(prev, snap, ans, N._priced_in_step(prev, snap, {})) == []


def test_2b_an_activity_post_is_its_own_read_and_takes_its_fall(monkeypatch):
    """Step 19: gold 222.51 at the AI's pilgrimage post (1075.10.11), 45.18 at the read (1076.02.18), the guardianship
    letter char_interaction.0080 answered EXCELLENT after it at 40.0 (1076.01.05)."""
    _no_files(monkeypatch, False)        # EXCELLENT moves no gold (the file: an empty option)
    N.reset()
    prev, snap = {"date": "1075.10.11", "monthly_income": 1.0, "actor_id": BRIAN}, \
        {"date": "1076.02.18", "gold": 45.18, "actor_id": BRIAN}
    ans = [{"event": "char_interaction.0080", "applied": "EXCELLENT", "date": "1076.01.05", "gold_at_pick": 40.0}]
    acted = [{"what": "activity", "key": "activity_pilgrimage", "date": "1075.10.11", "gold_at_attempt": 222.51}]
    rows = N.ai_pick_gold(prev, snap, ans, [], acted)
    assert len(rows) == 1 and "activity activity_pilgrimage" in rows[0]["what"]
    assert -190 < rows[0]["expected"] < -180 and rows[0]["lever"]["args"]["args"]["what"] == "activity"


def test_2b_an_event_whose_option_moves_no_gold_is_given_no_fall(monkeypatch):
    prev, snap = {"date": "1085.06.01", "monthly_income": 3.0}, {"date": "1085.09.01", "gold": -4.0}
    ans = [{"event": "birth.3002", "applied": "birth.3002.a", "date": "1085.06.10", "gold_at_pick": 121.0}]
    _no_files(monkeypatch, False)
    assert N.ai_pick_gold(prev, snap, ans) == []
    _no_files(monkeypatch, True)                                   # CONTROL: an option that moves gold keeps its row
    assert N.ai_pick_gold(prev, snap, ans)[0]["expected"] < -100
    _no_files(monkeypatch, None)                                   # CONTROL: files silent: as before
    assert N.ai_pick_gold(prev, snap, ans)


@need_files
def test_2b_the_game_files_say_which_options_move_gold():
    assert EP.option_gold("char_interaction.0080", "EXCELLENT") is False
    assert EP.option_gold("birth.3002", "birth.3002.a") is False
    assert EP.option_gold("court_chaplain_task.0301", "court_chaplain_task.0301.a") is True   # its file's own effect
    assert EP.option_gold("court_chaplain_task.0301", "court_chaplain_task.0301.b") is False
    assert EP.option_gold("no_such_event.1", "x") is None


# ── 3. the partition forecast's leaving sons, in the news when first true ───────────────────────────────────

def _fc_leaving():
    return {"partition": True, "takers": [BRIAN, ERNAN, 67308], "intact": False,
            "younger_shares": [{"id": ERNAN, "name": "Ernán", "fate": "leaves_realm",
                                "takes_title": {"title": "d_ulster", "title_id": 480, "created": True,
                                                "counties": {"in_your_realm": 2, "of": 3}}, "takes": ["d_ulster"]},
                               {"id": 67308, "fate": "stays_vassal", "takes": [524]}]}


def test_3_the_brief_names_what_a_leaving_son_takes():
    b = R._forecast_brief(_fc_leaving())
    assert b["leaves_realm"] == [ERNAN]
    assert b["leaves_with"] == [{"id": ERNAN, "name": "Ernán", "takes": "d_ulster", "created_by_the_law": True,
                                 "counties": {"in_your_realm": 2, "of": 3}}]
    stays = _fc_leaving()
    stays["younger_shares"][0].update(fate="stays_vassal")
    assert "leaves_with" not in R._forecast_brief(stays)


def test_3_the_news_says_it_when_it_first_becomes_true_and_not_again():
    """A recorded game: the forecast at turns 3-7 had Ernán stay a vassal with c_ennis; once the realm met d_ulster's rule
    (Ulster joined 1071.03.20) the law would create d_ulster for him: the first read that says so names it."""
    before = {"split": {"takers": [BRIAN, ERNAN, 67308], "intact": False, "leaves_realm": []}, "age": 45, "health": 5}
    now = {"split": R._forecast_brief(_fc_leaving()), "age": 46, "health": 5}
    rows = N.split_rows(before, now, {})
    assert rows["split_leaves_realm"]["at_your_death"][0]["takes"] == "d_ulster"
    assert "split_leaves_realm" not in N.split_rows(now, dict(now), {})           # the same: once
    other = R._forecast_brief(_fc_leaving())
    other["leaves_with"][0]["takes"] = "d_connacht"
    assert "split_leaves_realm" in N.split_rows(now, {"split": other}, {})      # a new title: again


# ── 4. plan_war: the goal's de jure counties a war takes ────────────────────────────────────────────────────

class WarGw:
    def __init__(self):
        self.reads = []

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_dejure_progress" and (params or {}).get("title_key") == "k_ireland":
            return {"held": 9, "total": 14, "counties": [
                {"county_id": 501, "you_control": False}, {"county_id": 474, "you_control": True},
                {"county_id": 513, "you_control": False}]}
        if action == "query_title_hierarchy":
            key = {501: "c_breifne", 470: "d_leinster"}.get((params or {}).get("title_id"), "")
            return {"title": {"key": key}}
        if action == "query_dejure_progress" and (params or {}).get("title_id") == 470:
            return {"counties": [{"county_id": 513, "you_control": True}, {"county_id": 900, "you_control": True},
                                 {"county_id": 474, "you_control": False}]}
        return {}


def test_4_a_claim_war_on_a_de_jure_county_of_the_goal_gains_it():
    """Turn 22: claim_cb on c_breifne (501), de jure k_ireland, read dejure_gain 0."""
    gw = WarGw()
    m = run(R.goal_dejure_map(gw, BRIAN, "k_ireland"))
    assert m == {501: False, 474: True, 513: False}
    ids = run(R.war_county_ids(gw, 34580, 501, {}))
    assert ids == [501] and R.dejure_gain_of(ids, m) == 1
    duchy = run(R.war_county_ids(gw, 40000, 470, {}))              # the target's counties under the duchy
    assert sorted(duchy) == [513, 900] and R.dejure_gain_of(duchy, m) == 1
    assert R.dejure_gain_of([474], m) == 0                         # CONTROL: already in your realm
    assert run(R.war_county_ids(gw, 1, 0, {})) is None             # CONTROL: a war for no title


# ── 5. disband by type ──────────────────────────────────────────────────────────────────────────────────────

class MaaGw:
    def __init__(self):
        self.writes = []

    def game(self):
        return {"date": "1090.03.03"}

    health = "ok"

    async def read(self, action, params=None):
        if action == "query_levy_readiness":
            return {"regiments": [{"regiment_id": 17770, "type": "light_horsemen"},
                                  {"regiment_id": 17769, "type": "light_horsemen"},
                                  {"regiment_id": 17700, "type": "pikemen_unit"}, {"regiment_id": 1, "type": "levy"}]}
        return {}

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        if action == "maa_disband":
            return {"success": True, "applied": not params.get("check"), "check": bool(params.get("check")),
                    "regiment_after": "absent", "regiment_id": params["regiment_id"]}
        return {}


def test_5_a_type_disbands_each_of_its_regiments():
    gw = MaaGw()
    env = run(R._do_maa_disband(gw, {"actor_id": BRIAN, "regiment_id": 0, "maa_type": "light_horsemen", "check": False},
                                {"id": BRIAN}))
    assert env["ok"] and env["verdict"] == "applied"
    assert [p["regiment_id"] for a, p in gw.writes] == [17770, 17769]
    assert all("maa_type" not in p for a, p in gw.writes)
    env = run(R._do_maa_disband(MaaGw(), {"actor_id": BRIAN, "regiment_id": 0, "maa_type": "archers", "check": True},
                                {"id": BRIAN}))
    assert not env["ok"] and env["blocked"]["raw_reason"] == "no_regiment_of_type"
    assert "light_horsemen, pikemen_unit" in env["blocked"]["summary"]


def test_5_the_type_reaches_the_toolkit_through_resolve_act():
    import tools as T
    act, params = T.resolve_act({"type": "maa", "action": "disband", "maa_type": "light_horsemen",
                                 "check": True, "actor_id": BRIAN})
    assert act == "maa_disband" and params["maa_type"] == "light_horsemen" and params["check"] is True


# ── 6. the turn-one note names decisions and activities, within the caps ────────────────────────────────────

def test_6_the_turn_one_note_names_what_the_preset_now_drops():
    doc = NT.load()
    t = next(n["text"] for n in doc["notes"] if n["id"] == "ai-spends-for-you")
    assert "activities" in t and "gold/income decisions" in t and len(t.encode()) <= 240
    assert len(json.dumps(NT.turn_one_texts(doc), ensure_ascii=False).encode()) <= 1700
    assert NT.problems(doc) == []
