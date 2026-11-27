"""The stress family rule and its news row, allies who cannot be called, reads that name a dead character, and the
prices shown before a hire or a declaration. A test named RED checks the behaviour each case below needs; a CONTROL or an unnamed one checks what stays as it was. Game files
are small stand-ins written to a temporary folder (CK3_GAME_DIR), shaped as the game's own. No game.

  1a the stress family rule named an option and no else on an event with no always-offered free option: when its
     option was not offered the game's AI picked, gold or not, and the reply did not say the event stayed exposed;
     an event whose options share keys got no rule at all though one free option had a key of its own; and the
     reply counted the events it left alone as "no_gold_option" whatever the reason
                                -> the else is the free option offered unless a condition holds; `exposed` names
                                   the events and the gold they can take; `not_guarded` names every event left
                                   alone and why; a free option with a key of its own is named
  1b an event the rule's else answered read "answered ... by the game's AI" in the purse, and its events_answered
     row sat on the news' second page
                                -> the purse row says your rule's else answered it, and `rules_answered` keeps one
                                   short row per rule-decided pick on the first page
  2  `query alliances` listed a vassal of the ruler as an ally, and three call_ally sends to him were refused by the
     game ("You cannot call Vassals as Allies in war")
                                -> the row is marked (relation, callable false, why); the send is refused first
  3  a marriage read naming a dead son answered actor_capital_unresolved, and the refused marriage send's pair
     reads called him "married"
                                -> a read or call naming a dead character says so first, before any other check
  4  the mercenary listing showed each price with no purse, no months of income and nothing of the goal's price;
     a declaration's reply gave the defeat's prestige and the words "a defeat also costs 3 years of income in gold",
     no number
                                -> the listing carries the purse and the next title's shortfall, each row its price
                                   in months of income; the declaration carries if_you_lose (loss_price)
"""
from __future__ import annotations

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
from test_seat_events import Table, TGw, actor  # noqa: E402

ME, VASSAL, ALLY, DEAD, BRIDE, HER_LIEGE = 65138, 36742, 38660, 36303, 30696, 29703

# 9101: two random-pair coping options (one free, one taking gold), and two closing options, one offered unless a
# condition holds and one only when it does (no option offered always). 9102: two options share one key (the first
# takes gold, the second does not), and the opt-out has a key of its own. 9103: no option at all.
STRESS = """namespace = stress_threshold
stress_threshold.9101 = {
\ttype = character_event
\toption = {
\t\tname = { trigger = { always = yes } text = stress_threshold.9101.a.reclusive.gain }
\t\ttrigger = { has_character_flag = stress_threshold_option_reclusive }
\t\tadd_trait = reclusive
\t\tai_chance = { base = 50 }
\t}
\toption = {
\t\tname = { trigger = { always = yes } text = stress_threshold.9101.b.profligate.gain }
\t\ttrigger = { has_character_flag = stress_threshold_option_profligate }
\t\tadd_trait = profligate
\t\tremove_short_term_gold = major_gold_value
\t\tai_chance = { base = 50 }
\t}
\toption = {
\t\tname = stress_threshold.9101.m
\t\ttrigger = { NAND = { has_trait = temperate is_cannibal_trigger = yes } }
\t\tadd_stress = 20
\t\tai_chance = { base = 0 }
\t}
\toption = {
\t\tname = stress_threshold.9101.n
\t\ttrigger = { has_trait = temperate is_cannibal_trigger = yes }
\t\ttrait = cannibal
\t\tai_chance = { base = 0 }
\t}
}
stress_threshold.9102 = {
\ttype = character_event
\toption = {
\t\tname = stress_threshold.9102.b
\t\ttrigger = { has_trait = comfort_eater }
\t\tremove_short_term_gold = minor_gold_value
\t\tai_chance = { base = 20 }
\t}
\toption = {
\t\tname = stress_threshold.9102.b
\t\ttrigger = { has_character_flag = stress_threshold_option_comfort_eater }
\t\tadd_trait = comfort_eater
\t\tai_chance = { base = 20 }
\t}
\toption = {
\t\tname = stress_threshold.9102.e
\t\tadd_stress = 40
\t\tai_chance = { base = 10 }
\t}
}
stress_threshold.9103 = {
\ttype = character_event
\thidden = yes
\timmediate = { add_stress = 1 }
}
stress_threshold.9104 = {
\ttype = character_event
\toption = {
\t\tname = stress_threshold.9104.a
\t\tadd_trait = journaller
\t}
\toption = {
\t\tname = stress_threshold.9104.b
\t\tadd_stress = 25
\t}
}
stress_threshold.9105 = {
\ttype = character_event
\toption = {
\t\tname = { trigger = { always = yes } text = stress_threshold.9105.a.improvident.gain }
\t\ttrigger = { has_character_flag = stress_threshold_option_improvident }
\t\tremove_short_term_gold = major_gold_value
\t}
\toption = {
\t\tname = { trigger = { always = yes } text = stress_threshold.9105.b.confider.gain }
\t\ttrigger = { has_character_flag = stress_threshold_option_confider }
\t\tadd_trait = confider
\t\tai_chance = { base = 30 }
\t}
\toption = {
\t\tname = { trigger = { has_two_stress_threshold_options = yes } text = stress_threshold.9105.endure }
\t\tname = { trigger = { has_two_stress_threshold_options = no } text = stress_threshold.9105.endure_no_option }
\t\tadd_stress = 40
\t}
}
"""
VALUES = """major_income_multiplier_value = 12
major_gold_min_value = 75
major_gold_max_value_static = 500
minor_income_multiplier_value = 3
minor_gold_min_value = 15
minor_gold_max_value_static = 150
"""


