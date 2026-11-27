"""Rows from the second measured Munster series (16 runs), replayed from its recorded replies.

  D1 a06 pay_if on 0302 with else=0302.b (the duchy letter) set, no warning; paid 87 twice  else_pays_gold; option=0302.b priced
  D3 a07 a gold>=78 rule paid a 90-gold letter at 83.54 (the chaplain's learning fell)        floor below price refused; raised at each read
  D2 a07 council_task "no_councillor_in_seat" beside a council read naming the steward       the refusal says the two disagree, retryable
  D4 a09 cbs counted the seat's own vassal (71775, liege 48246) as the defender's joiner     the game's liege-vassal bar
  D6 a14 the goal line said no duchy for gold alone while inspect named d_ulster (162.5)    chain_step: one reading, both readers

The values are the runs' own (named beside each); the game's files are read where a test says so (skipped when they
are not on this host). No game.
"""
from __future__ import annotations

import asyncio
import copy
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1]))

import redesign as R  # noqa: E402
import seat_events as SE  # noqa: E402
from test_ireland_walk_replies import Gw  # noqa: E402

FILES = SE.game_dir() is not None
need_files = pytest.mark.skipif(not FILES, reason="the game's files are not on this host")
MURCHAD, BRIAN = 31762, 36303
L0301, L0302 = "court_chaplain_task.0301", "court_chaplain_task.0302"


def run(c):
    return asyncio.run(c)


def _purse(gold, income=4.44, cid=MURCHAD):
    return {"id": cid, "name": "Murchad", "alive": True, "liege_id": 0, "top_liege_id": cid, "gold": gold,
            "income_breakdown": {"total_monthly": income}}


def _letter_gw(gold, learning, rules=()):
    gw = Gw({"query_council": {"council": [{"position_key": "councillor_court_chaplain", "holder_id": 9,
                                            "skills": {"learning": learning}}]},
             "query_character": _purse(gold),
             "query_event_rules": {"success": True, "seats": [{"character_id": MURCHAD, "rules": list(rules)}],
                                   "picks": []},
             "query_armies": {"armies": []}})
    return gw


# ── D1: the else option of a pay_if pays too ────────────────────────────────────────────────────────────────────

@need_files
def test_D1_the_files_say_which_options_of_the_claim_letters_pay():
    assert SE.option_pays_gold(L0302, "court_chaplain_task.0302.a") is True
    assert SE.option_pays_gold(L0302, "court_chaplain_task.0302.b") is True      # the duchy claim letter
    assert SE.option_pays_gold(L0302, "court_chaplain_task.0301.b") is False     # its decline
    assert SE.option_pays_gold(L0301, "court_chaplain_task.0301.b") is False
    assert SE.option_pays_gold(L0302, 1) is True and SE.option_pays_gold(L0302, "nope") is None


@need_files
def test_D1_RED_the_recorded_call_is_refused_and_nothing_is_set():
    """a06 s24 1073.05.27 (gold 58.37): pay_if gold>=87 option=0302.a else=0302.b answered applied, warnings []; the
    else paid 87 at 27.53 (1078.07.09) and at -91.79 (1080.01.30)."""
    gw = _letter_gw(58.37, 11)
    a = {"op": "set", "event": L0302, "answer": "pay_if", "option": "court_chaplain_task.0302.a",
         "else": "court_chaplain_task.0302.b", "currency": "gold", "amount": 87}
    env = run(SE.do_event_rule(gw, a, MURCHAD, {"id": MURCHAD}))
    assert env["verdict"] == "rejected" and env["blocked"]["raw_reason"] == "else_pays_gold", env
    assert "pays gold too" in env["blocked"]["summary"] and "else=court_chaplain_task.0301.b" in env["blocked"]["summary"]
    assert env["result"]["instead"]["args"]["args"]["else"] == "court_chaplain_task.0301.b"
    assert [w for w in gw.writes if w[0] == "set_event_rule"] == []


@need_files
def test_D1_CONTROL_the_decline_as_else_is_set_on_both_variants():
    gw = _letter_gw(158.0, 11)
    a = {"op": "set", "event": L0302, "answer": "pay_if", "option": "court_chaplain_task.0302.a",
         "else": "court_chaplain_task.0301.b", "currency": "gold", "amount": 87}
    env = run(SE.do_event_rule(gw, a, MURCHAD, {"id": MURCHAD}))
    assert env["blocked"] is None or env["blocked"].get("raw_reason") != "else_pays_gold", env
    sent = [p for w, p in gw.writes if w == "set_event_rule"]
    assert sent and sent[0]["else"] == "court_chaplain_task.0301.b"


