"""The rules under test: a ledger row names a cause only where a record stands behind it, and a reply that prices
something prices it from the game's own files.

  1  A purse row names an interaction (the head of faith's gold) as a cause only when its letter is among the step's
     answered events. A gold rise beside a piety fall is no record: with the letters read and that letter not among
     them, the move stays `unexplained` and `ruled_out` names what was looked at. The letter an interaction's block
     sends while charging the receiver (request_excommunication_interaction: 500 piety) prices that block, and says
     who sent it.
  2  A war's end is priced from its casus belli's block for that ending, for the side the ruler fought on: a lost claim
     war is -10 x the war's prestige factor, which the engine sets and no file gives (no number stands for it); an
     attacker's win gives prestige experience, not prestige; a lost defensive independence war takes the block's 150.
     `if_you_lose` of a claim war says the same rule, never a number.
  3  An alliance row marks an ally the game's call_ally refuses whatever the war (one who holds no realm, a hegemon
     above the caller's rank, the caller's vassal) as not callable, with the game's words; an ally under 100 strength
     is marked by the call's is_shown; a call to one who holds no realm is refused before it is sent.
  4  A rule (or a standing answer) that names an option states what that option does, read from its event's file and
     the scripted effects it calls (gold, prestige, piety, county control, modifiers with their durations, titles),
     or which effects could not be read.
  5  A gold, prestige or piety row of 50 or more stays on the news' first page; a listing cut to fit says how many
     rows it cut, which, and the call that reads them; the mercenary listing is the cheapest first, and its months of
     income are given on both prices.

Game files are small stand-ins written to a temporary folder (CK3_GAME_DIR), shaped as the game's own. No game.
"""
from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import declare_gates as DG  # noqa: E402
import event_prices as EP  # noqa: E402
import faction_letters as FL  # noqa: E402
import factions as FA  # noqa: E402
import news  # noqa: E402
import redesign as R  # noqa: E402
import reply_fit  # noqa: E402
import seat_events as SE  # noqa: E402
from test_ireland_walk_replies import Gw  # noqa: E402
from test_seat_events import Table, TGw, actor  # noqa: E402

ME, POPE, RIVAL, COURTIER, VASSAL, ALLY, HEGEMON = 31762, 29012, 40001, 30695, 30822, 31266, 50001