def _clear():
    for mod, names in ((SE, ("_EVENT_CACHE", "_NAMESPACE_EVENTS")), (FL, ("_FILES",)),
                       (EP, ("_ON_ACTIONS", "_SCRIPT_VALUES", "_DECISION_FILE", "_SCRIPTED_EFFECTS", "_DECISION_KEYS"))):
        for n in names:
            c = getattr(mod, n, None)
            if c is not None:
                c.clear()


@pytest.fixture
def game(tmp_path, monkeypatch):
    for rel, text in {"events/stress_events/stress_test.txt": STRESS, "common/script_values/values.txt": VALUES,
                      "common/scripted_effects/none.txt": ""}.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    monkeypatch.setenv("CK3_GAME_DIR", str(tmp_path))
    _clear()
    news.reset()
    yield tmp_path
    _clear()
    news.reset()


@pytest.fixture(autouse=True)
def _fresh():
    news.reset()
    yield
    news.reset()


def run(c):
    return asyncio.run(c)


def _family(game_gw=None):
    gw = game_gw or TGw(Table())
    env = run(SE.do_event_rule(gw, {"op": "set", "event": "stress_threshold.*", "answer": "decline"}, 0, actor()))
    sets = {p["event"]: p for a, p in gw.writes if a == "set_event_rule" and p.get("op") == "set"}
    return env, sets


# ── 1a: the family rule's else, its exposure and the events it leaves alone ──────────────────────────────────────────

def test_1a_RED_with_no_option_offered_always_the_else_is_the_one_offered_unless(game):
    env, sets = _family()
    assert env["verdict"] == "applied", env
    s = sets["stress_threshold.9101"]
    assert s["option"] == "stress_threshold.9101.a.reclusive.gain"
    assert s["else"] == "stress_threshold.9101.m"


def test_1a_RED_an_event_left_exposed_is_named_with_the_gold_it_can_take(game):
    env, _ = _family()
    res = env["result"]
    assert res["exposed"] == ["stress_threshold.9101"]
    row = next(r for r in res["guarded"] if r["event"] == "stress_threshold.9101")
    ex = row["exposed"]
    assert ex["when"] == SE.EXPOSED_WHEN
    assert ex["named_offered"]["stress_threshold.9101.m"].startswith("unless: NAND")
    assert ex["named_offered"]["stress_threshold.9101.a.reclusive.gain"].startswith("if: ")
    (g,) = ex["gold_options"]
    assert g["option"] == "stress_threshold.9101.b.profligate.gain" and "12 months of income" in g["takes"]
    assert res["rule_holds"] == SE.RULE_HOLDS


