"""The purse's step window, an answered event's piety, and price reads made for the bound ruler. A test named RED
checks the behaviour each case below needs; a CONTROL or an unnamed one
checks what stays as it was. The game files are small stand-ins written to a temporary folder (CK3_GAME_DIR), shaped
as the game's own.

  1  a standing order minted the kingdom on the day the step ended, before that day's read: the step's purse charged
     it, and the next step's charged it again (its window began on the same day), reading the price back as
     "unexplained" gold and prestige                                   -> one place in the server's order per act
  2  piety fell by an answered event's own price (culture_tradition_events.0100.b: massive_piety_loss) and gold rose a
     little: the purse took it for the head of faith's gold, +100 expected, and left the gold "unexplained"
                                                                        -> the event's piety, from its file
  3  the ruler-change briefing said "no price read under your late ruler" when every price read of his had gone out
     for the bound ruler (character 0)                                  -> kept under the ruler's own id
No game.
"""
from __future__ import annotations

import asyncio
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import event_prices as EP  # noqa: E402
import faction_letters as FL  # noqa: E402
import news  # noqa: E402
import redesign as R  # noqa: E402
import seat_events as SE  # noqa: E402
from test_ireland_walk_replies import Gw  # noqa: E402

MURCHAD, BRIAN = 31762, 36303


def run(c):
    return asyncio.run(c)


def snap(date, gold, prestige, piety=500.0, actor=MURCHAD):
    """A read as observe makes it (news.snapshot), with the purse and its monthly rates."""
    return news.snapshot(actor_id=actor, date=date, chr_sect={"gold": gold, "prestige": prestige, "piety": piety,
                                                             "monthly_income": 0.0},
                         wars={}, claims={}, council=None, heir=None)


@pytest.fixture
def clean():
    news.reset()
    yield
    news.reset()


# ── 1: an act on a read's own day is in one step ────────────────────────────────────────────────────────────────

def test_1_RED_an_order_fired_before_the_days_read_is_charged_in_that_step_only(clean):
    s44 = snap("1085.12.01", 271.88, 540.0)
    # the order fired on the day the advance stopped, then the step's read on that day
    news.note_act(MURCHAD, "1086.11.23", "mint_title k_ireland", cost_known=True, gold=-325.0, prestige=400.0)
    s45 = snap("1086.11.23", 3.94, 940.0)
    s46 = snap("1087.02.21", 6.68, 941.0)
    g45 = news._resource_change(s44, s45, "gold")
    assert g45["your_acts"] == [{"what": "mint_title k_ireland", "gold": -325.0}], g45
    g46 = news._resource_change(s45, s46, "gold")
    p46 = news._resource_change(s45, s46, "prestige")
    assert "your_acts" not in g46 and "your_acts" not in (p46 or {}), (g46, p46)
    assert "unexplained" not in g46, g46


def test_1_an_act_sent_after_the_days_read_is_the_next_steps(clean):
    s1 = snap("1086.11.23", 3.94, 940.0)
    # the seat's own turn, on the day of the read it opened with
    news.note_act(MURCHAD, "1086.11.23", "mint_title d_meath", cost_known=True, gold=-1.0, prestige=300.0)
    s2 = snap("1087.02.21", 2.94, 1240.0)
    p = news._resource_change(s1, s2, "prestige")
    assert p["your_acts"] == [{"what": "mint_title d_meath", "prestige": 300.0}], p
    assert news.own_act_in_step(s1, s2, "mint_title", "d_meath") is not None
    s3 = snap("1087.05.21", 2.94, 1240.0)
    assert news.own_act_in_step(s2, s3, "mint_title", "d_meath") is None


#: The functions of news.py that read the acts directly, each with why it is not a step window over priced acts
ACT_READERS = {
    "reset": "clears them",
    "note_act": "writes them",
    "acts_in_step": "THE window (act_in_step)",
    "_events_for": "the war-declared row matches all acts by war id (no window, no amount)",
    "_own_acts_unpriced": "a name with no amount, named in both steps a read's day bounds (its cost may come in either)",
    "block": "the mercenary companies this server hired, by company id (no window)",
    "letter_sender": "a letter's sender: the act of its interaction before the letter, by date, across steps (no "
                     "amount; the letter comes days after the send)",
}