@need_files
def test_D1_an_always_pay_rule_on_the_duchy_letter_is_priced_as_the_duchy_letter():
    """Before: only an option key ending .a counted as paying, so option=0302.b (7 x (40 - learning), at least 80-100)
    passed at any gold."""
    gw = _letter_gw(40.0, 11)
    env = run(SE.do_event_rule(gw, {"op": "set", "event": L0302, "answer": "option",
                                    "option": "court_chaplain_task.0302.b"}, MURCHAD, {"id": MURCHAD}))
    # An always-pay rule is set floored at the letter's price (here the duchy letter's), never into debt
    assert env["result"]["event_check"]["always_pay_floor"]["letter_costs"] == float((40 - 11) * 7), env
    sent = [p for w, p in gw.writes if w == "set_event_rule"]
    assert sent[0]["answer"] == "pay_if" and sent[0]["amount"] == (40 - 11) * 7
    assert sent[0]["option"] == "court_chaplain_task.0302.b" and sent[0]["else"] == "court_chaplain_task.0301.b"


@need_files
def test_D1_a_variant_whose_else_would_pay_is_not_set(monkeypatch):
    params = {"character_id": MURCHAD, "op": "set", "event": L0301, "answer": "pay_if",
              "option": "court_chaplain_task.0301.a", "currency": "gold", "amount": 87,
              "else": "court_chaplain_task.0301.b"}
    monkeypatch.setattr(SE, "variant_rule", lambda e, o, r: dict(r, event=o, option="court_chaplain_task.0302.a",
                                                                  **{"else": "court_chaplain_task.0302.b"}))
    gw = _letter_gw(158.0, 11)
    env = run(SE.do_event_rule(gw, {k: v for k, v in params.items() if k != "character_id"}, MURCHAD,
                               {"id": MURCHAD}))
    assert env["result"]["variants"] == [{"event": L0302, "set": False, "why": SE.VARIANT_ELSE_PAYS}]
    assert [p["event"] for w, p in gw.writes if w == "set_event_rule"] == [L0301]


# ── D3: a pay_if floor below the letter's price ─────────────────────────────────────────────────────────────────

@need_files
def test_D3_RED_a_floor_below_the_price_is_refused_whatever_the_gold_now():
    """a07: the 1084.06.23 rule gold>=78 was right for a 78-gold letter (learning 14); at learning 10 the letter is 90
    and the rule pays it from 78-89 gold. Before, a floor of 78 at 100 gold was accepted (the gold covered it now)."""
    gw = _letter_gw(100.0, 10)
    env = run(SE.do_event_rule(gw, {"op": "set", "event": L0301, "answer": "pay_if",
                                    "option": "court_chaplain_task.0301.a", "currency": "gold", "amount": 78},
                               MURCHAD, {"id": MURCHAD}))
    assert env["blocked"]["raw_reason"] == "pays_letter_into_debt", env
    cw = env["result"]["cost_warning"]
    assert cw["pays_from"] == 78 and cw["each_letter"] == 90.0 and cw["after_one_letter"] == -12.0
    assert "whenever the gold is at least 78" in env["blocked"]["summary"]
    assert cw["instead"]["args"]["args"]["amount"] == 91


@need_files
def test_D3_CONTROL_a_floor_at_the_price_is_set():
    gw = _letter_gw(100.0, 14)
    env = run(SE.do_event_rule(gw, {"op": "set", "event": L0301, "answer": "pay_if",
                                    "option": "court_chaplain_task.0301.a", "currency": "gold", "amount": 78},
                               MURCHAD, {"id": MURCHAD}))
    assert env["blocked"] is None or env["blocked"].get("raw_reason") != "pays_letter_into_debt", env


def test_D3_a_rule_left_below_a_new_price_is_raised_at_the_read():
    """a07 s45 1088.12.02: the letter read 90 (learning 10), the rule held gold>=78; it paid at 83.54 on 1090.12.06."""
    rules = [{"event": L0301, "answer": "pay_if", "option": "court_chaplain_task.0301.a", "currency": "gold",
              "amount": 78, "else": "ai"},
             {"event": L0302, "answer": "pay_if", "option": "court_chaplain_task.0302.a", "currency": "gold",
              "amount": 78, "else": "court_chaplain_task.0301.b"},
             {"event": "stress_threshold.1721", "answer": "option", "option": "x", "else": "ai"}]
    gw = _letter_gw(60.0, 10, rules)
    rows = run(SE.refresh_letter_floors(gw, MURCHAD, 10))
    sent = [p for w, p in gw.writes if w == "set_event_rule"]
    assert [(p["event"], p["amount"]) for p in sent] == [(L0301, 90), (L0302, 90)]
    assert "else" not in sent[0] and sent[1]["else"] == "court_chaplain_task.0301.b"
    assert rows[0]["amount_was"] == 78 and rows[0]["amount_now"] == 90 and rows[0]["set"] is True
    assert rows[0]["why"] == SE.FLOOR_RAISED and "why" not in rows[1]


