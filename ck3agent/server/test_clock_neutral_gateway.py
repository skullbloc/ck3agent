"""L13: no tool but session() changes the clock (contract D5/D6).

The owner's rule: every tool works whether the game is paused or running, and
only session(action="pause"|"unpause"|"speed"|"wait") touches the clock, on the
player's call. These rows drive a REAL mcp_server.BridgeGateway (and the agent
loop's _AgentGateway) over a recording fake bridge that answers every call and
records every action sent, then assert the clock verbs never appear:

  N1  read / write / read_many / confirmed_write, paused and running;
  N2  every redesign tool impl that can be driven offline (observe, plan_war,
      cbs, declare_war, succession_plan, heir_plan, set_phase, scheme_types,
      scheme_targets, every do() intent and action_class, every inspect
      subject), paused and running;
  N3  session's non-clock actions;
  N4  the agent loop's gateway, the same surface;
  C1  THE CONTROL: session's own clock actions DO reach the recorder, so a
      green N1-N4 is not a recorder that cannot see a clock verb;
  P1  confirmed_write on a paused game: stage 1 (the drain) reads back in
      either clock state; an outcome still not visible needs game days, so it is
      pending drains_when "clock_advances" with no world poll and no clock verb;
  P2  running, the world poll runs (bounded) and stops when the player pauses;
  W1  the clock is READ (get_tick_info), never taken from a pushed frame the
      bridge does not send: with no tick_start ever pushed (the live state) a
      running game is polled and the envelope says running;
  S1  stage 1, the drain: a queued write whose verdict appears on the 2nd read,
      paused, resolves; one whose verdict never appears is pending
      drains_when "next_pass" with its intent id; a drain refusal is refused.
"""
from __future__ import annotations

import asyncio
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

ms = pytest.importorskip("mcp_server")
import redesign as R  # noqa: E402

#: The bridge verbs that move the game clock. Only session() sends them (L13
#: contract D5/D6); the rows below prove no other gateway path does.
CLOCK = {"pause_game", "unpause_game", "set_speed", "set_pause"}
ME, FOE = 7, 9


class Recorder:
    """The bridge, as the gateway's two send paths see it. It answers from a
    small table and keeps a clock it lets only the clock verbs move."""

    def __init__(self, paused: bool):
        self.paused = paused
        self.sent: list[str] = []
        #: write replies by action, and query_dispatch_result answers in order
        #: (the last one repeats); empty: the defaults below.
        self.write_replies: dict = {}
        self.dispatch: list = []

    def reply(self, action: str, params: dict):
        self.sent.append(action)
        if action == "pause_game":
            self.paused = True
        elif action == "unpause_game":
            self.paused = False
        if action in self.write_replies:
            return dict(self.write_replies[action])
        if action == "query_dispatch_result":
            if not self.dispatch:
                return {}
            row = self.dispatch.pop(0) if len(self.dispatch) > 1 else self.dispatch[0]
            return dict(row, dispatch_id=(params or {}).get("dispatch_id"))
        if action in CLOCK or action.startswith("set_") or action in (
                "send_interaction", "declare_war", "call_war_create_core", "create_title",
                "scheme_action", "faction_action", "send_decision", "construct_building"):
            return {"success": True, "action": action}
        if action == "get_tick_info":
            return {"paused": self.paused, "game_speed": 1, "game_date": "867.1.2",
                    "tick_id": 77, "controlled_character_id": ME}
        if action == "query_character":
            cid = (params or {}).get("character_id") or ME
            return {"id": cid, "alive": True, "name": "Halfdan",
                                  "primary_title_key": "k_york", "age": 30, "gold": 100}
        if action == "get_controlled_roster":
            # The emitter's keys: dispatch_router.cpp do_get_controlled_roster
            # (action, primary_character_id, roster, player_character_id).
            return {"action": "get_controlled_roster", "primary_character_id": ME,
                    "roster": [ME], "player_character_id": 0}
        return {}


@pytest.fixture
def wired(monkeypatch):
    """A real BridgeGateway over the recorder; sleeps are instant."""
    def make(paused: bool):
        rec = Recorder(paused)
        state = ms.BridgeState()
        state.connected = True
        # THE LIVE STATE: the bridge pushes no tick_start (witness W1), so the
        # gateway's view of the clock can only come from get_tick_info.
        state.game_date = "867.1.2"

        async def ab(_s, action, params, timeout=5.0):
            return rec.reply(action, params)

        async def qb(_s, action, params, timeout=15.0):
            return rec.reply(action, params)

        monkeypatch.setattr(ms, "_action_bridge", ab)
        monkeypatch.setattr(ms, "_query_bridge", qb)
        return rec, ms.BridgeGateway(state)

    real_sleep = asyncio.sleep

    async def fast(_s=0, *a, **k):
        await real_sleep(0)
    monkeypatch.setattr(asyncio, "sleep", fast)
    monkeypatch.setattr(R, "_SLEEP", fast)
    return make


