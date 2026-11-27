"""Rows from the Carinthia pre-flight, a Munster seated run and the once pre-flight, replayed from its recorded replies.

  1  takes_title named the other son's duchy (a Carinthia test run)        one assignment: takes_title is the tier title in takes
  2  "never at the last step's net" after a 162.5-gold mint        the step's priced one-offs out of the net, said so
  3  change_law refused on price before the game's conditions      law_reach: the game's conditions first; listing filtered
  4  a grant to another liege's vassal refused with no words       the liege chain walked; a grant's later price said
  5  isolate_family_decision cost 350 prestige past the gold rule  in the spending preset's decision keys, effect in news
  7  "spares k_ireland alone again" beside `spared: []`            the list names the goal (rows in test_goal_chain_spending_veto)

Row 1 also re-runs the one computation over every forecast recorded in the evidence tree. No game; the game's files
are read where a row says so (skipped when they are not on this host).
"""
from __future__ import annotations

import asyncio
import copy
import gzip
import json
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
import seat_events as SE  # noqa: E402

BERTHOLD, HERMANN, BERTHOLD_JR, GEBHARD = 30331, 36345, 36857, 37151


def run(c):
    return asyncio.run(c)


# ── 1. the partition forecast ───────────────────────────────────────────────────────────────────

def _forecasts(o, parent=None):
    if isinstance(o, dict):
        if isinstance(o.get("younger_shares"), list):
            yield o, parent
        for v in o.values():
            yield from _forecasts(v, o)
    elif isinstance(o, list):
        for v in o:
            yield from _forecasts(v, parent)


def _map_titles(reply):
    """The per-title map rows recorded in the same reply (succession_plan carries both)."""
    def walk(o):
        if isinstance(o, dict):
            m = o.get("per_title_map")
            if isinstance(m, dict) and isinstance(m.get("titles"), list):
                return m["titles"]
            for v in o.values():
                got = walk(v)
                if got is not None:
                    return got
        elif isinstance(o, list):
            for v in o:
                got = walk(v)
                if got is not None:
                    return got
        return None
    return walk(reply)


def rederive(fc, titles, reconcile=PF.reconcile_tier_titles):
    """The recorded forecast put through the one assignment again: its tier titles (each son's takes_title and the
    untaken ones), the owners the per-title map names (else the recorded takes)."""
    fc = copy.deepcopy(fc)
    shares = [s["takes_title"] for s in fc["younger_shares"] if isinstance(s.get("takes_title"), dict)]
    shares += [t for t in fc.get("tier_shares_untaken") or [] if isinstance(t, dict)]
    young = {s.get("id") for s in fc["younger_shares"]}
    if titles:
        owner = {t.get("title_id"): t.get("heir_id") for t in titles if isinstance(t, dict) and t.get("heir_id") in young}
        for s in fc["younger_shares"]:
            s["takes"] = sorted(t for t, h in owner.items() if h == s.get("id")) or s.get("takes")
    else:
        owner = {t: s["id"] for s in fc["younger_shares"] for t in s.get("takes") or [] if isinstance(t, int)}
    reconcile(fc, shares, owner)
    return fc


def test_1_a_son_with_no_tier_title_left_stays_a_vassal():
    fc = {"younger_shares": [{"id": 1, "fate": "leaves_realm", "takes_title": {"title": "d_a", "title_id": 10,
                                                                               "held": True}, "takes": [20]}]}
    out = PF.reconcile_tier_titles(fc, [{"title": "d_a", "title_id": 10, "held": True}], {10: 99, 20: 1})
    (s,) = out["younger_shares"]
    assert "takes_title" not in s and s["fate"] == "stays_vassal" and s["why"] == PF.STAYS_BELOW_TIER


def test_1_a_created_duchy_off_the_map_keeps_its_place():
    fc = {"younger_shares": [{"id": 1, "fate": "leaves_realm", "takes": [469]}]}
    out = PF.reconcile_tier_titles(fc, [{"title": "d_meath", "title_id": 462, "created": True}], {469: 1})
    assert out["younger_shares"][0]["takes_title"]["title"] == "d_meath"
    assert "the law creates it" in out["younger_shares"][0]["why"]


# ── 2. the net a month without the step's one-offs ─────────────────────────────────────────────


