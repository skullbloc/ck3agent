"""do(intent="army") (army_orders.py), its switches and its surface.

The bridge replies are the shapes bridge/src/army_orders.cpp writes (do_army_disband,
do_army_move, do_army_hold). None of it has run against a live game.
"""
from __future__ import annotations

import asyncio
import re
from pathlib import Path

import army_orders as A
import redesign as R
import tool_text as TT
from test_ireland_walk_replies import Gw

ME = 31506
ON = {A.ENV_ORDERS: "1"}
HOLD = {A.ENV_HOLD: "1"}
BRIDGE_SRC = Path(__file__).resolve().parents[2] / "bridge" / "src" / "army_orders.cpp"


def run(c):
    return asyncio.run(c)


class BridgeGw(Gw):
    def __init__(self, reply):
        super().__init__()
        self.reply = reply

    async def write(self, action, params=None):
        self.writes.append((action, dict(params or {})))
        return self.reply(action, dict(params or {})) if callable(self.reply) else self.reply

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        return self.reply(action, dict(params or {})) if callable(self.reply) else self.reply


def actor():
    return {"id": ME, "name": "Murchad"}


def gate(env):
    return (env.get("blocked") or {}).get("gate"), (env.get("blocked") or {}).get("raw_reason")


# ── the switches ────────────────────────────────────────────────────────────────
def test_exactly_1_is_on():
    for v in (None, "", "0", "yes", "true", "1 ", "11"):
        e = {} if v is None else {A.ENV_ORDERS: v}
        assert not A.orders_on(e) and not A.listed(e), v
    assert A.orders_on(ON) and A.listed(ON) and not A.hold_on(ON)
    assert A.hold_on(HOLD) and A.listed(HOLD) and not A.orders_on(HOLD)


def test_off_the_surface_is_the_one_before_this_lane(monkeypatch):
    # The module-level INTENTS were computed at import with the test environment's
    # switches; the gate runs with both unset.
    assert A.ENV_ORDERS not in __import__("os").environ and A.ENV_HOLD not in __import__("os").environ
    assert "army" not in R.INTENTS
    assert A.do_sentence({}) == ("Armies: the game's AI raises and disbands them, no tool does, and the "
                                 "game's declare-war refuses while they are raised. ")
    assert A.do_sentence({}) in TT.DO
    import do_intent_args as DIA
    assert "army" not in DIA.DEDICATED and "army" not in DIA.table_text()


def test_on_the_sentence_names_only_what_is_on():
    s = A.do_sentence(ON)
    assert "disband" in s and "move" in s and "hold" not in s and "no tool does" not in s
    # A live 1066 game (a recorded game rearmy-checklist.md): run live; the sentence says what the game did
    assert "not yet run" not in s
    assert "within a day" in s and "names why" in s
    h = A.do_sentence(HOLD)
    assert "hold" in h and "disband" not in h and "not yet run" not in h


# ── refusals when off: nothing reaches the bridge ───────────────────────────────
def test_off_disband_and_move_are_refused_before_the_bridge():
    for action, extra in (("disband", {}), ("move", {"title": "c_meath"})):
        gw = BridgeGw({"success": True})
        env = run(A.do_army(gw, {"action": action, "army_id": 7, **extra}, ME, actor(), environ={}))
        assert env["ok"] is False and gate(env) == ("experimental_write_disabled", "army_orders_off"), env
        assert "nothing was sent" in env["blocked"]["summary"]
        assert gw.writes == [] and gw.reads == []


def test_off_hold_is_refused_before_the_bridge():
    gw = BridgeGw({"success": True})
    env = run(A.do_army(gw, {"action": "hold", "army_id": 7, "days": 30}, ME, actor(), environ=ON))
    assert gate(env) == ("experimental_write_disabled", "army_hold_off") and gw.writes == []


def test_do_impl_answers_the_intent_even_when_it_is_not_listed(monkeypatch):
    monkeypatch.delenv(A.ENV_ORDERS, raising=False)
    gw = BridgeGw({"success": True})
    env = run(R.do_impl(gw, intent="army", character_id=ME, args={"action": "disband", "army_id": 7}))
    assert gate(env) == ("experimental_write_disabled", "army_orders_off"), env
    assert not [w for w in gw.writes if w[0].startswith("army_")]


