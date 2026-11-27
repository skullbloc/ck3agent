"""Rows from the seated Sweden 867 run and of the live proofs, replayed from its recorded replies.

The fixture `fixtures/sweden867_replies.json` is the run's seat-facing tool replies, cut by row from
a recorded game's tool-call log (tool replies only; no seat text). The replays below drive the real tool
bodies (mcp_server.do / declare_war) with the redesign impl stubbed to answer what the game answered.
"""
import asyncio
import copy
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import mcp_server as ms  # noqa: E402
import redesign as R  # noqa: E402
import refusal_memo as RM  # noqa: E402

FIX = json.loads((Path(__file__).parent / "fixtures" / "sweden867_replies.json").read_text())


def run(coro):
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


class _Gw:
    health = "ok"

    def __init__(self, now):
        self.now = now

    def game(self):
        return dict(self.now)

    async def read(self, action, params=None):
        return {"armies": []}


@pytest.fixture
def seat(monkeypatch):
    """The `do` tool with the game stubbed: `answers` is a queue of envelopes the game gives, `now` the game's
    moment the tool reads; `sent` lists every call that reached the game."""
    st = {"now": {"tick": None, "date": None, "paused": True, "speed": 3}, "answers": [], "sent": []}

    async def ensure(*_a, **_k):
        return None

    async def gw_now():
        return _Gw(st["now"])

    async def do_impl(gw, **kw):
        st["sent"].append(copy.deepcopy(kw.get("args")))
        env = copy.deepcopy(st["answers"].pop(0))
        env["game"] = dict(st["now"])
        return env

    async def declare_impl(gw, **kw):
        st["sent"].append(("declare", copy.deepcopy(kw.get("declare_args"))))
        env = copy.deepcopy(st["answers"].pop(0))
        env["game"] = dict(st["now"])
        return env

    monkeypatch.setattr(ms, "_ensure_connected", ensure)
    monkeypatch.setattr(ms, "_gateway_now", gw_now)
    monkeypatch.setattr(ms, "_get_state", lambda: None)
    monkeypatch.setattr(ms, "_state_game", lambda _s: dict(st["now"]))
    monkeypatch.setattr(ms, "_REFUSALS", RM.Memo())
    monkeypatch.setattr(R, "do_impl", do_impl)
    monkeypatch.setattr(R, "declare_war_impl", declare_impl)
    return st


def _at(st, call, tick):
    st["now"] = {"tick": tick, "date": call["date"], "paused": True, "speed": 3}


def _do(call):
    a = call["args"]
    return run(ms.do(intent=a.get("intent"), action_class=a.get("action_class"), args=copy.deepcopy(a.get("args"))))


# ── The repeat guard keys on what changed, not on the calendar month ─────────────────────────────

def test_1_T18_the_disband_after_a_passing_check_reaches_the_game(seat, monkeypatch):
    """A recorded game, turns 17-18: disband refused 0869.12.06 (tick 1069, "Cannot disband while in Retreat"); at
    0869.12.20 (tick 1083) check=true said allowed, then the disband came back from memory, "not sent". Red without
    the rule (keyed on the month 0869.12) for an army that stands where it was refused: the moved_since
    re-asks only a MOVED army (the recorded replies carry no army_at, so whether army 38 had moved is
    not read); the state that changed here (the retreat ended) is not the army's place."""
    async def stood(_gw, _env):
        return False
    monkeypatch.setattr(ms._army_orders, "moved_since", stood)
    refused, check, disband, applied = FIX["repeat_t17_t18"]
    assert refused["env"]["verdict"] == "rejected" and refused["env"]["game"]["tick"] == 1069
    _at(seat, refused, 1069)
    seat["answers"].append(refused["env"])
    _do(refused)
    _at(seat, check, 1083)
    seat["answers"].append(check["env"])
    _do(check)
    seat["answers"].append(applied["env"])          # the game's answer to it (applied at turn 19 on the next turn)
    out = _do(disband)
    assert len(seat["sent"]) == 3, "the disband was answered from memory"
    assert "repeat_of" not in out and out["result"]["raw"]["applied"] is True


