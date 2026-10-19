"""lane-prebind: the pre-bind waits for the game.

lane-multi run 1: `ck3agent run` started while the save was still loading, the
pre-bind of both declared rulers was refused `character_not_found` at once and
never retried; the run prompt recovered it by binding for itself. Now a
handshake ack whose session_state is not `active` defers the bind to a task
that polls get_tick_info every PREBIND_POLL_S for up to CK3_AGENT_PREBIND_WAIT_S,
binds when the game is active, and on the deadline attempts the bind anyway so
a game that never comes up yields the bridge's own refusal, as before. The
session record notes when the bind landed (tick, date) and how long it waited.
"""
import asyncio
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import mcp_server  # noqa: E402


class _Client:
    """A handshake that answers `ack`; a recv that never delivers (the listener
    the bind starts for a real client idles instead of erroring)."""

    def __init__(self, ack):
        self.ack = ack

    async def handshake(self, **kw):
        return dict(self.ack)

    async def recv(self, timeout=5.0):
        await asyncio.sleep(timeout)
        return None


def _run(coro):
    return asyncio.get_event_loop_policy().new_event_loop().run_until_complete(coro)


async def _drain(st):
    """Await the deferred bind, if any, then stop the idle listener."""
    if st._prebind_task is not None:
        try:
            await st._prebind_task
        except asyncio.CancelledError:          # a test cancelled it on purpose
            pass
    if st._listener_task is not None:
        st._listener_task.cancel()
        try:
            await st._listener_task
        except asyncio.CancelledError:
            pass


REFUSED = {"success": False, "reason": "character_not_found"}
STAMP = {"tick": 12, "game_date": "867.1.13"}


def _bind(monkeypatch, env, ack, states, reply=None, wait="0", poll=0.0):
    """Handshake with `ack`, then run any deferred bind to completion.

    `states` is what successive get_tick_info reads answer for session_state
    (the last one repeats). `reply(params)` is the bridge's answer to a bind;
    the default binds with a clock stamp. Returns (state, sent binds, polls).
    """
    sent, polls = [], []
    seq = list(states)

    async def _fake_action(state, action, params):
        sent.append((action, dict(params)))
        if reply is not None:
            return reply(params)
        return {"success": True, "added": bool(params.get("add")), **STAMP}

    async def _fake_query(state, action, params, timeout=15.0):
        assert action == "get_tick_info"
        st = seq.pop(0) if len(seq) > 1 else seq[0]
        polls.append(st)
        return {"session_state": st} if st is not None else {}

    monkeypatch.setattr(mcp_server, "_action_bridge", _fake_action)
    monkeypatch.setattr(mcp_server, "_query_bridge", _fake_query)
    monkeypatch.setattr(mcp_server, "PREBIND_POLL_S", poll)
    monkeypatch.setenv(mcp_server.AGENT_CHARACTER_ENV, env)
    monkeypatch.setenv(mcp_server.PREBIND_WAIT_ENV, wait)
    st = mcp_server.BridgeState()
    client = _Client(ack)
    st._client = client

    async def go():
        await mcp_server._handshake_and_bind(client, st)
        await _drain(st)

    _run(go())
    return st, sent, polls


def test_an_active_ack_binds_at_once_and_never_polls(monkeypatch):
    st, sent, polls = _bind(monkeypatch, "13902,12145", {"session_state": "active"}, ["dormant"])
    assert polls == [] and st._prebind_task is None
    assert [p["character_id"] for _, p in sent] == [13902, 12145]
    assert st.prebind["ok"] is True and st.prebind["also"][0]["ok"] is True
    assert st.prebind["wait"] == {"waited_s": 0.0, "polls": 0, "game_active": True}
    assert st.prebind["bound_at"] == {"tick": 12, "date": "867.1.13"}


def test_an_ack_that_names_no_state_binds_at_once_and_says_nothing_was_seen(monkeypatch):
    """An older bridge, or a fake: nobody looked, and the record says so (None,
    not True)."""
    st, sent, polls = _bind(monkeypatch, "13902", {"status": "ok"}, ["dormant"])
    assert polls == [] and len(sent) == 1
    assert st.prebind["ok"] is True and st.prebind["wait"]["game_active"] is None


def test_the_bind_lands_after_N_polls_once_the_game_is_active(monkeypatch):
    st, sent, polls = _bind(monkeypatch, "13902,12145", {"session_state": "dormant"},
                            ["dormant", "dormant", "active"], wait="30")
    assert polls == ["dormant", "dormant", "active"]
    assert [p["character_id"] for _, p in sent] == [13902, 12145]
    assert st.prebind["ok"] is True and st.prebind["also"] == [
        {"character_id": 12145, "ok": True, "reason": None,
         "bound_at": {"tick": 12, "date": "867.1.13"}}]
    w = st.prebind["wait"]
    assert w["polls"] == 3 and w["game_active"] is True and w["waited_s"] >= 0.0


