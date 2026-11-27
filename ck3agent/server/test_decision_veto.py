"""The game's AI took convert_to_local_culture_decision for the seat's ruler in both run-eight
arms (1,000 prestige; one game 1087.10, another 1082-83) and no lever stopped a decision; four answered
events cost -75 prestige each in 1070-71 with no row naming them; 36 declarations were refused in
1087-1096 with "none of those conditions failed".

- `ai_veto what=decision` (optionally `keys`), its rows priced from the decision's cost block, the
keys carried to a new ruler, the turn-one note and the first-news lever list naming it.
- an answered event's pick row carries its prestige from the file and the rule that takes the
option costing least; the server watches the ruler's events so the picks are recorded at all.
- the refused declaration asks the game the rest of declare_war_interaction's script conditions;
a refusal with none failed names what is left (the engine's own checks).
"""
from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

import event_prices as EP
import news
import notes as N
import redesign as R
import seat_events as SE
from test_ireland_walk_replies import Gw
from test_ally_call_and_perk_veto import ME, Veto, VGw, actor, run, snap, two  # noqa: F401  (fixtures: _fresh is autouse there)

#: The game's files (CK3_GAME_DIR or the Steam default): the rows marked need_files read them.
need_files = pytest.mark.skipif(SE.game_dir() is None, reason="the game's files are not on this host")

BRIDGE = Path(__file__).resolve().parents[2] / "bridge"
DOMNALL = 36124
CONVERT = "convert_to_local_culture_decision"


@pytest.fixture(autouse=True)
def _fresh():
    news.reset()
    getattr(SE, "WATCHED_BY_SERVER", {}).clear()       # (absent on the base: each test reds on its own)
    getattr(EP, "CAPITAL_CULTURE", {}).clear()
    yield
    news.reset()


class DVeto(Veto):
    """The bridge's veto table with the decision keys (ai_veto.cpp emit_ruler)."""

    def __init__(self, records=None):
        super().__init__(records)
        self.keys: list = []

    def write(self, p):
        r = super().write(p)
        if r.get("success") and p["what"] == "decision":
            self.keys = [k for k in (p.get("keys") or "").split(",") if k] if p.get("op") == "set" else []
        return r

    def read(self, p):
        r = super().read(p)
        r["rulers"][0]["decision_keys"] = list(self.keys)
        return r


def dec_rec(seq, key=CONVERT, dropped=True, origin="game", date="1087.10.03", prestige=1212.9):
    """A query_ai_veto record of a decision post, as do_query_ai_veto writes it."""
    return {"seq": seq, "date": date, "character_id": ME, "what": "decision", "dropped": dropped, "origin": origin,
            "site": "0x3cd1f06", "key": key, "gold": 70.0, "prestige": prestige, "piety": 300.0, "priced": "no",
            "unpriced_why": "decision_cost_in_files"}


# ── The decision kind ─────────────────────────────────────────────────────────────

def test_1_the_kinds_are_the_bridges_names_one_list_two_readers():
    h = (BRIDGE / "include" / "ai_veto.h").read_text()
    names = re.findall(r'case \d+: return "([a-z_]+)";', h)
    assert tuple(names) == SE.AI_VETO_WHATS and "decision" in names
    assert re.search(r"kMaxDecisionKeys = (\d+);", h).group(1) == str(SE.DECISION_KEYS_MAX)


def test_1_set_decision_with_keys_is_sent_as_one_list_and_read_back():
    t = DVeto()
    gw = VGw(t)
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "decision", "keys": [CONVERT]}, 0, actor()))
    assert env["verdict"] == "applied", env
    assert gw.writes == [("set_ai_veto", {"character_id": ME, "op": "set", "what": "decision", "keys": CONVERT})]
    assert env["result"]["decision_keys"] == [CONVERT] and "note" not in env["result"]
    assert "do take_decision" in env["result"]["applies_to"]


def test_1_set_decision_without_keys_says_it_drops_the_free_stress_ones_too():
    gw = VGw(DVeto())
    env = run(SE.do_ai_veto(gw, {"op": "set", "what": "decision"}, 0, actor()))
    assert env["verdict"] == "applied" and env["result"]["decision_keys"] == []
    assert "stress_loss_" in env["result"]["note"]


