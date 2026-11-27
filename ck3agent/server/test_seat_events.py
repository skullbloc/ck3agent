"""do(intent=event_rule / gold_reserve) and their news rows (seat_events.py).

The bridge replies here are the shapes factions.cpp do_set_event_rule /
do_query_event_rules / do_query_ai_budget and action_handlers.cpp do_script_action
(gold_reserve) write; the live replies of a recorded run back the fixture below.

"""
from __future__ import annotations

import asyncio
import json

import pytest

import news
import redesign as R
import seat_events as SE
from test_ireland_walk_replies import Gw

ME = 31506
LETTER = "court_chaplain_task.0301"
OPTS = ["court_chaplain_task.0301.a", "court_chaplain_task.0301.b"]


def run(c):
    return asyncio.run(c)


def actor():
    return {"id": ME, "name": "Murchad"}


class Table:
    """A bridge event table: set_event_rule writes it, query_event_rules reads it."""

    def __init__(self, picks=None):
        self.rules, self.watched, self.picks = [], False, list(picks or [])

    def write(self, p):
        if p.get("op") == "set":
            r = {"event": p["event"], "answer": p["answer"]}
            r.update({"option": p["option"]} if "option" in p else {"option_index": p.get("option_index", 0)})
            if p["answer"] == "pay_if":
                r.update(currency=p.get("currency", "gold"), amount=p["amount"])
            self.rules = [x for x in self.rules if x["event"] != p["event"]] + [r]
            self.watched = True
        elif p.get("op") == "clear":
            before = len(self.rules)
            self.rules = [x for x in self.rules if x["event"] != p["event"]]
            if len(self.rules) == before:
                return {"success": False, "action": "set_event_rule", "reason": "no_rule_for_event"}
        elif p.get("op") == "watch":
            self.watched = True
        return {"success": True, "action": "set_event_rule", "op": p.get("op"), "character_id": ME,
                "watched": self.watched, "rules": self.rules}

    def read(self, p):
        seats = [{"character_id": ME, "rules": self.rules}] if self.watched else []
        return {"success": True, "action": "query_event_rules", "hook_installed": True, "disabled_by_env": False,
                "last_seq": len(self.picks), "seats": seats, "picks": self.picks}


class TGw(Gw):
    def __init__(self, table, replies=None):
        super().__init__(dict(replies or {}, query_event_rules=table.read))
        self.table = table

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        if action == "set_event_rule":
            return self.table.write(dict(params or {}))
        return {"success": True}


def pick(seq, ai, applied, rule=None, why=None, gold=84.0, **kw):
    p = {"seq": seq, "date": "1067.11.27", "character_id": ME, "event": LETTER, "options": OPTS,
         "ai_pick": {"index": ai, "key": OPTS[ai]}, "applied": {"index": applied, "key": OPTS[applied]},
         "rule": rule, "gold": gold, "prestige": 300.0, "piety": 100.0}
    if rule:
        p["why"] = why
    p.update(kw)
    return p


# ── event_rule ────────────────────────────────────────────────────────────────
def test_decline_is_sent_to_the_bridge_and_read_back():
    t = Table()
    gw = TGw(t)
    env = run(SE.do_event_rule(gw, {"op": "set", "event": LETTER, "answer": "decline"}, 0, actor()))
    assert env["ok"] is True and env["verdict"] == "applied", env
    assert gw.writes[0] == ("set_event_rule", {"character_id": ME, "op": "set", "event": LETTER,
                                               "answer": "decline"})
    assert env["result"]["rules"][0] == {"event": LETTER, "answer": "decline", "option_index": 0}
    # The claim letter's other variant gets the same answer, its decline named by what it does (when the
    # game's files are here); nothing else is written
    assert all(p["event"] == "court_chaplain_task.0302" and p["answer"] == "option" for _a, p in gw.writes[1:])
    assert env["result"]["readback"]["read"] is True


def test_pay_if_sends_currency_and_amount_whole_units():
    t = Table()
    gw = TGw(t)
    env = run(SE.do_event_rule(gw, {"event": LETTER, "answer": "pay_if", "amount": "96",
                                    "option": OPTS[0]}, 0, actor()))
    assert env["verdict"] == "applied", env
    sent = gw.writes[0][1]
    assert sent["amount"] == 96 and sent["option"] == OPTS[0] and "option_index" not in sent


def test_an_option_index_rides_as_option_index():
    gw = TGw(Table())
    run(SE.do_event_rule(gw, {"event": LETTER, "answer": "option", "option": 1}, 0, actor()))
    assert gw.writes[0][1]["option_index"] == 1 and "option" not in gw.writes[0][1]


