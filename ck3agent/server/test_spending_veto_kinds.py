"""The gold leaks that left one seated run short of the
Kingdom of Ireland by 75 gold, and the additions another recorded game showed.

- ai_veto what=spending sets the kinds whose drop costs the seat nothing but the AI's own pick in one call;
the first observation and every news with ai_acted rows name the kinds still not vetoed; men_at_arms is a veto kind
(the bridge's hook rows are ctest test_ai_veto 18a-f; the vtables are checked here from the constructors' bytes).
- a title's price row names its base, its discounts and the price under the heir.
- a mercenary hire the bridge recorded in the step counts in the step's explained gold; an AI men-at-arms
purchase is priced in the purse.
- the chaplain's claim letter comes as court_chaplain_task.0301 or 0302; both are attributed, and the
call that sets a rule on one names the other and what a letter costs against the kingdom's price.
- a title the seat minted reads `how` created by you.
- a kingdom's tier rule reads met when the game's check ran
and listed no 'Titles Required' line.
"""
import asyncio
import json
import os
import re
from pathlib import Path

import pytest

import event_prices as EP
import news
import redesign as R
import seat_events as SE
from test_ireland_walk_replies import Gw

MUNSTER, BRIAN = 31762, 36303
ROOT = Path(__file__).resolve().parents[2]
GAME = Path(os.environ.get("CK3_GAME_DIR") or
            Path.home() / ".local/share/Steam/steamapps/common/Crusader Kings III/game")
BINARY = GAME.parent / "binaries" / "ck3"


def run(c):
    return asyncio.run(c)


@pytest.fixture(autouse=True)
def _fresh():
    news.reset()
    yield
    news.reset()


def veto_gw(refuse=(), vetoes_after=None):
    """A gateway whose bridge refuses set_ai_veto for the kinds in `refuse` and reads `vetoes_after` back
    (default: every kind written with op=set)."""
    gw = Gw()
    written: list = []

    async def write(action, params=None):
        gw.writes.append((action, dict(params or {})))
        if action == "set_ai_veto":
            if params["what"] in refuse:
                return {"success": False, "reason": "call_ally_interaction_unresolved"}
            if params["op"] == "set":
                written.append(params["what"])
        return {"success": True}
    gw.write = write
    gw.replies["query_ai_veto"] = lambda p: {"success": True, "rulers": [
        {"character_id": MUNSTER, "vetoes": list(written if vetoes_after is None else vetoes_after)}],
        "records": []}
    return gw


# ── The preset, and the kinds not vetoed ──────────────────────────────────────────────────────

def test_1a_one_call_sets_the_spending_kinds():
    gw = veto_gw()
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "spending"}, MUNSTER, {"id": MUNSTER}))
    assert env["ok"] is True and env["verdict"] == "applied", env
    sent = [p["what"] for a, p in gw.writes if a == "set_ai_veto"]
    assert sent == ["law", "mercenary", "construction", "men_at_arms", "create_title", "vassal_offer", "court_type",
                    "legacy", "decision", "activity"]   # legacy; decision (the gold rule and the income keys); activity
    res = env["result"]
    assert res["vetoes"] == sent and set(res["left_out"]) == {"ally_call", "perk", "war", "faction",
                                                              "call_in"}   # call_in; decision in the preset
    assert "defensive" in res["left_out"]["ally_call"]


def test_1a_CONTROL_a_kind_the_bridge_refuses_rejects_the_call_and_names_it():
    gw = veto_gw(refuse=("vassal_offer",))
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "spending"}, MUNSTER, {"id": MUNSTER}))
    assert env["ok"] is False and env["result"]["refused"] == {"vassal_offer": "call_ally_interaction_unresolved"}
    assert "vassal_offer" in env["blocked"]["summary"]


def test_1a_CONTROL_a_readback_missing_a_kind_rejects():
    gw = veto_gw(vetoes_after=["law"])
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "spending"}, MUNSTER, {"id": MUNSTER}))
    assert env["ok"] is False and "mercenary" in env["result"]["not_as_asked"]


