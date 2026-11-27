"""Ducal conquest rows, the purse, and a decision's price, each row replayed
from its recorded reply.

- cbs handed a Ducal Conquest ready call with `cb_title_id 0` (1074.01.10, Ulster 62947), declare_war sent it
and the game refused it blaming cost or "the engine's own rule" (1074.05.09); the same call with c_ulster applied.
- the purse's "unexplained" gold, the claim-letter rule set at gold 0.56, the debt sentence.
- isolate_family_decision priced "free (no cost block)"; its effect fires epidemic_events.0100 (-350).
Rules read from the game's own files are checked against the install when one is present, skipped otherwise.
"""
import asyncio
import os
from pathlib import Path

import pytest

import declare_gates as DG
import event_prices as EP
import news
import redesign as R
import seat_events as SE
from test_ireland_walk_replies import Gw, cbs_by_target
import test_script_actions as SA

ME, ULSTER_LORD, C_ULSTER, D_ULAID = 31506, 62947, 474, 466
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")


def run(c):
    return asyncio.run(c)


def need_game():
    if not (GAME / "common" / "decisions").is_dir():
        pytest.skip(f"no game install at {GAME}")


@pytest.fixture(autouse=True)
def _fresh(monkeypatch):
    news.reset()
    R.CB_COST_SEEN.clear()

    async def instant(_s=0):
        return None
    monkeypatch.setattr(R, "_SLEEP", instant)
    yield
    news.reset()


# ── A title-scoped casus belli with no title ────────────────────────────────────────────

#: the bridge's cbs row for Ulster as a seated game's reply shows it (1074.05.09: valid, target_mode 1, title 0)
DUCAL_ROW = {"cb_key": "ducal_conquest_cb", "valid": True, "scope": "county", "target_mode": 1, "cb_title_id": 0,
             "family": "conquest", "cost_prestige": 50, "cost_piety": 0, "affordable": None, "requirements_unmet": [],
             "grants": "one county", "notes": ["title-targeted: REQUIRES cb_title_id to dispatch"]}
CBS = {"target_state": {"independent": True, "imprisoned": False, "in_alliance": False, "in_truce": False,
                        "already_at_war_opposing": False}, "cbs": [DUCAL_ROW]}
#: query_conquest_targets: d_ulaid (uncreated) holds c_ulster (Ulster's, next to your land) beside your county;
#: d_meath (created) holds another of his counties, which the rule excludes
CT = {"duchies": [
    {"duchy_id": D_ULAID, "duchy_key": "d_ulaid", "partially_held": True, "held_by_ruler_count": 1,
     "remaining_counties": [{"county_id": C_ULSTER, "county_key": "c_ulster", "holder_id": ULSTER_LORD,
                             "holder_top_liege_id": ULSTER_LORD, "adjacent": True}]},
    {"duchy_id": 999, "duchy_key": "d_meath", "partially_held": True, "held_by_ruler_count": 1,
     "remaining_counties": [{"county_id": 998, "county_key": "c_meath", "holder_id": ULSTER_LORD,
                             "holder_top_liege_id": ULSTER_LORD, "adjacent": True}]}]}


def _hier(p):
    return {"title": {"holder_id": 0 if p["title_id"] == D_ULAID else 777}}


def _purse(gold, income=2.9):
    return {"id": ME, "name": "Murchad", "alive": True, "age": 48, "liege_id": 0, "top_liege_id": ME, "gold": gold,
            "prestige": 283.21, "piety": 600.0, "faith_key": "insular_celtic",
            "income_breakdown": {"total_monthly": income}}


def _cbs_gw(gold=1.44, ct=CT):
    gw = Gw(cbs_by_target(CBS), date="1074.05.09")
    gw.replies.update({"query_conquest_targets": ct, "query_title_hierarchy": _hier,
                       "query_character": lambda p: (_purse(gold) if p.get("character_id") == ME else
                                                     {"id": p.get("character_id"), "alive": True, "liege_id": 0,
                                                      "top_liege_id": p.get("character_id")})})
    return gw


def test_1a_cbs_hands_one_row_per_eligible_county_with_its_title():
    res = run(R.cbs_impl(_cbs_gw(), target_id=ULSTER_LORD, character_id=ME))["result"]
    (o,) = res["valid_options"]
    assert o["declare_args"]["cb_title_id"] == C_ULSTER and o["cb_title_id"] == C_ULSTER, o.get("declare_args")
    (row,) = o["by_title"]
    assert row["title"] == "c_ulster" and row["duchy"] == "d_ulaid"
    assert row["declare_args"] == {"target_id": ULSTER_LORD, "cb": "ducal_conquest_cb", "cb_title_id": C_ULSTER,
                                   "claimant_id": None}
    assert row["if_you_lose"]["prestige"] == -150 and "you" not in row["if_you_lose"]
    assert "neighbor_land" in o["title_rule"]