def test_1a_CONTROL_an_event_with_an_option_offered_always_is_not_exposed(game):
    env, sets = _family()
    s = sets["stress_threshold.9105"]
    # the free option the AI weighs highest, the opt-out (a name in two blocks: its first) as else
    assert (s["option"], s["else"]) == ("stress_threshold.9105.b.confider.gain", "stress_threshold.9105.endure")
    row = next(r for r in env["result"]["guarded"] if r["event"] == "stress_threshold.9105")
    assert "exposed" not in row


def test_1a_RED_a_free_option_with_a_key_of_its_own_is_named_beside_shared_keys(game):
    _env, sets = _family()
    s = sets["stress_threshold.9102"]
    # .b names the gold option first (the bridge takes the first key that matches): never named
    assert s["option"] == "stress_threshold.9102.e" and "else" not in s


def test_1a_RED_every_event_left_alone_is_named_with_why(game):
    env, sets = _family()
    ng = env["result"]["not_guarded"]
    assert (ng["count"], ng["of"]) == (2, 5)
    assert ng["events"]["no_options"]["events"] == ["stress_threshold.9103"]
    assert ng["events"]["no_gold_option"]["events"] == ["stress_threshold.9104"]
    assert ng["events"]["no_options"]["why"] == SE.NOT_GUARDED_WHY["no_options"]
    assert env["result"]["no_gold_option"] == 1
    assert set(sets) == {"stress_threshold.9101", "stress_threshold.9102", "stress_threshold.9105"}


def test_1a_the_offer_classes_are_read_from_the_trigger():
    assert SE.offered_when([("name", "x")]) == "always"
    assert SE.offered_when([("trigger", [("NAND", [("a", "yes")])])]) == "unless"
    assert SE.offered_when([("trigger", [("NOT", [("a", "yes")]), ("b", "yes")])]) == "if"
    assert SE.offered_when([("trigger", [("has_character_flag", "f")])]) == "if"


def test_1a_the_rule_format_holds_one_option_and_one_else():
    """The reply's RULE_HOLDS rests on the bridge's rule record: one option key and one else key."""
    h = HERE.parents[1] / "bridge" / "include" / "event_rules.h"
    if not h.is_file():
        pytest.skip("no bridge sources in this layout")
    text = h.read_text()
    body = text[text.index("struct Rule {"):text.index("};", text.index("struct Rule {"))]
    assert "std::string option_key;" in body and "std::string else_key;" in body
    assert "std::vector" not in body


# ── 1b: who answered, in the purse and on the news' first page ──────────────────────────────────────────────────────

ELSE_PICK = {"seq": 7, "date": "1083.10.02", "character_id": ME, "event": "stress_threshold.9105",
             "options": ["stress_threshold.9105.a.improvident.gain", "stress_threshold.9105.b.confider.gain",
                         "stress_threshold.9105.endure"],
             "ai_pick": {"index": 0, "key": "stress_threshold.9105.a.improvident.gain"},
             "applied": {"index": 2, "key": "stress_threshold.9105.endure"},
             "rule": "option option=stress_threshold.9105.b.confider.gain else=stress_threshold.9105.endure",
             "why": "option_not_available", "answered_by": "event_rule_else",
             "wanted": {"index": 1, "key": "stress_threshold.9105.b.confider.gain"}, "gold": 106.0}


def test_1b_RED_the_purse_row_says_your_rules_else_answered_it(tmp_path, monkeypatch):
    # an event the files do not define: the purse gives the window's fall to the pick
    (tmp_path / "events").mkdir()
    monkeypatch.setenv("CK3_GAME_DIR", str(tmp_path))
    _clear()
    row = SE.pick_row(ELSE_PICK)
    prev = {"date": "1083.09.01", "gold": 120.0, "monthly_income": 1.0}
    snap = {"date": "1083.11.01", "gold": 90.0}
    (p,) = news.ai_pick_gold(prev, snap, [row])
    assert "by your rule's else option" in p["what"] and "by the game's AI" not in p["what"], p
    assert "stress_threshold.9105.b.confider.gain was not taken: option_not_available" in p["what"]