def test_1_a_refused_offer_a_game_day_later_in_the_same_month_reaches_the_game(seat):
    """Not an army write (no moved_since): a white peace refused at turn 17's tick is asked again at turn 18's.
    Red without the rule (the same month)."""
    t17, t18 = FIX["repeat_t17_t18"][0], FIX["repeat_t17_t18"][2]
    no = R.envelope("do", ok=False, blocked=R.why_blocked("recipient_refuses", summary="x")) | {"verdict": "rejected"}
    args = {"war_id": 49, "terms": "white_peace"}
    _at(seat, t17, 1069)
    seat["answers"] += [no, no]
    run(ms.do(intent="make_peace", args=dict(args)))
    _at(seat, t18, 1083)
    out = run(ms.do(intent="make_peace", args=dict(args)))
    assert len(seat["sent"]) == 2 and "repeat_of" not in out


def test_1_T10_a_check_is_never_answered_from_memory_nor_remembered(seat):
    """A recorded game, turns 9-10: `disband check=true` at tick 608 refused; at tick 636 the same check came back
    from memory. do_key is None for a check; kept here as a replay."""
    first, again = FIX["repeat_t9_t10"]
    assert first["args"]["args"]["check"] is True and "repeat_of" in again["env"]
    _at(seat, first, 608)
    seat["answers"] += [first["env"], first["env"]]
    _do(first)
    _do(first)                                      # the same check at the same tick: asked again
    _at(seat, again, 636)
    seat["answers"].append(first["env"])
    _do(again)
    assert len(seat["sent"]) == 3
    # and a check never stands in for the write: the real disband at the same tick is sent
    seat["answers"].append(first["env"])
    run(ms.do(intent="army", args={"army_id": 38, "action": "disband"}))
    assert len(seat["sent"]) == 4


def test_1_CONTROL_the_same_write_at_the_same_tick_with_nothing_applied_is_not_sent(seat, monkeypatch):
    """The guard still does its job: the turn-17 disband asked twice at tick 1069, the army where it was."""
    async def stood(_gw, _env):
        return False
    monkeypatch.setattr(ms._army_orders, "moved_since", stood)
    refused = FIX["repeat_t17_t18"][0]
    _at(seat, refused, 1069)
    seat["answers"].append(refused["env"])
    _do(refused)
    out = _do(refused)
    assert len(seat["sent"]) == 1 and out["repeat_of"]["game_date"] == "0869.12.06"
    assert out["result"].get("sent", False) is False
    assert any(w["code"] == "repeat_of_refused_write" and "no game day has passed" in w["detail"]
               for w in out["warnings"])


def test_1_a_write_of_yours_that_applied_since_re_asks_the_game(seat, monkeypatch):
    """Turn 17: the seat surrendered war 49 (applied) at the same tick as the refused disband; any applied write
    may change what the game checks, so the refusal is not repeated after it."""
    async def stood(_gw, _env):
        return False
    monkeypatch.setattr(ms._army_orders, "moved_since", stood)
    refused = FIX["repeat_t17_t18"][0]
    _at(seat, refused, 1069)
    seat["answers"].append(refused["env"])
    _do(refused)
    seat["answers"].append(R.envelope("do", result={"x": 1}) | {"verdict": "applied"})
    run(ms.do(intent="make_peace", args={"war_id": 49, "terms": "white_peace"}))
    seat["answers"].append(refused["env"])
    _do(refused)
    assert len(seat["sent"]) == 3


def test_1_CONTROL_an_unread_moment_answers_nothing_from_memory(seat):
    refused = FIX["repeat_t17_t18"][0]
    seat["now"] = {"tick": None, "date": None}
    seat["answers"] += [refused["env"], refused["env"]]
    _do(refused)
    _do(refused)
    assert len(seat["sent"]) == 2


def test_1_a_reply_without_a_verdict_counts_as_a_change():
    """The army replies carry no verdict (turn 19's applied disband): counted as a change (fail open)."""
    assert RM.may_have_changed(FIX["repeat_t17_t18"][3]["env"]) is True
    assert RM.may_have_changed(FIX["repeat_t17_t18"][0]["env"]) is False
    assert RM.may_have_changed(FIX["declare_t8"]["env"]) is False


def test_1_the_moment_reads_the_tick_first_then_the_date():
    assert RM.moment({"tick": 1069, "date": "0869.12.06"}) == ("tick", 1069)
    assert RM.moment({"tick": None, "date": "869.12.6"}) == ("date", "0869.12.06")
    assert RM.moment("0869.12.06") == ("date", "0869.12.06")
    assert RM.moment({"tick": True, "date": None}) is None and RM.moment(None) is None


# ── No 1066 text in an 867 run; the vassalization limit is the one read for this game ─────────────