def test_2_the_levy_upkeep_is_not_a_one_off(monkeypatch):
    monkeypatch.setattr(N, "_REALISED", {})
    monkeypatch.setattr(N, "_ACTS", {})
    N._resource_change({"actor_id": 8, "date": "1072.01.01", "gold": 100.0, "armies_men": 900},
                       {"actor_id": 8, "date": "1072.05.01", "gold": 80.0, "armies_men": 900}, "gold")
    assert "one_offs" not in N.realised_net(8)


def test_2_a_falling_purse_without_its_one_off_still_says_never(monkeypatch):
    rn = {"monthly": -50.0, "from": "1071.05.26", "to": "1071.09.23", "one_offs": {"total": -100.0, "rows": []},
          "monthly_without_one_offs": -2.0}
    mt = R.months_to_target(50, rn)
    assert mt["months"] is None and "its one-offs left out" in mt["why"]


# ── 3. the succession laws by the game's conditions ────────────────────────────────────────────

CARINTHIA_LAWS = ["crown_authority_0", "confederate_partition_succession_law", "male_preference_law"]
OPTIONS = ["partition_succession_law", "high_partition_succession_law", "single_heir_succession_law",
           "single_heir_succession_kurultai_law", "single_heir_succession_law_youngest", "single_heir_dynasty_house",
           "acclamation_succession_law", "clan_antagonistic_partition_succession_law", "city_succession_law",
           "bishop_theocratic_succession_law", "mercenary_company_succession_law"]


def test_3_carinthia_partition_names_the_game_reasons():
    why = LR.refusal_reasons("partition_succession_law", government="feudal", laws=CARINTHIA_LAWS, innovations=set())
    assert why and "innovation_hereditary_rule" in why[0] and "crown_authority_1" in why[1]


def test_3_carinthia_listing_keeps_none_of_another_governments_laws():
    kept, left = LR.listing(OPTIONS, current="confederate_partition_succession_law", government="feudal",
                            laws=CARINTHIA_LAWS, innovations=set())
    assert kept == []
    assert left["other_governments"] == 6
    assert {f["law"] for f in left["two_or_more_steps"]} == {
        "partition_succession_law", "high_partition_succession_law", "single_heir_succession_law",
        "single_heir_succession_law_youngest", "single_heir_dynasty_house"}


def test_3_one_step_away_is_listed_with_what_is_missing():
    kept, _left = LR.listing(OPTIONS, current="confederate_partition_succession_law", government="feudal",
                             laws=["crown_authority_1"], innovations={"innovation_hereditary_rule"})
    by = {k["law"]: k for k in kept}
    assert by["partition_succession_law"]["possible"] is True
    assert by["high_partition_succession_law"]["steps"] == 1 and "innovation_heraldry" in \
        by["high_partition_succession_law"]["missing"][0]
    assert "single_heir_succession_law" not in by            # primogeniture and a second crown authority level


def test_3_an_unread_government_keeps_every_row_unchecked():
    kept, left = LR.listing(OPTIONS, current=None, government=None, laws=[], innovations=None)
    assert len(kept) == len(OPTIONS) and left == {} and all("not_checked" in k for k in kept)
    assert LR.government_of(None, ["clan_government_law", "crown_authority_1"]) is None
    assert LR.government_of(None, ["tribal_authority_1"]) == "tribal"
    assert LR.government_of("feudal_government") == "feudal"


class LawGw:
    def __init__(self, prestige=0.0, laws=CARINTHIA_LAWS, gov="feudal_government", known=()):
        self.prestige, self.laws, self.gov, self.known = prestige, laws, gov, known

    def game(self):
        return {"date": "1066.09.29"}

    async def read(self, action, params=None):
        if action == "query_character":
            return {"id": BERTHOLD, "prestige": self.prestige, "culture_id": 40}
        if action == "query_realm_laws":
            return {"active_laws": [{"name": n} for n in self.laws]}
        if action == "query_maa_types":
            return {"government": self.gov}
        if action == "query_innovation_progress":
            return {"innovations": [{"name": n, "is_known": True} for n in self.known]}
        return {}


def _refused(gw, law):
    env = {"verdict": "rejected", "blocked": {"gate": "cannot_afford", "raw_reason": "not_allowed_or_unaffordable"}}
    run(R._explain_law_refusal(gw, env, law, BERTHOLD))
    return env["blocked"]