def test_1b_CONTROL_a_pick_no_rule_took_is_the_game_ais():
    assert news.answered_by_words({"rule": None}) == "by the game's AI"
    ai = news.answered_by_words({"rule": "option option=x", "rule_wanted": "x", "why": "option_not_available",
                                 "applied_by": "the game's AI (your rule's else: ai)"})
    assert ai.startswith("by the game's AI (your rule's x was not taken")


def test_1b_RED_a_rule_decided_pick_has_a_first_page_row(game):
    applied = dict(ELSE_PICK, seq=8, answered_by=None, applied=ELSE_PICK["wanted"], why="applied",
                   wanted=None)
    out = SE.news_part({"event_picks": []}, {"event_watched": True, "event_picks": [ELSE_PICK, applied]})
    assert out["rules_answered"] == [
        {"date": "1083.10.02", "event": "stress_threshold.9105", "applied": "stress_threshold.9105.endure",
         "by": "your rule's else option", "rule_wanted": "stress_threshold.9105.b.confider.gain",
         "why": "option_not_available"},
        {"date": "1083.10.02", "event": "stress_threshold.9105", "applied": "stress_threshold.9105.b.confider.gain",
         "by": "your rule"}]
    assert "news.rules_answered" in reply_fit.SPECS["observe"]["keep"]


def test_1b_CONTROL_a_pick_no_rule_took_has_no_rule_row(game):
    out = SE.news_part({"event_picks": []}, {"event_watched": True,
                                             "event_picks": [dict(ELSE_PICK, rule=None, answered_by=None)]})
    assert "rules_answered" not in out and out["events_answered"]


def test_1b_RED_an_option_named_in_blocks_is_found_for_its_gold(game):
    # the purse gives an answered option no fall when its file says it moves no gold
    assert EP.option_gold("stress_threshold.9105", "stress_threshold.9105.b.confider.gain") is False
    assert EP.option_gold("stress_threshold.9105", "stress_threshold.9105.a.improvident.gain") is True


# ── 2: a vassal among the alliances ─────────────────────────────────────────────────────────────────────────────────

def _chars(p):
    cid = p.get("character_id")
    rows = {ME: {"id": ME, "alive": True, "liege_id": 0, "top_liege_id": ME},
            VASSAL: {"id": VASSAL, "alive": True, "liege_id": ME, "top_liege_id": ME},
            ALLY: {"id": ALLY, "alive": True, "liege_id": 0, "top_liege_id": ALLY},
            DEAD: {"id": DEAD, "name": "Brian", "alive": False, "spouses": [{"id": 36614, "alive": True}],
                   "is_female": False},
            BRIDE: {"id": BRIDE, "alive": True, "is_female": True, "spouses": []}}
    return rows.get(cid, {"id": cid, "alive": True})


ALLIANCES = {"action": "query_alliances", "character_id": ME, "alliances": [
    {"ally_id": ALLY, "is_forced": False, "military_strength": 1582},
    {"ally_id": VASSAL, "is_forced": False, "military_strength": 606}],
    "total_ally_strength": 2188, "total_callable_strength": 3385}


def test_2_RED_a_vassal_among_the_alliances_is_marked_not_callable():
    gw = Gw({"query_character": _chars})
    out = run(R.alliance_rows(json.loads(json.dumps(ALLIANCES)), gw.read))
    a, v = out["alliances"]
    assert "relation" not in a and "callable" not in a
    assert v["relation"] == "your vassal" and v["callable"] is False
    assert R.CALL_ALLY_VASSAL_WORDS in v["why"]
    assert out["not_callable"]["ids"] == [VASSAL] and out["not_callable"]["strength"] == 606