def _threat_gw_867(innovations):
    from test_succession_study_fixes import _threat_gw
    gw = _threat_gw({27052: [{"cb_key": "vassalization_cb", "valid": False}]}, my_counties=12)
    gw.extra["query_innovation_progress"] = {"innovations": [{"name": n, "is_known": True} for n in innovations]}
    return gw


def test_2_the_recorded_867_rows_carried_1066_text():
    """The hazard as received (turn 21 attackers_new; turn 23 income_levers)."""
    assert "a crown for a 1066 king (limit 7)" in FIX["attackers_new_t21"]["attackers_new"]["vassalization"]
    assert "1066 Dublin seat" in json.dumps(FIX["income_levers_t23"]["rows"])


def test_2_the_limit_is_the_one_read_from_the_attackers_innovations():
    """An 867 tribal neighbour with no war innovation: his limit is 4, so 5 counties close it (the 1066
    constant said 8). Red without the rule: the verdict was the constant 'limit 7' for a 1066 king."""
    import threats as T
    T.reset()
    res = run(T.read(_threat_gw_867([]), 31506))
    v = res["vassalization"]
    assert "1066" not in json.dumps(v) and v["limit_read"] == 4
    assert v["verdict"].startswith("closed at 5 counties or a crown (limit 4") and "you are above it" in v["verdict"]
    T.reset()
    v = run(T.read(_threat_gw_867(["innovation_casus_belli", "innovation_chronicle_writing",
                                   "innovation_divine_right"]), 31506))["vassalization"]
    assert v["limit_read"] == 13 and v["verdict"].endswith("you hold 12: 2 more close it")


def test_2_CONTROL_no_limit_read_gives_no_number():
    import threats as T
    v = T.vassalization_summary(2, 6, [None])
    assert v["verdict"].startswith("unknown") and "limit_read" not in v and "7" not in v["verdict"]
    assert "crown" in T.vassalization_summary(4, 3)["verdict"]


def test_2_no_served_reply_string_or_note_names_a_start():
    """The census (fail open, from the code): every string constant of the shipped server modules that names a
    start date or a start's seat/realm, outside the key-spelling examples, the evidence labels of
    the pending table, the mock state and the retired API loop (listed in that run's notes)."""
    import ast
    import re
    pat = re.compile(r"\b1066\b|\b867\b|Dublin|Munster|Murchad|Francia|Leinster", re.I)
    leave = re.compile(r"live" r"867 |c_dublin|^_?\d+[._]\d|_1066_|negotiate_the|e\.g\. ")
    left = {"agent.py", "prompts.py", "guide.py", "tools.py"}           # the retired API loop and its prompt
    found = []
    for p in sorted(Path(__file__).parent.glob("*.py")):
        if p.name.startswith("test_") or p.name in left:
            continue
        tree = ast.parse(p.read_text())
        docs = {id(n.body[0].value) for n in ast.walk(tree)
                if isinstance(n, (ast.Module, ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))
                and n.body and isinstance(n.body[0], ast.Expr) and isinstance(n.body[0].value, ast.Constant)}
        mock = {id(c) for f in ast.walk(tree) if isinstance(f, ast.FunctionDef) and f.name == "_make_mock_state"
                for c in ast.walk(f)}
        for n in ast.walk(tree):
            if isinstance(n, ast.Constant) and isinstance(n.value, str) and id(n) not in docs | mock \
                    and pat.search(n.value) and not leave.search(n.value):
                found.append(f"{p.name}:{n.lineno}: {n.value[:80]}")
    assert found == [], found


def test_2_the_sweden_set_serves_no_1066_text():
    import notes as N
    texts = [n["text"] for n in N.derive(N.load(), "sweden867")["notes"]]
    assert texts and not [t for t in texts if "1066" in t]


# ── A hire into debt needs args.confirm_debt=true; check=true shows it ───────────────────────────

class _HireGw(_Gw):
    def __init__(self, gold):
        super().__init__({"tick": 1799, "date": "0871.12.06", "paused": True, "speed": 3})
        self.gold = gold

    async def read(self, action, params=None):
        if action == "query_character":
            return {"id": 12330, "gold": self.gold, "prestige": 625.0, "piety": 500.0, "ruler_tier": 3,
                    "income_breakdown": {"total_monthly": 4.78}}
        return {}