def test_1b_the_unset_spending_kinds_and_the_one_call():
    """A recorded game, turn 1: law, ally_call, vassal_offer, create_title, construction, perk (no mercenary)."""
    row = SE.spending_unset(["law", "ally_call", "vassal_offer", "create_title", "construction", "perk"])
    assert row["not_vetoed"] == ["mercenary", "men_at_arms", "court_type", "legacy", "decision", "activity"]   # legacy added
    assert row["one_call"]["args"] == {"intent": "ai_veto", "args": {"op": "set", "what": "spending"}}
    assert SE.spending_unset(list(SE.AI_VETO_PRESETS["spending"])) is None
    assert "unread" in SE.spending_unset(None)


def test_1b_every_news_with_ai_acted_names_the_unset_kinds():
    """A recorded game, step 65: ai_acted mercenary company 536 with no mercenary veto set."""
    rec = {"seq": 9, "what": "mercenary", "dropped": False, "origin": "game", "company_id": 536,
           "date": "1093.03.06", "gold": 246.11, "priced": "no", "unpriced_why": "no_price_read"}
    snap = {"ai_veto_records": [rec], "ai_vetoes": ["law", "ally_call", "vassal_offer", "create_title",
                                                    "construction", "perk"]}
    out = SE.veto_news({"ai_veto_records": []}, snap)
    assert out["ai_acted"][0]["company_id"] == 536
    assert out["ai_veto_spending"]["not_vetoed"] == ["mercenary", "men_at_arms", "court_type", "legacy",
                                                     "decision", "activity"]  # legacy added


def test_1b_CONTROL_no_ai_acted_no_row():
    out = SE.veto_news({"ai_veto_records": []}, {"ai_veto_records": [], "ai_vetoes": []})
    assert "ai_veto_spending" not in out


def test_1c_the_turn_one_note_names_the_preset():
    doc = json.loads((ROOT / "ck3agent" / "server" / "notes.json").read_text())
    by = {n["id"]: n for n in doc["notes"]}
    t = by["ai-spends-for-you"]["text"]
    assert "do ai_veto what=spending" in t and "ai-spends-for-you" in doc["turn_one"]
    assert len(t.encode()) <= 240


def test_1d_men_at_arms_is_a_kind_and_its_purchase_is_priced():
    assert "men_at_arms" in SE.AI_VETO_WHATS and "men_at_arms" in SE.AI_VETO_PRESETS["spending"]
    mangonel = {"what": "men_at_arms", "key": "mangonel", "regiment": "new", "date": "1078.03.01",
                "price": {"gold": 66.0}}
    prev = {"actor_id": MUNSTER, "gold": 237.31, "date": "1077.09.25"}
    snap = {"actor_id": MUNSTER, "gold": 160.0, "date": "1078.03.01"}
    g = news._resource_change(prev, snap, "gold", {"ai_acted": [mangonel]})
    (row,) = [e for e in g["in_this_step"] if "men-at-arms" in e["what"]]
    assert row["expected"] == -66.0 and "mangonel" in row["what"]
    assert row["lever"]["args"]["args"] == {"op": "set", "what": "men_at_arms"}


#: the constructors' first bytes, 1.18.3.1 (md5 09ba163b...): CreateMaARegiment 0x0658fce0 and
#: CAddMAAToRegimentCommand's ctor 0x06591ba0 (header offsets = file offsets in the text segment)
MAA_CTORS = {
    "CREATE_MAA_REGIMENT_CMD_VTABLE": (0x658fce0, 0x23, 0x0bc4b138),
    "ADD_MAA_TO_REGIMENT_CMD_VTABLE": (0x6591ba0, 0x1d, 0x0bc4b3f0),
}


def _lea_target(code: bytes, va: int, at: int) -> int:
    assert code[at:at + 3] == b"\x48\x8d\x05", code[at:at + 7].hex()
    return va + at + 7 + int.from_bytes(code[at + 3:at + 7], "little", signed=True)


def test_1e_the_maa_vtables_are_what_the_constructors_store_at_cmd_0():
    h = (ROOT / "re" / "offsets" / "ck3_offsets.h").read_text()
    if not BINARY.is_file():
        pytest.skip("the game binary is not installed here")
    data = BINARY.read_bytes()
    for name, (va, lea_at, want) in MAA_CTORS.items():
        code = data[va:va + 0x40]
        assert _lea_target(code, va, lea_at) == want
        m = re.search(rf"constexpr uintptr_t {name}\s*=\s*(0x[0-9a-fA-F]+);", h)
        assert m and int(m.group(1), 16) == want, name


