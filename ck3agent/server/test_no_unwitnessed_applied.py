"""ck-9rto — no branch of session_impl may say "applied" over a reply that
does not support it. The CLASS, asserted once, for every verb.

Three of us fixed instances of one pattern and stopped. ck-zgh4 found `control`;
I found `release`; four more were in the same 218 lines the whole time — `speed`
and `auto_events` carrying `raw: res` into a result whose verdict contradicts
it, `unpause` re-reading gw.game() so only its verdict was wrong, and `pause`
assigning the reply to `res`, never mentioning `res` again, and hard-coding
`{"paused": True}`.

IT IS REACHABLE, NOT THEORETICAL. All six verbs go out on the gateway's action
path, and there `gw.write` answers a timeout with `{"status": "sent_no_confirmation"}` —
the bridge's explicit "I do not know whether this landed". `pause` turned that
into `verdict="applied", result={"paused": True}`: the one shape the bridge
takes trouble to report honestly was the one the caller overwrote with a
literal.

This test would have failed on six branches before the control fix and on four
before the guard. Four more `if not ok` blocks would have closed four holes and
left the seventh branch to whoever adds it.
"""

import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

redesign = pytest.importorskip("redesign")

#: Every action of session_impl that WRITES, with the kwargs it needs. If a
#: seventh write verb is added and not listed here, `test_the_write_verbs_are_ALL
#: _listed` reds — the list is checked against the source, not maintained on
#: trust.
WRITE_ACTIONS = {
    "control":     {"character_id": 30853},
    "release":     {"character_id": 30853},
    "speed":       {"speed": 3},
    "pause":       {},
    "unpause":     {},
}
# (auto_events was the sixth write verb; its door was deleted in L14, finding 7.)


class _RefusingGw:
    """Answers every write with the reply under test and every read with
    something harmless, so only the write's treatment is being measured."""

    def __init__(self, reply):
        self._reply = reply
        self.health = "ok"
        # The clock pause/unpause read back (L14 FF1 M-5): a write the bridge did
        # not refuse moves it, as the bridge's PAUSE_SET does.
        self._paused = None
        # ... and the speed (L14 FF2 M-5): set_speed is read back as game_speed.
        self._speed = None

    async def write(self, action, params=None):
        if self._reply.get("success") is not False and action in ("pause_game", "unpause_game"):
            self._paused = action == "pause_game"
        if self._reply.get("success") is not False and action == "set_speed":
            self._speed = (params or {}).get("speed")
        return dict(self._reply)

    async def read(self, action, params=None):
        if action == "get_tick_info":
            ti = {} if self._paused is None else {"paused": self._paused}
            if self._speed is not None:
                ti.update(game_speed=self._speed, speed_read_status="ok")
            return ti
        if action == "query_character":
            return {"id": (params or {}).get("character_id"), "name": "X"}
        if action == "get_active_wars":
            return {"wars": []}
        return {}

    def game(self):
        return {"tick": 1, "date": "1066.09.15", "paused": True, "speed": 3}


def _run(action, reply):
    gw = _RefusingGw(reply)
    return asyncio.run(redesign.session_impl(gw, action, **WRITE_ACTIONS[action]))


@pytest.mark.parametrize("action", sorted(WRITE_ACTIONS))
def test_a_REFUSED_write_never_reports_applied(action):
    out = _run(action, {"success": False, "reason": "owned_by_other"})
    assert out.get("verdict") != "applied", (action, out)
    assert out.get("ok") is False, (action, out)
    assert out.get("blocked"), (action, out)