@pytest.fixture
def hire(monkeypatch):
    """Turn 28 as read: company 33 listed at 158.06 (upper 179.61) with the listing's debt_after_hire; the purse
    71.85 (the listing's gold_after -107.76 at the upper price)."""
    row = FIX["merc_listing_t28"]["company_33"][0]
    sent = []

    async def rows(gw, me, include_hired=False):
        return [copy.deepcopy(row)], 0, "note"

    async def actor(gw, cid, tool):
        return {"id": 12330, "name": "Björn"}, None

    async def act(gw, **kw):
        sent.append(copy.deepcopy(kw["args"]))
        return copy.deepcopy(FIX["hire_t28"]["env"])

    async def nothing(*_a, **_k):
        return None

    monkeypatch.setattr(R, "mercenary_rows", rows)
    monkeypatch.setattr(R, "_resolve_actor", actor)
    monkeypatch.setattr(R, "_do_impl", act)
    monkeypatch.setattr(R, "_explain_hire_refusal", nothing)
    return sent


def test_3_T28_the_hire_into_debt_is_not_sent_unconfirmed(hire):
    """Red without the rule: the hire for 157 at 71 gold was sent and applied (3.5 years in debt followed)."""
    assert FIX["hire_t28"]["env"]["verdict"] == "applied" and FIX["hire_t28"]["env"]["result"]["purse_moved"]["gold"] == -157.0
    out = run(R.do_impl(_HireGw(71.85), intent="hire_merc", character_id=12330, args={"company_id": 33}))
    assert hire == [] and out["verdict"] == "rejected"
    b = out["blocked"]
    assert b["gate"] == "debt_unconfirmed" and "no new wars while in debt" in b["summary"]
    assert b["fix"]["args"]["args"] == {"company_id": 33, "confirm_debt": True}
    assert out["result"]["debt_after_hire"]["gold_after"] == -107.76 and out["result"]["sent"] is False


def test_3_check_shows_the_debt_and_sends_nothing(hire):
    out = run(R.do_impl(_HireGw(71.85), intent="hire_merc", character_id=12330,
                        args={"company_id": 33, "check": True}))
    r = out["result"]
    assert hire == [] and r["check"] is True and r["sent"] is False and r["needs"] == "args.confirm_debt=true"
    assert r["consequence"].startswith("no new wars while in debt")


def test_3_confirmed_it_is_sent_without_the_confirm_key(hire):
    out = run(R.do_impl(_HireGw(71.85), intent="hire_merc", character_id=12330,
                        args={"company_id": 33, "confirm_debt": True}))
    assert hire == [{"company_id": 33}] and out["verdict"] == "applied"


def test_3_CONTROL_a_hire_the_purse_covers_is_sent_unconfirmed(hire):
    run(R.do_impl(_HireGw(400.0), intent="hire_merc", character_id=12330, args={"company_id": 33}))
    assert hire == [{"company_id": 33}]


def test_3_the_argument_table_lists_both_keys_and_no_other_intent_takes_them():
    import do_intent_args as D
    assert D.unknown_args("hire_merc", {"company_id": 1, "check": True, "confirm_debt": True})[0] == []
    assert "confirm_debt" in D.unknown_args("mint_title", {"title_key": "k_sweden", "confirm_debt": True})[0]



# ── The price probe's harmless script errors are kept out of the seat's reply (logged) ───────────

def _t8_sent():
    raw = copy.deepcopy(FIX["declare_t8"]["env"]["result"]["raw"])
    return {"raw": raw, "guard": raw["guard"], "errors": list(raw["errors"]), "late": False, "marks": raw["marks"],
            "channel": {"state": "open"}}


def test_5_T8_the_refused_declaration_carries_no_probe_noise(caplog):
    """Red without the rule: game_errors and raw.errors carried the 8 lines beside armies_raised."""
    assert len(FIX["declare_t8"]["env"]["result"]["game_errors"]) == 8
    import logging
    caplog.set_level(logging.INFO, logger="ck3agent.redesign")
    v = run(R.script_verdict(_Gw({"tick": 587, "date": "0868.08.11"}), _t8_sent(), landed=None, before=None,
                             what="war", guard_gate="can_declare_war_false", guard_reason="can_declare_war",
                             error_gate="script_error"))
    assert v["verdict"] == "rejected"
    assert v["result"]["game_errors"] == [] and v["result"]["raw"]["errors"] == []
    assert v["result"]["probe_noise_logged"] == 8 and "ck3a_cbp" not in json.dumps(v)
    assert "8 price-probe error lines" in caplog.text and "ck3a_cbp" in caplog.text