def test_1e_CONTROL_the_hire_ctor_decodes_to_its_own_vtable():
    """The same decoder on CHireMercenaryCompanyCommand's ctor: its +0x00 store is HIRE_MERC_CMD_VTABLE_PRIMARY."""
    if not BINARY.is_file():
        pytest.skip("the game binary is not installed here")
    code = BINARY.read_bytes()[0x600a410:0x600a450]
    at = code.index(b"\x48\x8d\x05")
    assert _lea_target(code, 0x600a410, at) == 0x0bb5d728


# ── The price and where its discount comes from ───────────────────────────────────────────────

def price_gw(ruler_perks, heir_perks, heir_culture="irish"):
    chars = {MUNSTER: {"culture_key": "irish", "traits": [{"name": "diligent"}]},
             BRIAN: {"culture_key": heir_culture, "traits": [{"name": "brave"}]}}
    perks = {MUNSTER: ruler_perks, BRIAN: heir_perks}
    return Gw({"query_character": lambda p: chars[p["character_id"]],
               "query_lifestyle": lambda p: {"perks": perks[p["character_id"]]},
               "query_succession": {"primary_heir_id": BRIAN}})


def _need_files():
    if not EP.title_cost_base():
        pytest.skip("the game's files are not installed here")


def test_2a_murchads_325_is_500_less_his_perk_and_his_culture_and_rises_at_succession():
    """A recorded game: 325 under Murchad (ducal_conquest_perk, irish), 425 under Brian (the final save's perks: no
    ducal_conquest_perk)."""
    _need_files()
    b = run(R.creation_price_basis(price_gw(["ducal_conquest_perk", "thoughtful_perk"], ["tax_man_perk"]),
                                   MUNSTER, "k_ireland", 325.0))
    assert b["base"] == 500.0 and b["discount"] == 0.35
    by = {x["key"]: x for x in b["sources"]}
    assert by["ducal_conquest_perk"]["gold"] == -100.0 and by["ethos_courtly (irish)"]["gold"] == -75.0
    assert not any(x["source"] == "other (not read)" for x in b["sources"])
    at = b["at_succession"]
    assert at["heir_id"] == BRIAN and at["price"] == 425.0 and at["rises"] == 100.0
    assert "ducal_conquest_perk" in at["why"]


def test_2a_CONTROL_an_heir_with_the_same_perk_pays_the_same():
    _need_files()
    b = run(R.creation_price_basis(price_gw(["ducal_conquest_perk"], ["ducal_conquest_perk"]),
                                   MUNSTER, "k_ireland", 325.0))
    assert b["at_succession"]["price"] == 325.0 and b["at_succession"].get("same") is True


def test_2a_CONTROL_another_culture_is_not_guessed():
    _need_files()
    b = run(R.creation_price_basis(price_gw(["ducal_conquest_perk"], [], heir_culture="norse"),
                                   MUNSTER, "k_ireland", 325.0))
    assert "price" not in b["at_succession"] and "norse" in b["at_succession"]["unread"]


def test_2b_a_discount_the_reads_do_not_name_is_said_unread():
    _need_files()
    b = run(R.creation_price_basis(price_gw([], []), MUNSTER, "k_ireland", 300.0))
    other = [x for x in b["sources"] if x["source"] == "other (not read)"]
    assert other and other[0]["gold"] == -125.0 and "may_be" in other[0]


# ── The hire counted in the step ──────────────────────────────────────────────────────────────

def test_3a_a_hire_recorded_in_the_step_leaves_the_unexplained_figure():
    """A recorded game, step 65: change -162.42, unexplained -198.03 beside mercenaries_expected -149.28 (company 536)."""
    g = {"change": -162.42, "unexplained": -198.03, "cause": "partly unknown"}
    news.count_hires_in_step(g, [{"company_id": 536, "price_estimate": 149.28}], {536})
    assert g["unexplained"] == -48.75
    assert any(e.get("expected") == -149.28 and "536" in e["what"] for e in g["in_this_step"])