def test_1b_in_debt_the_blocked_row_names_the_county_too():
    res = run(R.cbs_impl(_cbs_gw(gold=-2.68), target_id=ULSTER_LORD, character_id=ME))["result"]
    (b,) = res["blocked_now"]
    assert b["declare_args"]["cb_title_id"] == C_ULSTER and [t["title"] for t in b["by_title"]] == ["c_ulster"]
    assert b["cb_title_id"] == C_ULSTER
    assert b["blocked_by"][0]["fix"].startswith("in debt: no declaration is possible")
    assert "out of debt in 0.9 months at +2.9 a month" in b["blocked_by"][0]["fix"]


def test_1c_no_eligible_county_means_no_ready_call():
    ct = {"duchies": [dict(CT["duchies"][1])]}          # only the created duchy
    res = run(R.cbs_impl(_cbs_gw(ct=ct), target_id=ULSTER_LORD, character_id=ME))["result"]
    (o,) = res["valid_options"]
    assert "declare_args" not in o and "without the title" in o["declare_args_unavailable"]


def _declare_gw(gold=1.44):
    base = _cbs_gw(gold)

    class G(SA.Gw):
        async def read(self, action, params=None):
            if action in base.replies:
                self.reads.append((action, dict(params or {})))
                r = base.replies[action]
                return r(dict(params or {})) if callable(r) else r
            return await super().read(action, params)
    return G(SA.ran("declare_war", guard="refused"))


def test_1d_declare_war_with_title_0_is_refused_before_sending_naming_the_county():
    """The recorded call (1074.05.09): declare_args {target 62947, ducal_conquest_cb, cb_title_id 0}."""
    gw = _declare_gw()
    env = run(R.declare_war_impl(gw, ME, declare_args={"target_id": ULSTER_LORD, "cb": "ducal_conquest_cb",
                                                       "cb_title_id": 0, "claimant_id": None},
                                 confirm_straddle=True))
    b = env["blocked"]
    assert env["verdict"] == "rejected" and b["raw_reason"] == "cb_title_required", b
    assert "c_ulster" in b["summary"] and "nothing was sent" in b["summary"]
    assert b["fix"]["args"]["declare_args"]["cb_title_id"] == C_ULSTER
    assert SA.scripts_sent(gw) == [], "nothing sent"


def test_1d_CONTROL_with_the_county_it_is_sent():
    gw = _declare_gw()
    run(R.declare_war_impl(gw, ME, declare_args={"target_id": ULSTER_LORD, "cb": "ducal_conquest_cb",
                                                 "cb_title_id": C_ULSTER, "claimant_id": None},
                           confirm_straddle=True))
    assert len(SA.scripts_sent(gw)) == 1


def test_1e_plan_war_and_cbs_loss_carry_the_defeat_prestige():
    lp = R.loss_price("ducal_conquest_cb", 4.6)
    assert lp["prestige"] == -150 and lp["gold"] == -110.4 and "you" not in lp
    # a claim war's defeat is -10 x the war's prestige factor, which the engine sets and no read gives: no number
    claim = R.loss_price("claim_cb", 2.66)
    assert isinstance(claim["prestige"], str) and "-10 x" in claim["prestige"] and "prestige_not_read" in claim


# ── Debt ────────────────────────────────────────────────────────────────────────────────

def test_2a_the_purse_prices_raised_armies():
    prev = {"actor_id": ME, "date": "1077.01.16", "gold": -39.5, "monthly_income": 3.06, "armies_men": 1500}
    snap = {"actor_id": ME, "date": "1077.07.06", "gold": -59.18, "monthly_income": 3.06, "armies_men": 1500}
    g = news._resource_change(prev, snap, "gold")
    (row,) = [r for r in g["in_this_step"] if r["what"].startswith("armies raised")]
    assert row["expected"] == pytest.approx(-0.003 * 1500 * (171 / 30.44), abs=0.05)
    assert "GOLD_COST_PER_SOLDIER" in row["basis"]
    assert g["likely"]["expenses_not_in_the_income_read"] < 0


def test_2a_CONTROL_no_armies_no_row():
    prev = {"actor_id": ME, "date": "1080.06.21", "gold": -88.83, "monthly_income": 2.84, "armies_men": 0}
    snap = {"actor_id": ME, "date": "1081.06.21", "gold": -73.92, "monthly_income": 2.84, "armies_men": 0}
    g = news._resource_change(prev, snap, "gold")
    assert not [r for r in g.get("in_this_step") or [] if r["what"].startswith("armies")]


