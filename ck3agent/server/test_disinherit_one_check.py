"""The fixes from two Munster runs: one where k_ireland was made by the game's AI, and a test run
(k_ireland never made).

  1  disinherit read two ways in one turn (the GOAL line: the game's check, not allowed; succession_plan: the
     dynasty-head rule, available)                       -> one verdict (partition_forecast.disinherit_allowed), the
                                                            game's check deciding; its Mystical Ancestors words said
                                                            the right way round
  2  a standing order minting k_ireland at gold >= 326 for a price of 325
                                                         -> the order's set record and end_turn's reply say the price
                                                            read beside the threshold
  3  for nine years the line said "it costs 325" while the bill, with the second duchy, was 487.5 and 99 months
     of net away, past the ruler's even-odds death     -> the bill, its months, the death beside them; the county
                                                            count said once
  4  the death forecast's law lever named only the innovation (crown_authority_1 left out)
                                                         -> every condition law_reach names
  5  `takes` named c_ossory, which the game kept with the primary heir at the death
                                                         -> the limit said in the forecast
  6  Connacht joined as a vassal, cause_unread         -> the game's other road named (offer_fealty_interaction)

Values are the runs' own where a test says so. No game.
"""
from __future__ import annotations

import asyncio
import copy
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT))

import law_reach as LR  # noqa: E402
import news as N  # noqa: E402
import partition_forecast as PF  # noqa: E402
import redesign as R  # noqa: E402

MURCHAD, BRIAN, DONNCHAD = 31762, 36303, 64917
DO = "mcp__ck3__do"
#: Turn 43's game check of disinherit on Donnchad, the bridge's words (as the GOAL line quoted them)
MYSTIC = {"target_id": DONNCHAD, "sendable": False, "refused_by": "interaction_not_valid_for_target",
          "engine_reason": "Culture does not have the Mystical Ancestors Tradition."}


def run(c):
    return asyncio.run(c)


# ── 1: one disinherit verdict ─────────────────────────────────────────────────────────────────────────────────────

def test_1_the_game_check_decides_for_every_reader():
    gc = PF._game_check(MYSTIC, {})
    assert PF.disinherit_allowed(MURCHAD, MURCHAD, gc) == (False, [PF.MYSTICAL_ANCESTORS_WHY])
    assert PF.disinherit_summary(MURCHAD, MURCHAD, [gc]) == (False, PF.MYSTICAL_ANCESTORS_WHY)
    ok = PF._game_check({"target_id": DONNCHAD, "sendable": True}, {})
    assert PF.disinherit_summary(MURCHAD, MURCHAD, [gc, ok]) == (True, None)


def test_1_the_mystical_ancestors_line_alone_names_no_failing_condition():
    # The pin: the line is printed pass or fail (the bridge log's raw text marks it trigger_pass_inactive),
    # so alone it names no failing condition and never says the culture has the tradition
    why = PF._game_check(MYSTIC, {})["refused"]["why"]
    assert why == PF.MYSTICAL_ANCESTORS_WHY and "your culture has" not in why
    # CONTROL: another refusal keeps the game's own words
    other = PF._game_check(dict(MYSTIC, engine_reason="Not enough renown"), {})["refused"]["why"]
    assert other == "the game does not allow it: Not enough renown"


def test_1_the_head_rule_alone_refuses_but_never_allows():
    assert PF.disinherit_allowed(99, MURCHAD, None)[0] is False
    assert PF.disinherit_allowed(None, MURCHAD, None) == (None, ["the dynasty's head was not read"])
    assert PF.disinherit_allowed(MURCHAD, MURCHAD, None)[0] is None
    # another head refuses even when a check reads sendable (the game's is_shown asks for the head)
    assert PF.disinherit_allowed(99, MURCHAD, {"sendable": True})[0] is False


class _Gw:
    """succession_plan's levers block: the reads it makes, the check answering as a test run's did."""

    def __init__(self, check=MYSTIC):
        self.check = check
        self.checks = []

    def game(self):
        return {"date": "1086.12.26"}

    async def read(self, action, params=None):
        p = params or {}
        if action == "query_character":
            return {"id": p.get("character_id"), "age": 0, "alive": True, "is_female": False, "dynasty_id": 2044}
        if action == "query_realm_laws":
            return {"active_laws": [{"name": "confederate_partition_succession_law",
                                     "group_name": "succession_order_laws"}]}
        if action == "query_dynasty":
            return {"dynasty_id": 2044, "head_character_id": MURCHAD}
        if action == "query_interaction_check":
            self.checks.append(p)
            return {"result": {"results": [self.check]}} if self.check else {}
        return {}