#: The replies the bridge and the gateway really send for a write that did not
#: run, none of which carries `success` (L14 FF2 N-MAJ-1; the witness's runs 2/3):
#:   memory_access_fault  MEMORY_ACCESS_FAULT_REPLY, bridge/include/bridge.h, sent
#:                        by every answer_memory_access_fault (case_set_speed,
#:                        case_pause_game, case_unpause_game in dispatch_router.cpp)
#:                        and by the SIGSEGV recovery in ipc_server.cpp
#:                        serve_on_game_thread;
#:   game_thread_timeout  the same function when the drain did not run the job
#:                        (the job is abandoned);
#:   bridge_disconnected  mcp_server.py BridgeGateway._dispatch while hung.
FAULT_REPLIES = {
    "memory_access_fault": {"error": "memory_access_fault"},
    "game_thread_timeout": {"error": "game_thread_timeout", "action": "set_speed",
                            "waited_ms": 2000,
                            "hint": "The game-thread drain did not run this read in time "
                                    "(a load in progress, or no session-pass hook and the "
                                    "clock stopped). Nothing was read."},
    "bridge_disconnected": {"reason": "bridge_disconnected"},
}


@pytest.mark.parametrize("shape", sorted(FAULT_REPLIES))
@pytest.mark.parametrize("action", sorted(WRITE_ACTIONS))
def test_a_FAULTED_write_never_reports_applied(action, shape):
    """RED on b84e3634: reply_refusal and this guard read only `success is False`,
    so every one of these answered applied (speed, control, release) or settled
    on a clock the write never moved."""
    out = _run(action, FAULT_REPLIES[shape])
    assert out.get("verdict") != "applied", (action, shape, out)
    assert out.get("ok") is False, (action, shape, out)
    assert out["blocked"]["raw_reason"] == shape, (action, shape, out["blocked"])


#: The gate each fault shape resolves to (L14 int5): the bridge's own fault and
#: timeout are bridge_fault (one typed gate, not retryable: the effect may have
#: landed); the gateway's disconnect keeps its own gate.
FAULT_GATES = {"memory_access_fault": "bridge_fault",
               "game_thread_timeout": "bridge_fault",
               "bridge_disconnected": "bridge_disconnected"}


@pytest.mark.parametrize("shape", sorted(FAULT_REPLIES))
@pytest.mark.parametrize("action", sorted(WRITE_ACTIONS))
def test_a_FAULTED_write_names_a_typed_gate(action, shape):
    """RED on e262847e: memory_access_fault came back as refusal_reason_unknown
    (control, release: unmapped_refusal elsewhere) and game_thread_timeout as
    drain_timeout, retryable, through the "timeout" substring."""
    out = _run(action, FAULT_REPLIES[shape])
    gate = out["blocked"]["gate"]
    assert gate not in ("refusal_reason_unknown", "unmapped_refusal"), (action, shape, out)
    assert gate == FAULT_GATES[shape], (action, shape, out["blocked"])
    if gate == "bridge_fault":
        assert out["blocked"]["retryable"] is False, out["blocked"]


@pytest.mark.parametrize("shape", sorted(FAULT_REPLIES))
def test_the_resolver_names_each_fault_shape(shape):
    """The one resolver every path asks (map_raw_reason): each shape's code."""
    assert redesign.map_raw_reason(shape)["gate"] == FAULT_GATES[shape]
    assert redesign.GATE_WHY[FAULT_GATES[shape]]


#: L14 FF3 N3-3: the listener's answer to a call in flight when the connection
#: dropped, mcp_server.py _listen `except ConnectionError` (every pending future):
#: {"error":"bridge_disconnected"}. The write was SENT, so it may have landed.
IN_FLIGHT_DROP = {"error": "bridge_disconnected"}


@pytest.mark.parametrize("action", sorted(WRITE_ACTIONS))
def test_a_write_IN_FLIGHT_at_a_disconnect_is_not_retryable(action):
    """RED on 293c0e00: the in-flight drop resolved to bridge_disconnected,
    retryable True, for a write the bridge may already have run (the double-apply
    hazard int5 removed from drain_timeout). It is write_unconfirmed: read back
    first."""
    out = _run(action, IN_FLIGHT_DROP)
    assert out.get("verdict") != "applied" and out.get("ok") is False, (action, out)
    b = out["blocked"]
    assert b["retryable"] is False, (action, b)
    assert b["gate"] == "write_unconfirmed", (action, b)
    assert b["raw_reason"] == redesign.DISCONNECTED_IN_FLIGHT, (action, b)
    assert "read the state back" in b["next"], (action, b)