INTERACTIONS = """request_excommunication_interaction = {
\tcategory = interaction_category_hostile
\ton_accept = {
\t\tif = {
\t\t\tlimit = { scope:secondary_recipient = { is_alive = yes } }
\t\t\tif = {
\t\t\t\tlimit = { scope:excommunication_hook = yes }
\t\t\t\tscope:actor = { use_hook = scope:recipient }
\t\t\t}
\t\t\tscope:actor = {
\t\t\t\tadd_piety = massive_piety_loss
\t\t\t\ttrigger_event = { id = religious_interaction.1022 }
\t\t\t}
\t\t\tscope:secondary_recipient = { trigger_event = { id = religious_interaction.1020 } }
\t\t}
\t}
\ton_decline = { scope:actor = { trigger_event = { id = religious_interaction.1023 } } }
}
hof_ask_for_gold_interaction = {
\ton_accept = {
\t\thof_ask_for_gold_interaction_effect = yes
\t\tscope:actor = { trigger_event = religious_interaction.3 }
\t}
\ton_decline = { scope:actor = { trigger_event = religious_interaction.32 } }
}
convert_at_war_interaction = {
\ton_accept = {
\t\tscope:actor = {
\t\t\ttrigger_event = religious_interaction.2500
\t\t\tconvert_at_war_interaction_effect = yes
\t\t}
\t}
}
"""
EFFECTS = """hof_ask_for_gold_interaction_effect = {
\tscope:recipient = { pay_short_term_gold = { target = scope:actor gold = scope:actor.head_of_faith_gold_value } }
\tscope:actor = {
\t\tif = { limit = { scope:hook = no } add_piety = major_piety_loss }
\t}
}
convert_at_war_interaction_effect = {
\tscope:actor = {
\t\tset_character_faith_with_conversion = scope:recipient.faith
\t\tadd_piety = medium_piety_loss
\t}
}
peasant_faction_demands_enforced = {
\tif = {
\t\tlimit = { scope:defender.top_liege.top_participant_group:dynastic_cycle ?= { participant_group_type = hegemon_ruler } }
\t\t$FACTION$ = { destroy_faction = yes }
\t}
\telse = {
\t\t$FACTION$ = {
\t\t\tevery_faction_county_member = {
\t\t\t\tchange_county_control = peasant_war_victory_county_control_loss
\t\t\t\tadd_county_modifier = { modifier = peasant_war_lost_county_modifier years = 10 }
\t\t\t}
\t\t\thidden_effect = { destroy_faction = yes }
\t\t}
\t}
}
"""
VALUES = """divorce_cost = 100
medium_piety_value = 100
major_piety_value = 250
massive_piety_value = 500
medium_prestige_value = 150
major_prestige_value = 350
"""
FACTION_VALUES = "peasant_war_victory_county_control_loss = -75\n"
MODIFIERS = """peasant_war_lost_county_modifier = {
\ticon = county_modifier_opinion_negative
\tcounty_opinion_add = 50
\tmonthly_county_control_growth_factor = -0.50
}
"""
EVENTS = """namespace = religious_interaction
religious_interaction.1022 = {
\ttype = letter_event
\toption = { name = religious_interaction.1000.positive }
}
religious_interaction.3 = {
\ttype = letter_event
\toption = { name = religious_interaction.3.a }
}
"""
DEMANDS = """namespace = faction_demand
faction_demand.1101 = {
\ttype = letter_event
\toption = {
\t\tname = "FACTION_DEMAND_PEASANT_ACCEPT"
\t\tpeasant_faction_demands_enforced = { FACTION = scope:faction }
\t}
\toption = {
\t\tname = "FACTION_DEMAND_PEASANT_REFUSE"
\t\tscope:faction = { faction_start_war = {} }
\t}
}
faction_demand.9001 = {
\ttype = letter_event
\toption = {
\t\tname = "FACTION_DEMAND_ACCEPT"
\t\tsome_engine_effect = yes
\t\tadd_gold = { value = 12 multiply = scope:x }
\t}
\toption = { name = "FACTION_DEMAND_REFUSE" }
}
"""
FACTION_TYPES = """peasant_faction = {
\tdemand = { trigger_event = faction_demand.1101 }
}
"""
CBS = """claim_cb = {
\ton_victory = {
\t\tmodify_all_participants_fame_values = {
\t\t\tWINNER = scope:attacker
\t\t\tLOSER = scope:defender
\t\t\tFAME_BASE = scope:cb_prestige_factor
\t\t\tIS_RELIGIOUS_WAR = no
\t\t\tWINNER_FAME_SCALE = 10
\t\t\tLOSER_FAME_SCALE = -10
\t\t}
\t}
\ton_white_peace = {
\t\tscope:attacker = { add_prestige = { value = scope:cb_prestige_factor multiply = -5.0 } }
\t}
\ton_defeat = {
\t\tscope:attacker = { pay_short_term_gold_reparations_effect = { GOLD_VALUE = 3 } }
\t\tmodify_all_participants_fame_values = {
\t\t\tWINNER = scope:defender
\t\t\tLOSER = scope:attacker
\t\t\tFAME_BASE = scope:cb_prestige_factor
\t\t\tIS_RELIGIOUS_WAR = no
\t\t\tWINNER_FAME_SCALE = 10
\t\t\tLOSER_FAME_SCALE = -10
\t\t}
\t}
}
independence_faction_war = {
\ton_victory = {
\t\tscope:attacker = { add_prestige = medium_prestige_value }
\t\tscope:defender = { add_prestige = { value = medium_prestige_value multiply = -1 } }
\t}
\ton_white_peace = { }
\ton_defeat = { scope:defender = { add_prestige = medium_prestige_value } }
}
"""


def _clear():
    for mod, names in ((SE, ("_EVENT_CACHE", "_NAMESPACE_EVENTS")), (FL, ("_FILES", "_LETTERS")),
                       (EP, ("_ON_ACTIONS", "_SCRIPT_VALUES", "_DECISION_FILE", "_SCRIPTED_EFFECTS", "_DECISION_KEYS",
                             "_VALUES", "_MODIFIERS", "_LETTERS", "_CB_BODIES"))):
        for n in names:
            c = getattr(mod, n, None)
            if c is not None:
                c.clear()


