"""L15 witness M1: the several-rulers guard fires on the bridge's own roster.

Both gateways' multi_actor() (now actor_gate(), redesign.actor_gate_of) read `get_controlled_roster` under keys the bridge
never wrote (`characters` / `controlled`), so `actor_required` could not fire and,
with several rulers held, a call with no character_id silently acted as the
primary. The doubles here carry the EMITTER's keys:
  get_controlled_roster  dispatch_router.cpp do_get_controlled_roster
                         (action, primary_character_id, roster, player_character_id)
  get_bridge_state       dispatch_router.cpp `r.key("sessions")` -> session.cpp
                         Session::to_json (session_id, primary_character_id,
                         claimed_character_ids, ...)
The roster is GLOBAL (every seat's claims); character_id=0 resolves to the asking
seat's own primary, and when that is 0 to the GLOBAL primary (dispatch_router.cpp
get_controlled_character), so the seat's own row decides: an unreadable seat or
roster must name its actor (m9), and a seat holding nothing while others hold
rulers answers no_ruler_bound (m8) instead of reading another seat's ruler.
"""
from __future__ import annotations

import asyncio
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

ms = pytest.importorskip("mcp_server")
import redesign as R  # noqa: E402

ME, OTHER, FOE = 7, 8, 9
SID = "s-mine"
WRITES = {"send_interaction", "set_council_task", "change_law", "declare_war"}


def _roster(ids):
    return {"action": "get_controlled_roster", "primary_character_id": ids[0] if ids else 0,
            "roster": list(ids), "player_character_id": 0}


def _session(sid, claims):
    return {"session_id": sid, "client_name": "t", "mode": "control", "claim_seq": 1,
            "session_state": "attached", "primary_character_id": claims[0] if claims else 0,
            "heir_character_id": 0, "claimed_character_ids": list(claims),
            "unbound_reason": "", "unbound_heir_id": 0}


class Bridge:
    """The bridge as the asking seat SID sees it. query_character(0) resolves as
    get_controlled_character does: the seat's own primary, else the global one
    (the front of the roster, do_get_controlled_roster's primary_character_id)."""

    def __init__(self, roster, sessions, roster_reply=None):
        self.roster, self.sessions, self.sent = roster, sessions, []
        self.roster_reply = roster_reply

    def _id0(self):
        mine = next((r for r in self.sessions if r["session_id"] == SID), None)
        own = mine["primary_character_id"] if mine else 0
        return own or (self.roster[0] if self.roster else 0)

    def reply(self, action, params):
        self.sent.append(action)
        if action == "get_controlled_roster":
            return self.roster_reply if self.roster_reply is not None else _roster(self.roster)
        if action == "get_bridge_state":
            return {"action": "get_bridge_state", "sessions": self.sessions}
        if action == "get_tick_info":
            return {"paused": True, "game_speed": 1, "game_date": "867.1.2", "tick_id": 3}
        if action == "query_character":
            cid = (params or {}).get("character_id") or self._id0()
            if not cid:
                return {"success": False, "action": "query_character"}
            return {"id": cid, "alive": True, "name": "Halfdan", "primary_title_id": 77}
        return {"success": True, "action": action}


def _mcp_gateway(monkeypatch, bridge, sid=SID):
    state = ms.BridgeState()
    state.connected = True
    state.game_date = "867.1.2"
    state._client = type("C", (), {"session_id": sid})()

    async def call(_s, action, params, timeout=5.0):
        return bridge.reply(action, params)
    monkeypatch.setattr(ms, "_action_bridge", call)
    monkeypatch.setattr(ms, "_query_bridge", call)
    return ms.BridgeGateway(state)


def _agent_gateway(bridge):
    agent = pytest.importorskip("agent")

    class Loop:
        _last_observation = {"paused": True, "tick_id": 1, "game_speed": 1}
        _last_game_date = "867.1.2"

        def __init__(self):
            self.bridge = type("B", (), {"connected": True})()

        async def _send_and_wait(self, action, params, timeout=10.0):
            return bridge.reply(action, params)

        async def _send_no_wait(self, action, params):
            bridge.reply(action, params)

        async def _read(self, action, params):
            return bridge.reply(action, params)
    return agent._AgentGateway(Loop())


def _do_without_an_id(gw):
    return asyncio.run(R.do_impl(gw, intent="council_task", character_id=0,
                                 args={"task": "chancellor_domestic_affairs"}))


def _blocked(env):
    return (env.get("blocked") or {}).get("gate")


def test_two_rulers_held_by_this_seat_a_write_without_an_id_is_actor_required(monkeypatch):
    bridge = Bridge([ME, OTHER], [_session(SID, [ME, OTHER])])
    env = _do_without_an_id(_mcp_gateway(monkeypatch, bridge))
    assert env["ok"] is False and _blocked(env) == "actor_required", env
    assert not WRITES & set(bridge.sent), bridge.sent