def test_1_every_reader_of_the_acts_is_the_one_rule_or_says_why_not():
    """The sweep, by the code (fails OPEN: a new reader is a red until it is listed): every function that reads
    news._ACTS is acts_in_step or an exception named above; a step window anywhere else calls acts_in_step."""
    import ast
    tree = ast.parse(Path(news.__file__).read_text())
    readers = {f.name for f in ast.walk(tree) if isinstance(f, (ast.FunctionDef, ast.AsyncFunctionDef))
               and any(isinstance(n, ast.Name) and n.id == "_ACTS" for n in ast.walk(f))}
    assert readers == set(ACT_READERS), sorted(readers ^ set(ACT_READERS))
    users = {f.name for f in ast.walk(tree) if isinstance(f, (ast.FunctionDef, ast.AsyncFunctionDef))
             and any(isinstance(n, ast.Name) and n.id == "acts_in_step" for n in ast.walk(f))}
    assert len(users) >= 9, sorted(users)


def test_1_a_record_without_its_place_falls_back_to_the_dates(clean):
    prev, now = {"actor_id": MURCHAD, "date": "1086.11.23", "gold": 300.0}, \
                {"actor_id": MURCHAD, "date": "1087.02.21", "gold": 0.0}
    x = {"date": "1086.11.23", "what": "mint_title k_ireland"}
    assert news.act_in_step(x, prev, now) is True
    assert news.act_in_step(dict(x, date="1087.03.01"), prev, now) is False


# ── 2: an answered event's piety ────────────────────────────────────────────────────────────────────────────────

TRADITION = """
tradition_test.0100 = {
\ttype = character_event
\toption = {
\t\tname = tradition_test.0100.a
\t\tadd_piety_level = 1
\t}
\toption = {
\t\tname = tradition_test.0100.b
\t\tadd_piety = massive_piety_loss
\t\tadd_piety_level = -2
\t}
}
"""
VALUES = """
medium_piety_value = 100
massive_piety_value = 500
massive_piety_loss = {
\tvalue = 0
\tsubtract = massive_piety_value
}
minor_prestige_value = 75
"""


@pytest.fixture
def game(tmp_path, monkeypatch):
    files = {"events/culture_events/tradition_test.txt": TRADITION,
             "common/script_values/00_basic_values.txt": VALUES, "common/scripted_effects/none.txt": ""}
    for rel, text in files.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    monkeypatch.setenv("CK3_GAME_DIR", str(tmp_path))
    for mod, names in ((SE, ("_EVENT_CACHE", "_NAMESPACE_EVENTS")), (FL, ("_FILES",)), (EP, ("_VALUES",))):
        for n in names:
            c = getattr(mod, n, None)
            if c is not None:
                c.clear()
    news.reset()
    yield tmp_path
    EP._VALUES.clear()
    news.reset()


ANSWERED = {"events_answered": [{"event": "tradition_test.0100", "applied": "tradition_test.0100.b",
                                 "date": "1069.03.17", "gold_at_pick": 2.35}]}


def _step(gold_was, gold_now, piety_was, piety_now):
    prev = {"actor_id": MURCHAD, "date": "1069.02.27", "gold": gold_was, "piety": piety_was,
            "monthly_income": 3.94, "monthly_piety": 2.78}
    now = {"actor_id": MURCHAD, "date": "1069.05.28", "gold": gold_now, "piety": piety_now}
    return prev, now


def test_2_RED_an_events_piety_is_not_the_head_of_faiths_gold(game):
    prev, now = _step(2.1, 3.53, 510.06, 18.09)
    g = news._resource_change(prev, now, "gold", ANSWERED)
    assert not any("head of faith" in str(e.get("what")) for e in g.get("in_this_step") or []), g
    # what is left is the income read's own shortfall (no +100 from a head of faith the game never paid)
    assert abs(g.get("unexplained", 0)) < 15, g
    p = news._resource_change(prev, now, "piety", ANSWERED)
    rows = [e for e in p.get("in_this_step") or [] if "tradition_test.0100.b" in e["what"]]
    assert rows and rows[0]["expected"] == -500, p
    assert "unexplained" not in p, p
    assert not any("head of faith" in str(e.get("what")) for e in p["in_this_step"]), p


