"""The fixes from one seated 1066 Munster game (the kingdom-of-Ireland task, k_ireland made at step 38).

  1  the GOAL line's death clause was 1,262-2,049 B (two or three sons with their county lists, five to seven levers
     each with its full call)                                   -> a 700-byte cap, titles first, then the price, then
                                                                    at most three levers allowed now, then a pointer
  2  vows 65888 read "allowed now" and he declined (the bridge's check had scored it maybe, 25)
                                                                -> "he decides", with the check's own pre-answer
  3  b_slemish (step 21) and b_armagh (step 26) gained "how unknown", a step after their county's war
                                                                -> the county's recorded cause, the source named
  5  the turns after a death: the rest of the news went whole to page 2, unread
                                                                -> laid by its rows, the goal's first

The values are the run's own where a test says so (its session_stdin GOAL lines, the bridge log's CHECK line). No game.
"""
from __future__ import annotations

import asyncio
import json

import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT))

import news as N  # noqa: E402
import partition_forecast as PF  # noqa: E402
import redesign as R  # noqa: E402
import reply_fit as F  # noqa: E402

MURCHAD = 31762
DO = "mcp__ck3__do"


def run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _clean(monkeypatch):
    monkeypatch.delenv(F.PAGES_ENV, raising=False)
    monkeypatch.delenv("CK3_GOAL_KIND", raising=False)
    monkeypatch.setenv("CK3_GOAL_TITLE", "k_ireland")
    F.reset_pages()
    N.reset()
    yield
    F.reset_pages()
    N.reset()


# ── 1 + 2: the death clause ─────────────────────────────────────────────────────────────────────────────────────

def _call(interaction, son):
    return {"tool": DO, "args": {"intent": "interact", "args": {"interaction": interaction, "target_id": son}}}


def _lever(lever, son, ok, why=None, **kw):
    it = {"disinherit": "disinherit_interaction", "vows": "take_vows_interaction"}[lever]
    return {"lever": lever, "son": son, "allowed_now": ok, **({"why_not": [why]} if why else {}),
            **({"he_decides": True} if lever == "vows" else {}), "call": _call(it, son), **kw}


def _t35(vows_answer=True):
    """Turn 35 (1079.10.06) as the run's GOAL line recorded it, with the bridge log's check of vows 65888
    (CHECK answer=1 accept_score=2500000: maybe, 25)."""
    vows = dict(answer="maybe", accept_score=25.0, chance_pct=25.0) if vows_answer else {}
    return {"title_key": "k_ireland",
            "price_basis": {"at_succession": {"price": 425.0, "rises": 100.0,
                                              "why": "your own ducal_conquest_perk discount dies with you"}},
            "at_death": {
                "ruler": {"age": 52, "health": 3.37, "act_now": True, "dies_within_years": {"p25": 3.6}},
                "leaves_realm": [
                    {"to": 64594, "name": "Donnchad", "takes": "d_leinster"},
                    {"to": 65888, "name": "Petair", "takes": "d_meath", "created_by_the_law": True,
                     "counties": {"in_your_realm": 2, "of": 2}, "county_keys": ["c_dublin", "c_athlone"]},
                    {"to": 72033, "name": "Máel-Bresail", "takes": "d_ulster", "created_by_the_law": True,
                     "counties": {"in_your_realm": 3, "of": 3}, "county_keys": ["c_ulster", "c_oriel", "c_ailech"]}],
                "levers": [
                    _lever("disinherit", 64594, False, "you cannot pay its price: Missing 55; Missing 50"),
                    _lever("disinherit", 65888, False, "you cannot pay its price: Missing 55; Missing 50"),
                    _lever("disinherit", 72033, True),
                    _lever("vows", 64594, False, "he would refuse: ai_accept -620.00"),
                    _lever("vows", 65888, True, **vows),
                    _lever("vows", 72033, False, "the game does not allow it: younger than 10"),
                    {"lever": "law", "law": "partition_succession_law", "allowed_now": False,
                     "why_not": ["your culture's innovation_hereditary_rule"],
                     "call": {"tool": DO, "args": {"intent": "change_law",
                                                   "args": {"law": "partition_succession_law"}}}}]}}


# ── 2: the check's pre-answer reaches the lever rows ────────────────────────────────────────────────────────────

def test_2_RED_the_game_check_keeps_the_answer_and_its_maybe_chance():
    gc = PF._game_check({"sendable": True, "answer": "maybe", "accept_score": 25.0,
                         "engine_cost": {"piety": 350.0}}, {"piety": 806})
    assert gc["answer"] == "maybe" and gc["chance_pct"] == 25.0 and gc["sendable"] is True
    yes = PF._game_check({"sendable": True, "answer": "yes", "accept_score": 140.0}, {})
    assert yes["answer"] == "yes" and "chance_pct" not in yes