def run(coro):
    return asyncio.run(coro)


async def _drive_impls(gw) -> list[str]:
    """Every redesign impl this test can drive offline. Returns the names of the
    calls that raised (they are reported, and the clock rows still hold)."""
    calls = [
        ("observe brief", lambda: R.observe_impl(gw, ME, "brief")),
        ("observe full", lambda: R.observe_impl(gw, ME, "full")),
        ("cbs", lambda: R.cbs_impl(gw, FOE, ME)),
        ("plan_war", lambda: R.plan_war_impl(gw, ME)),
        ("declare_war", lambda: R.declare_war_impl(gw, ME, target_id=FOE, cb="claim_cb",
                                                   confirm_straddle=True)),
        ("succession_plan", lambda: R.succession_plan_impl(gw, ME)),
        ("heir_plan", lambda: R.heir_plan_impl(gw, ME)),
        ("good_guardians", lambda: R.good_guardian_candidates_impl(gw, ME)),
        ("set_phase", lambda: R.set_phase_impl(gw, R.PHASES[0], ME)),
        ("scheme_types", lambda: R.scheme_types_impl(gw)),
        ("scheme_targets", lambda: R.scheme_targets_impl(gw, "murder", ME, target_id=FOE)),
    ]
    for intent in R.INTENTS:
        calls.append((f"do {intent}", lambda i=intent: R.do_impl(
            gw, intent=i, character_id=ME,
            args={"target_id": FOE, "war_id": 1, "title_key": "k_york", "scheme": "murder",
                  "action": "start", "interaction": "befriend_interaction"})))
    for cls in R.ACTION_CLASSES:
        calls.append((f"do class {cls}", lambda c=cls: R.do_impl(
            gw, action_class=c, character_id=ME, candidates=["d1"])))
    for subject in R.INSPECT_SUBJECTS:
        calls.append((f"inspect {subject}", lambda s=subject: R.inspect_impl(
            gw, s, character_id=ME, target_id=FOE)))
    raised = []
    for name, call in calls:
        try:
            out = await call()
            assert isinstance(out, dict), name
        except Exception as exc:                          # noqa: BLE001
            raised.append(f"{name}: {type(exc).__name__}: {exc}"[:200])
    return raised


@pytest.mark.parametrize("paused", [True, False])
def test_N1_the_gateway_surface_sends_no_clock_verb(wired, paused):
    rec, gw = wired(paused)

    async def go():
        await gw.read("query_character", {"character_id": ME})
        await gw.read("query_conquest_targets", {"character_id": ME})   # deferred drain
        await gw.read("construction_check", {"actor_id": ME})           # game thread
        await gw.write("set_council_task", {"actor_id": ME})
        await gw.read_many([("query_realm", {}), ("query_cbs_for_target", {}),
                            ("get_active_wars", {})])
        await gw.confirmed_write("send_interaction", {}, readback=lambda raw: _none())
    run(go())
    assert len(rec.sent) >= 8, rec.sent
    assert not CLOCK & set(rec.sent), rec.sent


async def _none():
    return None


@pytest.mark.parametrize("paused", [True, False])
def test_N2_no_tool_impl_sends_a_clock_verb(wired, paused):
    rec, gw = wired(paused)
    raised = run(_drive_impls(gw))
    assert len(set(rec.sent)) > 20, "the impls reached too few bridge actions (fail OPEN)"
    assert not CLOCK & set(rec.sent), sorted(CLOCK & set(rec.sent))
    assert not raised, raised


def test_N3_session_non_clock_actions_send_no_clock_verb(wired):
    rec, gw = wired(True)
    clock_actions = {"pause", "unpause", "speed", "wait"}
    for action in R.SESSION_ACTIONS:
        if action in clock_actions:
            continue
        run(R.session_impl(gw, action, character_id=ME, flag="off_goal_veto"))
    assert rec.sent, "session reached nothing (fail OPEN)"
    assert not CLOCK & set(rec.sent), rec.sent


def test_C1_CONTROL_session_clock_actions_reach_the_recorder(wired):
    """Without this a recorder that never sees a clock verb would green N1-N4."""
    rec, gw = wired(False)
    run(R.session_impl(gw, "pause"))
    run(R.session_impl(gw, "unpause"))
    run(R.session_impl(gw, "speed", speed=2))
    assert {"pause_game", "unpause_game", "set_speed"} <= set(rec.sent), rec.sent


