"""The rules under test: a ledger row's unexplained part is its change less every priced row of its step, and a reply
that prices or attributes something says only what the game's files and the step's own rows support.

  1  ONE function decides a purse row's unexplained part (news.unexplained_of): the change less every priced row of
     the step. A post the game's AI made for the ruler with a price (ai_acted) is a priced row and is subtracted
     whatever its amount; the income is subtracted on every row (a prestige row over 50 never reads the whole
     change as unexplained beside its income). Census: every function that writes an `expected` into a purse row
     is listed, and a step built for each kind of priced row reads unexplained = change - every priced row.
  2  A letter is attributed to the act that sent it, across steps (the recipient answers days after the send): an
     ask of the seat's this news answers with the letter, an own act of that interaction, a call the ruler's AI
     made; a statement that rules a cause out is derived from the same rows the reply carries (an acceptance
     letter among the step's events is an ally who joined), and an AI call the letter answered is priced by the
     ally's tier.
  3  An if / else_if / else chain is one of its branches, never their sum: its price is the possible totals (or the
     range), the largest the charge can come to, never the plain sum.
  4  What the files price is priced: an answered event's option that gives gold (the wedding's gift), a realm law's
     pass cost (none for a law with no pass_cost block); a letter several interactions send names them.
  5  An option's effects follow the scripted effects it calls (the event file's own too), name the events and
     on_actions it fires, what its tooltip shows done another way (a faction's war) and the truces it sets; a
     branch's condition is cut at a word, never inside one, its conditions joined by AND; a value the file writes as
     an expression is written out.
  6  A cut listing names every row it cut, on every path a cut takes (a second pass after the host's prefix and
     wrapper too), and shows no company without its price; the mercenary price counts the hirer's own perks'
     mercenary_hire_cost_mult, and its months of income are on the estimate.
  7  A ruler change's purse stays on the news' first page.
  8  An ally the reply's forced_allies list holds is is_forced in his alliance row too.
  9  A letter the game's script fires from other places too (an illness event) is "one of" its senders: no charge
     is counted for an interaction that may not have run.

Game files are small stand-ins written to a temporary folder (CK3_GAME_DIR), shaped as the game's own. The numbers
are those of recorded rows. No game.
"""
from __future__ import annotations

import ast
import asyncio
import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import event_prices as EP  # noqa: E402
import faction_letters as FL  # noqa: E402
import news  # noqa: E402
import redesign as R  # noqa: E402
import reply_fit  # noqa: E402
import seat_events as SE  # noqa: E402
from test_ireland_walk_replies import Gw  # noqa: E402

ME, ALLY, ALLY2 = 36303, 31146, 33738