@pytest.fixture
def game(tmp_path, monkeypatch):
    for rel, text in {"common/character_interactions/00_religious_interactions.txt": INTERACTIONS,
                      "common/scripted_effects/00_effects.txt": EFFECTS,
                      "common/script_values/00_basic_values.txt": VALUES,
                      "common/script_values/00_faction_values.txt": FACTION_VALUES,
                      "common/modifiers/00_county_modifiers.txt": MODIFIERS,
                      "common/casus_belli_types/00_claim.txt": CBS,
                      "common/factions/00_factions.txt": FACTION_TYPES,
                      "events/religion_events/religious_interaction_events.txt": EVENTS,
                      "events/factions/faction_demands.txt": DEMANDS}.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    monkeypatch.setenv("CK3_GAME_DIR", str(tmp_path))
    _clear()
    news.reset()
    yield tmp_path
    _clear()
    news.reset()


@pytest.fixture
def no_files(tmp_path, monkeypatch):
    """A game folder with an events/ folder and nothing else: no file says anything."""
    (tmp_path / "events").mkdir()
    monkeypatch.setenv("CK3_GAME_DIR", str(tmp_path))
    _clear()
    news.reset()
    yield tmp_path
    _clear()
    news.reset()


def run(c):
    return asyncio.run(c)


# ── 1: the head of faith's gold needs its letter; an interaction's letter prices its block ──────────────────────────
# A step's reads: piety 894.24 -> 330.85, gold -170.72 -> -161.19, prestige
# 120.36 -> 54.35 over a year at 1.11 gold, 3.05 piety and 2.93 prestige a month; the step's answered events: the
# letter request_excommunication_interaction's on_accept sends (1085.03.21) and a birth.

def _step(events, picks_read=True):
    prev = {"actor_id": ME, "date": "1085.01.03", "gold": -170.72, "piety": 894.24, "prestige": 120.36,
            "monthly_income": 1.11, "monthly_piety": 3.05, "monthly_prestige": 2.93, "laws": [], "titles": {}}
    snap = dict(prev, date="1086.01.03", gold=-161.19, piety=330.85, prestige=54.35, event_watched=picks_read,
                event_picks=[] if picks_read else None)
    out = {"events_answered": events}
    return prev, snap, out


EXCOMMUNICATION_LETTER = {"date": "1085.03.21", "event": "religious_interaction.1022",
                          "applied": "religious_interaction.1000.positive", "rule": None, "gold_at_pick": -169.21}
BIRTH = {"date": "1085.04.18", "event": "birth.1003", "applied": "birth.1003.a", "rule": None, "gold_at_pick": -168.45}


def test_1_RED_no_head_of_faith_gold_without_its_letter(game):
    prev, snap, out = _step([BIRTH])
    piety = news._resource_change(prev, snap, "piety", out)
    gold = news._resource_change(prev, snap, "gold", out)
    for row in (piety, gold):
        assert not any("head of faith" in str(e.get("what")) for e in row.get("in_this_step") or []), row
    assert piety["unexplained"] < -500
    assert any(news.HOF_GOLD_LETTER in r and "not among" in r for r in piety["ruled_out"]), piety
    assert "not_read" in piety


def test_1_RED_the_excommunication_letter_prices_500_piety_sent_by_the_ai(game):
    prev, snap, out = _step([EXCOMMUNICATION_LETTER, BIRTH])
    piety = news._resource_change(prev, snap, "piety", out)
    (row,) = [e for e in piety["in_this_step"] if "request_excommunication_interaction" in e["what"]]
    assert row["expected"] == -500.0 and "massive_piety_loss" in row["basis"]
    assert row["sent_by"].startswith("the game's AI for your ruler")
    assert not any("head of faith" in str(e.get("what")) for e in piety["in_this_step"])
    # -563.39 less the income (36.57) and the 500: what is left is the step's own
    assert abs(piety["unexplained"] - (-563.39 - 36.57 + 500)) < 0.05