# ── arguments ──────────────────────────────────────────────────────────────────
def test_bad_action_and_missing_ids():
    gw = BridgeGw({"success": True})
    env = run(A.do_army(gw, {"action": "siege", "army_id": 7}, ME, actor(), environ=ON))
    assert gate(env)[0] == "invalid_argument"
    env = run(A.do_army(gw, {"action": "disband"}, ME, actor(), environ=ON))
    assert gate(env) == ("missing_required_argument", "army_id_required")
    env = run(A.do_army(gw, {"action": "move", "army_id": 7}, ME, actor(), environ=ON))
    assert gate(env) == ("missing_required_argument", "province_required")
    for d in (None, 0, 366, "x"):
        env = run(A.do_army(gw, {"action": "hold", "army_id": 7, "days": d}, ME, actor(), environ=HOLD))
        assert gate(env)[1] in ("days_required", "bad_days"), d
    assert gw.writes == []


# ── on: what is sent and what comes back ────────────────────────────────────────
def test_disband_check_sends_the_check_and_says_so():
    gw = BridgeGw(lambda a, p: {"success": True, "action": a, "army_id": 7, "check": p["check"],
                                "allowed": True, "applied": False, "army_after": "present"})
    env = run(A.do_army(gw, {"action": "disband", "army_id": 7, "check": True}, ME, actor(), environ=ON))
    assert gw.writes == [("army_disband", {"actor_id": ME, "army_id": 7, "check": True})]
    assert env["ok"] is True and env["result"]["checked_only"] is True


def test_disband_refused_by_the_game_carries_the_games_reason():
    gw = BridgeGw({"success": False, "action": "army_disband", "reason": "refused_by_game",
                   "game_reason": "Cannot disband while in combat"})
    env = run(A.do_army(gw, {"action": "disband", "army_id": 7}, ME, actor(), environ=ON))
    assert gate(env) == ("engine_refused", "refused_by_game")
    assert "Cannot disband while in combat" in env["blocked"]["summary"]


def test_move_sends_the_title_or_the_province():
    # A key is looked up through title_refs first (query_de_jure), as every title argument
    gw = BridgeGw(lambda a, p: ({"title": {"title_id": 1829, "key": "c_meath", "name": "Meath"}}
                                if a == "query_de_jure" else
                                {"success": True, "action": a, "after": "path_ends_at_target", "check": False}))
    run(A.do_army(gw, {"action": "move", "army_id": 7, "title": "c_meath"}, ME, actor(), environ=ON))
    run(A.do_army(gw, {"action": "move", "army_id": 7, "province_id": 1830}, ME, actor(), environ=ON))
    assert gw.writes == [("army_move", {"actor_id": ME, "army_id": 7, "title": "c_meath", "check": False}),
                         ("army_move", {"actor_id": ME, "army_id": 7, "province_id": 1830, "check": False})]


def test_hold_release_holds():
    gw = BridgeGw(lambda a, p: {"success": True, "action": a, "op": p.get("op")})
    run(A.do_army(gw, {"action": "hold", "army_id": 7, "days": 30}, ME, actor(), environ=HOLD))
    run(A.do_army(gw, {"action": "release", "army_id": 7}, ME, actor(), environ=HOLD))
    run(A.do_army(gw, {"action": "holds"}, ME, actor(), environ=HOLD))
    assert gw.writes == [("army_hold", {"actor_id": ME, "army_id": 7, "op": "hold", "days": 30}),
                         ("army_hold", {"actor_id": ME, "army_id": 7, "op": "release"})]
    assert gw.reads == [("army_hold", {"actor_id": ME, "op": "holds"})]


# ── the codes: the bridge's, this module's and the shared table agree ───────────
def test_every_code_has_its_gate_in_the_shared_table():
    for code, g in A.REFUSALS.items():
        assert g in R.GATE_TABLE, code
        assert R.RAW_REASON_TO_GATE.get(code) == g, code


def test_every_bridge_refusal_code_is_mapped():
    src = BRIDGE_SRC.read_text(encoding="utf-8")
    codes = set(re.findall(r'refuse\(response_out, "army_\w+", "(\w+)"', src))
    codes |= set(re.findall(r'return "(\w+)";', src))
    codes |= set(re.findall(r'r\.value_string\("(in_combat|cannot_be_ordered_there|refused_by_game)"\)', src))
    assert {"army_orders_off", "army_hold_off", "not_your_army", "refused_by_game"} <= codes
    missing = sorted(c for c in codes if c not in A.REFUSALS and c not in ("apply_crashed", "unknown_op"))
    assert not missing, missing


def test_the_bridge_reads_the_same_two_names():
    src = BRIDGE_SRC.read_text(encoding="utf-8")
    hdr = (BRIDGE_SRC.parents[1] / "include" / "army_orders.h").read_text(encoding="utf-8")
    assert set(re.findall(r'getenv\("(CK3_BRIDGE_ARMY_\w+)"\)', src)) == {A.ENV_ORDERS, A.ENV_HOLD}
    assert f'kOrdersEnv = "{A.ENV_ORDERS}"' in hdr and f'kHoldEnv = "{A.ENV_HOLD}"' in hdr