def test_2_the_liege_is_marked_with_the_contract_rule():
    def chars(p):
        if p.get("character_id") == ME:
            return {"id": ME, "alive": True, "liege_id": ALLY, "top_liege_id": ALLY}
        return {"id": p.get("character_id"), "alive": True, "liege_id": 0, "top_liege_id": p.get("character_id")}
    out = run(R.alliance_rows({"character_id": ME, "alliances": [{"ally_id": ALLY}]}, Gw({"query_character": chars}).read))
    (row,) = out["alliances"]
    assert row["relation"] == "your liege" and "vassal_contract_liege_forced_war_override" in row["why"]
    assert "not_callable" not in out


def _war_gw():
    war = {"war_id": 268435463, "name": "Claim War", "primary_attacker": ME, "primary_defender": 31506,
           "attackers": [ME], "defenders": [31506]}
    return Gw({"query_character": _chars, "query_wars": {"scope": "all_active", "wars": [war]},
               "query_alliances": ALLIANCES})


def test_2_RED_a_call_to_a_vassal_is_refused_before_it_is_sent():
    gw = _war_gw()
    env = run(R._do_war_intent(gw, "call_ally", {"war_id": 268435463, "target_id": VASSAL}, ME, {"id": ME}))
    assert env["ok"] is False and env["blocked"]["gate"] == "interaction_invalid", env
    assert "is your vassal" in env["blocked"]["summary"] and R.CALL_ALLY_VASSAL_WORDS in env["blocked"]["summary"]
    assert env["blocked"]["measured"]["callable"] is False
    assert gw.writes == []


class _Sent(Exception):
    pass


def test_2_CONTROL_a_call_to_an_ally_reaches_the_send():
    gw = _war_gw()
    sent = []

    async def confirmed_write(action, params, **_kw):
        sent.append((action, params))
        raise _Sent()
    gw.confirmed_write = confirmed_write
    with pytest.raises(_Sent):
        run(R._do_war_intent(gw, "call_ally", {"war_id": 268435463, "target_id": ALLY}, ME, {"id": ME}))
    assert sent == [("send_interaction", {"interaction_type": "call_ally_interaction", "actor_id": ME,
                                          "target_id": ALLY, "war_id": 268435463})]


# ── 3: a dead character named by a read or a call ────────────────────────────────────────────────────────────────────

def test_3_RED_a_call_naming_a_dead_secondary_says_he_is_dead_first():
    gw = Gw({"query_character": _chars})
    env = run(R.do_impl(gw, intent="interact", character_id=ME,
                        args={"interaction": "arrange_marriage_interaction", "target_id": HER_LIEGE,
                              "secondary_actor_id": DEAD, "secondary_recipient_id": BRIDE}))
    b = env["blocked"]
    assert env["ok"] is False and b["gate"] == "character_dead", env
    assert f"Brian ({DEAD}) is dead (args.secondary_actor_id" in b["summary"]
    assert R.DEATH_DATE_UNREAD in b["summary"] and gw.writes == []


def test_3_RED_the_news_date_of_death_is_named_when_read():
    news.note_dead(DEAD, "1072.07.04")
    b = R.dead_character_blocked(DEAD, {"name": "Brian", "alive": False}, "args.ally_id")
    assert "your news read him dead on 1072.07.04" in b["summary"] and b["measured"]["read_dead_on"] == "1072.07.04"


def test_3_RED_the_pair_reads_say_dead_not_married():
    env = {"verdict": "rejected", "blocked": {"gate": "unmapped_refusal"}}
    gw = Gw({"query_character": _chars})
    run(R.marriage_refusal_reads(env, {"interaction": "arrange_marriage_interaction", "secondary_actor_id": DEAD,
                                       "secondary_recipient_id": BRIDE}, gw.read))
    bars = env["blocked"]["pair_bars"]
    assert bars[0].startswith(f"{DEAD} is dead")
    assert not any("is married" in x for x in bars), bars


def test_3_RED_the_bridges_not_living_refusal_is_character_dead():
    assert R.map_raw_reason("secondary_actor_not_living:36303")["gate"] == "character_dead"
    assert R.map_raw_reason("secondary_recipient_not_living:30696")["gate"] == "character_dead"