def test_3a_CONTROL_a_company_not_recorded_in_this_step_is_not_counted():
    g = {"change": -162.42, "unexplained": -198.03}
    news.count_hires_in_step(g, [{"company_id": 536, "price_estimate": 149.28}], set())
    assert g["unexplained"] == -198.03 and "in_this_step" not in g


# ── the claim letter's family ───────────────────────────────────────────────────────────────────────

def test_A_the_letter_family_is_every_chaplain_event_that_charges_a_claim():
    f = GAME / "events" / "councillor_task_events" / "court_chaplain_task_events.txt"
    if not f.is_file():
        pytest.skip("the game's files are not installed here")
    text = f.read_text(encoding="utf-8-sig", errors="replace")
    heads = [(m.start(), m.group(1)) for m in re.finditer(r"^(court_chaplain_task\.\d+)\s*=\s*\{", text, re.M)]
    charging = []
    for i, (at, key) in enumerate(heads):
        body = text[at:heads[i + 1][0] if i + 1 < len(heads) else len(text)]
        if "court_chaplain_task_0301_charge_prestige_or_gold_effect" in body:
            charging.append(key)
    assert tuple(charging) == news.CLAIM_LETTERS


def test_A_a_0302_letter_is_priced_as_the_chaplains():
    """A recorded game, step 35: events_answered court_chaplain_task.0302 applied .a, the purse said not the letter."""
    pick = {"seq": 5, "event": "court_chaplain_task.0302", "applied": "court_chaplain_task.0302.a",
            "options": ["court_chaplain_task.0302.a", "court_chaplain_task.0302.b", "court_chaplain_task.0301.b"]}
    prev = {"event_picks": [], "chaplain_learning": 14}
    snap = {"event_watched": True, "event_picks": [pick], "event_rule_events": ["court_chaplain_task.0301"]}
    letters = news._letter_claims(prev, snap, [{"title_id": 480}])
    assert letters[480] is True
    d = news._letter_decided(snap, 78)
    assert "0302" in d["decided_by"] and d["lever"]["args"]["args"]["event"] == "court_chaplain_task.0302"
    d = news._letter_decided({"event_rule_events": []}, 78)
    assert d["lever"]["args"]["args"]["event"] == "court_chaplain_task.0301"
    # The 0302 lever is a gold rule too (pay_if, the decline as its else)
    assert d["lever_also"]["args"]["args"] == {"op": "set", "event": "court_chaplain_task.0302", "answer": "pay_if",
                                               "option": "court_chaplain_task.0302.a", "currency": "gold",
                                               "amount": 78, "else": "court_chaplain_task.0301.b"}


def test_A_the_duchy_claim_option_is_paid_and_the_0301_b_option_declines():
    a = {"event": "court_chaplain_task.0302", "applied": "court_chaplain_task.0302.b",
         "options": ["court_chaplain_task.0302.a", "court_chaplain_task.0302.b", "court_chaplain_task.0301.b"]}
    assert news._declined(a) is False
    assert news._declined(dict(a, applied="court_chaplain_task.0301.b")) is True
    assert news._declined({"event": "court_chaplain_task.0301", "applied": "court_chaplain_task.0301.b",
                           "options": ["court_chaplain_task.0301.a", "court_chaplain_task.0301.b"]}) is True
    assert news.claim_duchy_gold_cost(14) == 182.0 and news.claim_duchy_gold_cost(30) == 90.0


def test_A_CONTROL_rules_on_both_events_decide():
    snap = {"event_rule_events": list(news.CLAIM_LETTERS)}
    assert news._letter_decided(snap, 78) == {"decided_by": "your event rules on court_chaplain_task.0301 and "
                                                            "court_chaplain_task.0302"}


def letter_gw():
    gw = Gw({"query_create_title_cost": {"success": True, "engine_valid": False, "price": {
                 "known": True, "currency": "gold", "cost": 425.0, "have": 350.0}}})
    return gw