def test_nothing_is_sent_before_the_game_is_active(monkeypatch):
    order = []

    async def _fake_query(state, action, params, timeout=15.0):
        order.append("poll")
        return {"session_state": "active" if order.count("poll") >= 2 else "dormant"}

    async def _fake_action(state, action, params):
        order.append("bind")
        return {"success": True, "added": bool(params.get("add"))}

    monkeypatch.setattr(mcp_server, "_action_bridge", _fake_action)
    monkeypatch.setattr(mcp_server, "_query_bridge", _fake_query)
    monkeypatch.setattr(mcp_server, "PREBIND_POLL_S", 0.0)
    monkeypatch.setenv(mcp_server.AGENT_CHARACTER_ENV, "13902")
    monkeypatch.setenv(mcp_server.PREBIND_WAIT_ENV, "30")
    st = mcp_server.BridgeState()
    client = _Client({"session_state": "dormant"})
    st._client = client

    async def go():
        await mcp_server._handshake_and_bind(client, st)
        assert st.prebind is None and order == []     # the handshake returned, the bind is pending
        await _drain(st)

    _run(go())
    assert order == ["poll", "poll", "bind"]


def test_a_never_active_game_yields_the_refusal_shape_of_before(monkeypatch):
    """The deadline attempts the bind anyway: the record carries the BRIDGE's
    refusal, not one minted by the wait, so the run prompt's recovery and the
    manifest's `refused_at_start` read exactly as they did."""
    before = _bind(monkeypatch, "13902,12145", {"status": "ok"}, [None],
                   reply=lambda p: dict(REFUSED))[0].prebind
    st, sent, polls = _bind(monkeypatch, "13902,12145", {"session_state": "dormant"},
                            ["dormant"], reply=lambda p: dict(REFUSED), wait="0")
    after = st.prebind
    keys = ("character_id", "ok", "reason")
    flat = lambda pb: [{k: e.get(k) for k in keys} for e in [pb, *pb["also"]]]  # noqa: E731
    assert flat(after) == flat(before) == [
        {"character_id": 13902, "ok": False, "reason": "character_not_found"},
        {"character_id": 12145, "ok": False, "reason": "character_not_found"}]
    assert "bound_at" not in after and "bound_at" not in after["also"][0]
    assert polls == ["dormant"] and len(sent) == 2
    assert after["wait"]["game_active"] is False and after["wait"]["polls"] == 1


def test_a_read_that_names_no_state_keeps_waiting_then_binds(monkeypatch):
    st, sent, polls = _bind(monkeypatch, "13902", {"session_state": "dormant"},
                            [None, "active"], wait="30")
    assert polls == [None, "active"] and len(sent) == 1 and st.prebind["ok"] is True


def test_the_agents_own_claim_during_the_wait_supersedes_the_prebind(monkeypatch):
    """Claude bound for itself (the run prompt's recovery) before the game was
    seen active: the deferred bind would switch or re-claim under it, so it is
    not attempted, and the record says why."""
    sent = []

    async def _fake_action(state, action, params):
        sent.append(dict(params))
        return {"success": True}

    async def _fake_query(state, action, params, timeout=15.0):
        # The agent's claim shows in the action log before the next poll.
        state.action_log.append({"tick": 0, "action": "set_controlled_character",
                                 "params": {"character_id": 13902}})
        return {"session_state": "active"}

    monkeypatch.setattr(mcp_server, "_action_bridge", _fake_action)
    monkeypatch.setattr(mcp_server, "_query_bridge", _fake_query)
    monkeypatch.setattr(mcp_server, "PREBIND_POLL_S", 0.0)
    monkeypatch.setenv(mcp_server.AGENT_CHARACTER_ENV, "13902,12145")
    monkeypatch.setenv(mcp_server.PREBIND_WAIT_ENV, "30")
    st = mcp_server.BridgeState()
    client = _Client({"session_state": "dormant"})
    st._client = client

    async def go():
        await mcp_server._handshake_and_bind(client, st)
        await _drain(st)

    _run(go())
    assert sent == []
    assert st.prebind["ok"] is False and st.prebind["reason"] == "superseded_by_agent_claim"
    assert st.prebind["also"] == [{"character_id": 12145, "ok": False,
                                   "reason": "superseded_by_agent_claim"}]