def test_1_CONTROL_the_head_of_faith_gold_is_named_with_its_letter(game):
    prev, snap, out = _step([{"date": "1085.03.21", "event": news.HOF_GOLD_LETTER, "applied": "religious_interaction.3.a",
                              "rule": None}])
    snap.update(gold=-50.0, piety=650.0)
    piety = news._resource_change(prev, snap, "piety", out)
    gold = news._resource_change(prev, snap, "gold", out)
    assert any(e.get("expected") == -250 and news.HOF_GOLD_LETTER in e["what"] for e in piety["in_this_step"])
    assert any(e.get("expected") == 100.0 for e in gold["in_this_step"])


def test_1_unread_letters_name_no_interaction_and_say_they_were_not_read(game):
    prev, snap, out = _step([], picks_read=False)
    out.pop("events_answered")
    piety = news._resource_change(prev, snap, "piety", out)
    assert not any("head of faith" in str(e.get("what")) for e in piety.get("in_this_step") or [])
    assert any("were not read" in x for x in piety["not_read"])


def test_1_a_spouse_seat_change_is_named_with_a_divorces_price_and_not_counted(game):
    prev, snap, out = _step([EXCOMMUNICATION_LETTER])
    out["council_changed"] = [{"seat": "councillor_spouse", "before": {"holder_id": 47792}, "now": {"holder_id": 47793}}]
    piety = news._resource_change(prev, snap, "piety", out)
    (row,) = [e for e in piety["in_this_step"] if "spouse seat" in e["what"]]
    assert row["if_a_divorce"] == -100.0 and "expected" not in row and "not read" in row["basis"]


def test_1_the_seats_own_interaction_is_named_as_sent_by_it(game):
    prev, snap, out = _step([{"date": "1080.09.30", "event": "religious_interaction.2500",
                              "applied": "religious_interaction.2500.a", "rule": None}])
    # sent before its letter (an act cannot send a letter dated before it)
    news.note_act(ME, "1080.09.25", "interact convert_at_war_interaction", cost_known=False, unpriced_kind="deferred")
    piety = news._resource_change(prev, snap, "piety", out)
    (row,) = [e for e in piety["in_this_step"] if "convert_at_war_interaction" in e["what"]]
    assert row["expected"] == -100.0 and row["sent_by"].startswith("you (your act interact convert_at_war")


def _creation_step(records):
    news.note_creation_price(ME, "d_meath", "gold", 212.5, "1066.09.15")
    prev = {"actor_id": ME, "date": "1073.11.27", "gold": 18.56, "monthly_income": 4.6, "prestige": 50.0}
    snap = dict(prev, date="1074.02.08", gold=-185.81, prestige=360.0,
                **({"ai_veto_records": records} if records is not None else {}))
    out = {"titles_gained": [{"key": "d_meath", "title_id": 462, "how": None}]}
    return prev, snap, out


def test_1_RED_a_title_gained_with_the_ais_posts_read_and_no_creation_names_no_ai_creation(game):
    prev, snap, out = _creation_step([])
    gold = news._resource_change(prev, snap, "gold", out)
    (row,) = [e for e in gold["in_this_step"] if "d_meath" in e["what"]]
    assert "expected" not in row and "holds no creation" in row["not_created_by_the_ai"], row
    prestige = news._resource_change(prev, snap, "prestige", out)
    (prow,) = [e for e in prestige["in_this_step"] if "d_meath" in e["what"]]
    assert "expected" not in prow


def test_1_CONTROL_without_that_record_the_price_match_still_reads_as_a_creation(game):
    prev, snap, out = _creation_step(None)
    gold = news._resource_change(prev, snap, "gold", out)
    (row,) = [e for e in gold["in_this_step"] if "d_meath" in e["what"]]
    assert row["expected"] == -212.5


# ── 2: a war's end, by its casus belli's block ──────────────────────────────────────────────────────────────────────

def test_2_RED_a_lost_claim_war_names_the_factor_rule_and_no_number(game):
    w = {"_cb": "claim_cb", "my_side": "attacker", "outcome": "defender_victory"}
    row = news.war_end_prestige(w)
    assert "expected" not in row and "-10 x the war's prestige factor" in row["basis"], row
    lp = R.loss_price("claim_cb", 5.79)
    assert isinstance(lp["prestige"], str) and "-10 x" in lp["prestige"] and "no file" in lp["prestige_not_read"]
    assert abs(lp["gold"] - (-3 * 12 * 5.79)) < 0.1