INTERACTIONS = """call_ally_interaction = {
\ton_accept = {
\t\tcall_ally_interaction_effect = yes
\t\tscope:actor = { trigger_event = call_ally.0100 }
\t}
\ton_decline = { scope:actor = { trigger_event = call_ally.0101 } }
}
call_house_member_to_war_interaction = {
\ton_accept = {
\t\tcall_ally_interaction_effect = yes
\t\tscope:actor = { trigger_event = call_ally.0100 }
\t}
}
execute_prisoner_interaction = {
\ton_accept = {
\t\tscope:actor = {
\t\t\tadd_piety = medium_piety_loss
\t\t\trandom = { chance = 20 trigger_event = health.1001 }
\t\t}
\t}
}
arrange_marriage_interaction = {
\ton_accept = { marriage_interaction_on_accept_effect = yes }
}
"""
EFFECTS = """call_ally_interaction_effect = {
\tscope:target = {
\t\tif = {
\t\t\tlimit = { is_attacker = scope:actor }
\t\t\tscope:actor = {
\t\t\t\tif = { limit = { scope:recipient.primary_title.tier = tier_barony } add_prestige = -10 }
\t\t\t\telse_if = { limit = { scope:recipient.primary_title.tier = tier_county } add_prestige = minor_prestige_loss }
\t\t\t\telse_if = { limit = { scope:recipient.primary_title.tier = tier_duchy } add_prestige = medium_prestige_loss }
\t\t\t\telse_if = { limit = { scope:recipient.primary_title.tier = tier_kingdom } add_prestige = major_prestige_loss }
\t\t\t\telse_if = { limit = { scope:recipient.primary_title.tier = tier_empire } add_prestige = massive_prestige_loss }
\t\t\t}
\t\t}
\t}
}
marriage_interaction_on_accept_effect = {
\thidden_effect = { scope:actor = { trigger_event = marriage_interaction.0010 } }
}
"""
HEALTH_EFFECTS = """fall_ill_effect = { trigger_event = health.1001 }
"""
VALUES = """minor_prestige_value = 75
medium_prestige_value = 150
major_prestige_value = 350
massive_prestige_value = 750
medium_piety_value = 100
"""
GOLD_VALUES = """major_income_multiplier_value = 12
major_gold_min_value = 75
major_gold_max_value_static = 500
"""
EVENTS = """namespace = marriage_effect
marriage_effect.2001 = {
\toption = { name = marriage_effect.2001.a add_treasury_or_gold = major_treasury_or_gold_value }
\toption = { name = marriage_effect.2001.b add_prestige = major_prestige_value }
}
"""
ALLY_EVENTS = """namespace = call_ally
call_ally.0100 = { type = letter_event option = { name = call_ally.0100.a } }
"""
HEALTH_EVENTS = """namespace = health
health.1001 = { option = { name = health.1001.a } }
"""
DEMANDS = """namespace = faction_demand
scripted_effect faction_demand_independence_rejected_effect = {
\ttrigger_event = { on_action = on_faction_demand_rejected }
\tshow_as_tooltip = { scope:faction = { faction_start_war = {} } }
\tscope:faction = { every_faction_member = { add_truce_both_ways = { character = root years = 5 } } }
}
faction_demand.0001 = {
\ttype = letter_event
\toption = {
\t\tname = "FACTION_DEMAND_REFUSE"
\t\tfaction_demand_independence_rejected_effect = yes
\t\tscope:faction_leader = { trigger_event = faction_demand.0003 }
\t}
}
"""
CHAPLAIN = """namespace = court_chaplain_task
scripted_effect charge_effect = {
\tif = {
\t\tlimit = {
\t\t\tany_character_struggle = { involvement = involved is_struggle_parameter_active_secondary_county_trigger = { PARAMETER = claims_in_region_cost_prestige_not_gold_for_involved_parties COUNTY = scope:county } }
\t\t\tscope:county != root
\t\t\tscope:county.holder = { is_ai = yes has_trait = ambitious has_trait = brave }
\t\t}
\t\tadd_prestige = { value = $COST$ multiply = -1 }
\t}
\telse = { remove_short_term_gold = $COST$ }
}
court_chaplain_task.0301 = {
\toption = {
\t\tname = court_chaplain_task.0301.a
\t\tcharge_effect = { COST = scope:councillor.court_chaplain_fabricate_claim_county_cost }
\t\thidden_effect = { scope:county_holder = { trigger_event = court_chaplain_task.0311 } }
\t}
}
"""
ON_ACTIONS = "on_faction_demand_rejected = {}\n"
CBS = """independence_faction_war = {
\ton_defeat = { scope:defender = { add_prestige = medium_prestige_value } }
}
"""
LAWS = """border_wars_laws = {
\tdefault = top_liege_redirected_to_border_wars_law
\ttop_liege_redirected_to_border_wars_law = { modifier = { can_vassals_be_attacked = no } }
\ttop_liege_not_redirected_to_border_wars_law = { modifier = { can_vassals_be_attacked = yes } }
}
crown_authority = {
\tcrown_authority_1 = { pass_cost = { prestige = 250 } }
}
"""
PERKS = """soldiers_of_lesser_fortune_perk = {
\tgovernment_character_modifier = {
\t\tflag = government_is_landless_adventurer
\t\tinvert_check = yes
\t\tmercenary_hire_cost_mult = -0.3
\t}
}
"""


def _clear():
    for mod, names in ((SE, ("_EVENT_CACHE", "_NAMESPACE_EVENTS")), (FL, ("_FILES", "_LETTERS")),
                       (EP, ("_ON_ACTIONS", "_SCRIPT_VALUES", "_DECISION_FILE", "_SCRIPTED_EFFECTS", "_DECISION_KEYS",
                             "_VALUES", "_MODIFIERS", "_LETTERS", "_CB_BODIES", "_MULT_BY_KEY", "_FIRED_ELSEWHERE",
                             "_EFFECT_TEXT", "_LAWS"))):
        for n in names:
            c = getattr(mod, n, None)
            if c is not None:
                c.clear()