def test_1_keys_that_do_not_read_back_are_not_applied():
    class Lossy(DVeto):
        def write(self, p):
            r = super().write(p)
            self.keys = []           # the bridge kept none of them
            return r
    env = run(SE.do_ai_veto(VGw(Lossy()), {"op": "set", "what": "decision", "keys": [CONVERT]}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["gate"] == "write_not_applied"


@pytest.mark.parametrize("args,code", [
    ({"op": "set", "what": "law", "keys": [CONVERT]}, "keys_only_with_set_decision"),
    ({"op": "clear", "what": "decision", "keys": [CONVERT]}, "keys_only_with_set_decision"),
    ({"op": "set", "what": "decision", "keys": ["Convert"]}, "bad_decision_keys"),
    ({"op": "set", "what": "decision", "keys": [f"d{i}" for i in range(9)]}, "bad_decision_keys"),
])
def test_1_bad_keys_are_refused_before_the_bridge(args, code):
    gw = VGw(DVeto())
    env = run(SE.do_ai_veto(gw, args, 0, actor()))
    assert env["ok"] is False and env["blocked"]["raw_reason"] == code and gw.writes == []
    assert SE.REFUSALS[code] == "invalid_argument" == R.RAW_REASON_TO_GATE[code]


@need_files
def test_1_a_dropped_conversion_is_news_with_its_price_from_both_cultures_and_the_seats_own_call():
    EP.CAPITAL_CULTURE[ME] = "norse"
    irish = {"gold": 70.0, "prestige": 1212.9, "piety": 300.0, "culture_key": "irish"}
    out = two(snap("1087.09.01", [], chr_sect=irish),
              snap("1087.11.01", [dec_rec(1), dec_rec(2, date="1087.10.20")], vetoes=["decision"], chr_sect=irish))
    (row,) = out["ai_vetoed"]
    assert row["what"] == "decision" and row["key"] == CONVERT and row["attempts"] == 2
    assert row["price"] == {"prestige": -1000} and "10_culture_conversion_decisions.txt" in row["price_basis"]
    assert row["prestige_at_attempt"] == 1212.9
    assert row["next"] == {"tool": "do", "args": {"intent": "take_decision", "args": {"decision": CONVERT}}}


@need_files
def test_1_an_unvetoed_decision_is_ai_acted_with_its_price_and_the_keyed_veto():
    out = two(snap("1070.01.01", []), snap("1070.03.01", [dec_rec(1, key="scrape_the_barrel_decision", dropped=False)]))
    (a,) = out["ai_acted"]
    assert a["what"] == "decision" and a["key"] == "scrape_the_barrel_decision" and a["price"] == {"prestige": -150.0}
    assert a["veto"]["args"]["args"] == {"op": "set", "what": "decision", "keys": ["scrape_the_barrel_decision"]}


@need_files
def test_1_a_free_or_computed_decision_price_is_said_so():
    out = two(snap("1070.01.01", []), snap("1070.03.01", [
        dec_rec(1, key="stress_loss_drunkard_decision", dropped=False),
        dec_rec(2, key="commission_epic_decision", dropped=False)]))
    free, epic = out["ai_acted"]
    # its effect fires the on_action stress_loss_drunkard, whose events take medium_prestige_loss (the files)
    assert free["price"] == {"prestige": -150} and "stress_trait_coping_decisions.1001" in " ".join(free["fires"])
    assert "price" not in epic and epic["price_unknown"]["gold"].startswith("unknown: medium_gold_value")


def test_1_the_seats_own_take_decision_is_not_the_ais():
    out = two(snap("1070.01.01", []), snap("1070.03.01", [dec_rec(1, dropped=False, origin="bridge")],
                                           vetoes=["decision"]))
    assert "ai_acted" not in out and "ai_vetoed" not in out


def test_1_the_decision_veto_and_its_keys_follow_the_seat_to_a_new_ruler():
    t = DVeto()
    gw = VGw(t)
    prev = {"actor_id": ME, "ai_vetoes": ["law", "decision"], "ai_veto_decision_keys": [CONVERT]}
    now = {"actor_id": DOMNALL, "ai_vetoes": []}
    row = run(news.carry_vetoes(gw, 0, prev, now))
    assert ("set_ai_veto", {"character_id": DOMNALL, "op": "set", "what": "decision", "keys": CONVERT}) in gw.writes
    assert row["carried"] == ["law", "decision"] and row["decision_keys"] == [CONVERT] and "not_carried" not in row
    assert now["ai_veto_decision_keys"] == [CONVERT]


def test_1_the_first_news_and_the_culture_warning_name_the_decision_lever():
    out = run(news.block(Gw({}), 0, snap("1066.09.15", []), offers_served=False))
    ai = out["ai_spends"]
    kinds = ai["veto"]["args"]["args"]["what"] if "turn_one_notes" in out else None
    assert kinds is None or "decision" in kinds
    full = news.ai_spends()
    assert full["decisions"]["lever"]["args"]["args"] == {"op": "set", "what": "decision", "keys": [CONVERT]}


def _graph_gw():
    graph = {"success": True, "text": "@c_dublin norse catholic Murchad 31506\n"}
    return Gw({"query_map_graph": graph})


def test_1_the_culture_warning_carries_the_keyed_veto_until_it_is_set(monkeypatch):
    import map_view
    monkeypatch.setattr(map_view, "parse_graph", lambda text: {"c_dublin": {"culture": "norse"}})
    s = {"actor_id": ME, "culture": "irish", "primary_title_key": "c_dublin", "prestige": 610.0, "ai_vetoes": ["law"]}
    row = run(news.culture_conversion_ahead(_graph_gw(), s))
    assert row["veto"] == {"tool": "do", "args": {"intent": "ai_veto", "args": {
        "op": "set", "what": "decision", "keys": [CONVERT]}}}
    assert "no veto stops" not in row["what"] and EP.CAPITAL_CULTURE[ME] == "norse"
    news.reset()
    s2 = dict(s, ai_vetoes=["decision"], ai_veto_decision_keys=[CONVERT])
    assert "veto" not in run(news.culture_conversion_ahead(_graph_gw(), s2))


def test_1_the_turn_one_note_names_the_decision_veto_within_the_budgets():
    doc = N.load()
    text = " ".join(N.turn_one_texts(doc))
    # The note no longer gives every ruler the culture key and the create_title veto (Munster
    # is Irish in Irish land, and a duke's create_title veto blocks his second duchy); the culture veto rides on
    # observe's culture_conversion_ahead row, which carries it keyed (test_1_... above), and create_title is a count's
    assert "culture_conversion_ahead" in text
    # In one test series every seat vetoes create_title on turn one and mints its duchies and k_ireland itself (the AI
    # spent the ruler's gold on d_meath and k_ireland at its own time; the seat's own mint_title proven live)
    # One call sets the spending kinds (create_title among them; seat_events.AI_VETO_PRESETS)
    assert "do ai_veto what=spending" in text
    assert "create_title only as a count" not in text
    by = {n["id"]: n for n in doc["notes"]}
    assert "no veto stops" not in by["ai-converts-culture"]["text"] and "ai_veto decision" in by["ai-converts-culture"]["text"]
    assert N.problems(doc) == []
    sizes = [len(json.dumps([N.render(n) for n in doc["notes"] if n["topic"] == t], ensure_ascii=False).encode())
             for t in doc["topics"]]
    assert sum(sizes) <= 7416


def test_1_the_listing_carries_decision_and_keys_within_its_caps():
    ms = pytest.importorskip("mcp_server")
    tools = run(ms.mcp.list_tools())
    out = [t.model_dump(mode="json", by_alias=True, exclude_none=True) for t in tools]
    out = [{"name": d["name"], "description": d.get("description", ""), "inputSchema": d["inputSchema"]}
           for d in out]
    dump = json.dumps(out)
    assert "ai_veto: args.{what=create_title|law|mercenary|construction|ally_call|perk|decision|vassal_offer|men_at_arms|war|court_type|faction|legacy|call_in|activity|spending, keys, replace, op}" in dump  # every kind, replace, and the spending preset
    assert len(out) == 13 and len(dump) <= 31_049 and len(json.dumps(out, indent=1)) <= 36_298


@need_files
def test_1_decision_price_reads_numbers_script_values_and_says_unknown_otherwise():
    assert EP.decision_price("scrape_the_barrel_decision")["prestige"] == -150.0
    assert EP.decision_price("stress_loss_drunkard_decision")["fires"][0]["on_action"] == "stress_loss_drunkard"
    assert EP.decision_price(CONVERT)["prestige"].startswith("unknown: -1000 x multipliers")
    assert EP.decision_price(CONVERT, "irish", "norse")["prestige"] == -1000
    assert "unread" in EP.decision_price("no_such_decision")


# ── Answered events at a prestige price ───────────────────────────────────────────

TRAVEL = "travel_danger_events.9010"


@need_files
def test_2_an_answered_event_names_its_price_and_the_rule_for_the_option_costing_least():
    row = SE.pick_row({"event": TRAVEL, "options": [TRAVEL + ".a", TRAVEL + ".b", TRAVEL + ".c"],
                       "applied": {"index": 1, "key": TRAVEL + ".b"}, "ai_pick": {"index": 1, "key": TRAVEL + ".b"},
                       "date": "1070.03.02", "rule": None})
    assert row["applied"] == TRAVEL + ".b" and row["prestige"] == -75          # a recorded game 1070.03: -74 that month
    assert row["cheaper_option"]["option"] == TRAVEL + ".a" and row["cheaper_option"]["prestige"] == 0
    assert row["next"]["args"]["args"] == {"op": "set", "event": TRAVEL, "answer": "option", "option": TRAVEL + ".a"}


def test_2_an_answered_event_that_costs_nothing_keeps_the_first_option_call():
    row = SE.pick_row({"event": TRAVEL, "options": [TRAVEL + ".a", TRAVEL + ".b"],
                       "applied": {"index": 0, "key": TRAVEL + ".a"}, "date": "1070.03.02"})
    assert "prestige" not in row and "cheaper_option" not in row
    assert row["next"]["args"]["args"]["option"] == TRAVEL + ".a"


class EvGw(Gw):
    def __init__(self):
        self.seated = False
        super().__init__({"query_event_rules": self.ev, "query_ai_budget": {"success": True},
                          "query_ai_veto": {"success": True, "rulers": [], "records": []}})

    def ev(self, p):
        seats = [{"character_id": ME, "rules": []}] if self.seated else []
        return {"success": True, "seats": seats, "picks": []}

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        if action == "set_event_rule" and (params or {}).get("op") == "watch":
            self.seated = True
        return {"success": True}


def test_2_the_server_watches_the_rulers_events_once_so_the_picks_are_recorded():
    gw = EvGw()
    ev, _b, _v = run(SE.news_reads(gw, ME))
    assert gw.writes == [("set_event_rule", {"character_id": ME, "op": "watch"})]
    assert SE._rules_of(ev, ME) == []
    run(SE.news_reads(gw, ME))
    assert len(gw.writes) == 1                                        # once
    assert SE.snapshot_part(ME, ev, None)["event_watched"] is True


# ── The refused declaration names the rest ─────────────────────────────────────────

def test_3_each_new_condition_the_game_marks_is_named():
    for word, says in (("TARGET_STRONG_HOOK", "strong hook"), ("TARGET_TRIBUTARY", "tributary"),
                       ("ALLIED_NO_ATTACK", "cannot attack allies"), ("CONFEDERATION", "confederation"),
                       ("LIEGE_AT_WAR", "liege"), ("SUBJUGATION_OFFER", "subjugation")):
        g = R.declare_game_says(["PROBED", "WHY_" + word, "PROBE_BARE_OK", "PROBE_TITLE_OK"])
        assert [f["condition"] for f in g["failed"]] == [word.lower()] and says in g["failed"][0]["game_says"]
        assert "none_failed" not in g


def test_3_envws8_none_failed_now_names_what_is_left_to_the_engine():
    """A recorded game, 1087.05.10 SACT_129: marks [] (a template without the probes): the old words, nothing more.
    The same refusal from a probing template with nothing marked: the engine's own checks are named."""
    old = R.declare_game_says([])
    assert "remaining" not in old and "none of those conditions failed" in old["none_failed"]
    new = R.declare_game_says(["PROBED"])
    assert new["remaining"] == R.ENGINE_RULES and "diplomatic range" in new["remaining"]
    assert "a strong hook on you" in new["asked"] and new["cb_part"] == R.CB_PART_CB


def test_3_the_template_asks_the_defender_side_with_whole_word_references():
    h = (BRIDGE / "include" / "script_templates.h").read_text()
    assert "why_trigger(w.trigger, A, D)" in h and '"TARGET_STRONG_HOOK", "D = { has_strong_hook = A }"' in h


@need_files
def test_1_the_list_prices_a_decision_attempt_from_its_files():
    """A live read, 1068.08.13: op=list showed isolate_family_decision with price_unread decision_cost_in_files."""
    t = DVeto([dec_rec(1, key="scrape_the_barrel_decision"), dec_rec(2, key="no_such_decision")])
    env = run(SE.do_ai_veto(VGw(t), {"op": "list"}, 0, actor()))
    a, b = env["result"]["attempts"]
    assert a["price"] == {"prestige": -150.0} and "price_unread" not in a
    assert b["price_unknown"].startswith("no file under common/decisions")


@need_files
def test_1_a_decision_cost_named_by_a_file_constant_is_read():
    """A live read: Castile's AI posted sale_of_titles_decision (dropped); its cost is the file's @constant."""
    p = EP.decision_price("sale_of_titles_decision")
    assert isinstance(p["prestige"], float) and p["prestige"] < 0, p