def test_2_RED_an_attackers_claim_win_is_prestige_experience_not_prestige(game):
    assert news.war_end_prestige({"_cb": "claim_cb", "my_side": "attacker", "outcome": "attacker_victory"}) \
        ["expected"] == 0
    assert DG.outcome_price("claim_cb")["win"] == 0


def test_2_RED_a_lost_defensive_independence_war_takes_150(game):
    row = news.war_end_prestige({"_cb": "independence_faction_war", "my_side": "defender",
                                 "outcome": "attacker_victory"})
    assert row["expected"] == -150.0 and "for the defender" in row["basis"]


def test_2_CONTROL_without_the_files_the_claim_row_still_carries_no_number(no_files):
    op = DG.outcome_price("claim_cb")
    assert isinstance(op["defeat"], str) and "unread" in op["defeat"]
    assert DG.outcome_price("vassalization_cb")["defeat"] == -350


# ── 3: alliance rows and the call ───────────────────────────────────────────────────────────────────────────────────

def _chars(p):
    cid = p.get("character_id")
    rows = {ME: {"id": ME, "alive": True, "liege_id": 0, "top_liege_id": ME, "has_realm": True, "ruler_tier": 3,
                 "house_id": 7},
            COURTIER: {"id": COURTIER, "alive": True, "liege_id": 0, "top_liege_id": 0, "has_realm": False,
                       "house_id": 7},
            VASSAL: {"id": VASSAL, "alive": True, "liege_id": ME, "top_liege_id": ME, "has_realm": True},
            ALLY: {"id": ALLY, "alive": True, "liege_id": 0, "top_liege_id": ALLY, "has_realm": True, "ruler_tier": 3},
            HEGEMON: {"id": HEGEMON, "alive": True, "liege_id": 0, "top_liege_id": HEGEMON, "has_realm": True,
                      "ruler_tier": 6}}
    return rows.get(cid, {"id": cid, "alive": True})


ALLIANCES = {"action": "query_alliances", "character_id": ME, "alliances": [
    {"ally_id": ALLY, "military_strength": 900}, {"ally_id": COURTIER, "military_strength": 0},
    {"ally_id": VASSAL, "military_strength": 600}, {"ally_id": HEGEMON, "military_strength": 9000},
    {"ally_id": 40656, "military_strength": 40}]}


def test_3_RED_a_non_ruler_ally_and_the_other_refused_ones_are_marked():
    out = run(R.alliance_rows(json.loads(json.dumps(ALLIANCES)), Gw({"query_character": _chars}).read))
    rows = {r["ally_id"]: r for r in out["alliances"]}
    assert "callable" not in rows[ALLY]
    assert rows[COURTIER]["relation"] == "not a ruler" and rows[COURTIER]["callable"] is False
    assert R.CALL_ALLY_NOT_RULER_WORDS in rows[COURTIER]["why"] and "house_member" in rows[COURTIER]
    assert rows[VASSAL]["callable"] is False and rows[HEGEMON]["callable"] is False
    assert R.CALL_ALLY_HEGEMON_WORDS in rows[HEGEMON]["why"]
    assert rows[40656]["callable"] is False and "under 100" in rows[40656]["why"]
    assert sorted(out["not_callable"]["ids"]) == sorted([COURTIER, VASSAL, HEGEMON, 40656])


def _war_gw():
    war = {"war_id": 167772174, "name": "Independence War", "primary_attacker": 32422, "primary_defender": ME,
           "attackers": [32422], "defenders": [ME]}
    return Gw({"query_character": _chars, "query_wars": {"scope": "all_active", "wars": [war]},
               "query_alliances": ALLIANCES})


def test_3_RED_a_call_to_one_who_holds_no_realm_is_refused_before_it_is_sent():
    gw = _war_gw()
    env = run(R._do_war_intent(gw, "call_ally", {"war_id": 167772174, "target_id": COURTIER}, ME, {"id": ME}))
    assert env["ok"] is False and env["blocked"]["gate"] == "interaction_invalid", env
    assert R.CALL_ALLY_NOT_RULER_WORDS in env["blocked"]["summary"]
    assert env["blocked"]["measured"]["callable"] is False and env["blocked"]["measured"]["has_realm"] is False
    assert gw.writes == []