@pytest.fixture
def game(tmp_path, monkeypatch):
    for rel, text in {"common/character_interactions/00_interactions.txt": INTERACTIONS,
                      "common/scripted_effects/00_interaction_effects.txt": EFFECTS,
                      "common/scripted_effects/20_health_effects.txt": HEALTH_EFFECTS,
                      "common/script_values/00_basic_values.txt": VALUES,
                      "common/script_values/01_dynamic_values.txt": GOLD_VALUES,
                      "common/on_action/faction_on_actions.txt": ON_ACTIONS,
                      "common/laws/00_laws.txt": LAWS,
                      "common/casus_belli_types/00_faction_wars.txt": CBS,
                      "common/lifestyle_perks/00_martial_2_authority_tree_perks.txt": PERKS,
                      "events/marriage_effect_events.txt": EVENTS,
                      "events/interaction_events/alliance_interaction_events.txt": ALLY_EVENTS,
                      "events/health_events.txt": HEALTH_EVENTS,
                      "events/factions/faction_demands.txt": DEMANDS,
                      "events/councillor_task_events/court_chaplain_task_events.txt": CHAPLAIN}.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    monkeypatch.setenv("CK3_GAME_DIR", str(tmp_path))
    _clear()
    news.reset()
    yield tmp_path
    _clear()
    news.reset()


def run(c):
    return asyncio.run(c)


def _step(frm, to, key, was, now, monthly):
    prev = {"actor_id": ME, "date": frm, key: was,
            ("monthly_income" if key == "gold" else f"monthly_{key}"): monthly}
    snap = {"actor_id": ME, "date": to, key: now, "event_watched": True, "event_picks": []}
    return prev, snap


# ── 1. the ledger: one rule, every priced row ────────────────────────────────────────────────────────────────────
#: A recorded step (1089.09.22 -> 1090.09.22): prestige 1492.83 -> 1106.95 at 2.98 a month; the bridge recorded the
#: AI's adopt_special_succession_decision on 1090.07.04, priced -300 from its cost block. The reply read -421.61.
DECISION_ACTED = [{"date": "1090.07.04", "what": "decision", "key": "adopt_special_succession_decision",
                   "price": {"prestige": -300.0},
                   "price_basis": "the cost block of adopt_special_succession_decision (80_major_decisions.txt)"}]


def test_1a_a_priced_ai_decision_is_subtracted_whatever_its_amount(game):
    prev, snap = _step("1089.09.22", "1090.09.22", "prestige", 1492.83, 1106.95, 2.98)
    row = news._resource_change(prev, snap, "prestige", {"ai_acted": DECISION_ACTED})
    (d,) = [e for e in row["in_this_step"] if "adopt_special_succession_decision" in e["what"]]
    assert d["expected"] == -300.0 and "ai_acted" in d["what"]
    assert row["unexplained"] == pytest.approx(-385.88 - 35.73 + 300, abs=0.02)        # -121.61
    assert "matches_price" not in row


def test_1a_CONTROL_without_the_record_the_whole_fall_less_income_stays(game):
    prev, snap = _step("1089.09.22", "1090.09.22", "prestige", 1492.83, 1106.95, 2.98)
    row = news._resource_change(prev, snap, "prestige", {})
    assert row["unexplained"] == pytest.approx(-421.61, abs=0.02)


def test_1b_a_prestige_row_over_50_subtracts_its_income(game):
    """A recorded step: prestige 819.23 -> 871.83 (+52.6) at 3.98 a month over 365 days (income 47.72). The reply read
    "unexplained": 52.6, the whole change."""
    prev, snap = _step("1082.03.03", "1083.03.03", "prestige", 819.23, 871.83, 3.98)
    row = news._resource_change(prev, snap, "prestige", {})
    assert row["income"]["expected"] == pytest.approx(47.72, abs=0.01)
    assert "unexplained" not in row and "ruled_out" not in row          # 4.88 left: under the floor


def test_1b_CONTROL_a_move_the_income_does_not_cover_stays_unexplained(game):
    prev, snap = _step("1082.03.03", "1083.03.03", "prestige", 819.23, 951.83, 3.98)
    row = news._resource_change(prev, snap, "prestige", {})
    assert row["unexplained"] == pytest.approx(132.6 - 47.72, abs=0.02) and row["ruled_out"]