def test_a_replaced_connection_abandons_the_wait_without_binding(monkeypatch):
    sent = []

    async def _fake_action(state, action, params):
        sent.append(dict(params))
        return {"success": True}

    async def _fake_query(state, action, params, timeout=15.0):
        state._client = object()                    # a reconnect replaced the client
        return {"session_state": "dormant"}

    monkeypatch.setattr(mcp_server, "_action_bridge", _fake_action)
    monkeypatch.setattr(mcp_server, "_query_bridge", _fake_query)
    monkeypatch.setattr(mcp_server, "PREBIND_POLL_S", 0.0)
    monkeypatch.setenv(mcp_server.AGENT_CHARACTER_ENV, "13902")
    monkeypatch.setenv(mcp_server.PREBIND_WAIT_ENV, "30")
    st = mcp_server.BridgeState()
    client = _Client({"session_state": "dormant"})
    st._client = client

    async def go():
        await mcp_server._handshake_and_bind(client, st)
        await _drain(st)

    _run(go())
    assert sent == [] and st.prebind is None


def test_cancelling_the_wait_binds_nothing(monkeypatch):
    sent = []

    async def _fake_action(state, action, params):
        sent.append(dict(params))
        return {"success": True}

    async def _fake_query(state, action, params, timeout=15.0):
        return {"session_state": "dormant"}

    monkeypatch.setattr(mcp_server, "_action_bridge", _fake_action)
    monkeypatch.setattr(mcp_server, "_query_bridge", _fake_query)
    monkeypatch.setattr(mcp_server, "PREBIND_POLL_S", 0.01)
    monkeypatch.setenv(mcp_server.AGENT_CHARACTER_ENV, "13902")
    monkeypatch.setenv(mcp_server.PREBIND_WAIT_ENV, "30")
    st = mcp_server.BridgeState()
    client = _Client({"session_state": "dormant"})
    st._client = client

    async def go():
        await mcp_server._handshake_and_bind(client, st)
        await asyncio.sleep(0.03)
        await mcp_server._cancel_prebind_wait(st)
        assert st._prebind_task.done()
        await _drain(st)

    _run(go())
    assert sent == [] and st.prebind is None


def test_the_session_record_notes_when_the_bind_landed(monkeypatch, tmp_path):
    rec = tmp_path / "s.session.json"
    rec.write_text(json.dumps({"session_id": "s1", "resume_token": "t"}))
    monkeypatch.setenv(mcp_server.SESSION_OUT_ENV, str(rec))
    monkeypatch.setenv(mcp_server.RELEASE_ON_EXIT_ENV, str(tmp_path / "rel.json"))
    _bind(monkeypatch, "13902,12145", {"session_state": "dormant"},
          ["dormant", "active"], wait="30")
    got = json.loads(rec.read_text())
    assert got["session_id"] == "s1" and got["resume_token"] == "t"
    assert got["prebind"] == [
        {"character_id": 13902, "ok": True, "reason": None, "bound_at": {"tick": 12, "date": "867.1.13"}},
        {"character_id": 12145, "ok": True, "reason": None, "bound_at": {"tick": 12, "date": "867.1.13"}}]
    assert got["prebind_wait"]["polls"] == 2 and got["prebind_wait"]["game_active"] is True
    assert (rec.stat().st_mode & 0o777) == 0o600


def test_a_refused_ruler_in_the_record_carries_no_bound_at(monkeypatch, tmp_path):
    rec = tmp_path / "s.session.json"
    rec.write_text(json.dumps({"session_id": "s1", "resume_token": "t"}))
    monkeypatch.setenv(mcp_server.SESSION_OUT_ENV, str(rec))
    monkeypatch.setenv(mcp_server.RELEASE_ON_EXIT_ENV, str(tmp_path / "rel.json"))
    _bind(monkeypatch, "13902,12145", {"session_state": "active"}, ["active"],
          reply=lambda p: ({"success": False, "reason": "owned_by_other"} if p.get("add")
                           else {"success": True, **STAMP}))
    got = json.loads(rec.read_text())
    assert got["prebind"] == [
        {"character_id": 13902, "ok": True, "reason": None, "bound_at": {"tick": 12, "date": "867.1.13"}},
        {"character_id": 12145, "ok": False, "reason": "owned_by_other"}]


def test_a_reply_without_a_clock_stamp_records_none_not_a_guess(monkeypatch):
    st, _, _ = _bind(monkeypatch, "13902", {"session_state": "active"}, ["active"],
                     reply=lambda p: {"success": True, "game_date": "unknown"})
    assert st.prebind["bound_at"] == {"tick": None, "date": None}


@pytest.mark.parametrize("raw,want", [(None, 120.0), ("", 120.0), ("45", 45.0), ("0", 0.0),
                                      ("1.5", 1.5), ("-3", 120.0), ("soon", 120.0), ("nan", 120.0)])
def test_the_wait_budget_env(monkeypatch, raw, want):
    if raw is None:
        monkeypatch.delenv(mcp_server.PREBIND_WAIT_ENV, raising=False)
    else:
        monkeypatch.setenv(mcp_server.PREBIND_WAIT_ENV, raw)
    assert mcp_server._prebind_wait_s() == want