class _Sent(Exception):
    pass


def test_3_CONTROL_a_call_to_a_ruling_ally_reaches_the_send():
    gw = _war_gw()
    sent = []

    async def confirmed_write(action, params, **_kw):
        sent.append((action, params))
        raise _Sent()
    gw.confirmed_write = confirmed_write
    with pytest.raises(_Sent):
        run(R._do_war_intent(gw, "call_ally", {"war_id": 167772174, "target_id": ALLY}, ME, {"id": ME}))
    assert sent and sent[0][1]["target_id"] == ALLY


ms = pytest.importorskip("mcp_server")
from test_query_tool import bridge  # noqa: E402,F401  (the fake bridge fixture)


def test_3_RED_the_batch_query_marks_the_allies_too(bridge):  # noqa: F811
    bridge.replies["query_character"] = lambda p: _chars(p)
    bridge.replies["query_alliances"] = json.loads(json.dumps(ALLIANCES))
    bridge.replies["query_wars"] = {"wars": []}
    env = run(ms.query(domains=["alliances", "wars"]))
    rows = {r["ally_id"]: r for r in env["result"]["alliances"]["alliances"]}
    assert rows[COURTIER]["callable"] is False and rows[COURTIER]["relation"] == "not a ruler"


# ── 4: what an option does ──────────────────────────────────────────────────────────────────────────────────────────

def test_4_RED_a_rule_accepting_a_peasant_demand_names_its_effects(game):
    gw = TGw(Table())
    env = run(SE.do_event_rule(gw, {"op": "set", "event": "faction_demand.1101", "answer": "option",
                                    "option": "FACTION_DEMAND_PEASANT_ACCEPT"}, 0, actor()))
    assert env["verdict"] == "applied", env
    eff = env["result"]["option_effects"]["FACTION_DEMAND_PEASANT_ACCEPT"]["effects"]
    ctrl = next(r for r in eff if r["effect"] == "change_county_control")
    assert ctrl["amount"] == -75.0 and "every_faction_county_member" in ctrl["on"] and "otherwise" in ctrl["when"]
    mod = next(r for r in eff if r["effect"] == "add_county_modifier")
    assert mod["for"] == "10 years" and mod["modifier_does"]["monthly_county_control_growth_factor"] == "-0.50"


def test_4_RED_effects_the_files_do_not_give_are_named(game):
    got = SE.named_option_effects("faction_demand.9001", "FACTION_DEMAND_ACCEPT")
    brief = got["FACTION_DEMAND_ACCEPT"]
    assert "some_engine_effect" in brief["not_read"]
    # an expression of names is written out as the file writes it (12 x scope:x), not "a value computed in play"
    assert next(r for r in brief["effects"] if r["effect"] == "add_gold")["value"] == "12 x scope:x"


def test_4_RED_the_standing_demand_answer_names_the_accepts_effects(game):
    eff = FA.demand_answer_effects({"factions": [{"type": "peasant_faction"}]}, "accept")
    assert any(r["effect"] == "change_county_control" for r in
               eff["faction_demand.1101"]["FACTION_DEMAND_PEASANT_ACCEPT"]["effects"])
    assert FA.demand_answer_effects({"factions": [{"type": "peasant_faction"}]}, "ai") is None


# ── 5: what a page and a cut leave out ──────────────────────────────────────────────────────────────────────────────

def _observe(purse_change):
    big = [{"id": i, "what": "x" * 400} for i in range(40)]
    # The blocks the news lays out before the purse (NEWS_FIRST) fill the first page to within a purse's size
    news_ = {"attackers_new": {"rows": big[:6]}, "wars_started": big[10:16], "factions_against_you": big[20:24],
             "purse": {"gold": {"was": -174.3, "now": -174.3 + purse_change, "change": purse_change,
                                "in_this_step": [{"what": "y" * 600}]},
                       "prestige": {"was": 10.0, "now": 12.0, "change": 2.0, "in_this_step": [{"what": "p" * 700}]},
                       "piety": {"was": 10.0, "now": 12.0, "change": 2.0}},
             "events_answered": big[30:40], "rules_answered": [{"event": "e", "by": "your rule"}]}
    return {"ok": True, "tool": "observe", "result": {"news": news_}, "warnings": [], "errors": []}