def _expected_writers() -> set:
    tree = ast.parse((HERE / "news.py").read_text())
    out = set()
    for fn in ast.walk(tree):
        if not isinstance(fn, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        for n in ast.walk(fn):
            if (isinstance(n, ast.Dict) and any(isinstance(k, ast.Constant) and k.value == "expected" for k in n.keys)) \
                    or (isinstance(n, ast.Subscript) and isinstance(n.ctx, ast.Store)
                        and isinstance(n.slice, ast.Constant) and n.slice.value == "expected") \
                    or (isinstance(n, ast.Call) and any(k.arg == "expected" for k in n.keywords)):
                out.add(fn.name)
    return out


def test_1c_census_every_function_that_writes_an_expected_is_a_listed_producer():
    """Read from the code: a new producer of a priced row is listed (and then covered below) or this reds."""
    found = _expected_writers()
    listed = set(news.PRICED_ROW_PRODUCERS) | set(news.NOT_PURSE_EXPECTED)
    assert found == listed, {"not listed": sorted(found - listed), "listed, not found": sorted(listed - found)}


def _priced(row: dict) -> float:
    return round(sum(e["expected"] for e in row.get("in_this_step") or []
                     if isinstance(e.get("expected"), (int, float)) and not e.get("counted_in"))
                 + sum(a.get(k, 0) for a in row.get("your_acts") or [] for k in ("gold", "prestige", "piety")
                       if isinstance(a.get(k), (int, float)))
                 + ((row.get("income") or {}).get("expected") or 0)
                 + sum(v for k, v in (row.get("matches_price") or {}).items() if k in ("gold", "prestige", "piety")
                       and isinstance(v, (int, float))), 2)


def _cases(game_dir) -> list:
    """(producer, key, prev, snap, out_events, the row's `what` it adds) for each kind of priced row."""
    yr = ("1080.01.01", "1081.01.01")
    cases = []
    p, s = _step(*yr, "prestige", 1000.0, 400.0, 1.0)
    cases.append(("ai_acted_rows", "prestige", p, s, {"ai_acted": [dict(DECISION_ACTED[0], date="1080.06.01")]},
                  "adopt_special_succession_decision"))
    p, s = _step(*yr, "gold", 300.0, 100.0, 1.0)
    cases.append(("_events_for", "gold", p, s, {"ai_acted": [{"date": "1080.06.01", "what": "men_at_arms",
                                                              "key": "bowmen", "price": {"gold": 120.0}}]},
                  "men-at-arms"))
    p, s = _step(*yr, "gold", 100.0, 290.0, 3.0)
    cases.append(("event_gold_rows", "gold", p, s, {"events_answered": [
        {"event": "marriage_effect.2001", "applied": "marriage_effect.2001.a", "date": "1080.05.01"}]},
                  "marriage_effect.2001"))
    p, s = _step(*yr, "prestige", 100.0, 500.0, 1.0)
    cases.append(("_event_rows", "prestige", p, s, {"events_answered": [
        {"event": "marriage_effect.2001", "applied": "marriage_effect.2001.b", "date": "1080.05.01"}]},
                  "marriage_effect.2001"))
    p, s = _step(*yr, "prestige", 500.0, 600.0, 1.0)
    cases.append(("war_end_prestige", "prestige", p, s, {"wars_ended": [
        {"war_id": 1, "name": "W", "outcome": "defender_victory", "my_side": "defender",
         "_cb": "independence_faction_war"}]},
                  "war ended"))
    p, s = _step(*yr, "prestige", 500.0, 300.0, 1.0)
    cases.append(("_events_for", "prestige", p, s, {"allies_joined": [
        {"war_id": 1, "ally_id": ALLY, "prestige": -75, "called_by": "you", "basis": "b"}]}, "joined"))
    p, s = _step(*yr, "prestige", 500.0, 300.0, 1.0)
    p["laws"], s["laws"] = [], ["top_liege_not_redirected_to_border_wars_law"]
    cases.append(("law_row", "prestige", p, s, {}, "realm law passed"))
    p, s = _step(*yr, "prestige", 500.0, 300.0, 1.0)
    p["truces"] = {40001: "1090.01.01"}
    cases.append(("_prestige_causes", "prestige", p, s, {"wars_started": [
        {"war_id": 2, "name": "W2", "my_side": "attacker", "defender_id": 40001}]}, "truce broken"))
    p, s = _step(*yr, "gold", 300.0, 150.0, 1.0)
    p["armies_men"], s["armies_men"] = 2000, 2000
    cases.append(("army_upkeep", "gold", p, s, {}, "armies raised"))
    p, s = _step(*yr, "gold", 300.0, 150.0, 1.0)
    cases.append(("ai_pick_gold", "gold", p, s, {"events_answered": [
        {"event": "marriage_effect.2001", "applied": "marriage_effect.2001.a", "date": "1080.06.01",
         "gold_at_pick": 300.0, "rule": None}]}, "by the game's AI"))
    p, s = _step(*yr, "gold", 300.0, 100.0, 1.0)
    cases.append(("_resource_change", "gold", p, s, {}, None))
    return cases


def test_1c_each_kind_of_priced_row_is_subtracted(game, monkeypatch):
    """For each kind: the row is in the step with its `expected`, and unexplained = change - every priced row (or no
    unexplained when what is left is under the floor). letter_rows, _law_decided, count_hires_in_step and
    count_constructions_in_step have their own steps in this file and in their modules' tests (2, 4 below)."""
    import truces as TR  # noqa: PLC0415
    monkeypatch.setattr(TR, "BREAK_PRICE", {"prestige": -150, "source": "stand-in"})
    covered = set()
    for producer, key, p, s, out, what in _cases(game):
        news.reset()
        row = news._resource_change(p, s, key, out)
        if what is not None:
            hits = [e for e in row.get("in_this_step") or [] if what in str(e.get("what")) and "expected" in e]
            assert hits, (producer, row)
        rest = round(row["change"] - _priced(row), 2)
        if "unexplained" in row:
            assert row["unexplained"] == rest, (producer, row)
        else:
            assert abs(rest) < max(1.0, 0.1 * abs(row["change"])), (producer, row)
        assert news.unexplained_of(row, key) == rest
        covered.add(producer)
    assert covered >= set(news.PRICED_ROW_PRODUCERS) - {"letter_rows", "_law_decided", "count_hires_in_step",
                                                       "count_constructions_in_step"}


def test_1c_a_hire_and_a_construction_added_after_the_purse_are_settled_by_the_same_rule():
    g = {"was": 300.0, "now": 50.0, "change": -250.0, "income": {"monthly": 1.0, "expected": 12.0},
         "unexplained": -262.0}
    news.count_hires_in_step(g, [{"company_id": 5, "price_estimate": 150.0}], {5})
    assert g["unexplained"] == news.unexplained_of(g, "gold") == -112.0
    news.count_constructions_in_step(g, [{"ordered_by": ME, "price_estimate": -112.0, "building": "b",
                                          "county": "c", "started": "x"}], ME, [])
    assert "unexplained" not in g and news.unexplained_of(g, "gold") == 0.0


# ── 2. letters: the act that sent them, across steps; a ruled-out derived from the same rows ──────────────────────
#: A recorded opening (1075.10.19 -> 1076.01.01): the seat sent call_ally to 29585 and 33738 on 1075.10.19, the turn
#: before; both acceptance letters came (1075.10.22, .25); the reply's letter rows said "the game's AI for your
#: ruler: no act of yours in this step sent it" beside asks_answered rows that named the same letters.
ASKS = [{"interaction": "call_ally_interaction", "target_id": 29585, "war_id": 7, "sent": "1075.10.19",
         "answer": "joined", "how": "the game's acceptance letter call_ally.0100 to your ruler on 1075.10.22"},
        {"interaction": "call_ally_interaction", "target_id": ALLY2, "war_id": 7, "sent": "1075.10.19",
         "answer": "joined", "how": "the game's acceptance letter call_ally.0100 to your ruler on 1075.10.25"}]
LETTERS = [{"event": "call_ally.0100", "date": "1075.10.22"}, {"event": "call_ally.0100", "date": "1075.10.25"}]
JOINED = [{"war_id": 7, "ally_id": 29585, "prestige": -75, "called_by": "you (do call_ally sent 1075.10.19)",
           "basis": "b"},
          {"war_id": 7, "ally_id": ALLY2, "prestige": -150, "called_by": "you (do call_ally sent 1075.10.19)",
           "basis": "b"}]


def test_2_a_letter_is_attributed_to_the_seats_call_that_asks_answered_names(game):
    prev, snap = _step("1075.10.19", "1076.01.01", "prestige", 1021.73, 712.94, 4.05)
    rows = news.letter_rows(prev, snap, {"events_answered": LETTERS, "asks_answered": ASKS,
                                         "allies_joined": JOINED}, "prestige")
    assert len(rows) == 2
    for r, ally in zip(rows, (29585, ALLY2)):
        assert r["sent_by"].startswith("you (your act do call_ally to " + str(ally)), r
        assert "ally-joined row" in r["counted_in"] and "expected" not in r and "if_its_conditions_held" not in r


def test_2_an_own_interaction_sent_in_an_earlier_step_is_its_letters_sender(game):
    news.note_act(ME, "1089.03.21", "interact call_ally_interaction", cost_known=False, unpriced_kind="deferred")
    prev, snap = _step("1089.03.25", "1089.06.01", "prestige", 1000.0, 900.0, 1.0)
    (r,) = news.letter_rows(prev, snap, {"events_answered": [{"event": "call_ally.0100", "date": "1089.03.29"}]},
                            "prestige")
    assert r["sent_by"].startswith("you (your act interact call_ally_interaction, sent 1089.03.21")


def test_2_CONTROL_with_no_act_and_no_ask_the_letter_is_the_ais(game):
    prev, snap = _step("1089.03.25", "1089.06.01", "prestige", 1000.0, 900.0, 1.0)
    (r,) = news.letter_rows(prev, snap, {"events_answered": [{"event": "call_ally.0100", "date": "1089.03.29"}]},
                            "prestige")
    assert r["sent_by"].startswith("the game's AI for your ruler")


#: A recorded opening (1089.11.21 -> 1090.01.15): the ruler's AI called ally 31146 into war 184549441 on 1089.12.04
#: (ai_acted), his acceptance letter came 1089.12.07, the war ended a win for the seat; the reply said `ruled_out:
#: "no ally joined a war you attack in"`, and the next openings priced the same ally's joins at -75 (a county).
AI_CALL = [{"date": "1089.12.04", "what": "ally_call", "ally_id": ALLY, "war_id": 184549441}]
AI_LETTER = [{"event": "call_ally.0100", "date": "1089.12.07"}]
WON = [{"war_id": 184549441, "name": "Athlone", "outcome": "attacker_victory", "my_side": "attacker"}]


def test_2_an_acceptance_letter_is_never_ruled_out_beside_it(game):
    prev, snap = _step("1089.11.21", "1090.01.15", "prestige", 1392.26, 1323.6, 2.11)
    ro = news._prestige_ruled_out(prev, snap, {"events_answered": AI_LETTER, "ai_acted": AI_CALL,
                                               "wars_ended": WON}, [])
    assert "no ally joined a war you attack in" not in ro


def test_2_CONTROL_with_no_letter_and_no_join_it_is_ruled_out(game):
    prev, snap = _step("1089.11.21", "1090.01.15", "prestige", 1392.26, 1323.6, 2.11)
    assert "no ally joined a war you attack in" in news._prestige_ruled_out(prev, snap, {"events_answered": []}, [])


def test_2_an_ai_call_its_letter_answered_is_priced_by_the_allys_tier(game):
    gw = Gw({"query_character": lambda p: {"id": p.get("character_id"), "ruler_tier": 2}})
    prev, snap = _step("1089.11.21", "1090.01.15", "prestige", 1392.26, 1323.6, 2.11)
    joined = run(news._joined_by_ai_call(gw, prev, snap, {"ai_acted": AI_CALL, "events_answered": AI_LETTER},
                                         {"wars_ended": WON}))
    assert [(j["ally_id"], j["prestige"]) for j in joined] == [(ALLY, -75)] and "ai_acted" in joined[0]["called_by"]
    out = {"events_answered": AI_LETTER, "ai_acted": AI_CALL, "wars_ended": WON, "allies_joined": joined}
    row = news._resource_change(prev, snap, "prestige", out)
    assert "unexplained" not in row, row                      # -68.66 = -75 + 6.24 income (0.1 left)
    (letter,) = [e for e in row["in_this_step"] if "call_ally.0100" in e["what"]]
    assert "ally-joined row" in letter["counted_in"] and "ai_acted" in letter["sent_by"]


def test_2_CONTROL_an_ai_call_no_letter_answered_adds_no_join(game):
    gw = Gw({"query_character": lambda p: {"id": p.get("character_id"), "ruler_tier": 2}})
    prev, snap = _step("1089.11.21", "1090.01.15", "prestige", 1392.26, 1323.6, 2.11)
    assert run(news._joined_by_ai_call(gw, prev, snap, {"ai_acted": AI_CALL, "events_answered": []},
                                       {"wars_ended": WON})) == []


# ── 3. an if / else_if chain is one of its branches ──────────────────────────────────────────────────────────────

def test_3_a_tier_chain_is_priced_as_one_of_never_the_sum(game):
    lc = EP.letter_charge("call_ally.0100", "prestige", "call_ally_interaction")
    assert lc["if_its_conditions_held"] == -750.0                       # the reply read -1,335
    assert lc["one_of"] == [-750.0, -350.0, -150.0, -75.0, -10.0, 0.0]


def test_3_CONTROL_independent_conditions_still_sum():
    rows = [{"amount": -10.0, "_branch": ((1, 0),)}, {"amount": -20.0, "_branch": ((2, 0),)}]
    assert EP.conditional_price(rows, set()) == {"if_its_conditions_held": -30.0}


# ── 4. what the files price ──────────────────────────────────────────────────────────────────────────────────────

def test_4_the_weddings_gold_option_is_priced(game):
    """A recorded step: gold 52.04 -> 130.06 at 3.02 a month; marriage_effect.2001.a answered 1089.04.06; the reply
    read +74.55 unexplained."""
    prev, snap = _step("1089.03.25", "1089.04.29", "gold", 52.04, 130.06, 3.02)
    row = news._resource_change(prev, snap, "gold", {"events_answered": [
        {"event": "marriage_effect.2001", "applied": "marriage_effect.2001.a", "date": "1089.04.06",
         "gold_at_pick": 52.28}]})
    (g,) = [e for e in row["in_this_step"] if "marriage_effect.2001" in e["what"]]
    assert g["expected"] == 75.0 and "unexplained" not in row


def test_4_a_law_with_no_pass_cost_block_costs_nothing_and_one_with_one_is_read(game):
    assert EP.law_pass_cost("top_liege_not_redirected_to_border_wars_law")["none"] is True
    assert EP.law_pass_cost("crown_authority_1")["prestige"] == -250.0
    row = news.law_row("top_liege_not_redirected_to_border_wars_law", {"date": "1080.01.01"}, {"date": "1081.01.01"})
    assert row["expected"] == 0 and "no pass_cost block" in row["basis"]


def test_4_a_letter_two_interactions_send_names_both_and_counts_neither_without_an_act(game):
    lc = EP.letter_charge("call_ally.0100", "prestige")
    assert lc["senders"] == ["call_ally_interaction on_accept", "call_house_member_to_war_interaction on_accept"]
    assert "amount" not in lc and "if_its_conditions_held" not in lc


# ── 5. an option's effects ───────────────────────────────────────────────────────────────────────────────────────

def test_5_a_refuse_that_calls_its_files_own_effect_names_the_war_truce_and_events(game):
    got = EP.option_effects_brief("faction_demand.0001", "FACTION_DEMAND_REFUSE")
    eff = {(r["effect"], r.get("value")) for r in got["effects"]}
    assert got["effects"] != "none: the option changes nothing the files name"
    assert ("faction_start_war", None) in eff and ("add_truce_both_ways", None) in eff
    assert ("trigger_event", "faction_demand.0003") in eff
    assert ("trigger_event", "on_action on_faction_demand_rejected") in eff
    war = next(r for r in got["effects"] if r["effect"] == "faction_start_war")
    assert war["shown_in_tooltip"] is True and "shown_in_tooltip" in got
    truce = next(r for r in got["effects"] if r["effect"] == "add_truce_both_ways")
    assert truce["for"] == "5 years"


def test_5_a_branchs_condition_is_cut_at_a_word_and_a_value_is_written_out(game):
    got = EP.option_effects_brief("court_chaplain_task.0301", "court_chaplain_task.0301.a")
    pr = next(r for r in got["effects"] if r["effect"] == "add_prestige")
    assert pr["value"] == "scope:councillor.court_chaplain_fabricate_claim_county_cost x -1"
    assert pr["when"].endswith(" ...") and "claims_in_region_cost_prestige_not_gold_" not in pr["when"].split()[-2:]
    words = pr["when"][:-4].split()
    assert all(w in CHAPLAIN.split() for w in words if w != "AND")    # every word whole, as the file writes it
    assert any(r["effect"] == "trigger_event" and r["value"] == "court_chaplain_task.0311" for r in got["effects"])


def test_5_conditions_of_one_block_are_joined_by_and():
    assert EP._cond_words([("", "scope:a"), ("!", "scope:b"), ("c", "d")]) == "scope:a != scope:b AND c = d"


# ── 6. listings: the cut is named on every path; the hire price ──────────────────────────────────────────────────

def _merc_reply(n: int, filler: int) -> dict:
    rows = [{"label": f"company {i}", "company_id": 100 + i, "price_estimate": 100.0 + i, "blob": "x" * filler,
             "do_call": {"tool": "do", "args": {"intent": "hire_merc", "args": {"company_id": 100 + i}}}}
            for i in range(n)]
    return {"ok": True, "result": {"action_class": "mercenaries", "available": rows, "unavailable": []}}


def test_6_a_second_pass_after_the_wrapper_keeps_the_cut_ids():
    """A recorded reply: 12 companies, cut in two passes (8,192, then 8,133 after the host's prefix and wrapper);
    the second pass's note said `cut: 6` with no ids."""
    env = _merc_reply(12, 600)
    first = reply_fit.fit(env, "do", {"action_class": "mercenaries"})
    second = reply_fit.fit(first, "do", {"action_class": "mercenaries"}, cap=reply_fit.CAP - 400)
    (lst,) = second["result"]["truncated"]["lists"]
    shown = {r["company_id"] for r in second["result"]["available"] if "not_shown" not in r}
    assert lst["cut"] == 12 - lst["shown"] and set(lst["cut_ids"]) == {100 + i for i in range(12)} - shown
    assert lst["read_them"]["args"]["candidates"] == lst["cut_ids"]
    assert all("price_estimate" in r for r in second["result"]["available"])        # no company without its price


def test_6_CONTROL_a_listing_under_the_cap_is_sent_as_it_is():
    env = _merc_reply(3, 10)
    assert reply_fit.fit(env, "do", {"action_class": "mercenaries"}) is env


def test_6_the_hirers_perk_lowers_the_price(game):
    """A recorded hire: company 58 (477 levies, 358 light footmen), a duke of 4 counties holding
    soldiers_of_lesser_fortune_perk, charged 121.0; the listing had read 166.73 (estimate) / 189.46 (upper)."""
    gw = Gw({"query_hireable_mercenaries": {"companies": [
                {"company_id": 58, "in_range": True, "troops": 835,
                 "regiments": [{"maa_type": "levies", "men": 477}, {"maa_type": "light_footmen", "men": 358}]}]},
             "query_character": {"id": ME, "ruler_tier": 3, "gold": 130.06,
                                 "income_breakdown": {"total_monthly": 3.14}},
             "query_realm": {"realm_total_counties": 4},
             "query_maa_types": {"types": [{"maa_type": "light_footmen", "recruit_cost": 45.0}]},
             "query_lifestyle": {"perks": ["soldiers_of_lesser_fortune_perk", "hard_rule_perk"]}})
    rows, _far, note = run(R.mercenary_rows(gw, ME))
    (r,) = rows
    assert r["price_estimate"] == pytest.approx(121.0, abs=0.5)
    assert r["months_of_income"] == pytest.approx(r["price_estimate"] / 3.14, abs=0.1)
    assert r["months_of_income_at_upper"] > r["months_of_income"] and "soldiers_of_lesser_fortune_perk" in note


def test_6_CONTROL_without_the_perk_the_rank_price_stands(game):
    gw = Gw({"query_hireable_mercenaries": {"companies": [
                {"company_id": 58, "in_range": True, "troops": 835,
                 "regiments": [{"maa_type": "levies", "men": 477}, {"maa_type": "light_footmen", "men": 358}]}]},
             "query_character": {"id": ME, "ruler_tier": 3, "gold": 130.06,
                                 "income_breakdown": {"total_monthly": 3.14}},
             "query_realm": {"realm_total_counties": 4},
             "query_maa_types": {"types": [{"maa_type": "light_footmen", "recruit_cost": 45.0}]},
             "query_lifestyle": {"perks": []}})
    (r,), _far, _note = run(R.mercenary_rows(gw, ME))
    assert r["price_estimate"] == pytest.approx(166.73, abs=0.5)


# ── 7. a ruler change's purse stays on page 1 ────────────────────────────────────────────────────────────────────

def test_7_a_ruler_changes_purse_is_kept():
    res = {"news": {"purse": {"new_ruler": {"from": 1, "to": 2, "gold": 208.55}}}}
    assert reply_fit.money_keep(res) == ["news.purse.new_ruler"]


def test_7_CONTROL_a_small_ordinary_purse_is_not_kept():
    res = {"news": {"purse": {"gold": {"was": 1.0, "now": 3.0, "change": 2.0}}}}
    assert reply_fit.money_keep(res) == []


# ── 8. one reply, one answer about a forced ally ─────────────────────────────────────────────────────────────────

def test_8_an_ally_listed_forced_is_forced_in_his_alliance_row(monkeypatch):
    async def rel(*_a, **_k):
        return None
    monkeypatch.setattr(R, "ally_relation", rel)
    reply = {"character_id": ME, "alliances": [{"ally_id": 47792, "is_forced": False},
                                               {"ally_id": 29585, "is_forced": False}],
             "forced_allies": [{"ally_id": 47792, "is_forced": True}]}
    out = run(R.alliance_rows(reply, Gw({}).read))
    a, b = out["alliances"]
    assert a["is_forced"] is True and "forced_allies" in a["forced_read"]
    assert b["is_forced"] is False and "forced_read" not in b                       # CONTROL: not listed forced


# ── 9. a letter other scripts fire too is "one of" ───────────────────────────────────────────────────────────────

def test_9_an_illness_letter_fired_elsewhere_counts_no_charge(game):
    lc = EP.letter_charge("health.1001", "piety")
    assert lc["senders"] == ["execute_prisoner_interaction on_accept"]
    assert lc["elsewhere"] == ["common/scripted_effects/20_health_effects.txt"] and "amount" not in lc
    prev, snap = _step("1090.03.01", "1090.06.01", "piety", 500.0, 400.0, 1.0)
    (r,) = news.letter_rows(prev, snap, {"events_answered": [{"event": "health.1001", "date": "1090.05.18"}]}, "piety")
    assert r["what"].startswith("letter health.1001 came on 1090.05.18: sent by execute_prisoner_interaction or by")
    assert "expected" not in r


def test_9_CONTROL_a_letter_one_interaction_alone_sends_is_priced(game, tmp_path):
    (game / "common" / "scripted_effects" / "20_health_effects.txt").write_text("fall_ill_effect = { }\n")
    _clear()
    lc = EP.letter_charge("health.1001", "piety")
    assert lc["amount"] == -100.0 and "senders" not in lc