def test_5_CONTROL_the_game_s_own_errors_stay():
    other = "Unknown trigger 'foo'"
    keep, noise = R.split_probe_noise([other, "Event target link 'var' returned an unset scope"])
    assert keep == [other, "Event target link 'var' returned an unset scope"] and noise == []
    keep, noise = R.split_probe_noise(FIX["declare_t8"]["env"]["result"]["raw"]["errors"] + [other])
    assert keep == [other] and len(noise) == 8


# ── a lost war's prestige counted once, at both observe depths ──────────────
#    A test run (Munster 1066): declare vassalization_cb on 29585 at 1066.09.15 (-125), surrender war 4 at
#    1066.09.22 (-350, read before/after), observe at 1066.10.06: prestige 728.16. The news depth (since 09.22)
#    said your_acts [-350] AND in_this_step [war ended, expected -350], unexplained +253.16; the full observe
#    (since 09.15) said -96.84 but "no war of yours ended".

ME9 = 31762
SURRENDER = {"date": "1066.09.22", "what": "ended the war against 29585: surrender", "cost_known": True,
             "prestige": -350.0, "measured": True}
WAR4 = {"war_id": 4, "name": "Munsterian-Athlonian Vassalization War", "attacker_id": ME9, "defender_id": 29585,
        "my_side": "attacker", "outcome": "defender_victory", "end_date": "1066.09.22", "_cb": "vassalization_cb"}


@pytest.fixture
def acts9(monkeypatch):
    import news
    monkeypatch.setattr(news, "_ACTS", {ME9: [{"date": "1066.09.15", "what": "declared war 4 (vassalization_cb)",
                                               "cost_known": True, "prestige": -125.0},
                                              dict(SURRENDER, war_id=4)]})
    return news


def test_9_the_news_depth_counts_the_surrender_once(acts9):
    """Red without the rule: unexplained +253.16 (the -350 subtracted twice)."""
    prev = {"actor_id": ME9, "date": "1066.09.22", "prestige": 1175.0, "monthly_prestige": 0.0}
    snap = {"actor_id": ME9, "date": "1066.10.06", "prestige": 728.16}
    r = acts9._resource_change(prev, snap, "prestige", {"wars_ended": [dict(WAR4)]})
    assert r["unexplained"] == -96.84, r
    row = next(e for e in r["in_this_step"] if e["what"].startswith("war ended"))
    assert "expected" not in row and row["counted_in"] == "your_acts" and row["your_act"] == SURRENDER["what"]


def test_9_an_act_recorded_without_a_war_id_pairs_only_one_to_one(acts9):
    acts9._ACTS[ME9][1].pop("war_id")
    prev = {"actor_id": ME9, "date": "1066.09.22", "prestige": 1175.0, "monthly_prestige": 0.0}
    snap = {"actor_id": ME9, "date": "1066.10.06", "prestige": 728.16}
    assert acts9._resource_change(prev, snap, "prestige", {"wars_ended": [dict(WAR4)]})["unexplained"] == -96.84
    two = acts9._resource_change(prev, snap, "prestige", {"wars_ended": [dict(WAR4), dict(WAR4, war_id=9)]})
    assert all("expected" in e for e in two["in_this_step"] if e["what"].startswith("war ended"))   # unpaired


def test_9_CONTROL_a_war_the_game_ended_keeps_its_price(acts9):
    acts9._ACTS[ME9] = []
    prev = {"actor_id": ME9, "date": "1066.09.22", "prestige": 1175.0, "monthly_prestige": 0.0}
    snap = {"actor_id": ME9, "date": "1066.10.06", "prestige": 825.0}
    r = acts9._resource_change(prev, snap, "prestige", {"wars_ended": [dict(WAR4)]})
    assert next(e for e in r["in_this_step"] if e["what"].startswith("war ended"))["expected"] == -350


class _HistGw:
    def __init__(self):
        self.reads = []

    async def read_many(self, reqs):
        self.reads += [(a, p) for a, p in reqs if "war_id" in p]
        # The window's reads by side (attacker_id / defender_id) are answered from the same ledger
        return [{"wars": [{"war_id": p.get("war_id", 4), "name": "Munsterian-Athlonian Vassalization War",
                           "attacker_id": ME9, "defender_id": 29585, "outcome": "defender_victory",
                           "end_date": "1066.09.22", "target_titles": []}]} for _a, p in reqs]