def test_1_succession_plans_block_is_the_same_verdict_as_the_lever_row():
    gw = _Gw()
    ruler = {"id": MURCHAD, "dynasty_id": 2044, "children": [DONNCHAD], "primary_title_id": 518}
    lev = run(R._succession_levers(gw, MURCHAD, [{"id": DONNCHAD}], ruler))["disinherit"]
    assert lev["available"] is False and lev["why_not"] == PF.MYSTICAL_ANCESTORS_WHY
    assert gw.checks and gw.checks[0]["interaction_type"] == "disinherit_interaction"
    # the lever row of the same son, from the same check: the same verdict
    fc = {"younger_shares": [{"id": DONNCHAD, "fate": "leaves_realm", "takes": [473]}]}
    fc_in = {"ruler_tier": 3, "succession_law": "confederate_partition_succession_law",
             "gender_law": "male_preference_law", "primary_heir_id": BRIAN, "held_counties": 3,
             "children": [{"id": BRIAN, "alive": True, "is_female": False},
                          {"id": DONNCHAD, "alive": True, "is_female": False}]}
    rows = PF.lever_rows(fc, fc_in, ruler={"id": MURCHAD, "prestige": 900, "piety": 900},
                         dynasty={"head_character_id": MURCHAD, "renown": 5000},
                         children=[{"id": DONNCHAD, "age": 0}],
                         checks={("disinherit_interaction", DONNCHAD): MYSTIC})
    row = rows["disinherit"][0]
    assert (row["allowed_now"], row["why_not"]) == (False, [lev["why_not"]])


def test_1_CONTROL_a_sendable_check_reads_available_in_both():
    gw = _Gw(check={"target_id": DONNCHAD, "sendable": True})
    ruler = {"id": MURCHAD, "dynasty_id": 2044, "children": [DONNCHAD]}
    lev = run(R._succession_levers(gw, MURCHAD, [{"id": DONNCHAD}], ruler))["disinherit"]
    assert lev["available"] is True and lev["why_not"] is None


# ── 2: the order's threshold and the price ────────────────────────────────────────────────────────────────────────

MINT = {"tool": "do", "args": {"intent": "mint_title", "args": {"title_key": "k_ireland"}}}
PRICE = {"title_key": "k_ireland", "cost": 325.0, "currency": "gold", "at_death": 425.0}


def _order(v, op=">="):
    return {"id": "mint_ireland", "when": [{"metric": "gold", "op": op, "value": v}], "do": MINT}


# ── 3: the bill (1085.10.20, the k_ireland row as the seat read it) ─────────────────────────────────────

_ROW = {
    "title_key": "k_ireland",
    "price_basis": {"at_succession": {"price": 425.0, "rises": 100.0,
                                      "why": "your own ducal_conquest_perk discount dies with you; create k_ireland "
                                             "before your death to pay 325.0"}},
    "at_death": {"price_after": {"price": 425.0, "rises": 100.0},
                 "ruler": {"act_now": True, "age": 58, "dies_within_years": {"p25": 1.6, "p50": 3.2}, "health": 2.52}},
    "requirements": [
        {"have": 7, "met": False, "need": 8, "of": 14, "requirement": "de_jure_counties"},
        {"cost": 325.0, "currency": "gold", "have": 51.92, "met": False, "requirement": "price",
         "months_to_target": {"months": 62, "net_monthly": 4.43, "short": 273.08,
                              "armies_raised": {"if_stood_down": {"months": 36, "net_monthly": 7.64},
                                                "men": 1070, "upkeep_a_month": 3.21}}},
        {"met": False, "reasons": ["You only have 7 of 8 required Counties", "Petty King Murchad does not have gold 325"],
         "requirement": "game_check"},
        {"bill": {"cost": 487.5, "currency": "gold", "of": ["d_connacht", "k_ireland"]},
         "chain": [{"cost": 162.5, "currency": "gold", "have": 51.92, "state": "lacks", "title_key": "d_connacht",
                    "lacks": ["Title already owned by Petty King Ruaidrí mag Áed of Connacht"]}],
         "dejure_unheld": ["d_meath", "d_ulster", "d_connacht", "d_leinster"], "met": False,
         "one_of": [{"have": 1, "need": 2, "tier": "duchy"}, {"have": 0, "need": 1, "tier": "kingdom"}],
         "requirement": "tier_rule", "rule": "hold 2 duchy titles (you hold 1) (or hold 1 title of kingdom tier or above)"},
    ]}