def test_the_agent_loop_gateway_answers_the_same(monkeypatch):
    bridge = Bridge([ME, OTHER], [_session(SID, [ME, OTHER])])
    env = _do_without_an_id(_agent_gateway(bridge))
    # The loop's gateway cannot name its seat (own_session_id is None), so a
    # roster of two is ambiguous for it and the call must name its actor.
    assert _blocked(env) == "actor_required", env


def test_with_an_id_the_same_seat_is_not_refused(monkeypatch):
    bridge = Bridge([ME, OTHER], [_session(SID, [ME, OTHER])])
    env = asyncio.run(R.do_impl(_mcp_gateway(monkeypatch, bridge), intent="council_task",
                                character_id=ME, args={"task": "chancellor_domestic_affairs"}))
    assert _blocked(env) != "actor_required", env


def _gate(gw):
    g = asyncio.run(gw.actor_gate())
    return None if g is None else g["gate"]


def test_two_seats_holding_one_ruler_each_are_not_multi_actor(monkeypatch):
    bridge = Bridge([ME, OTHER], [_session(SID, [ME]), _session("s-other", [OTHER])])
    assert _gate(_mcp_gateway(monkeypatch, bridge)) is None


def test_a_seat_whose_own_row_is_absent_must_name_its_actor(monkeypatch):
    bridge = Bridge([ME, OTHER], [_session("s-other", [OTHER])])
    assert _gate(_mcp_gateway(monkeypatch, bridge)) == "actor_required"


def test_one_ruler_held_is_not_multi_actor(monkeypatch):
    # The seat's own row is read even for a roster of one: that one ruler may be
    # another seat's (m8).
    bridge = Bridge([ME], [_session(SID, [ME])])
    assert _gate(_mcp_gateway(monkeypatch, bridge)) is None
    assert "get_bridge_state" in bridge.sent


def test_nobody_holding_a_ruler_leaves_the_bridge_to_answer_no_ruler_bound(monkeypatch):
    bridge = Bridge([], [_session(SID, [])])
    gw = _mcp_gateway(monkeypatch, bridge)
    assert _gate(gw) is None
    env = asyncio.run(R.observe_impl(gw))
    assert _blocked(env) == "no_ruler_bound", env


# m8: a seat holding nothing while another seat holds a ruler. The bridge would
# resolve character_id=0 to the GLOBAL primary, the other seat's ruler.
@pytest.mark.parametrize("roster", [[OTHER], [OTHER, FOE]], ids=["one", "two"])
def test_an_empty_seat_is_not_shown_another_seats_ruler(monkeypatch, roster):
    others = [_session("s-other", [r]) for r in roster]
    bridge = Bridge(roster, [_session(SID, [])] + others)
    env = asyncio.run(R.observe_impl(_mcp_gateway(monkeypatch, bridge)))
    assert env["ok"] is False and _blocked(env) == "no_ruler_bound", env
    assert "query_character" not in bridge.sent, bridge.sent
    env = _do_without_an_id(_mcp_gateway(monkeypatch, bridge))
    assert _blocked(env) == "no_ruler_bound", env
    assert not WRITES & set(bridge.sent), bridge.sent


def test_an_empty_seat_answers_with_its_unbound_reason(monkeypatch):
    mine = dict(_session(SID, []), unbound_reason="heir_is_player", unbound_heir_id=5)
    bridge = Bridge([OTHER], [mine, _session("s-other", [OTHER])])
    env = asyncio.run(R.observe_impl(_mcp_gateway(monkeypatch, bridge)))
    assert env["blocked"]["measured"]["unbound_reason"] == "heir_is_player", env


# m9: a roster read that fails must not let a seat holding two rulers act as its
# primary.
@pytest.mark.parametrize("unread", [
    {"success": False, "action": "get_controlled_roster"},
    {"action": "get_controlled_roster", "primary_character_id": ME},
    {"error": "read_timeout"},
], ids=["refusal", "no_roster_key", "error"])
def test_an_unread_roster_must_name_its_actor(monkeypatch, unread):
    bridge = Bridge([ME, OTHER], [_session(SID, [ME, OTHER])], roster_reply=unread)
    env = _do_without_an_id(_mcp_gateway(monkeypatch, bridge))
    assert _blocked(env) == "actor_required", env
    assert not WRITES & set(bridge.sent), bridge.sent


def test_a_roster_read_that_raises_must_name_its_actor(monkeypatch):
    bridge = Bridge([ME, OTHER], [_session(SID, [ME, OTHER])])
    gw = _mcp_gateway(monkeypatch, bridge)

    async def boom(_s, action, params, timeout=5.0):
        if action == "get_controlled_roster":
            raise ConnectionError("gone")
        return bridge.reply(action, params)
    monkeypatch.setattr(ms, "_action_bridge", boom)
    monkeypatch.setattr(ms, "_query_bridge", boom)
    assert _do_without_an_id(gw)["blocked"]["gate"] == "actor_required"


def test_roster_ids_reads_the_emitted_key_only():
    assert R.roster_ids(_roster([ME, OTHER])) == [ME, OTHER]
    assert R.roster_ids({"characters": [ME, OTHER]}) is None
    assert R.roster_ids(None) is None