# ── 3: a barony gained a step after its county ──────────────────────────────────────────────────────────────────

def _held(*rows):
    return {"held_titles": [{"title_id": i, "key": k, "de_jure_parent_key": p} for i, k, p in rows]}


def test_3_RED_a_barony_a_step_after_its_county_is_attributed_to_the_countys_cause():
    """Step 20 (1073.10.26): c_ulster gained by war 67108907; step 21 (1074.02.23): b_slemish, "how unknown"."""
    import test_ai_veto as TV
    hist = {474: [{"date": "1073.10.20", "event_type_raw": 0, "char_id": TV.ME}]}
    gw = TV.Gw({"query_title_history": lambda p: {"entries": hist.get(p.get("title_id"), [])}})
    s1 = TV.snap("1073.06.01", [], held=_held((463, "c_dublin", "d_meath")))
    s2 = TV.snap("1073.10.26", [], held=_held((463, "c_dublin", "d_meath"), (474, "c_ulster", "d_ulster")))
    s3 = TV.snap("1074.02.23", [], held=_held((463, "c_dublin", "d_meath"), (474, "c_ulster", "d_ulster"),
                                               (476, "b_slemish", "c_ulster")))
    run(N.block(gw, 0, s1, offers_served=False))
    (c,) = run(N.block(gw, 0, s2, offers_served=False))["titles_gained"]
    assert c["key"] == "c_ulster" and c["how"].startswith("conquest: to you on 1073.10.20")
    (b,) = run(N.block(gw, 0, s3, offers_served=False))["titles_gained"]
    assert b["key"] == "b_slemish" and b["inside_county"] == "c_ulster"
    assert b["how"] == f"inside c_ulster, which you gained on or before 1073.10.26: {c['how']}"
    assert b["barony_source"].startswith("the county's recorded cause") and "title history did not name you" in \
        b["barony_source"]


def test_3_a_barony_in_a_county_with_no_recorded_cause_stays_unknown_and_names_the_county():
    import test_ai_veto as TV
    gw = TV.Gw({"query_title_history": {"entries": []}})
    s1 = TV.snap("1074.01.01", [], held=_held((474, "c_ulster", "d_ulster")))
    s2 = TV.snap("1074.02.23", [], held=_held((474, "c_ulster", "d_ulster"), (476, "b_slemish", "c_ulster")))
    run(N.block(gw, 0, s1, offers_served=False))
    (b,) = run(N.block(gw, 0, s2, offers_served=False))["titles_gained"]
    assert b["how"] == "unknown" and b["how_reason"] == "no_record_in_step" and b["inside_county"] == "c_ulster"


# ── 5: the news after a death ───────────────────────────────────────────────────────────────────────────────────

def _death_news():
    """Turn 45's shape: the kept rows (ruler_changed ~2.2 KB, titles_lost ~1.8 KB, dynasty_head_changed) and the rest."""
    return {"ok": True, "tool": "observe", "game": {"date": "1085.02.13"}, "result": {"news": {
        "events_answered": [{"event": f"e.{i}", "applied": "x" * 300} for i in range(12)],
        "council_changed": [{"seat": f"s{i}", "pad": "c" * 200} for i in range(6)],
        "purse": {"gold": {"now": -40.0, "pad": "p" * 900}},
        "attackers_new": [{"war_id": 9, "attacker_id": 64594, "claimant": "k_ireland"}],
        "factions_against_you": [{"faction": "claimant_faction", "power": 85}],
        "titles_lost": [{"key": f"c_{i}", "how": "partition: " + "t" * 150} for i in range(10)],
        "ruler_changed": {"from": 31762, "to": 36303, "pad": "r" * 2100},
        "dynasty_head_changed": {"from": 31762, "to": 36303}}}}


def test_5_RED_after_a_death_the_threat_rows_are_on_page_one():
    out = F.paginate(json.loads(json.dumps(_death_news())), "observe", {"depth": "news"}, cap=F.CAP)
    news = out["result"]["news"]
    later = [w for ws in out["result"]["more"]["on_later_pages"].values() for w in ws]
    assert {"ruler_changed", "titles_lost", "dynasty_head_changed"} <= set(news)       # the kept rows, as before
    assert "attackers_new" in news and "factions_against_you" in news and "purse" in news
    assert "news: whole" not in later and any(w.startswith("news.events_answered") for w in later)
    assert F.size(out) <= F.CAP


def test_5_CONTROL_a_news_that_fits_is_given_as_it_is():
    env = {"ok": True, "tool": "observe", "game": {"date": "1085.02.13"},
           "result": {"news": {"events_answered": [], "attackers_new": [{"war_id": 9}]}}}
    assert F.paginate(env, "observe", {"depth": "news"}, cap=F.CAP) is env