def test_5_RED_a_100_gold_fall_stays_on_the_first_page(monkeypatch):
    monkeypatch.setenv("CK3_REPLY_PAGES", "on")
    reply_fit.reset_pages()
    out = reply_fit.paginate(_observe(-100.51), "observe", {"depth": "news"}, cap=8192)
    n = out["result"]["news"]
    assert out["result"]["more"]["page"].endswith("of " + out["result"]["more"]["page"].split()[-1])
    assert n["purse"]["gold"]["change"] == -100.51
    # nothing is laid out twice: the small piety row is on some page, once
    pages = [out] + [reply_fit.next_page() for _ in range(len(reply_fit._STORES["observe:news"]["pages"]))]
    seen = [p["result"].get("news", {}).get("purse", {}).get("piety") for p in pages]
    assert sum(1 for x in seen if x) == 1
    assert sum(1 for p in pages if (p["result"].get("news", {}).get("purse") or {}).get("gold")) == 1


def test_5_CONTROL_a_small_purse_row_is_laid_out_as_before(monkeypatch):
    monkeypatch.setenv("CK3_REPLY_PAGES", "on")
    reply_fit.reset_pages()
    assert reply_fit.money_keep(_observe(-3.0)["result"]) == []
    assert reply_fit.money_keep(_observe(-100.51)["result"]) == ["news.purse.gold"]


def _companies(n=12):
    return {"companies": [{"company_id": 100 + i, "label": f"company {i}", "troops": 300 + 100 * ((i * 7) % 12),
                           "in_range": True, "regiments": [{"maa_type": "levies", "men": 300 + 100 * ((i * 7) % 12)}]}
                          for i in range(n)]}


def _merc_gw():
    return Gw({"query_hireable_mercenaries": _companies(),
               "query_character": lambda p: {"id": ME, "alive": True, "gold": 30.0, "ruler_tier": 3,
                                             "income_breakdown": {"total_monthly": 4.32}},
               "query_maa_types": {"types": []}, "query_realm": {"realm_total_counties": 6}})


def test_5_RED_the_mercenary_listing_is_cheapest_first_with_months_on_both_prices():
    env = run(R._do_enumerate(_merc_gw(), "mercenaries", 0, [], {"id": ME}, {}))
    rows = env["result"]["available"]
    prices = [r["price_estimate"] for r in rows]
    assert prices == sorted(prices)
    r = rows[0]
    # months_of_income is on the estimate, the price the game charges when nothing unread applies; the upper beside it
    assert r["months_of_income"] == round(r["price_estimate"] / 4.32, 1)
    assert r["months_of_income_at_upper"] == round(r["price_upper"] / 4.32, 1)
    basis = env["result"]["rows_read"]["months_basis"]
    assert "price_upper" in basis and "price_estimate" in basis and "months_basis" not in r


def test_5_RED_a_cut_listing_names_the_rows_cut_and_the_call_that_reads_them():
    env = run(R._do_enumerate(_merc_gw(), "mercenaries", 0, [], {"id": ME}, {}))
    for r in env["result"]["available"]:
        r["filler"] = "z" * 900
    out = reply_fit.fit(env, "do", {"action_class": "mercenaries"}, cap=8192)
    (lst,) = out["result"]["truncated"]["lists"]
    assert lst["cut"] == lst["of"] - lst["shown"] and lst["cut"] > 0
    shown = [r["company_id"] for r in out["result"]["available"] if "company_id" in r]
    assert lst["read_them"] == {"tool": "do", "args": {"action_class": "mercenaries", "candidates": lst["cut_ids"]}}
    assert not set(lst["cut_ids"]) & set(shown[:lst["shown"] - (1 if lst.get("last_row_cut") else 0)])
    # the call reads those companies
    again = run(R._do_enumerate(_merc_gw(), "mercenaries", 0, lst["cut_ids"], {"id": ME}, {}))
    assert sorted(r["company_id"] for r in again["result"]["available"]) == sorted(lst["cut_ids"])