def test_pay_if_without_amount_refuses_and_sends_nothing():
    gw = TGw(Table())
    env = run(SE.do_event_rule(gw, {"event": LETTER, "answer": "pay_if"}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["gate"] == "missing_required_argument"
    assert gw.writes == []


def test_a_bad_answer_refuses():
    gw = TGw(Table())
    env = run(SE.do_event_rule(gw, {"event": LETTER, "answer": "ai"}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["gate"] == "invalid_argument" and gw.writes == []


def test_an_event_not_in_the_game_files_refuses(monkeypatch):
    monkeypatch.setattr(SE, "event_in_files", lambda e: (False, ""))
    gw = TGw(Table())
    env = run(SE.do_event_rule(gw, {"event": "no_such.0001", "answer": "decline"}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["gate"] == "name_not_found" and gw.writes == []


def test_unreadable_game_files_set_the_rule_and_say_so(monkeypatch):
    monkeypatch.setattr(SE, "event_in_files", lambda e: (None, "not readable here"))
    gw = TGw(Table())
    env = run(SE.do_event_rule(gw, {"event": LETTER, "answer": "decline"}, 0, actor()))
    assert env["verdict"] == "applied"
    assert env["result"]["event_check"] == {"in_game_files": None, "why": "not readable here"}


def test_the_event_check_reads_the_real_files_when_present(tmp_path, monkeypatch):
    ev = tmp_path / "events" / "x"
    ev.mkdir(parents=True)
    (ev / "a.txt").write_text("namespace = t\n" + LETTER + " = {\n type = letter_event\n}\n")
    monkeypatch.setenv("CK3_GAME_DIR", str(tmp_path))
    SE._EVENT_CACHE.clear()
    assert SE.event_in_files(LETTER) == (True, "events/x/a.txt")
    assert SE.event_in_files("court_chaplain_task.0399")[0] is False
    SE._EVENT_CACHE.clear()


def test_a_bridge_refusal_is_typed():
    class Refusing(TGw):
        async def write(self, action, params=None):
            self.writes.append((action, params))
            return {"success": False, "action": "set_event_rule", "reason": "event_hook_not_installed"}
    gw = Refusing(Table())
    env = run(SE.do_event_rule(gw, {"op": "watch"}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["gate"] == "bridge_fault"
    assert env["blocked"]["raw_reason"] == "event_hook_not_installed"


def test_a_readback_without_the_rule_is_not_applied():
    class Forgetful(Table):
        def write(self, p):
            out = super().write(p)
            self.rules = []
            return out
    env = run(SE.do_event_rule(TGw(Forgetful()), {"event": LETTER, "answer": "decline"}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["gate"] == "write_not_applied"


def test_list_shows_rules_and_the_picks_with_the_next_call():
    t = Table([pick(1, 0, 0), pick(2, 0, 1, rule="decline option=0", why="applied")])
    t.watched = True
    env = run(SE.do_event_rule(TGw(t), {"op": "list"}, 0, actor()))
    res = env["result"]
    assert res["watched"] is True and res["picks_total"] == 2
    first, second = res["picks"]
    assert first["ai_would_pick"] == OPTS[0] and first["applied"] == OPTS[0] and first["rule"] is None
    assert first["next"]["args"]["intent"] == "event_rule" and first["next"]["args"]["args"]["event"] == LETTER
    assert second["applied"] == OPTS[1] and second["why"] == "applied" and "next" not in second


def test_every_bridge_code_has_a_gate():
    for code, gate in SE.REFUSALS.items():
        assert gate in R.GATE_TABLE, code
        assert R.RAW_REASON_TO_GATE.get(code) == gate, code


# ── news ──────────────────────────────────────────────────────────────────────
def test_news_names_the_picks_since_the_last_read_once():
    prev = SE.snapshot_part(ME, {"success": True, "seats": [{"character_id": ME, "rules": []}],
                                 "picks": [pick(1, 0, 0)]}, None)
    now = SE.snapshot_part(ME, {"success": True, "seats": [{"character_id": ME, "rules": []}],
                                "picks": [pick(1, 0, 0), pick(2, 0, 1, rule="decline option=0", why="applied")]},
                           None)
    out = SE.news_part(prev, now)
    assert [r["applied"] for r in out["events_answered"]] == [OPTS[1]]
    assert SE.news_part(now, now) == {}


def test_news_says_nothing_of_events_for_an_unwatched_ruler():
    snap = SE.snapshot_part(ME, {"success": True, "seats": [], "picks": []}, None)
    assert SE.news_part(snap, snap) == {}


def test_an_unread_table_after_a_watch_is_unread_not_none():
    prev = SE.snapshot_part(ME, {"success": True, "seats": [{"character_id": ME, "rules": []}], "picks": []}, None)
    now = SE.snapshot_part(ME, None, None)
    assert "events_answered_unread" in SE.news_part(prev, now)


BUDGET = {"success": True, "action": "query_ai_budget", "character_id": ME, "gold": 84.0,
          "buckets": {"reserved": 60.0, "war_chest": 0.0, "long_term": 0.0, "short_term": 24.0},
          "buckets_sum_to_gold": True, "reserved_max": 200, "war_chest_target": 25.0}


def test_the_purse_shows_the_buckets_only_with_a_reserve():
    snap = SE.snapshot_part(ME, None, BUDGET)
    assert SE.news_part(snap, snap)["budget"]["reserved"] == 60.0
    none = dict(BUDGET, reserved_max=0, buckets=dict(BUDGET["buckets"], reserved=0.0))
    s2 = SE.snapshot_part(ME, None, none)
    assert "budget" not in SE.news_part(s2, s2)


def test_the_news_block_carries_events_and_budget():
    news.reset()
    base = dict(actor_id=ME, chr_sect={"gold": 84.0}, wars={}, claims={}, council=None, heir=None)
    ev1 = {"success": True, "seats": [{"character_id": ME, "rules": []}], "picks": []}
    ev2 = dict(ev1, picks=[pick(1, 0, 1, rule="decline option=0", why="applied")])
    gw = Gw()
    run(news.block(gw, 0, news.snapshot(date="1066.09.15", event_rules=ev1, ai_budget=BUDGET, **base),
                   offers_served=False))
    out = run(news.block(gw, 0, news.snapshot(date="1066.11.27", event_rules=ev2, ai_budget=BUDGET, **base),
                         offers_served=False))
    assert out["events_answered"][0]["event"] == LETTER
    assert out["purse"]["budget"]["reserved_max"] == 200
    news.reset()


# ── gold_reserve ──────────────────────────────────────────────────────────────
def test_reserve_without_amount_reads_the_buckets():
    gw = TGw(Table(), {"query_ai_budget": BUDGET})
    env = run(SE.do_gold_reserve(gw, {}, 0, actor()))
    assert env["result"]["budget"]["short_term"] == 24.0 and gw.writes == []
    assert "Forced charges" in env["result"]["note"]


def test_reserve_sends_the_template_and_reads_the_maximum_back(monkeypatch):
    monkeypatch.setattr(R, "_SLEEP", lambda s: asyncio.sleep(0))
    reads = iter([dict(BUDGET, reserved_max=0), BUDGET])

    class Sgw(TGw):
        async def write(self, action, params=None):
            self.writes.append((action, dict(params or {})))
            return {"success": True, "action": "script_action", "template": "gold_reserve", "token": "SACT_3",
                    "guard": "ran", "marks": [], "errors": [], "script_log_channel": {"state": "open"}}
    gw = Sgw(Table(), {"query_ai_budget": lambda p: next(reads)})
    env = run(SE.do_gold_reserve(gw, {"amount": 200, "move": True}, 0, actor()))
    assert env["verdict"] == "applied", json.dumps(env)[:800]
    assert gw.writes == [("script_action", {"actor_id": ME, "amount": 200, "move": True,
                                            "template": "gold_reserve"})]
    assert env["result"]["budget_before"]["reserved_max"] == 0


def test_reserve_whose_maximum_did_not_change_is_not_applied(monkeypatch):
    monkeypatch.setattr(R, "_SLEEP", lambda s: asyncio.sleep(0))

    class Sgw(TGw):
        async def write(self, action, params=None):
            return {"success": True, "action": "script_action", "template": "gold_reserve", "token": "SACT_3",
                    "guard": "ran", "marks": [], "errors": [], "script_log_channel": {"state": "open"}}
    gw = Sgw(Table(), {"query_ai_budget": dict(BUDGET, reserved_max=0)})
    env = run(SE.do_gold_reserve(gw, {"amount": 200}, 0, actor()))
    assert env["ok"] is False and env["blocked"]["gate"] == "write_not_applied"


def test_reserve_read_back_from_a_suspect_layout_is_unread_not_not_applied(monkeypatch):
    # The script ran, the budget read's buckets did not sum to the gold
    # field, and the seat was told "not applied". A suspect read says nothing either way.
    monkeypatch.setattr(R, "_SLEEP", lambda s: asyncio.sleep(0))

    class Sgw(TGw):
        async def write(self, action, params=None):
            return {"success": True, "action": "script_action", "template": "gold_reserve", "token": "SACT_3",
                    "guard": "ran", "marks": [], "errors": [], "script_log_channel": {"state": "open"}}
    gw = Sgw(Table(), {"query_ai_budget": dict(BUDGET, reserved_max=0, buckets_sum_to_gold=False)})
    env = run(SE.do_gold_reserve(gw, {"amount": 200}, 0, actor()))
    assert (env.get("blocked") or {}).get("gate") != "write_not_applied", json.dumps(env)[:800]
    assert env["verdict"] != "applied"
    # the control: the same reply with a layout that sums is still "not applied"
    gw2 = Sgw(Table(), {"query_ai_budget": dict(BUDGET, reserved_max=0)})
    env2 = run(SE.do_gold_reserve(gw2, {"amount": 200}, 0, actor()))
    assert env2["blocked"]["gate"] == "write_not_applied"


@pytest.mark.parametrize("bad", [-1, 100001, "x", None])
def test_reserve_bad_amounts_refuse(bad):
    gw = TGw(Table(), {"query_ai_budget": BUDGET})
    a = {"amount": bad} if bad is not None else {"amount": 5, "move": "yes"}
    env = run(SE.do_gold_reserve(gw, a, 0, actor()))
    assert env["ok"] is False and env["blocked"]["gate"] == "invalid_argument" and gw.writes == []


def test_both_intents_are_declared_and_tabled():
    import do_intent_args as D
    assert "event_rule" in R.INTENTS and "gold_reserve" in R.INTENTS
    t = D.table_text()
    assert "event_rule: args.{" in t and "gold_reserve: args.{amount, move}" in t


# ── the live X1 run ─────────────────────────────────────────────
LIVE = json.load(open(__import__("pathlib").Path(__file__).with_name("fixtures") / "seat_event_replies.json"))


def _bridge_pick_from_live(row: dict, seq: int) -> dict:
    """the bridge's pick shape rebuilt from a recorded seat row (the keys pick_row read)"""
    opts = row["options"]
    idx = lambda v: {"index": opts.index(v), "key": v} if isinstance(v, str) else {"index": v}  # noqa: E731
    p = {"seq": seq, "date": row["date"], "character_id": ME, "event": row["event"], "options": opts,
         "ai_pick": idx(row["ai_would_pick"]), "applied": idx(row["applied"]), "rule": row["rule"],
         "gold": row.get("gold_at_pick")}
    if row["rule"]:
        p["why"] = row["why"]
        if "covered" in row:
            p["covered"] = row["covered"]
    return p


def test_live_a_one_option_event_offers_no_rule_call():
    """A recorded game, 1066.11.06: marriage_interaction.0010 has one option (EXCELLENT); the reply offered a
    `next` that sets a rule for it, a call that can change nothing."""
    rows = LIVE["list_t14"]["reply"]["result"]["result"]["picks"]
    one = next(r for r in rows if r["event"] == "marriage_interaction.0010")
    assert "next" in one                      # the recorded defect
    assert "next" not in SE.pick_row(_bridge_pick_from_live(one, 1))
    two = next(r for r in rows if r["event"] == "bp2_yearly.1040")
    assert SE.pick_row(_bridge_pick_from_live(two, 2))["next"]["args"]["args"]["event"] == "bp2_yearly.1040"


def test_live_the_two_claim_letters_reshape_unchanged():
    rows = [r for r in LIVE["list_t14"]["reply"]["result"]["result"]["picks"]
            if r["event"] == "court_chaplain_task.0301"]
    assert [(r["ai_would_pick"], r["applied"], r["why"]) for r in rows] == [
        ("court_chaplain_task.0301.b", "court_chaplain_task.0301.b", "same_as_ai"),
        ("court_chaplain_task.0301.a", "court_chaplain_task.0301.a", "same_as_ai")]
    for i, r in enumerate(rows):
        again = SE.pick_row(_bridge_pick_from_live(r, i + 1))
        assert {k: again.get(k) for k in r} == r


def test_live_the_reserve_reply_read_the_maximum_back():
    res = LIVE["reserve_t03"]["reply"]["result"]
    assert res["verdict"] == "applied"
    assert res["result"]["readback"]["reserved_max"] == 200
    assert res["result"]["raw"]["template"] == "gold_reserve"