def test_an_experimental_flag_write_IN_FLIGHT_at_a_disconnect_is_not_retryable():
    """The same sweep over set_experimental, which named its refusal from the
    reply's own fields (`reason or error`), so the in-flight drop read as the
    retryable bridge_disconnected. RED on 293c0e00."""
    flag = sorted(redesign._EXPERIMENTAL_FLAGS)[0]
    out = asyncio.run(redesign._set_experimental_flag(_RefusingGw(IN_FLIGHT_DROP), flag, True))
    assert out["ok"] is False and out["blocked"]["gate"] == "write_unconfirmed", out
    assert out["blocked"]["retryable"] is False, out["blocked"]


def test_the_never_sent_disconnect_stays_retryable():
    """The flip: the gateway's never-sent {"reason":"bridge_disconnected"}
    (BridgeGateway._dispatch while hung) is still the retryable gate."""
    out = _run("speed", FAULT_REPLIES["bridge_disconnected"])
    assert out["blocked"]["gate"] == "bridge_disconnected", out["blocked"]
    assert out["blocked"]["retryable"] is True, out["blocked"]


def test_a_HUNG_gateway_never_reports_applied(monkeypatch):
    """The real gateway, hung (no connection): every write answers
    {"reason": "bridge_disconnected"} (BridgeGateway._dispatch), and no session
    write verb, nor set_phase, says applied over it."""
    import mcp_server as ms

    async def one_read(gw, settled, timeout_s=3.0):
        return await redesign._fresh_tick(gw)
    monkeypatch.setattr(redesign, "_settle_tick", one_read)
    state = ms.BridgeState()
    state.connected, state.mock_mode = False, False
    gw = ms.BridgeGateway(state)
    assert gw.health == "hung"
    for action, kw in sorted(WRITE_ACTIONS.items()):
        out = asyncio.run(redesign.session_impl(gw, action, **kw))
        assert out.get("verdict") != "applied" and out.get("ok") is False, (action, out)
    out = asyncio.run(redesign.set_phase_impl(gw, phase="expansion", character_id=30853))
    assert out.get("verdict") != "applied" and out.get("ok") is False, out


@pytest.mark.parametrize("action", sorted(WRITE_ACTIONS))
def test_an_UNCONFIRMED_write_never_reports_applied(action):
    """`sent_no_confirmation` is what gw.write returns when a write times
    out. It is neither a refusal nor an application, and the
    vocabulary already has the word: confirmation_pending — VERIFY, do not
    re-declare."""
    out = _run(action, {"status": "sent_no_confirmation", "action": action})
    assert out.get("verdict") != "applied", (action, out)
    assert out["blocked"]["gate"] == "confirmation_pending", (action, out["blocked"])
    assert out["blocked"]["retryable"] is False, (
        "re-declaring an unconfirmed write is how a double-declare happens; the "
        "agent must VERIFY instead")


@pytest.mark.parametrize("action", sorted(WRITE_ACTIONS))
def test_a_WITNESSED_write_is_untouched_by_the_guard(action):
    """The flip that makes the two above non-vacuous: the same verbs, a reply
    that DOES support the claim, and the guard changes nothing."""
    out = _run(action, {"success": True})
    assert out.get("ok", True) is True, (action, out)
    assert out.get("verdict") == "applied", (action, out)
    assert not out.get("blocked"), (action, out)