class _Loop:
    """The autonomous loop's bridge primitives, recording every action."""

    def __init__(self, rec: Recorder):
        self.rec = rec
        self._last_observation = {"paused": rec.paused, "tick_id": 1, "game_speed": 1}
        self._last_game_date = "867.1.2"
        self.bridge = type("B", (), {"connected": True})()

    async def _send_and_wait(self, action, params, timeout=10.0):
        return self.rec.reply(action, params)

    async def _send_no_wait(self, action, params):
        self.rec.reply(action, params)

    async def _read(self, action, params):
        return self.rec.reply(action, params)


@pytest.mark.parametrize("paused", [True, False])
def test_N4_the_agent_loop_gateway_sends_no_clock_verb(wired, paused):
    agent = pytest.importorskip("agent")
    rec = Recorder(paused)
    gw = agent._AgentGateway(_Loop(rec))
    run(gw.read("query_character", {"character_id": ME}))
    run(gw.read("query_cb_validity_strict", {}))
    run(gw.write("set_council_task", {}))
    run(gw.read_many([("query_realm", {}), ("get_active_wars", {})]))
    run(gw.confirmed_write("send_interaction", {}, readback=lambda raw: _none()))
    raised = run(_drive_impls(gw))
    assert len(set(rec.sent)) > 20, "the impls reached too few bridge actions (fail OPEN)"
    assert not CLOCK & set(rec.sent), sorted(CLOCK & set(rec.sent))
    assert not raised, raised


STAGE1 = len(R.DRAIN_SCHEDULE_S)


def _probe(n, *, resolve_at=None, then=None):
    """A readback counting its calls; resolves on call `resolve_at`, runs `then(k)`."""
    async def readback(raw):
        n["probes"] = n.get("probes", 0) + 1
        if then:
            then(n["probes"])
        return {"seen": True} if resolve_at and n["probes"] >= resolve_at else None
    return readback


def test_P1_confirmed_write_paused_needs_days_is_clock_advances_without_a_world_poll(wired):
    rec, gw = wired(True)
    n = {}
    out = run(gw.confirmed_write("send_interaction", {"x": 1}, readback=_probe(n)))
    assert out["resolved"] is None and out["refused"] is None, out
    assert out["drains_when"] == "clock_advances", out
    assert out["game_date_at_submit"] == "867.1.2"
    # the readback once, then once per stage-1 pass; no world poll while paused
    assert n["probes"] == 1 + STAGE1 and out["polls"] == STAGE1, (n, out)
    assert rec.sent.count("get_tick_info") == 2, rec.sent      # at submit, at the world gate
    assert not CLOCK & set(rec.sent), rec.sent


def test_P2_confirmed_write_running_polls_the_world_bounded_and_stops_on_a_pause(wired):
    rec, gw = wired(False)
    n = {}

    def player_pauses(k):
        if k == 1 + STAGE1 + 2:            # the player pauses after two world polls
            rec.paused = True
    out = run(gw.confirmed_write("send_interaction", {}, readback=_probe(n, then=player_pauses),
                                 max_polls=8))
    assert out["polls"] == STAGE1 + 2 and out["drains_when"] == "clock_advances", out
    assert not CLOCK & set(rec.sent), rec.sent

    rec2, gw2 = wired(False)
    out2 = run(gw2.confirmed_write("send_interaction", {}, readback=_probe({}), max_polls=3))
    assert out2["polls"] == STAGE1 + 3 and out2["drains_when"] == "clock_advances", out2


@pytest.mark.parametrize("paused,world_polls", [(False, 3), (True, 0)])
def test_W1_the_clock_is_read_not_taken_from_a_frame_the_bridge_never_pushes(wired, paused,
                                                                            world_polls):
    """Witness W1's discriminating row. No tick_start was ever pushed (the live
    state) and get_tick_info answers `paused`. Running, the world poll runs;
    paused, it does not. The pre-fix gateway read `state.latest_tick_start` (a
    cache only a pushed tick_start filled), defaulted it to paused, and never
    polled: polls == 0 in both rows."""
    rec, gw = wired(paused)
    assert not hasattr(gw.state, "latest_tick_start"), "the frame cache is back"
    out = run(gw.confirmed_write("send_interaction", {}, readback=_probe({}), max_polls=3))
    assert out["polls"] - STAGE1 == world_polls, out
    assert gw.game()["paused"] is paused, gw.game()


