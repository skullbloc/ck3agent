"""Gold the game's AI pays through events and decisions, and four reads around a succession. A test named RED
checks the behaviour each case below needs; a CONTROL
or an unnamed one checks what stays as it was. The game files are small stand-ins written to a temporary folder
(CK3_GAME_DIR), shaped as the game's own.

  1  a pay_if rule with else=decline and no option: the else named no option of the event, so at fire time the
     game's AI picked, and it paid a three-option letter                -> the else is checked whatever the option
  2  a stress-relief decision priced "free (no cost block, fires no event)": its effect fires an on_action whose one
     event takes medium_gold_value                                      -> on_action events are followed, gold read
  3  each stress event fired once, each a new key, and the AI took the option that pays gold; a rule could be set
     only per key, after the fact                                       -> event="<namespace>.*" answer=decline
  4  the call offered after a pick that paid gold set the event's first option (a secret revealed)
                                                                        -> the family guard, or a free option
  5  the purse's event row is the window's fall only                    -> the option's own price from its file
  6  the ruler-change briefing said "no price read under your late ruler" beside his many reads
                                                                        -> per-ruler price reads, and why it moved
  7  counties_left_realm counted from the asking baseline's last read (an observe five years old: 7 -> 6, where the
     turn's news counted 9 -> 6)                                        -> the late ruler's newest read
  8  "declared by you ... before the next read listed it" when the turn's news had listed the war
  9  the levers that keep a duchy in the realm sat on page two of succession_plan
No game.
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

MURCHAD, BRIAN = 31762, 36303

STRESS = """namespace = stress_threshold
stress_threshold.9001 = {
\ttype = character_event
\toption = {
\t\ttrigger = { has_character_flag = opt_contrite }
\t\tname = { trigger = { can_be_contrite = yes } text = stress_threshold.9001.contrite.gain }
\t\tname = { trigger = { has_trait = contrite } text = stress_threshold.9001.contrite.effect }
\t\tai_chance = { base = 1 }
\t\tadd_trait = contrite
\t}
\toption = {
\t\ttrigger = { has_character_flag = opt_improvident }
\t\tname = { trigger = { can_be_improvident = yes } text = stress_threshold.9001.improvident.gain }
\t\tname = { trigger = { has_trait = improvident } text = stress_threshold.9001.improvident.effect }
\t\tai_chance = { base = 1 modifier = { add = 200 has_trait = improvident } }
\t\tadd_trait = improvident
\t\tremove_short_term_gold = major_gold_value
\t}
\toption = {
\t\ttrigger = { has_character_flag = opt_journaller }
\t\tname = { trigger = { can_be_journaller = yes } text = stress_threshold.9001.journaller.gain }
\t\tai_chance = { base = 5 }
\t\tadd_trait = journaller
\t}
\toption = {
\t\tname = stress_threshold.9001.endure
\t\tadd_stress = 25
\t\tai_chance = { base = 1 }
\t}
}
stress_threshold.9002 = {
\ttype = character_event
\toption = {
\t\tname = stress_threshold.9002.a
\t\tadd_trait = reclusive
\t}
\toption = {
\t\tname = stress_threshold.9002.b
\t\tadd_stress = 25
\t}
}
"""
LETTER = """namespace = ask_letter
ask_letter.0002 = {
\ttype = letter_event
\toption = {
\t\tname = ask_letter.0002.a
\t\tremove_short_term_gold = 50
\t\tadd_claim = yes
\t}
\toption = {
\t\tname = ask_letter.0002.b
\t\tremove_short_term_gold = 100
\t\tadd_claim = yes
\t}
\toption = {
\t\tname = ask_letter.0001.b
\t}
}
"""
COPING = """coping_test.0001 = {
\ttype = character_event
\toption = {
\t\tname = coping_test.0001.a
\t\ttrait = improvident
\t\tremove_short_term_gold = medium_gold_value
\t\tadd_stress = medium_stress_loss
\t}
}
"""
DECISION = """stress_loss_testy_decision = {
\tis_shown = { has_trait = improvident }
\tis_valid_showing_failures_only = { short_term_gold >= medium_gold_value }
\teffect = {
\t\tshow_as_tooltip = { remove_short_term_gold = medium_gold_value }
\t\ttrigger_event = { on_action = stress_loss_testy }
\t}
}
"""
ON_ACTION = """stress_loss_testy = {
\trandom_events = {
\t\t100 = coping_test.0001
\t}
}
"""
VALUES = """medium_income_multiplier_value = 6
medium_gold_min_value = 50
medium_gold_max_value_static = 300
major_income_multiplier_value = 12
major_gold_min_value = 75
major_gold_max_value_static = 500
"""


def _clear():
    # the caches of the files read (getattr: the same test runs on a tree without the newer ones)
    for mod, names in ((SE, ("_EVENT_CACHE", "_NAMESPACE_EVENTS")), (FL, ("_FILES",)),
                       (EP, ("_ON_ACTIONS", "_SCRIPT_VALUES", "_DECISION_FILE", "_SCRIPTED_EFFECTS", "_DECISION_KEYS"))):
        for n in names:
            c = getattr(mod, n, None)
            if c is not None:
                c.clear()


@pytest.fixture
def game(tmp_path, monkeypatch):
    files = {"events/stress_events/stress_test.txt": STRESS, "events/letters/ask.txt": LETTER,
             "events/stress_events/coping_test.txt": COPING, "common/decisions/stress_test.txt": DECISION,
             "common/on_action/stress_test.txt": ON_ACTION, "common/script_values/values.txt": VALUES,
             "common/scripted_effects/none.txt": ""}
    for rel, text in files.items():
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


# ── 1: an else the event does not have ──────────────────────────────────────────────────────────────────────────────

def test_1_RED_an_else_that_names_no_option_is_refused_with_the_free_one(game):
    gw = TGw(Table())
    env = run(SE.do_event_rule(gw, {"op": "set", "event": "ask_letter.0002", "answer": "pay_if", "currency": "gold",
                                    "amount": 400, "else": "decline"}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["raw_reason"] == "bad_else_option_key", env
    assert gw.writes == []
    assert env["result"]["instead"]["args"]["args"]["else"] == "ask_letter.0001.b"
    assert "the game's AI would pick instead" in env["blocked"]["summary"]


def test_1_an_else_of_the_event_still_sets(game):
    gw = TGw(Table())
    env = run(SE.do_event_rule(gw, {"op": "set", "event": "ask_letter.0002", "answer": "pay_if", "currency": "gold",
                                    "amount": 400, "else": "ask_letter.0001.b"}, 0, actor()))
    assert env["verdict"] == "applied", env
    assert gw.writes[0][1]["else"] == "ask_letter.0001.b"


def test_1_an_else_that_pays_gold_is_still_refused_with_the_free_one(game):
    gw = TGw(Table())
    env = run(SE.do_event_rule(gw, {"op": "set", "event": "ask_letter.0002", "answer": "pay_if", "currency": "gold",
                                    "amount": 75, "option": "ask_letter.0002.a", "else": "ask_letter.0002.b"},
                               0, actor()))
    assert env["blocked"]["raw_reason"] == "else_pays_gold" and gw.writes == []
    assert env["result"]["instead"]["args"]["args"]["else"] == "ask_letter.0001.b"


# ── 2: a decision that pays gold through its on_action ──────────────────────────────────────────────────────────────

def test_2_RED_a_decision_whose_on_action_event_takes_gold_is_not_free(game):
    p = EP.decision_price("stress_loss_testy_decision", monthly_income=4.28)
    assert not p.get("free"), p
    (fired,) = p["fires"]
    assert fired["event"] == "coping_test.0001" and fired["on_action"] == "stress_loss_testy"
    assert p["gold"] == -50.0 and "6 months of income, at least 50" in p["gold_basis"]
    row = {"what": "decision", "key": "stress_loss_testy_decision"}
    SE.decision_row_price(row, {"monthly_income": 4.28})
    assert row["price"]["gold"] == -50.0 and "free" not in json.dumps(row)


def test_2_the_gold_scale_by_income_and_its_floor(game):
    assert EP.gold_value("medium_gold_value", 4.28)["gold"] == 50.0            # 25.7 under the floor
    assert EP.gold_value("medium_gold_value", 10.1)["gold"] == 65.0            # 60.6 rounded up to 5
    assert "unknown" in EP.gold_value("medium_gold_value", 45.0)                # near the era's maximum: not guessed
    assert "unknown" in EP.gold_value("medium_gold_value", None)
    assert EP.gold_value("not_a_gold_value", 4.0) is None


def test_2_the_spending_preset_drops_both_stress_reliefs_that_pay_gold():
    assert {"stress_loss_improvident_decision", "stress_loss_profligate_decision"} <= set(SE.SPENDING_DECISION_KEYS)
    assert len(SE.SPENDING_DECISION_KEYS) <= SE.DECISION_KEYS_MAX
    assert "the stress relief that pays nothing still passes" in SE.SPENDING_DECISIONS


# ── 3: the family guard ─────────────────────────────────────────────────────────────────────────────────────────────

def test_3_RED_a_family_rule_guards_each_event_whose_options_take_gold(game):
    gw = TGw(Table())
    env = run(SE.do_event_rule(gw, {"op": "set", "event": "stress_threshold.*", "answer": "decline"}, 0, actor()))
    assert env["verdict"] == "applied", env
    sets = [p for a, p in gw.writes if a == "set_event_rule" and p.get("op") == "set"]
    # 9002 has no option taking gold: no rule; 9001: the free option the AI weighs highest, else the opt-out
    assert sets == [{"character_id": SE._R()._as_int(actor()["id"]), "op": "set", "event": "stress_threshold.9001",
                     "answer": "option", "option": "stress_threshold.9001.journaller.gain",
                     "else": "stress_threshold.9001.endure"}]
    res = env["result"]
    assert res["family"] == "stress_threshold.*" and res["events_in_files"] == 2 and res["no_gold_option"] == 1
    assert res["guarded"][0]["takes_gold"] == ["stress_threshold.9001.improvident.gain"]
    assert res["guarded"][0]["read_back"] is True


def test_3_a_rule_of_yours_is_kept_and_a_full_table_refuses_whole(game):
    t = Table()
    t.watched = True
    t.rules = [{"event": "stress_threshold.9001", "answer": "option", "option": "stress_threshold.9001.endure"}]
    gw = TGw(t)
    env = run(SE.do_event_rule(gw, {"op": "set", "event": "stress_threshold.*", "answer": "decline"}, 0, actor()))
    assert env["result"]["kept_yours"] == ["stress_threshold.9001"] and gw.writes == []
    t.rules = [{"event": f"other.{i}", "answer": "decline"} for i in range(SE.RULES_MAX)]
    env = run(SE.do_event_rule(TGw(t), {"op": "set", "event": "stress_threshold.*", "answer": "decline"}, 0,
                               actor()))
    assert env["ok"] is False and env["blocked"]["raw_reason"] == "too_many_rules"


def test_3_a_family_rule_takes_decline_only_and_clears_its_own(game):
    env = run(SE.do_event_rule(TGw(Table()), {"op": "set", "event": "stress_threshold.*", "answer": "option",
                                              "option": "x"}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["raw_reason"] == "family_answer"
    t = Table()
    gw = TGw(t)
    run(SE.do_event_rule(gw, {"op": "set", "event": "stress_threshold.*", "answer": "decline"}, 0, actor()))
    env = run(SE.do_event_rule(gw, {"op": "clear", "event": "stress_threshold.*"}, 0, actor()))
    assert env["result"]["cleared"] == [{"event": "stress_threshold.9001", "cleared": True}] and t.rules == []


def test_3_the_spending_preset_names_the_guard():
    assert SE.SPENDING_EVENTS_ROW["guard"]["args"]["args"] == {"op": "set", "event": "stress_threshold.*",
                                                                "answer": "decline"}


# ── 4: the call offered after a pick that paid gold ────────────────────────────────────────────────────────────────

PICK = {"seq": 3, "date": "1087.03.20", "character_id": MURCHAD, "event": "stress_threshold.9001",
        "options": ["stress_threshold.9001.contrite.gain", "stress_threshold.9001.improvident.gain",
                    "stress_threshold.9001.journaller.gain", "stress_threshold.9001.endure"],
        "ai_pick": {"index": 1, "key": "stress_threshold.9001.improvident.gain"},
        "applied": {"index": 1, "key": "stress_threshold.9001.improvident.gain"}, "rule": None, "gold": 317.88}


def test_4_RED_a_pick_that_paid_gold_offers_the_family_guard(game):
    row = SE.pick_row(PICK)
    assert row["applied_takes_gold"] is True
    assert row["next"]["args"]["args"] == {"op": "set", "event": "stress_threshold.*", "answer": "decline"}
    assert "stress_threshold.9001.contrite.gain" not in json.dumps(row["next"])


def test_4_a_pick_that_paid_nothing_keeps_its_call(game):
    row = SE.pick_row(dict(PICK, ai_pick={"index": 2, "key": PICK["options"][2]},
                           applied={"index": 2, "key": PICK["options"][2]}))
    assert "applied_takes_gold" not in row and row["next"]["args"]["args"]["event"] == "stress_threshold.9001"


# ── 5: the purse's event row with the option's own price ────────────────────────────────────────────────────────────

def test_5_RED_the_event_row_carries_the_options_price_from_its_file(game):
    answered = [SE.pick_row(PICK)]
    prev = {"date": "1086.09.07", "gold": 297.73, "monthly_income": 4.28}
    snap = {"date": "1087.09.07", "gold": 262.49, "actor_id": MURCHAD}
    (row,) = news.ai_pick_gold(prev, snap, answered)
    assert row["file_price"]["gold"] == -75.0 and "12 months of income, at least 75" in row["file_price"]["basis"]
    assert row["next"]["args"]["args"]["event"] == "stress_threshold.*"


# ── 6: the late ruler's price at the briefing ───────────────────────────────────────────────────────────────────────

def test_6_RED_the_briefing_reads_the_late_rulers_price_and_says_why_it_moved(monkeypatch):
    news.reset()
    news.note_creation_price(MURCHAD, "k_ireland", "gold", 325.0, "1078.09.09")
    news.note_price_basis(MURCHAD, "k_ireland", {"base": 500, "discount": 0.35, "sources": [
        {"source": "perk", "key": "ducal_conquest_perk", "mult": -0.2, "gold": -100.0},
        {"source": "culture ethos", "key": "ethos_courtly (irish)", "mult": -0.15, "gold": -75.0}]})
    # the heir's read of the same title, made earlier the same turn, takes the shared slot
    news.note_creation_price(BRIAN, "k_ireland", "gold", 425.0, "1078.12.16")
    brian_basis = {"base": 500, "discount": 0.15, "sources": [
        {"source": "culture ethos", "key": "ethos_courtly (irish)", "mult": -0.15, "gold": -75.0}]}

    async def price(gw, cid, key, *a, **k):
        return None, {"currency": "gold", "cost": 425.0, "gold": 57.78}

    async def basis(gw, cid, key, cost):
        return brian_basis
    monkeypatch.setattr(R, "mint_title_price", price)
    monkeypatch.setattr(R, "creation_price_basis", basis)
    out = run(news.ruler_change_briefing(Gw({}), {"actor_id": MURCHAD}, {"actor_id": BRIAN}, "k_ireland"))
    p = out["price"]
    assert p["was"] == 325.0 and p["was_read"] == "1078.09.09" and p["now"] == 425.0, p
    assert "ducal_conquest_perk (-100.0) died with him" in p["why_changed"]
    news.reset()


# ── 7: the counties a succession took, from the late ruler's newest read ───────────────────────────────────────────

ULSTER, ORIEL, AILECH = 474, 479, 485


def test_7_RED_the_observe_counts_from_the_late_rulers_newest_read(monkeypatch):
    news.reset()
    six = {str(c): MURCHAD for c in range(1, 7)}
    held = {"now": None}

    class G(Gw):
        async def read(self, action, params=None):
            if action == "query_dejure_progress":
                return {"counties": [{"county_id": int(c), "holder_id": h, "you_control": True}
                                     for c, h in held["now"].items()]}
            if action == "query_title_hierarchy":
                return {"title": {"key": {ULSTER: "c_ulster", ORIEL: "c_oriel", AILECH: "c_ailech"}.get(
                    params.get("title_id")), "holder_id": 15013}}
            return await super().read(action, params)

    async def mc(gw, me, date, baseline):
        return {}
    monkeypatch.setattr(news._map_view, "news_part", mc)
    gw = G({})

    def block(date, who, counties, baseline):
        held["now"] = counties
        snap = {"actor_id": who, "date": date}
        return run(news.block(gw, 0, snap, offers_served=False, fabricate_within="k_ireland", baseline=baseline))
    block("1073.02.24", MURCHAD, dict(six, **{str(AILECH): 32421}), "observe")                     # 7
    block("1078.09.09", MURCHAD, dict(six, **{str(AILECH): 32421, str(ULSTER): MURCHAD,
                                              str(ORIEL): 34907}), "turn")                         # 9
    t = block("1078.12.16", BRIAN, six, "turn")
    o = block("1078.12.16", BRIAN, six, "observe")
    rt, ro = t["ruler_changed"]["counties_left_realm"], o["ruler_changed"]["counties_left_realm"]
    assert (rt["was"], rt["now"]) == (9, 6)
    assert (ro["was"], ro["now"]) == (9, 6), ro
    assert ro["was_read"] == "1078.09.09"
    assert sorted(c["title_id"] for c in ro["counties"]) == [ULSTER, ORIEL, AILECH]
    news.reset()


# ── 8: "declared by you", scoped to the reads ───────────────────────────────────────────────────────────────────────

def test_8_RED_a_war_the_turns_news_listed_is_not_said_unlisted():
    news.reset()
    news._ACTS[MURCHAD] = [{"what": "declared war 106 (claim_cb)", "war_id": 106, "date": "1070.02.22"}]
    news._WARS_LISTED[MURCHAD] = {106: "1070.06.22"}
    row = {"war_id": 106, "end_date": "1071.06.24", "outcome": "attacker_victory"}
    news._say_declared(row, {"date": "1069.11.09"}, {"actor_id": MURCHAD, "date": "1071.06.29"})
    assert row["declared_by"] == "you" and "before the next read listed it" not in row["seen"]
    assert "after this read's last one (1069.11.09); another read listed it on 1070.06.22" in row["seen"]
    news._WARS_LISTED.clear()
    row = {"war_id": 106, "end_date": "1071.06.24", "outcome": "attacker_victory"}
    news._say_declared(row, {"date": "1069.11.09"}, {"actor_id": MURCHAD, "date": "1071.06.29"})
    assert row["seen"].endswith("before any read listed it")
    news.reset()


def test_8_every_read_records_the_wars_it_lists(monkeypatch):
    news.reset()

    async def mc(gw, me, date, baseline):
        return {}
    monkeypatch.setattr(news._map_view, "news_part", mc)
    run(news.block(Gw({}), 0, {"actor_id": MURCHAD, "date": "1070.06.22", "wars": {106: {"name": "w"}}},
                   offers_served=False, baseline="turn"))
    assert news._WARS_LISTED[MURCHAD] == {106: "1070.06.22"}
    news.reset()


# ── 9: the levers that keep a duchy, on page one ────────────────────────────────────────────────────────────────────

FC = {"younger_shares": [{"id": 15013, "name": "Lennan", "fate": "leaves_realm", "takes": [474],
                          "takes_title": {"title": "d_ulster", "title_id": 473, "created": True}},
                         {"id": 65944, "name": "Ennis", "fate": "stays_vassal", "takes": [524]}]}
ROWS = {"disinherit": [{"son_id": 15013, "price": {"prestige": 300, "renown": 150}, "allowed_now": False,
                        "why_not": ["spendable renown 120 < 150"], "to_allow": "more spendable renown",
                        "call": {"tool": "do", "args": {"intent": "interact", "args": {
                            "interaction": "disinherit_interaction", "target_id": 15013}}}, "after": {"x": 1}}],
        "vows": [{"son_id": 15013, "price": {"piety": 500}, "allowed_now": True, "why_not": None,
                  "call": {"tool": "do", "args": {"intent": "interact", "args": {
                      "interaction": "take_vows_interaction", "target_id": 15013}}}, "after": {"x": 1}}]}


def test_9_RED_the_first_page_names_the_lever_and_its_price_for_a_son_who_leaves():
    (row,) = R.prevent_split_rows(FC, ROWS)
    assert row["son_id"] == 15013 and row["takes"] == "d_ulster"
    assert row["disinherit"]["price"] == {"prestige": 300, "renown": 150} and row["disinherit"]["allowed_now"] is False
    assert row["vows"]["allowed_now"] is True and "after" not in row["vows"]
    order = reply_fit.PAGE_ORDER["succession_plan"]
    assert order.index("prevent_split") < order.index("partition_forecast") < order.index("levers")
    assert "prevent_split" in news.SPLIT_LEVERS["read"]


def test_9_a_long_reply_keeps_prevent_split_on_its_first_page(monkeypatch):
    monkeypatch.setenv(reply_fit.PAGES_ENV, "on")
    reply_fit.reset_pages()
    res = {"outcome": "splits", "passes_intact": False, "breaker": {"id": 15013, "fate": "leaves_realm"},
           "prevent_split": R.prevent_split_rows(FC, ROWS),
           "partition_forecast": {"younger_shares": FC["younger_shares"], "rule": "x" * 600},
           "levers": {"take_out_of_split": ROWS, "pad": ["y" * 300 for _ in range(12)]},
           "per_title_map": {"titles": [{"key": f"c_{i}", "pad": "z" * 200} for i in range(10)]}}
    env = {"ok": True, "tool": "succession_plan", "result": res}
    first = reply_fit.paginate(env, "succession_plan", {}, cap=4096)
    assert "more" in first["result"] and "prevent_split" in first["result"], list(first["result"])
    assert first["result"]["prevent_split"][0]["disinherit"]["price"]["renown"] == 150
    reply_fit.reset_pages()