def _rule_gw(gold, learning=12):
    gw = Gw({"query_council": {"council": [{"position_key": "councillor_court_chaplain", "holder_id": 9,
                                            "skills": {"learning": learning}}]},
             "set_event_rule": {"success": True},
             "query_event_rules": {"success": True, "rulers": [{"character_id": ME, "rules": [
                 {"event": "court_chaplain_task.0301", "answer": "option", "option": "court_chaplain_task.0301.a"}]}]},
             "query_armies": {"armies": []}})
    gw.replies["query_character"] = _purse(gold, 2.71)
    return gw


def test_2b_an_always_pay_letter_rule_at_low_gold_says_its_cost_and_the_debt():
    """Turn 9 (1069.10.02): `option .a` set at gold 0.56; the letter cost 84 and left -86.11. without
    args.debt the rule is floored at the price (test_reply_pages_and_partition_forecast); with it, it is set as asked and says its cost and debt."""
    env = run(SE.do_event_rule(_rule_gw(0.56), {"op": "set", "event": "court_chaplain_task.0301", "answer": "option",
                                                "option": "court_chaplain_task.0301.a", "debt": True}, ME, None))
    cw = env["result"]["cost_warning"]
    assert cw["each_letter"] == 84.0 and cw["after_one_letter"] == -83.44 and "below 0" in cw["debt"]
    assert cw["months_to_repay"] == round(83.44 / 2.71, 1)
    assert cw["instead"]["args"]["args"]["answer"] == "pay_if"


def test_2b_CONTROL_with_gold_to_cover_it_no_warning():
    env = run(SE.do_event_rule(_rule_gw(150.0), {"op": "set", "event": "court_chaplain_task.0301",
                                                 "answer": "option", "option": "court_chaplain_task.0301.a"}, ME, None))
    assert "cost_warning" not in env["result"]


def test_2d_the_debt_sentence():
    s = DG.debt_words(-106.0, 2.71)
    assert s.startswith("in debt: no declaration is possible") and "out of debt in 39.1 months at +2.71" in s


# ── A decision that fires a priced event is not free ────────────────────────────────────

def test_3a_isolate_family_is_priced_by_the_event_it_fires():
    need_game()
    p = EP.decision_price("isolate_family_decision")
    assert "free" not in p and p["prestige"] == -350
    assert "epidemic_events.0100" in [f["event"] for f in p["fires"]]
    a = {"what": "decision", "key": "isolate_family_decision"}
    SE.decision_row_price(a, {})
    assert a["price"] == {"prestige": -350} and any("epidemic_events.0100: .a -350" in w for w in a["fires"])


def test_3b_CONTROL_a_decision_that_fires_nothing_stays_free():
    need_game()
    p = EP.decision_price("take_vow_of_poverty_decision")
    assert p.get("free") is True and "fires no event" in p["basis"]


def test_2e_a_kingdom_short_of_gold_says_how_many_months_of_income():
    """a seated game's turn 1 (1066.09.15): k_ireland 425 gold against 84 held at +2.34 a month."""
    gw = Gw({"query_dejure_progress": {"held": 1, "total": 14, "fraction": 0.07},
             "query_create_title_cost": {"success": True, "engine_valid": False, "engine_reasons": [],
                                         "price": {"known": True, "currency": "gold", "cost": 425.0, "have": 84.0}}})
    gw.replies["query_character"] = _purse(84.0, 2.34)
    row, _ = run(R.title_requirements(gw, ME, "k_ireland"))
    (pr,) = [r for r in row["requirements"] if r["requirement"] == "price"]
    assert pr["months_of_income"] == round(341.0 / 2.34) and "income_levers" in pr["income_basis"]


def test_2e_CONTROL_a_duchy_row_is_unchanged():
    gw = Gw({"query_dejure_progress": {"held": 1, "total": 2, "fraction": 0.5},
             "query_create_title_cost": {"success": True, "engine_valid": False, "engine_reasons": [],
                                         "price": {"known": True, "currency": "gold", "cost": 212.5, "have": 84.0}}})
    row, _ = run(R.title_requirements(gw, ME, "d_meath"))
    (pr,) = [r for r in row["requirements"] if r["requirement"] == "price"]
    assert "months_of_income" not in pr


# ── A title lost at the liege's death names how ─────────────────────────────────────────

def _lost_gw():
    """a seated game 1091.08.20: c_dublin (463) to 77756, son of the liege 31762 who died; the game's title history
    (read live) records type 10 for it."""
    chars = {77756: {"id": 77756, "alive": True, "liege_id": 0, "top_liege_id": 77756, "father_id": 31762},
             31762: {"id": 31762, "alive": False, "liege_id": 0, "top_liege_id": 31762}}
    return Gw({"query_title_hierarchy": {"title": {"holder_id": 77756}},
               "query_character": lambda p: chars.get(p.get("character_id"), {}),
               "query_title_history": {"title_id": 463, "entries": [
                   {"date": "1091.08.14", "event_type": "revoked", "event_type_raw": 10, "char_id": 77756}]}})