def test_W1_the_envelope_game_block_is_the_fresh_reading_never_a_default(wired, monkeypatch):
    """game() before any reading says paused None (not a guessed True); every tool
    enters through _gateway_now, which reads the clock, so a running game's
    envelope says running."""
    rec, gw = wired(False)
    assert gw.game()["paused"] is None, gw.game()
    monkeypatch.setattr(ms, "_get_gateway", lambda: gw)
    got = run(ms._gateway_now())
    assert got is gw and gw.game()["paused"] is False and gw.game()["speed"] == 1, gw.game()
    env = run(R.cbs_impl(gw, FOE, ME))
    assert env["game"]["paused"] is False, env["game"]


@pytest.mark.parametrize("paused", [True, False])
def test_W1_the_agent_loop_gateway_reads_the_clock_too(wired, paused):
    agent = pytest.importorskip("agent")
    rec = Recorder(paused)
    loop = _Loop(rec)
    loop._last_observation = {}            # no observation frame is ever pushed
    gw = agent._AgentGateway(loop)
    assert gw.game()["paused"] is None
    out = run(gw.confirmed_write("send_interaction", {}, readback=_probe({}), max_polls=3))
    assert out["polls"] - STAGE1 == (0 if paused else 3), out
    assert gw.game()["paused"] is paused


QUEUED = {"success": True, "status": "queued", "action": "send_interaction",
          "method": "intent_dispatch", "intent_request_id": "si_12627_31_39007",
          "pending_count": 1}


def test_S1_a_queued_write_drained_on_the_next_pass_resolves_PAUSED(wired):
    """The live gl5-p reply. The verdict appears on the 2nd drain read with the
    clock paused; the readback sees the outcome the drain produced: resolved,
    no world poll. The pre-fix server answered pending at once."""
    rec, gw = wired(True)
    rec.write_replies["send_interaction"] = QUEUED
    rec.dispatch = [{"status": "pending"}, {"status": "success"}]
    drained = lambda: rec.sent.count("query_dispatch_result") >= 2   # noqa: E731

    async def readback(raw):
        return {"joined": True} if drained() else None
    out = run(gw.confirmed_write("send_interaction", {}, readback=readback))
    assert out["resolved"] == {"joined": True}, out
    assert out["drain"]["status"] == "success" and out["intent_id"] == QUEUED["intent_request_id"]
    assert out["drains_when"] is None
    assert not CLOCK & set(rec.sent), rec.sent


def test_S1_a_verdict_never_read_is_NEXT_PASS_with_the_intent_id(wired):
    rec, gw = wired(True)
    rec.write_replies["send_interaction"] = QUEUED
    rec.dispatch = [{"status": "pending"}]
    out = run(gw.confirmed_write("send_interaction", {}, readback=_probe({})))
    assert out["resolved"] is None and out["drains_when"] == "next_pass", out
    assert out["intent_id"] == QUEUED["intent_request_id"]
    assert rec.sent.count("query_dispatch_result") == 1 + STAGE1, rec.sent
    pend = R.conf_pending(out, expected_change="x")
    assert pend["drains_when"] == "next_pass" and pend["intent_id"] == QUEUED["intent_request_id"]
    assert pend["game_date_at_submit"] == "867.1.2"


def test_S1_a_drain_refusal_is_refused_not_pending(wired):
    rec, gw = wired(True)
    rec.write_replies["send_interaction"] = QUEUED
    rec.dispatch = [{"status": "rejected", "rejection_reason": "target_would_reject"}]
    out = run(gw.confirmed_write("send_interaction", {}, readback=_probe({})))
    assert out["refused"] == "target_would_reject" and out["drains_when"] is None, out


def test_S1_a_pending_must_name_its_wait():
    with pytest.raises(ValueError):
        R.write_pending("unpaused", game_date_at_submit="867.1.2")
    with pytest.raises(ValueError):
        R.conf_pending({"drains_when": None, "game_date_at_submit": "867.1.2"})


def test_m12_the_agent_loop_delta_cursor_moves_with_the_clock_reading(wired):
    """The twin's delta feeds read `since_tick` from `_last_observation`, which no
    live frame fills, so every call re-read the feeds from tick 0. The cursor now
    follows the gateway's fresh clock reading."""
    agent = pytest.importorskip("agent")
    rec = Recorder(True)
    seen = []
    loop = _Loop(rec)
    loop._last_observation = None

    async def rd(action, params):
        seen.append((action, (params or {}).get("since_tick")))
        return rec.reply(action, params)
    loop._read = rd
    gw = agent._AgentGateway(loop)
    run(gw.clock())
    run(gw.delta_records_since_last())
    run(gw.delta_records_since_last())
    ticks = [t for a, t in seen if a == "query_war_declarations"]
    assert ticks == [0, 77], seen