def test_5_the_rule_call_on_a_letter_says_what_it_costs_against_the_kingdom(monkeypatch):
    async def purse(gw, me):
        return {"gold": 350.0, "monthly_income": 6.3}

    async def learning(gw, council):
        return 14
    monkeypatch.setattr(R, "ruler_purse", purse)
    monkeypatch.setattr(news, "chaplain_learning", learning)
    monkeypatch.setitem(R._NEWS_WITHIN, MUNSTER, "k_ireland")
    lf = run(SE.letter_facts(letter_gw(), MUNSTER, "court_chaplain_task.0301", "pay_if", "court_chaplain_task.0301.a",
                             [{"event": "court_chaplain_task.0301"}]))
    assert lf["each_letter"]["gold"] == 78.0 and lf["months_of_income"] == 12.4
    assert "until your ruler's death" in lf["claim_lasts"]
    assert lf["k_ireland"] == {"price": 425.0, "gold": 350.0, "short_now": 75.0, "short_after_one_letter": 153.0}
    assert "court_chaplain_task.0302" in lf["not_covered"]


def test_5_CONTROL_a_declining_rule_carries_no_price():
    lf = run(SE.letter_facts(letter_gw(), MUNSTER, "court_chaplain_task.0302", "option", news.CLAIM_DECLINE_OPTION,
                             [{"event": "court_chaplain_task.0301"}, {"event": "court_chaplain_task.0302"}]))
    assert lf is None
    assert run(SE.letter_facts(letter_gw(), MUNSTER, "some_event.0001", "pay_if", None, [])) is None


# ── B, C ──────────────────────────────────────────────────────────────────────────────────────────────

def test_C_a_check_that_ran_without_a_titles_required_line_passed_the_tier_rule():
    """A recorded game: after d_ulster k_ireland's row read the tier rule unknown."""
    gw = Gw({"query_dejure_progress": {"held": 9, "total": 14, "fraction": 0.64},
             "query_create_title_cost": {"success": True, "engine_valid": True, "engine_reasons": [],
                                         "price": {"known": True, "currency": "gold", "cost": 325.0, "have": 330.0}}})
    row, blk = run(R.title_requirements(gw, MUNSTER, "k_ireland"))
    (tr,) = [q for q in row["requirements"] if q["requirement"] == "tier_rule"]
    assert tr["met"] is True and "listed no" in tr["rule"]


def test_C_CONTROL_an_unread_check_keeps_the_rule_unknown():
    gw = Gw({"query_dejure_progress": {"held": 9, "total": 14, "fraction": 0.64},
             "query_create_title_cost": {"success": False, "reason": "no_reply"}})
    row, blk = run(R.title_requirements(gw, MUNSTER, "k_ireland"))
    (tr,) = [q for q in row["requirements"] if q["requirement"] == "tier_rule"]
    assert tr["met"] is None and tr["unread"] == R.TIER_RULE_UNREAD


# ── superseded: the "cap" was another siege's event timer ─────────────────────────

#: A recorded game, 1070.03.27 inspect war_stance's siege row (b_donegal), and the war's occupation 0: a bridge with the superseded siege read's
DONEGAL = {"siege_id": 100663305, "besieger_id": MUNSTER, "besieger_side": "attacker", "holding_title_id": 486,
           "defender_id": 32421, "progress": 20.0, "progress_max": 20.0, "progress_pct": 100.0, "state": 5,
           "active": True, "status": "won"}


def test_4_a_pre_lane_row_has_its_progress_unread_never_won_or_full():
    rows, verdict = R.siege_rows([DONEGAL], 0)
    assert verdict == "unread" and rows[0]["progress_unread"] == R.SIEGE_PROGRESS_MISREAD
    assert not {"progress", "progress_pct", "status", "state", "progress_max"} & set(rows[0]), rows[0]


def test_4_the_stance_view_reads_no_verdict_for_an_unread_siege():
    war = {"war_id": 56, "attacker_id": MUNSTER, "defender_id": 6098, "is_attacker": True, "name": "Ailech"}
    gw = Gw({"get_active_wars": {"wars": [war]},
             "query_wars": {"wars": [{"war_id": 56, "sieges": [DONEGAL]}]},
             "query_war_score_breakdown": {"total_warscore": 30, "battle_score": 149, "occupation_score": 0},
             "query_war_stance": {}})
    v = run(R._war_stance_view(gw, MUNSTER))
    (w,) = v["wars"]
    assert w["stall_suspect"] is None and "progress_unread" in w["sieges"][0]
    assert any(x["code"] == "siege_progress_unread" for x in w["limitations"])
    assert not any(x["code"] == "siege_full_not_fallen" for x in w["limitations"])