def test_4a_a_title_lost_at_the_liege_death_names_the_cause():
    how = run(news._lost_to(_lost_gw(), 69915, 463, 31762))
    assert how["cause"] == "liege_died" and how["holder_id"] == 77756 and how["history_type"] == 10
    assert "died" in how["how"] and "revoked" in how["how"] and "1091.08.14" in how["how"]


def test_4a_CONTROL_without_a_dead_liege_it_names_the_history_only():
    how = run(news._lost_to(_lost_gw(), 69915, 463, None))
    assert "cause" not in how and how["how"].startswith("revoked: to 77756 on 1091.08.14")


def test_2e_inspect_title_carries_the_months_and_the_whole_bill():
    """The creation block of inspect(title k_ireland) (the recorded 1073.02.08 rows): the price and the bill
    (two duchies and the kingdom, 850) in months of income at +2.71."""
    import gzip, json
    import title_table as TT
    rec = json.loads(gzip.open(Path(__file__).resolve().parent / "fixtures" / "declare_war_gate_replies.json.gz",
                               "rt").read())["title_k_ireland_1073_02_08"]
    costs = {"k_ireland": {"success": True, "engine_valid": False, "engine_reasons": [
                 "Duchy Titles Required: 2 (currently: 0)", "Kingdom Titles Required: 1 (currently: 0)"],
                 "price": {"known": True, "currency": "gold", "cost": 425.0, "have": 24.29}}}
    for d in ("d_leinster", "d_meath", "d_connacht", "d_ulster", "d_munster"):
        costs[d] = {"success": True, "engine_valid": False, "engine_reasons": [],
                    "price": {"known": True, "currency": "gold", "cost": 212.5, "have": 24.29}}
    gw = Gw({"query_create_title_cost": lambda p: costs[p["title_key"]]}, date="1073.02.08")
    gw.replies["query_character"] = _purse(24.29, 2.71)
    out = run(TT._creation(gw, ME, "k_ireland", {"held": 4, "total": 14, "fraction": 0.29}, rec["duchies"], []))
    assert out["price"]["months_of_income"] == round((425.0 - 24.29) / 2.71)
    assert out["price"]["bill_months_of_income"] == round((850.0 - 24.29) / 2.71)


def test_4b_an_offer_recorded_in_an_earlier_diff_still_names_the_liege_change():
    """A live read of the 1076 save: the offer of 1077.03.20 was named in the diff to 03.27; the liege read changed in the diff to
    04.26, which read cause_unread."""
    snap = {"date": "1077.04.26", "ai_veto_records": [
        {"character_id": ME, "date": "1077.03.20", "what": "vassal_offer", "offered_by": 31762, "dropped": False}]}
    c = news.liege_cause({"from": 0, "to": 31762}, {}, {}, snap)
    assert c["cause"] == "offer_accepted" and "1077.03.20" in c["how"]


def test_4b_CONTROL_an_offer_from_someone_else_is_not_the_cause():
    snap = {"date": "1077.04.26", "ai_veto_records": [
        {"character_id": ME, "date": "1077.03.20", "what": "vassal_offer", "offered_by": 99, "dropped": False}]}
    assert "cause_unread" in news.liege_cause({"from": 0, "to": 31762}, {}, {}, snap)


def test_4c_the_liege_change_and_the_baronies_name_the_death():
    """A live read of the 1091 save (1091.08.14): liege 31762 -> 77756 read cause_unread and b_dublin how null beside c_dublin's
    liege_died."""
    out = {"titles_lost": [{"title_id": 463, "key": "c_dublin", "cause": "liege_died", "how": "x"}]}
    c = news.liege_cause({"from": 31762, "to": 77756}, out, {}, {})
    assert c["cause"] == "liege_died" and "31762 died" in c["how"]
    gw = _lost_gw()
    prev = {"actor_id": 69915, "date": "1091.07.28", "liege_id": 31762, "titles": {463: "c_dublin", 464: "b_dublin"},
            "title_parents": {463: "d_meath", 464: "c_dublin"}}
    snap = {"actor_id": 69915, "date": "1091.08.20", "liege_id": 77756, "titles": {}, "title_parents": {}}
    news.reset()
    run(news.block(gw, 0, prev, offers_served=False))
    blk = run(news.block(gw, 0, snap, offers_served=False))
    lost = {r["key"]: r for r in blk["titles_lost"]}
    assert lost["c_dublin"]["cause"] == "liege_died"
    assert lost["b_dublin"]["with_county"] == "c_dublin" and lost["b_dublin"]["how"] == lost["c_dublin"]["how"]
    assert blk["liege_changed"]["cause"] == "liege_died"