def test_3_RED_an_inspect_naming_a_dead_character_says_so():
    gw = Gw({"query_character": _chars})
    env = run(R.inspect_impl(gw, subject="marriage", character_id=DEAD))
    assert env["ok"] is False and env["blocked"]["gate"] == "character_dead", env


def test_3_CONTROL_the_profile_of_a_dead_character_still_reads():
    assert {"character", "memories", "dynasty"} <= R.INSPECT_DEAD_READ_OK


def test_3_every_do_argument_naming_a_character_is_checked():
    """Census: each do intent's accepted argument that names another character is in DEAD_CHECKED_ARGS."""
    import do_intent_args as DIA
    named = {"target_id", "secondary_actor_id", "secondary_recipient_id", "ally_id", "leader_id",
             "target_character_id", "councillor_id", "vassal_id", "enemy_id"}
    acc = DIA.accepted()
    want = {(i, k) for i, ks in acc.items() for k in ks if k in named}
    have = {(i, k) for i, ks in R.DEAD_CHECKED_ARGS.items() for k in ks}
    assert want and want <= have, sorted(want - have)


# ── 3 through the query tool ─────────────────────────────────────────────────────────────────────────────────────────

ms = pytest.importorskip("mcp_server")
from test_query_tool import bridge  # noqa: E402,F401  (the fake bridge fixture)


def test_3_RED_a_query_naming_a_dead_character_says_so_before_the_read(bridge):  # noqa: F811
    bridge.replies["query_character"] = lambda p: _chars(p)
    env = run(ms.query(domain="marriage_candidates", character_id=DEAD))
    assert env["ok"] is False and env["blocked"]["gate"] == "character_dead", env
    assert "query_marriage_candidates" not in bridge.actions()


def test_3_CONTROL_a_living_character_is_read(bridge):  # noqa: F811
    bridge.replies["query_character"] = lambda p: _chars(p)
    bridge.replies["query_marriage_candidates"] = {"candidates": []}
    run(ms.query(domain="marriage_candidates", character_id=BRIDE))
    assert "query_marriage_candidates" in bridge.actions()


def test_2_RED_the_alliances_domain_marks_the_vassal(bridge):  # noqa: F811
    bridge.replies["query_character"] = lambda p: _chars(p)
    bridge.replies["query_alliances"] = json.loads(json.dumps(ALLIANCES))
    env = run(ms.query(domain="alliances"))
    rows = {r["ally_id"]: r for r in env["result"]["alliances"]}
    assert rows[VASSAL]["callable"] is False and "callable" not in rows[ALLY]


# ── 4: prices against the purse ──────────────────────────────────────────────────────────────────────────────────────

def test_4_RED_the_mercenary_listing_carries_the_purse_and_months_of_income():
    gw = Gw({"query_hireable_mercenaries": {"companies": [
                {"company_id": 58, "label": "Mercenary company from Devon, 835 men", "troops": 835, "in_range": True,
                 "regiments": [{"maa_type": "levies", "men": 800}]}]},
             "query_character": lambda p: {"id": ME, "alive": True, "gold": 232.0, "ruler_tier": 2,
                                           "income_breakdown": {"total_monthly": 4.0}},
             "query_maa_types": {"types": []}, "query_realm": {"realm_total_counties": 6}})
    env = run(R._do_enumerate(gw, "mercenaries", 0, [], {"id": ME}, {}))
    (row,) = env["result"]["available"]
    assert row["months_of_income"] == round(row["price_estimate"] / 4.0, 1)          # on the estimate
    assert env["result"]["purse"] == {"gold": 232.0, "monthly_income": 4.0}


def test_4_RED_the_declaration_carries_the_price_of_losing():
    import test_defeat_prices as TD
    env, _acts = TD._declare(85.5)
    assert env["verdict"] == "applied"
    # the stand-in's character read carries no income: the price as loss_price gives it then
    assert env["result"]["if_you_lose"] == R.loss_price("claim_cb", None)