# ── 4: the law lever names every condition ────────────────────────────────────────────────────────────────────────

def test_4_the_law_lever_carries_crown_authority(monkeypatch):
    async def law(gw, ruler, names):
        r = LR.reach("partition_succession_law", government="feudal", laws=["crown_authority_0"], innovations=set())
        return {"law": "partition_succession_law", "allowed_now": r["possible"], "needs": r["missing"],
                "price": {"prestige": 500}, "call": {"tool": "do", "args": {"intent": "change_law"}}}

    monkeypatch.setattr(R, "_succession_law_lever", law)
    out = run(R._death_levers(_Gw(check=None), {"id": MURCHAD, "dynasty_id": 2044}, {"younger_shares": []}, [], {},
                              ["confederate_partition_succession_law"], set()))
    row = next(x for x in out if x["lever"] == "law")
    assert row["why_not"][0] == "your culture's innovation_hereditary_rule"
    assert row["why_not"][1].startswith("crown_authority_1 (you have crown_authority_0")


# ── 5: the forecast says what its takes are ───────────────────────────────────────────────────────────────────────

def test_5_a_share_of_several_titles_carries_the_limit():
    kids = [{"id": BRIAN, "alive": True, "is_female": False, "name": "Brian"},
            {"id": DONNCHAD, "alive": True, "is_female": False, "name": "Donnchad"}]
    sim = {"partition_heir_basis": "children",
           "heirs": [{"character_id": BRIAN, "titles": [{"title_id": 518, "tier": 3}, {"title_id": 519, "tier": 2}]},
                     {"character_id": DONNCHAD, "titles": [{"title_id": 473, "tier": 3}, {"title_id": 474, "tier": 2},
                                                           {"title_id": 513, "tier": 2}]}]}
    fc = PF.forecast_from_reads(ruler={"ruler_tier": 3}, law_names=["confederate_partition_succession_law",
                                                                    "male_preference_law"],
                                children=kids, sim=sim, primary_heir_id=BRIAN, held_counties=3)
    assert fc["younger_shares"][0]["takes"] == [473, 474, 513]
    assert fc["takes_limit"] == PF.TAKES_LIMIT
    # CONTROL: one title, no limit said
    sim["heirs"][1]["titles"] = [{"title_id": 474, "tier": 2}]
    fc = PF.forecast_from_reads(ruler={"ruler_tier": 3}, law_names=["confederate_partition_succession_law",
                                                                    "male_preference_law"],
                                children=kids, sim=sim, primary_heir_id=BRIAN, held_counties=3)
    assert "takes_limit" not in fc


# ── 6: the vassal join names the road no read records ─────────────────────────────────────────────────────────────

def test_6_a_join_with_no_cause_names_the_fealty_offer():
    N.reset()
    row = N.vassals_joined({"actor_id": MURCHAD, "realm_counties": 10, "date": "1091.10.09"},
                           {"actor_id": MURCHAD, "realm_counties": 13, "date": "1091.10.16"}, {})
    assert "cause_unread" in row and "offer_fealty_interaction" in row["or_his_offer"]
    # CONTROL: an offer of the seat's AI counted in the step is the cause, and the fealty words are not added
    row = N.vassals_joined({"actor_id": MURCHAD, "realm_counties": 10, "ai_vassal_offers_sent": [{"count": 0}]},
                           {"actor_id": MURCHAD, "realm_counties": 13, "date": "1091.10.17",
                            "ai_vassal_offers_sent": [{"count": 1, "last": "1091.10.12"}]}, {})
    assert row["cause"] == "your_ai_offered_vassalage" and "or_his_offer" not in row
    N.reset()