def test_3_the_refusal_gives_the_game_reasons_before_the_price():
    b = _refused(LawGw(prestige=0.0), "partition_succession_law")
    assert b["gate"] == "missing_prerequisite_law"
    assert b["summary"].index("innovation_hereditary_rule") < b["summary"].index("crown_authority_1")
    assert b["measured"]["prestige"] == {"cost": 500, "have": 0.0}


def test_3_CONTROL_conditions_met_and_short_of_prestige_is_the_price():
    b = _refused(LawGw(prestige=0.0, laws=["crown_authority_1", "confederate_partition_succession_law"],
                       known=("innovation_hereditary_rule",)), "partition_succession_law")
    assert b["gate"] == "insufficient_prestige"


def test_3_single_heir_dynasty_house_is_explained_too():
    b = _refused(LawGw(prestige=900.0), "single_heir_dynasty_house")
    assert b["gate"] == "missing_prerequisite_law" and "innovation_heraldry" in b["summary"]


# ── 4. grants ───────────────────────────────────────────────────────────────────────────────────

RUDOLF, EMPEROR = 1001, 36911


class GrantGw:
    # (id, liege, top liege, has_realm, dynasty, primary title)
    chars = {BERTHOLD: (BERTHOLD, EMPEROR, EMPEROR, True, 4235, 964),
             BERTHOLD_JR: (BERTHOLD_JR, RUDOLF, EMPEROR, True, 4235, 5),
             RUDOLF: (RUDOLF, EMPEROR, EMPEROR, True, None, 6),
             EMPEROR: (EMPEROR, 0, EMPEROR, True, None, 7),
             GEBHARD: (GEBHARD, BERTHOLD, EMPEROR, False, 4235, None),
             31010: (31010, BERTHOLD, EMPEROR, True, None, 965)}

    def game(self):
        return {"date": "1066.09.15"}

    async def read(self, action, params=None):
        if action == "query_character":
            c = self.chars.get(params["character_id"])
            if c is None:
                return {}
            return {"id": c[0], "liege_id": c[1], "top_liege_id": c[2], "has_realm": c[3], "dynasty_id": c[4],
                    "primary_title_id": c[5]}
        if action == "query_interaction_check":
            return {"result": {"results": [{"target_id": GEBHARD, "sendable": True,
                                            "engine_cost": {"prestige": 300.0, "renown": 300.0}}]}}
        return {}


def test_4_a_grant_to_another_lieges_vassal_is_refused_with_his_lieges():
    b = run(R._grant_guard(GrantGw(), BERTHOLD, BERTHOLD_JR))
    assert b is not None and b["raw_reason"] == "grant_recipient_not_in_realm"
    assert b["measured"]["liege_chain"] == [RUDOLF, EMPEROR] and "no words" in b["summary"]


def test_4_CONTROL_a_vassal_actors_own_vassal_passes():
    assert run(R._grant_guard(GrantGw(), BERTHOLD, 31010)) is None


def test_4_a_grant_to_a_landless_son_says_disinherit_will_cost_more():
    fx = run(R.grant_price_effects(GrantGw(), BERTHOLD, GEBHARD))
    assert fx["disinherit"]["now"] == {"prestige": 300.0, "renown": 300.0}
    assert fx["disinherit"]["once_landed"] == {"prestige": 900, "renown": 450}
    assert "300 prestige + 300 renown now, 900 prestige + 450 renown once he holds land" in fx["note"]


def test_4_CONTROL_no_note_for_a_landed_recipient():
    assert run(R.grant_price_effects(GrantGw(), BERTHOLD, BERTHOLD_JR)) is None


# ── 5. isolate_family_decision ─────────────────────────────────────────────────────────────────

def test_5_the_preset_drops_isolate_family_by_key():
    assert "isolate_family_decision" in SE.SPENDING_DECISION_KEYS
    assert len(SE.SPENDING_DECISION_KEYS) <= SE.DECISION_KEYS_MAX
    assert "-350 prestige" in SE.SPENDING_DECISION_KEYS["isolate_family_decision"]


@pytest.mark.skipif(SE.game_dir() is None, reason="the game's files are not on this host")
def test_5_its_news_row_names_the_effect():
    a = {"what": "decision", "key": "isolate_family_decision"}
    SE.decision_row_price(a, {"actor_id": 1})
    assert a.get("fires") and "isolating_modifier for 5 years" in a["effect"]