def test_2_control_a_fall_with_the_head_of_faiths_letter_is_read_as_his_gold(game):
    prev, now = _step(2.1, 110.0, 510.06, 262.0)
    letter = {"events_answered": [{"event": news.HOF_GOLD_LETTER, "applied": "religious_interaction.3.a",
                                   "date": "1069.03.17"}]}
    g = news._resource_change(prev, now, "gold", letter)
    assert any("head of faith" in str(e.get("what")) for e in g.get("in_this_step") or []), g
    # the same purse with no letter names no head of faith: a shape is no record of the ask
    g = news._resource_change(prev, now, "gold", {"events_answered": []})
    assert not any("head of faith" in str(e.get("what")) for e in g.get("in_this_step") or []), g


def test_2_the_option_reads_its_piety_from_the_file(game):
    assert EP.option_piety("tradition_test.0100", "tradition_test.0100.b")["amount"] == -500
    assert EP.option_piety("tradition_test.0100", "tradition_test.0100.a") is None
    assert EP.option_prestige("tradition_test.0100", "tradition_test.0100.b") is None


# ── 3: a price read for the bound ruler ─────────────────────────────────────────────────────────────────────────

def _price_gw(cost=850.0, have=200.0):
    return Gw({"query_dejure_progress": {"held": 9, "total": 14, "fraction": 9 / 14},
               "query_create_title_cost": {"success": True, "engine_valid": False, "actor_id": MURCHAD,
                                           "engine_reasons": ["Must hold more counties"],
                                           "price": {"known": True, "currency": "gold", "cost": cost,
                                                     "have": have, "affordable": False}}})


def test_3_RED_a_read_for_the_bound_ruler_is_kept_under_his_id(clean):
    run(R.title_requirements(_price_gw(), 0, "e_britannia", news_actor=MURCHAD))
    assert news._CREATION_PRICE_BY.get((MURCHAD, "e_britannia"), {}).get("cost") == 850.0
    assert (0, "e_britannia") not in news._CREATION_PRICE_BY


def test_3_RED_observes_next_title_read_is_kept_under_the_ruler(clean):
    run(R.next_title_block(_price_gw(), 0, {"primary_title_key": "k_ireland"},
                           {"held_titles": [{"key": "k_ireland", "de_jure_parent_key": "e_britannia"}]},
                           news_actor=MURCHAD))
    assert news._CREATION_PRICE_BY.get((MURCHAD, "e_britannia"), {}).get("cost") == 850.0


def test_3_RED_the_briefing_reads_the_late_rulers_price_read_for_the_bound_ruler(clean, monkeypatch):
    run(R.title_requirements(_price_gw(), 0, "e_britannia", news_actor=MURCHAD))

    async def price(gw, cid, key, *a, **k):
        return None, {"currency": "gold", "cost": 850.0, "gold": 20.0}

    async def basis(gw, cid, key, cost, **k):
        return {"base": 1000.0, "discount": 0.15, "sources": []}
    monkeypatch.setattr(R, "mint_title_price", price)
    monkeypatch.setattr(R, "creation_price_basis", basis)
    out = run(news.ruler_change_briefing(Gw({}), {"actor_id": MURCHAD}, {"actor_id": BRIAN}, "e_britannia"))
    assert out["price"]["was"] == 850.0 and out["price"].get("was_read"), out["price"]


def test_3_control_a_read_naming_its_ruler_is_kept_under_him(clean):
    run(R.title_requirements(_price_gw(), BRIAN, "e_britannia", news_actor=MURCHAD))
    assert (BRIAN, "e_britannia") in news._CREATION_PRICE_BY
    assert (MURCHAD, "e_britannia") not in news._CREATION_PRICE_BY