def test_the_write_verbs_are_ALL_listed():
    """The list above is checked against the SOURCE rather than maintained on
    trust: every `gw.write` inside the branch function must belong to a branch
    this test drives. A seventh write verb reds here."""
    import ast
    import inspect
    # The branches and every module function they call, transitively (the clock
    # verbs write inside _session_clock_write / _session_speed_write): a write
    # moved into a helper is still a session write.
    written, seen, todo = set(), set(), ["_session_impl_branches"]
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        tree = ast.parse(inspect.getsource(getattr(redesign, name)).lstrip())
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            if (isinstance(node.func, ast.Attribute)
                    and node.func.attr == "write" and node.args
                    and isinstance(node.args[0], ast.Constant)):
                written.add(node.args[0].value)
            if (isinstance(node.func, ast.Name) and node.func.id.startswith("_session")
                    and callable(getattr(redesign, node.func.id, None))):
                todo.append(node.func.id)
    assert {"_session_clock_write", "_session_speed_write"} <= seen, seen
    # the bridge verbs each listed action sends
    expected = {"set_controlled_character", "release_character", "set_speed",
                "pause_game", "unpause_game"}
    assert written == expected, (
        "the set of bridge verbs session_impl writes has changed — add the new "
        f"action to WRITE_ACTIONS so the guard is proved over it too: {written ^ expected}")


def test_the_guard_is_applied_ONCE_and_not_per_branch():
    """The shape of the fix is the point. `session_impl` is a thin wrapper that
    runs the branches and applies one rule to whatever they return, so a branch
    added tomorrow is covered without its author knowing this rule exists."""
    import ast
    import inspect

    def calls_to(fn, name):
        tree = ast.parse(inspect.getsource(fn).lstrip())
        return sum(1 for n in ast.walk(tree)
                   if isinstance(n, ast.Call) and isinstance(n.func, ast.Name)
                   and n.func.id == name)

    # CALLS, not mentions — the wrapper's docstring names the guard as well, and
    # counting the string said 2 where the behaviour is 1.
    assert calls_to(redesign.session_impl, "unwitnessed_applied") == 1
    assert "_session_impl_branches" in inspect.getsource(redesign.session_impl)
    assert calls_to(redesign._session_impl_branches, "unwitnessed_applied") == 0, (
        "the guard has leaked into the branches — that is the per-instance shape "
        "this replaced")


def test_EVERY_write_is_considered_not_only_the_last():
    """ck-zgh4's structural note, closed rather than documented.

    Every branch performs exactly one write today, so a last-write-wins slot
    would be CORRECT — and "one guard, one recorded reply" would be a
    load-bearing assumption nothing states. A branch that ever wrote twice would
    leave the FIRST reply unwitnessed, and the source check cannot catch that:
    it checks the write VERBS against the branches it drives, so `pause_game`
    then `set_speed` uses two verbs already listed and reds nothing.

    The guard's contract is tested directly, because the branch that would
    exercise it does not exist yet — which is the point: this row exists so the
    branch can be written safely, not because something is broken."""
    applied = {"ok": True, "verdict": "applied", "result": {"done": True}}

    # the FIRST write refused, the second succeeded: a last-write-wins slot sees
    # only the success and lets `applied` stand
    out = redesign.unwitnessed_applied(applied, [
        ("pause_game", {"success": False, "reason": "no_session"}),
        ("set_speed", {"success": True}),
    ])
    assert out.get("verdict") != "applied", out
    assert out["blocked"]["raw_reason"] == "no_session", out["blocked"]

    # an unconfirmed FIRST write, likewise
    out = redesign.unwitnessed_applied(applied, [
        ("pause_game", {"status": "sent_no_confirmation", "action": "pause_game"}),
        ("set_speed", {"success": True}),
    ])
    assert out["blocked"]["gate"] == "confirmation_pending", out

    # and the flip: every write fine, guard changes nothing
    out = redesign.unwitnessed_applied(applied, [
        ("pause_game", {"success": True}), ("set_speed", {"success": True})])
    assert out == applied


def test_the_FIRST_failing_reply_is_the_one_reported():
    """Two failures in one branch: the earliest wins, because everything after
    it ran on a premise that had already broken."""
    out = redesign.unwitnessed_applied(
        {"ok": True, "verdict": "applied"},
        [("pause_game", {"success": False, "reason": "no_session"}),
         ("set_speed", {"success": False, "reason": "observe_mode"})])
    assert out["blocked"]["raw_reason"] == "no_session", out["blocked"]