def test_9_the_full_depth_sees_a_war_your_act_ended_inside_its_window(acts9):
    """The full observe's window (09.15 -> 10.06) held war 4's start and end: in neither list. Red without the rule:
    wars_ended [] and ruled_out "no war of yours ended"."""
    prev = {"actor_id": ME9, "date": "1066.09.15"}
    snap = {"actor_id": ME9, "date": "1066.10.06"}
    gw = _HistGw()
    ended = run(acts9.wars_ended(gw, prev, snap, {}, {}))
    assert [r["war_id"] for r in ended] == [4] and ended[0]["my_side"] == "attacker"
    assert ended[0]["outcome"] == "defender_victory"
    full = acts9._resource_change(dict(prev, prestige=1300.0, monthly_prestige=0.0),
                                  dict(snap, prestige=728.16), "prestige", {"wars_ended": ended})
    assert full["unexplained"] == -96.84 and "no war of yours ended" not in full.get("ruled_out", [])


def test_9_CONTROL_a_war_the_last_read_listed_is_read_once(acts9):
    gw = _HistGw()
    ended = run(acts9.wars_ended(gw, {"actor_id": ME9, "date": "1066.09.22"}, {"actor_id": ME9, "date": "1066.10.06"},
                                 {4: {k: v for k, v in WAR4.items() if not k.startswith("_")}}, {}))
    assert [r["war_id"] for r in ended] == [4] and len(gw.reads) == 1


# ── after session add every id-less call is refused actor_required ──────────────
#    Intended for a seat holding several rulers (character_id=0 cannot say which); a seat holding one is
#    not refused. The refusal names the primary to pass.

class _ActorGw:
    def __init__(self, claims, primary=14445):
        import types
        self.state = types.SimpleNamespace(_client=types.SimpleNamespace(session_id="s1"))
        self.claims, self.primary = claims, primary

    async def read(self, action, params=None):
        if action == "get_controlled_roster":
            return {"roster": list(self.claims)}
        if action == "get_bridge_state":
            return {"sessions": [{"session_id": "s1", "primary_character_id": self.primary,
                                  "claimed_character_ids": list(self.claims)}]}
        return {}


def test_10_one_ruler_is_not_refused():
    assert run(R.actor_gate_of(_ActorGw([12015], primary=12015))) is None


def test_10_several_rulers_are_refused_naming_the_primary():
    b = run(R.actor_gate_of(_ActorGw([12015, 14445, 14446], primary=12015)))
    assert b["gate"] == "actor_required" and "pass character_id=12015" in b["summary"]
    assert b["measured"]["primary"] == 12015 and b["measured"]["own_claims"] == 3


# ── net_realised describes the step just read, for the ruler read ────────────────

def test_12_net_realised_is_re_read_after_the_news_block(monkeypatch):
    """A West Francia game, 0867.08.08: the price row said from 03.15 to 05.27 (the step before the last): observe builds the
    goal rows before its news block measures this step. Red without the rule: no refresh_net_realised."""
    import news
    monkeypatch.setattr(news, "_REALISED", {12015: {"monthly": 3.1, "from": "0867.03.15", "to": "0867.05.27"}})
    result = {"goal": {"next_title": {"requirements": [{"requirement": "price",
                                                        "net_realised": R.net_realised_row(12015)}]}}}
    news._resource_change({"actor_id": 12015, "date": "0867.05.27", "gold": 100.0},
                          {"actor_id": 12015, "date": "0867.08.08", "gold": 130.0}, "gold", {})
    R.refresh_net_realised(result, 12015)
    nr = result["goal"]["next_title"]["requirements"][0]["net_realised"]
    assert (nr["from"], nr["to"]) == ("0867.05.27", "0867.08.08")


def test_12_a_still_purse_is_a_step_read_too(monkeypatch):
    import news
    monkeypatch.setattr(news, "_REALISED", {})
    news._resource_change({"actor_id": 7, "date": "0867.05.27", "gold": 100.0},
                          {"actor_id": 7, "date": "0867.08.08", "gold": 100.4}, "gold", {})
    assert news.realised_net(7)["to"] == "0867.08.08"


def test_12_CONTROL_id_less_observe_reads_the_ruler_not_key_0(monkeypatch):
    import news
    monkeypatch.setattr(news, "_REALISED", {12015: {"monthly": 1.0, "from": "a", "to": "b"}})
    res = {"x": {"net_realised": R.net_realised_row(0)}}
    assert res["x"]["net_realised"].startswith("unread")
    R.refresh_net_realised(res, 12015)
    assert res["x"]["net_realised"]["to"] == "b"