def test_D3_CONTROL_a_rule_at_or_above_the_price_or_no_learning_writes_nothing():
    rules = [{"event": L0301, "answer": "pay_if", "option": "court_chaplain_task.0301.a", "currency": "gold",
              "amount": 91, "else": "ai"}]
    gw = _letter_gw(60.0, 10, rules)
    assert run(SE.refresh_letter_floors(gw, MURCHAD, 10)) is None
    assert run(SE.refresh_letter_floors(gw, MURCHAD, None)) is None
    assert gw.writes == []


def test_D3_the_news_block_raises_the_floor_and_says_so(monkeypatch):
    import news as N
    called = []

    async def fake(gw, me, learning):
        called.append((me, learning))
        return [{"event": L0301, "amount_was": 78, "amount_now": 90}]
    monkeypatch.setattr(SE, "refresh_letter_floors", fake)
    N.reset()
    try:
        out = run(N.block(Gw(), MURCHAD, {"actor_id": MURCHAD, "date": "1088.12.02", "chaplain_learning": 10},
                          offers_served=False))
    finally:
        N.reset()
    assert called == [(MURCHAD, 10)] and out["letter_rule_raised"][0]["amount_now"] == 90


# ── D2: the council read and the game's check disagree ──────────────────────────────────────────────────────────

def _refused():
    return {"verdict": "rejected", "result": {},
            "blocked": R.write_blocked(gate="engine_refused", raw_reason="no_councillor_in_seat",
                                       summary="the game's own check said no (no_councillor_in_seat)", raw={})}


def test_D2_RED_the_refusal_names_the_holder_read_and_can_be_sent_again():
    """a07 s8 1069.09.25: observe and the read before the send named Brian 36303 steward; the script's
    `exists = cp:councillor_steward` said no, retry never; s9 1070.03.24 the same call applied."""
    v = _refused()
    assert v["blocked"]["retry"] == "never"                                   # the recorded refusal
    R._seat_read_disagrees(v, [{"position_key": "councillor_steward", "holder_id": BRIAN, "holder_source": "display"}],
                           "councillor_steward")
    b = v["blocked"]
    assert b["retryable"] is True and b["retry"] != "never"
    assert str(BRIAN) in b["detail"] and "display" in b["detail"] and "cp:councillor_steward" in b["detail"]
    assert b["measured"] == {"council_read_holder": BRIAN, "holder_source": "display", "game_check": "no councillor"}


def test_D2_CONTROL_a_seat_the_read_found_vacant_keeps_the_refusal():
    v = _refused()
    R._seat_read_disagrees(v, [{"position_key": "councillor_steward", "holder_id": 0xFFFFFF, "vacant": True}],
                           "councillor_steward")
    assert v["blocked"]["retry"] == "never" and "council_read_holder" not in str(v["blocked"])


# ── D4: the seat's own vassal counted as the defender's ally ────────────────────────────────────────────────────

SEAT, DEFENDER, VASSAL, OTHER = 48246, 63632, 71775, 40862


def _war_gw(vassal_liege=SEAT):
    chars = {SEAT: {"id": SEAT, "is_ai": True, "ruler_tier": 3, "liege_id": 0, "top_liege_id": SEAT, "culture_id": 74},
             DEFENDER: {"id": DEFENDER, "is_ai": True, "ruler_tier": 2, "liege_id": 0, "top_liege_id": DEFENDER,
                        "culture_id": 74},
             VASSAL: {"id": VASSAL, "is_ai": True, "ruler_tier": 2, "liege_id": vassal_liege,
                      "top_liege_id": vassal_liege or VASSAL, "culture_id": 74, "personality": {"honor": 0}},
             OTHER: {"id": OTHER, "is_ai": True, "ruler_tier": 2, "liege_id": 0, "top_liege_id": OTHER,
                     "culture_id": 74, "personality": {"honor": 0}}}

    def alliances(p):
        if p.get("character_id") == DEFENDER:
            return {"own_military_strength": 310, "total_callable_strength": 880,
                    "alliances": [{"ally_id": VASSAL, "military_strength": 570}]}
        return {"own_military_strength": 451, "alliances": []}
    return Gw({"query_alliances": alliances, "query_character": lambda p: chars.get(p["character_id"], {}),
               "query_opinion": lambda p: {"opinion": 24}})


def test_D4_RED_your_own_vassal_does_not_join_the_defender():
    """a09 s41 1092.05.02: cbs on Donnchad 63632 listed 71775 (liege_id 48246, the seat's ruler) would_join true,
    score 99, likely_strength 880 against 451: 'outnumbered_with_likely_allies'."""
    da = run(R.defender_allies(_war_gw(), DEFENDER, attacker_id=SEAT))
    (a,) = da["allies"]
    assert a["would_join"] is False and "your vassal" in a["why"] and "liege_vassal_check" in a["why"]
    assert da["likely_strength"] == 310 and da["with_allies"] == 310 and da["barred_strength"] == 570
    assert R.war_verdict(451, da) == "stronger_than_all_his_allies"


def test_D4_CONTROL_an_independent_ally_is_still_estimated():
    da = run(R.defender_allies(_war_gw(vassal_liege=0), DEFENDER, attacker_id=SEAT))
    (a,) = da["allies"]
    assert a["would_join"] is True and a["score"] == 20 + 50 + 0 + 5 + 24
    assert da["likely_strength"] == 310 + 570 and da["with_allies"] == 880 and "barred_strength" not in da


def test_D4_your_liege_and_a_vassal_of_your_vassal_are_barred_too():
    assert R.liege_bar([], SEAT, [VASSAL], VASSAL).startswith("your liege")
    assert R.liege_bar([OTHER, SEAT], SEAT, [], VASSAL).startswith("your vassal")
    assert R.liege_bar([OTHER], SEAT, [], VASSAL) is None


def test_D4_the_liege_chain_walks_to_the_top():
    chars = {2: {"id": 2, "liege_id": 3, "top_liege_id": 4}, 3: {"id": 3, "liege_id": 4, "top_liege_id": 4}}
    gw = Gw({"query_character": lambda p: chars.get(p["character_id"], {})})
    assert run(R.liege_chain(gw, {"id": 1, "liege_id": 2, "top_liege_id": 4})) == [2, 3, 4]
    assert run(R.liege_chain(gw, {"id": 1, "liege_id": 0, "top_liege_id": 1})) == []


# ── D6: one reading of a chain title for the goal line and inspect ──────────────────────────────────────────────

def _duchy_row(reasons, have=86.09, cost=162.5, blocked=True):
    return {"title_key": "d_ulster", "do_call": {"intent": "mint_title", "args": {"title_key": "d_ulster"}},
            "blocked": {"gate": "cannot_afford"} if blocked else None, "price": {"cost": cost, "currency": "gold"},
            "requirements": [{"requirement": "price", "currency": "gold", "cost": cost, "have": have,
                              "met": have >= cost},
                             {"requirement": "game_check", "met": not reasons, "reasons": list(reasons)}]}


def _kingdom_row(unheld=("d_meath", "d_ulster", "d_connacht", "d_leinster")):
    return {"title_key": "k_ireland", "requirements": [
        {"requirement": "de_jure_counties", "have": 9, "need": 8, "of": 14, "met": True},
        {"requirement": "price", "currency": "gold", "cost": 325.0, "have": 86.09, "met": False},
        {"requirement": "game_check", "met": False, "reasons": ["Petty King Brian II does not have gold 325"]},
        {"requirement": "tier_rule", "met": False, "rule": "hold 2 duchy titles (you hold 1)",
         "one_of": [{"tier": "duchy", "need": 2, "have": 1}], "dejure_unheld": list(unheld)}]}


def test_D6_chain_step_reads_the_three_states():
    assert R.chain_step(_duchy_row(["Petty King Brian II does not have gold 162"]))["state"] == "gold_only"
    lk = R.chain_step(_duchy_row(["You only have 1 of 2 required Counties", "does not have gold 162"]))
    assert lk["state"] == "lacks" and lk["lacks"] == ["You only have 1 of 2 required Counties"]
    now = R.chain_step(_duchy_row([], have=200.0, blocked=False))
    assert now["state"] == "creatable_now" and now["do_call"]["args"]["intent"] == "mint_title"


def test_D6_formable_titles_puts_the_chain_on_the_target_row():
    k, d = _kingdom_row(), _duchy_row(["Petty King Brian II does not have gold 162"])
    R.attach_chain(k, {"k_ireland": k, "d_ulster": d})
    tr = k["requirements"][-1]
    assert [s["title_key"] for s in tr["chain"]] == ["d_ulster"] and tr["chain"][0]["state"] == "gold_only"
    assert tr["chain_not_priced"] == ["d_meath", "d_connacht", "d_leinster"]
    k2 = _kingdom_row()
    R.attach_chain(k2, {"k_ireland": k2})                 # none priced: no chain (the env reads them)
    assert "chain" not in k2["requirements"][-1]


