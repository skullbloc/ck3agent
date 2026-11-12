"""v3 §7.5 (lane s0-seats): the seat is a LINEAGE — the MCP-side step.

`decide` is pure over four reads; every branch is driven here without a
bridge. `step` is driven through a fake gateway that answers the same verbs
the real one does (get_bridge_state, query_character, query_title_hierarchy,
set_controlled_character), and `observe` is shown INERT without the env.

Red on 0a969f2e: the module does not exist; `observe` has no lineage step.
"""
import asyncio
import json
import os
import sys

import pytest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import seat_lineage as sl  # noqa: E402

ROW = {"session_id": "s1", "primary_character_id": 5, "heir_character_id": 0,
       "unbound_reason": "", "claimed_character_ids": [5]}


def _title(holder):
    """query_title_hierarchy(title_id=9) as do_query_title_hierarchy writes it
    (query_handlers.cpp, read by test_reply_key_census): the title's fields,
    its sub_titles and sub_title_count all INSIDE `title` (emit_title_info is
    called between that object's begin and end), holder_id only when held."""
    if holder:
        return {"title": {"title_id": 9, "key": "k_x", "tier": 4, "holder_id": holder,
                          "sub_titles": [], "sub_title_count": 0}}
    return {"title": {"title_id": 9, "key": "k_x", "tier": 4, "sub_titles": [], "sub_title_count": 0}}


def _row(**kw):
    r = dict(ROW)
    r.update(kw)
    return r


def test_first_sighting_learns_the_ruler_and_title_and_records_nothing():
    d = sl.decide(None, _row(), {"id": 5, "alive": True, "primary_title_id": 9}, None)
    assert d.kind is None and d.refresh_title == 9


def test_a_living_ruler_is_held_and_a_duke_s_title_refreshes_but_k_s_registered_one_does_not():
    p = sl.Prior(ruler=5, title_id=9)
    d = sl.decide(p, _row(), {"id": 5, "alive": True, "primary_title_id": 11}, None)
    assert d.kind is None and d.refresh_title == 11
    d = sl.decide(p, _row(), {"id": 5, "alive": True, "primary_title_id": 11}, None,
                  registered_title=9)
    assert d.refresh_title == 9


def test_a_bridge_handoff_is_witnessed_as_a_rebind_with_the_title_that_carried():
    p = sl.Prior(ruler=5, title_id=9)
    d = sl.decide(p, _row(primary_character_id=12, heir_character_id=12),
                  {"id": 12, "alive": True, "primary_title_id": 9}, None)
    assert (d.kind, d.old_id, d.new_id, d.title_id, d.source) == ("rebind", 5, 12, 9, "bridge_handoff")
    assert d.bind_target == 0


def test_a_primary_that_moved_without_the_heir_witness_is_recorded_not_assumed():
    d = sl.decide(sl.Prior(5, 9), _row(primary_character_id=12, heir_character_id=0),
                  {"id": 12, "alive": True}, None)
    assert d.kind == "rebind" and d.source == "explicit_control"
    assert d.reason == "primary_moved_without_heir_witness"


def test_a_dead_ruler_the_bridge_did_not_move_is_rebound_through_the_title_holder():
    d = sl.decide(sl.Prior(5, 9), _row(), {"id": 5, "alive": False}, _title(12))
    assert d.kind == "rebind" and d.bind_target == 12 and d.new_id == 12


def test_a_title_with_no_holder_ends_the_seat_lost():
    d = sl.decide(sl.Prior(5, 9), _row(primary_character_id=0), {"id": 5, "alive": False},
                  _title(None))
    assert d.kind == "lost" and d.reason == "title_has_no_holder"
    d = sl.decide(sl.Prior(5, 9), _row(primary_character_id=0), {"id": 5, "alive": False},
                  {"error": "title_not_found", "title_id": 9})
    assert d.kind == "lost"


def test_no_title_to_follow_is_lost_with_its_reason():
    d = sl.decide(sl.Prior(5, None), _row(), {"id": 5, "alive": False}, None)
    assert d.kind == "lost" and d.reason == sl.R_NO_TITLE


def test_holder_still_the_corpse_is_pending_not_lost():
    d = sl.decide(sl.Prior(5, 9), _row(), {"id": 5, "alive": False}, _title(5))
    assert d.kind == "pending"


def test_instrument_side_failures_are_seat_unbound_with_the_leg_named():
    p = sl.Prior(5, 9)
    assert sl.decide(p, None, None, None).reason == sl.R_SESSION_ROW_ABSENT
    assert sl.decide(p, _row(), {}, None).reason == sl.R_CHARACTER_READ_REFUSED
    assert sl.decide(p, _row(), {"id": 5, "alive": False}, None).reason == sl.R_TITLE_READ_REFUSED
    d = sl.decide(p, _row(), {"id": 5, "alive": False}, _title(7),
                  held_elsewhere=frozenset({7}))
    assert d.kind == "seat_unbound" and d.reason == sl.R_HOLDER_HELD_ELSEWHERE
    d = sl.decide(p, _row(), {"id": 5, "alive": False}, _title(7), player_id=7)
    assert d.reason == sl.R_HEIR_IS_PLAYER
    d = sl.decide(p, _row(primary_character_id=0, unbound_reason="heir_is_player", unbound_heir_id=7),
                  None, None)
    assert d.kind == "seat_unbound" and d.reason == sl.R_HEIR_IS_PLAYER and d.new_id == 7


def test_a_released_living_ruler_is_unbound_not_a_succession():
    d = sl.decide(sl.Prior(5, 9), _row(primary_character_id=0), {"id": 5, "alive": True}, None)
    assert d.kind == "seat_unbound" and d.reason == sl.R_RELEASED


def test_an_ended_seat_records_nothing_more():
    assert sl.decide(sl.Prior(5, 9, ended=True), _row(), {"id": 5, "alive": True}, None).kind is None


class _GW:
    """A fake gateway answering the verbs `step` reads, with a scripted world."""

    def __init__(self, row, alive, holder, bind_reply=None, player=0):
        self.row, self.alive, self.holder = row, alive, holder
        self.bind_reply = bind_reply if bind_reply is not None else {"success": True}
        self.player = player
        self.writes = []

    async def read(self, action, params=None):
        return await self.answer(action, params)

    async def answer(self, action, params=None):
        """The scripted world. Subclasses call this, never `super().read`: the
        reply-key census (test_reply_key_census) follows `params` through every
        `read(...)` call, and read->super().read across six subclasses is a
        cycle it walks to its depth bound at a branching of six."""
        if action == "get_bridge_state":
            return {"sessions": [self.row] if self.row else [],
                    "player_character_id": self.player}
        if action == "query_character":
            return {"id": params["character_id"], "alive": self.alive, "primary_title_id": 9}
        if action == "query_title_hierarchy":
            return _title(self.holder)
        return {}

    async def write(self, action, params=None):
        self.writes.append((action, params))
        return self.bind_reply

    def game(self):
        return {"tick": 40, "date": "868.03.02", "paused": True}


def _step(gw, prior, sink):
    return asyncio.run(sl.step(gw, prior, sink=str(sink), seat_session_id="seat-K-5",
                               own_session_id=lambda g: "s1",
                               own_session_row=lambda bs, sid: next(
                                   (r for r in bs.get("sessions", []) if r["session_id"] == sid), None)))


def test_step_rebinds_explicitly_and_writes_the_row_the_driver_lifts(tmp_path):
    sink = tmp_path / "seat.jsonl"
    gw = _GW(_row(), alive=True, holder=None)
    prior = _step(gw, None, sink)                      # first sighting: the `bound` row
    assert prior.ruler == 5 and prior.title_id == 9
    gw.alive, gw.holder = False, 12
    prior = _step(gw, prior, sink)
    assert gw.writes == [("set_controlled_character", {"character_id": 12, "disable_native_ai": False})]
    rows = [json.loads(ln) for ln in sink.read_text().splitlines()]
    assert [r["kind"] for r in rows] == ["bound", "rebind"]
    assert rows[1]["kind"] == "rebind" and rows[1]["old_id"] == 5 and rows[1]["new_id"] == 12
    assert rows[1]["title_id"] == 9 and rows[1]["game_date"] == "868.03.02"
    assert rows[1]["seat_session_id"] == "seat-K-5" and rows[1]["source"] == "explicit_control"
    assert prior.ruler == 12 and not prior.ended


def test_step_records_a_refused_bind_as_seat_unbound_and_ends_the_seat(tmp_path):
    sink = tmp_path / "seat.jsonl"
    gw = _GW(_row(), alive=False, holder=12, bind_reply={"success": False, "reason": "watchlist_full"})
    prior = _step(gw, sl.Prior(5, 9), sink)
    row = json.loads(sink.read_text().splitlines()[0])
    assert row["kind"] == "seat_unbound" and row["reason"] == "bind_refused:watchlist_full"
    assert prior.ended


def test_step_ends_the_seat_lost_when_the_title_has_no_holder(tmp_path):
    sink = tmp_path / "seat.jsonl"
    gw = _GW(_row(primary_character_id=0), alive=False, holder=None)
    prior = _step(gw, sl.Prior(5, 9), sink)
    row = json.loads(sink.read_text().splitlines()[0])
    assert row["kind"] == "lost" and prior.ended and gw.writes == []


def test_step_never_raises_and_a_failed_step_is_no_verdict(tmp_path):
    """2026.12.6 (owner ruling 2026-11-10): a step whose reads raise is retried
    at the next observe, never counted as an unbind. Red on c4884cc4: it wrote
    `seat_unbound step_failed:RuntimeError`."""
    class Boom(_GW):
        async def read(self, action, params=None):
            raise RuntimeError("socket gone")
    sink = tmp_path / "seat.jsonl"
    prior = _step(Boom(_row(), True, None), sl.Prior(5, 9), sink)
    rows = [json.loads(ln) for ln in sink.read_text().splitlines()]
    assert [r["kind"] for r in rows] == ["read_failed"] and rows[0]["reason"] == "step:RuntimeError"
    assert prior.ruler == 5 and not prior.ended


def test_inert_without_the_env_and_the_title_env_is_read_as_an_int(monkeypatch):
    monkeypatch.delenv(sl.LINEAGE_OUT_ENV, raising=False)
    assert sl.armed() is None
    assert sl.armed({sl.LINEAGE_OUT_ENV: "  "}) is None
    assert sl.armed({sl.LINEAGE_OUT_ENV: "/x/y.jsonl"}) == "/x/y.jsonl"
    assert sl.registered_title_from_env({sl.LINEAGE_TITLE_ENV: "77"}) == 77
    assert sl.registered_title_from_env({sl.LINEAGE_TITLE_ENV: "0"}) is None
    assert sl.registered_title_from_env({}) is None


def test_observe_calls_the_step_only_when_armed(monkeypatch):
    import mcp_server
    calls = []

    async def fake_step(gw, prior, **kw):
        calls.append(kw["sink"])
        return sl.Prior(1, None)

    async def fake_observe_impl(gw, character_id=0, depth="brief"):
        return {"ok": True}

    async def fake_gateway():
        return object()

    async def fake_ensure(state, name):
        return None

    state = mcp_server.BridgeState()
    monkeypatch.setattr(mcp_server, "_get_state", lambda: state)
    monkeypatch.setattr(mcp_server.seat_lineage, "step", fake_step)
    monkeypatch.setattr(mcp_server.redesign, "observe_impl", fake_observe_impl)
    monkeypatch.setattr(mcp_server, "_gateway_now", fake_gateway)
    monkeypatch.setattr(mcp_server, "_ensure_connected", fake_ensure)
    monkeypatch.delenv(sl.LINEAGE_OUT_ENV, raising=False)
    fn = getattr(mcp_server.observe, "fn", mcp_server.observe)
    assert asyncio.run(fn()) == {"ok": True} and calls == []
    monkeypatch.setenv(sl.LINEAGE_OUT_ENV, "/tmp/never-written.jsonl")
    assert asyncio.run(fn()) == {"ok": True} and calls == ["/tmp/never-written.jsonl"]
    assert state.seat_lineage_prior.ruler == 1


def test_a_living_seat_writes_one_bound_row_at_its_first_step_and_nothing_while_held(tmp_path):
    """cell0 gap 2 (2026.12.4): `step` wrote no row while the ruler was held, so a
    seat whose ruler never died had no sink and read `unwitnessed` (n_at_end 0).
    The first step that holds a ruler writes ONE `bound` row; later held steps
    write nothing more."""
    sink = tmp_path / "seat.jsonl"
    gw = _GW(_row(), alive=True, holder=None)
    prior = _step(gw, None, sink)
    for _ in range(3):
        prior = _step(gw, prior, sink)
    rows = [json.loads(ln) for ln in sink.read_text().splitlines()]
    assert len(rows) == 1
    assert rows[0]["kind"] == sl.KIND_BOUND == "bound"
    assert rows[0]["old_id"] == 0 and rows[0]["new_id"] == 5 and rows[0]["title_id"] == 9
    assert rows[0]["seat_session_id"] == "seat-K-5" and rows[0]["game_date"] == "868.03.02"
    assert prior.ruler == 5 and not prior.ended


def test_a_step_that_finds_no_ruler_writes_no_bound_row(tmp_path):
    """No `bound` row without a ruler. Since 2026.12.6 (TK6 witness r2 item 3)
    the step is a counted, non-terminal `not_bound_yet` note."""
    sink = tmp_path / "seat.jsonl"
    gw = _GW(_row(primary_character_id=0), alive=True, holder=None)
    prior = _step(gw, None, sink)
    rows = [json.loads(ln) for ln in sink.read_text().splitlines()]
    assert prior.ruler == 0 and not prior.ended
    assert [r["kind"] for r in rows] == ["read_failed"]
    assert rows[0]["reason"] == "get_bridge_state:not_bound_yet"


def test_a_resumed_seats_server_seeds_its_lineage_from_the_sink(monkeypatch, tmp_path):
    """L23 witness r2 item 1: a cell's resumed claude starts a NEW server whose
    in-memory Prior was None, so it re-learned the seat from scratch (a second
    `bound` row, or the dead original). Its first observe now seeds the Prior
    from the sink: the rebind's ruler and title. Red on b6998ec5: prior None."""
    import mcp_server
    seen = []

    async def fake_step(gw, prior, **kw):
        seen.append(prior)
        return prior or sl.Prior(0, None)

    async def fake_observe_impl(gw, character_id=0, depth="brief"):
        return {"ok": True}

    async def fake_gateway():
        return object()

    async def fake_ensure(state, name):
        return None

    sink = tmp_path / "seat-K-5.seat_lineage.jsonl"
    sink.write_text(json.dumps({"kind": "bound", "old_id": 0, "new_id": 5, "title_id": 9}) + "\n"
                    + json.dumps({"kind": "rebind", "old_id": 5, "new_id": 12, "title_id": 9}) + "\n")
    state = mcp_server.BridgeState()
    monkeypatch.setattr(mcp_server, "_get_state", lambda: state)
    monkeypatch.setattr(mcp_server.seat_lineage, "step", fake_step)
    monkeypatch.setattr(mcp_server.redesign, "observe_impl", fake_observe_impl)
    monkeypatch.setattr(mcp_server, "_gateway_now", fake_gateway)
    monkeypatch.setattr(mcp_server, "_ensure_connected", fake_ensure)
    monkeypatch.setenv(sl.LINEAGE_OUT_ENV, str(sink))
    fn = getattr(mcp_server.observe, "fn", mcp_server.observe)
    asyncio.run(fn())
    p = seen[0]
    assert getattr(p, "ruler", None) == 12 and getattr(p, "title_id", None) == 9
    assert p.ended is False and len(p.rows) == 2


def test_a_seeded_seat_holding_its_heir_writes_no_second_bound_row(tmp_path):
    """The seeded Prior and the carried ruler (cli carries 12): the step holds,
    writes nothing; an ended sink stays ended."""
    sink = tmp_path / "seat.jsonl"
    sink.write_text(json.dumps({"kind": "bound", "old_id": 0, "new_id": 5, "title_id": 9}) + "\n"
                    + json.dumps({"kind": "rebind", "old_id": 5, "new_id": 12, "title_id": 9}) + "\n")
    prior = sl.prior_from_sink(str(sink))
    gw = _GW(_row(primary_character_id=12, claimed_character_ids=[12]), alive=True, holder=None)
    prior = _step(gw, prior, sink)
    assert len(sink.read_text().splitlines()) == 2 and prior.ruler == 12 and gw.writes == []
    ended = tmp_path / "ended.jsonl"
    ended.write_text(json.dumps({"kind": "lost", "old_id": 5, "new_id": 0}) + "\n")
    assert sl.prior_from_sink(str(ended)).ended is True
    assert sl.prior_from_sink(str(tmp_path / "absent.jsonl")) is None


def test_a_resumed_seat_holding_its_heir_writes_exactly_one_bound_row(monkeypatch, tmp_path):
    """L23 witness r3 item 2, red by ASSERTION on a tree without the seeding:
    the seat's sink already holds its `bound` row and the rebind to the heir;
    a resumed server's observe runs the REAL step on a bridge where the seat
    holds the living heir. Seeded from the sink it writes nothing; unseeded
    (Prior None) it wrote a second `bound` row."""
    import mcp_server

    async def fake_observe_impl(gw, character_id=0, depth="brief"):
        return {"ok": True}

    gw = _GW(_row(primary_character_id=12, claimed_character_ids=[12]), alive=True, holder=None)

    async def fake_gateway():
        return gw

    async def fake_ensure(state, name):
        return None

    sink = tmp_path / "seat-K-5.seat_lineage.jsonl"
    sink.write_text(json.dumps({"kind": "bound", "old_id": 0, "new_id": 5, "title_id": 9}) + "\n"
                    + json.dumps({"kind": "rebind", "old_id": 5, "new_id": 12, "title_id": 9}) + "\n")
    state = mcp_server.BridgeState()
    monkeypatch.setattr(mcp_server, "_get_state", lambda: state)
    monkeypatch.setattr(mcp_server.redesign, "observe_impl", fake_observe_impl)
    monkeypatch.setattr(mcp_server.redesign, "own_session_id", lambda g: "s1")
    monkeypatch.setattr(mcp_server.redesign, "own_session_row",
                        lambda bs, sid: next((r for r in bs.get("sessions", []) if r["session_id"] == sid), None))
    monkeypatch.setattr(mcp_server, "_gateway_now", fake_gateway)
    monkeypatch.setattr(mcp_server, "_ensure_connected", fake_ensure)
    monkeypatch.setenv(sl.LINEAGE_OUT_ENV, str(sink))
    fn = getattr(mcp_server.observe, "fn", mcp_server.observe)
    asyncio.run(fn())
    kinds = [json.loads(ln)["kind"] for ln in sink.read_text().splitlines()]
    assert kinds.count("bound") == 1 and kinds == ["bound", "rebind"], kinds
    assert gw.writes == []


# ── 2026.12.6: a read that carried no session list is no verdict ─────────────
#
# v3 pilot flat_hom-3 (lane/v3pilot4 a1c8beff, evidence/v3pilot4/README.md):
# D4 and D5 got `seat_unbound session_row_absent` at 867.10.22/23, about 20 s
# after hand-overs, while both held their rulers on the same bridge session to
# the horizon. The gateway answers `{}` for a read that timed out or lost its
# connection (mcp_server.BridgeGateway._dispatch); `own_session_row({}, sid)`
# is None and `decide` made that the terminal unbound. Red on c4884cc4: rows 1
# and 2 (an unbound row is written and the seat ends). Row 3 is the signed rule
# and the CONTROL: green before and after.

class _Flaky(_GW):
    """get_bridge_state answers `replies` in turn (then the real list)."""

    def __init__(self, replies, **kw):
        super().__init__(_row(), alive=True, holder=None, **kw)
        self.replies = list(replies)

    async def read(self, action, params=None):
        if action == "get_bridge_state" and self.replies:
            return self.replies.pop(0)
        return await self.answer(action, params)


@pytest.mark.parametrize("reply", [{}, {"reason": "bridge_disconnected"},
                                   {"error": "memory_access_fault"}, None, {"sessions": None}])
def test_a_read_without_a_session_list_writes_nothing_and_the_next_step_holds(tmp_path, reply):
    sink = tmp_path / "seat.jsonl"
    gw = _Flaky([reply])
    prior = _step(gw, sl.Prior(5, 9), sink)            # the list-less read
    kinds = [json.loads(ln)["kind"] for ln in sink.read_text().splitlines()]
    assert not prior.ended and kinds == ["read_failed"]
    prior = _step(gw, prior, sink)                     # the next observe: the row is there
    kinds = [json.loads(ln)["kind"] for ln in sink.read_text().splitlines()]
    assert not prior.ended and prior.ruler == 5 and kinds == ["read_failed", "reads_recovered"]


def test_no_own_session_id_is_no_verdict_either(tmp_path):
    sink = tmp_path / "seat.jsonl"
    prior = asyncio.run(sl.step(_GW(_row(), True, None), sl.Prior(5, 9), sink=str(sink),
                                seat_session_id="seat-K-5", own_session_id=lambda g: None,
                                own_session_row=lambda bs, sid: None))
    rows = [json.loads(ln) for ln in sink.read_text().splitlines()]
    assert not prior.ended and [r["kind"] for r in rows] == ["read_failed"]
    assert rows[0]["reason"] == "get_bridge_state:no_own_session_id"


def test_a_read_list_without_the_seats_row_is_still_the_signed_unbound(tmp_path):
    """The signed rule (v3 §7.5, session_row_absent): the list was read and this
    seat's row is not in it. CONTROL, green before and after."""
    sink = tmp_path / "seat.jsonl"
    gw = _Flaky([{"sessions": [{"session_id": "someone-else", "primary_character_id": 7}]}])
    prior = _step(gw, sl.Prior(5, 9), sink)
    row = json.loads(sink.read_text().splitlines()[0])
    assert row["kind"] == "seat_unbound" and row["reason"] == sl.R_SESSION_ROW_ABSENT
    assert prior.ended


def _cli_session_rows():
    """The CLI's `_bridge._session_rows` (and its `_tick`), executed from the
    shipped source: release/overlay/ck3agent/_bridge.py here, ck3agent/_bridge.py
    in an exported tree."""
    import ast
    here = Path(__file__).resolve()
    for cand in (here.parents[2] / "release" / "overlay" / "ck3agent" / "_bridge.py",
                 here.parents[1] / "_bridge.py"):
        if cand.is_file():
            tree = ast.parse(cand.read_text(encoding="utf-8"))
            fns = [n for n in tree.body if isinstance(n, ast.FunctionDef)
                   and n.name in ("_tick", "_session_rows")]
            ns: dict = {}
            exec(compile(ast.Module(body=fns, type_ignores=[]), str(cand), "exec"), ns)
            return ns["_session_rows"]
    raise AssertionError("the CLI's _bridge.py was not found")


@pytest.mark.parametrize("reply", [None, {}, [], "x", {"error": "memory_access_fault"},
                                   {"reason": "bridge_disconnected"}, {"sessions": None},
                                   {"sessions": "x"}, {"sessions": []},
                                   {"sessions": [{"session_id": "s1"}, 3, {"session_id": "s2"}]}])
def test_the_server_and_the_cli_read_a_session_list_alike(reply):
    """One rule, two readers (the server cannot import the CLI): checked here."""
    assert sl.session_rows(reply) == _cli_session_rows()(reply)


# ── 2026.12.6, the coordinator's extension: every read of the step ──────────
#
# One predicate (sl.read_failed) for the three reads. A FAILED read (nothing
# came back, the gateway's {} or bridge_disconnected, the bridge's
# game_thread_timeout / memory_access_fault) is no verdict; a read the bridge
# MADE and refused keeps the signed reason. Red on c4884cc4: every
# failed-read row (an unbound row is written). The refusal rows are the
# signed rule, CONTROLS green before and after.

FAILED = [None, {}, "x", {"reason": "bridge_disconnected"},
          {"error": "game_thread_timeout", "action": "query_character", "waited_ms": 2000,
           "hint": "The game-thread drain did not run this read in time. Nothing was read."},
          {"error": "memory_access_fault"},
          # TK6 witness r1 P1a/P1b: the listener fails a read IN FLIGHT when the
          # connection drops with {"error": "bridge_disconnected"} (mcp_server
          # _bridge_listener). Red on 10e9cea5 for the character and title reads.
          {"error": "bridge_disconnected"},
          # P1c/P1d: a lookup that faulted (mark_reply_degraded): not the bridge's
          # answer about the subject. Red on 10e9cea5 (refused / lost).
          {"success": False, "action": "query_character", "degraded": True, "fault_count": 1},
          {"error": "title_not_found", "title_id": 9, "degraded": True, "fault_count": 2}]


class _Scripted(_GW):
    """Answers `script[action]` once for that action, then the real world."""

    def __init__(self, script, row, alive, holder):
        super().__init__(row, alive=alive, holder=holder)
        self.script = dict(script)

    async def read(self, action, params=None):
        if action in self.script:
            return self.script.pop(action)
        return await self.answer(action, params)


def _rows_of(sink):
    return [json.loads(ln) for ln in sink.read_text().splitlines()] if sink.exists() else []


@pytest.mark.parametrize("reply", FAILED)
@pytest.mark.parametrize("read", ["get_bridge_state", "query_character", "query_title_hierarchy"])
def test_a_failed_read_of_any_kind_is_no_verdict_and_is_read_again(tmp_path, read, reply):
    sink = tmp_path / "seat.jsonl"
    # the dead ruler the bridge did not move: the step reads all three
    world = dict(row=_row(), alive=False, holder=12)
    gw = _Scripted({read: reply}, **world)
    prior = _step(gw, sl.Prior(5, 9), sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["read_failed"] and not prior.ended, rows
    assert rows[0]["reason"].startswith(read + ":") and rows[0]["consecutive"] == 1
    assert gw.writes == []
    # the next observe reads again and the lineage goes on: recovered, then the
    # explicit rebind
    prior = _step(gw, prior, sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["read_failed", "reads_recovered", "rebind"]
    assert rows[2]["new_id"] == 12 and prior.consecutive_failed == 0 and not prior.ended


def test_a_refused_character_read_stays_the_signed_reason(tmp_path):
    """query_character of a character the bridge cannot resolve:
    do_query_character returns false, dispatch_action_with_response answers
    {"success": false, "action": "query_character"}. A READ, refused: signed
    `character_read_refused`. CONTROL."""
    sink = tmp_path / "seat.jsonl"
    gw = _Scripted({"query_character": {"success": False, "action": "query_character"}},
                   row=_row(), alive=True, holder=None)
    prior = _step(gw, sl.Prior(5, 9), sink)
    row = _rows_of(sink)[0]
    assert row["kind"] == "seat_unbound" and row["reason"] == sl.R_CHARACTER_READ_REFUSED
    assert prior.ended


def test_a_refused_title_read_stays_the_signed_reason(tmp_path):
    """query_title_hierarchy that answered the count error alone
    (query_handlers.cpp title_count_clamped). A READ, refused: signed
    `title_read_refused`. CONTROL."""
    sink = tmp_path / "seat.jsonl"
    refusal = {"title_id": 9, "error": "title_count_clamped", "count_field": "sub_title_count",
               "raw_count": 9001, "max_count": 500}
    gw = _Scripted({"query_title_hierarchy": refusal}, row=_row(), alive=False, holder=12)
    prior = _step(gw, sl.Prior(5, 9), sink)
    row = _rows_of(sink)[0]
    assert row["kind"] == "seat_unbound" and row["reason"] == sl.R_TITLE_READ_REFUSED
    assert prior.ended


def test_a_title_the_bridge_cannot_find_is_still_lost(tmp_path):
    """{"error": "title_not_found"} is the bridge's answer about the title, not a
    failed read: the seat ends `lost` as signed. CONTROL."""
    sink = tmp_path / "seat.jsonl"
    gw = _Scripted({"query_title_hierarchy": {"error": "title_not_found", "title_id": 9}},
                   row=_row(), alive=False, holder=12)
    prior = _step(gw, sl.Prior(5, 9), sink)
    assert _rows_of(sink)[0]["kind"] == "lost" and prior.ended


@pytest.mark.parametrize("reply,why", [(None, "no_reply"), ({}, "empty_reply"), ("x", "not_an_object"),
                                       ({"reason": "bridge_disconnected"}, "bridge_disconnected"),
                                       ({"error": "game_thread_timeout"}, "game_thread_timeout"),
                                       ({"error": "memory_access_fault"}, "memory_access_fault"),
                                       ({"error": "bridge_disconnected"}, "bridge_disconnected"),
                                       ({"success": False, "action": "query_character",
                                         "degraded": True}, "degraded"),
                                       ({"error": "title_not_found", "degraded": True}, "degraded"),
                                       ({"id": 5, "alive": True, "degraded": False}, None),
                                       ({"success": False, "action": "query_character"}, None),
                                       ({"error": "title_not_found"}, None),
                                       ({"error": "title_count_clamped"}, None),
                                       ({"sessions": []}, None),
                                       ({"id": 5, "alive": True}, None)])
def test_read_failed_names_the_failed_shapes_and_only_those(reply, why):
    assert sl.read_failed(reply) == why



# ── 2026.12.6, the owner's bound (2026-11-10, verbatim: "If there are something
# like multiple timeouts in a row, that might be fair to call an issue.") ────
#
# MAX_CONSECUTIVE_FAILED_READS (3) failed steps in a row end the seat
# `seat_unbound reads_failed_repeatedly`, DATED at the first of them. A step
# that reads again resets the count. The count survives a continuation through
# the sink (prior_from_sink). Red on c4884cc4: every failed step wrote a
# terminal seat_unbound at once (session_row_absent or step_failed).

class _Clock(_GW):
    """get_bridge_state fails `fails` times, the game date advancing a day a step."""

    def __init__(self, fails):
        super().__init__(_row(), alive=True, holder=None)
        self.fails, self.day = fails, 0

    async def read(self, action, params=None):
        if action == "get_bridge_state":
            self.day += 1
            if self.fails > 0:
                self.fails -= 1
                return {}
        return await self.answer(action, params)

    def game(self):
        return {"tick": 100 + self.day, "date": f"867.10.{20 + self.day:02d}", "paused": False}


@pytest.mark.parametrize("fails", [1, 2])
def test_one_or_two_failed_steps_then_a_read_end_nothing(tmp_path, fails):
    sink = tmp_path / "seat.jsonl"
    gw = _Clock(fails)
    prior = sl.Prior(5, 9)
    for _ in range(fails + 1):
        prior = _step(gw, prior, sink)
    kinds = [r["kind"] for r in _rows_of(sink)]
    assert kinds == ["read_failed"] * fails + ["reads_recovered"]
    assert not prior.ended and prior.consecutive_failed == 0


def test_three_failed_steps_in_a_row_end_the_seat_dated_at_the_first(tmp_path):
    assert sl.MAX_CONSECUTIVE_FAILED_READS == 3
    sink = tmp_path / "seat.jsonl"
    gw = _Clock(5)
    prior = sl.Prior(5, 9)
    for _ in range(3):
        prior = _step(gw, prior, sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["read_failed"] * 3 + ["seat_unbound"]
    end = rows[-1]
    assert end["reason"] == sl.R_READS_FAILED_REPEATEDLY and prior.ended
    assert end["game_date"] == rows[0]["game_date"] == "867.10.21" and end["tick"] == rows[0]["tick"]
    assert end["first_failed_utc"] == rows[0]["written_utc"] and end["consecutive"] == 3
    prior = _step(gw, prior, sink)                     # ended: nothing more
    assert len(_rows_of(sink)) == 4


def test_a_refusal_still_ends_at_once_with_its_signed_reason(tmp_path):
    """CONTROL (green on c4884cc4): no count, no wait."""
    sink = tmp_path / "seat.jsonl"
    gw = _Scripted({"query_character": {"success": False, "action": "query_character"}},
                   row=_row(), alive=True, holder=None)
    prior = _step(gw, sl.Prior(5, 9), sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["seat_unbound"]
    assert rows[0]["reason"] == sl.R_CHARACTER_READ_REFUSED and prior.ended


def test_the_count_survives_a_continuation_through_the_sink(tmp_path):
    """A resumed seat's NEW server starts from the sink: two failed steps under
    the old server, one under the new, end the seat dated at the first."""
    sink = tmp_path / "seat.jsonl"
    gw = _Clock(5)
    prior = sl.Prior(5, 9)
    for _ in range(2):
        prior = _step(gw, prior, sink)
    seeded = sl.prior_from_sink(str(sink))             # the new server process
    assert seeded.consecutive_failed == 2 and seeded.first_failed["game_date"] == "867.10.21"
    seeded.ruler, seeded.title_id = 5, 9               # (a real sink opens with its bound row)
    prior = _step(gw, seeded, sink)
    rows = _rows_of(sink)
    assert rows[-1]["kind"] == "seat_unbound" and rows[-1]["reason"] == sl.R_READS_FAILED_REPEATEDLY
    assert rows[-1]["game_date"] == "867.10.21" and prior.ended


def test_a_recovered_row_resets_the_count_for_the_next_server(tmp_path):
    sink = tmp_path / "seat.jsonl"
    gw = _Clock(2)
    prior = sl.Prior(5, 9)
    for _ in range(3):
        prior = _step(gw, prior, sink)
    seeded = sl.prior_from_sink(str(sink))
    assert seeded.consecutive_failed == 0 and seeded.first_failed is None



# ── TK6 witness r1 ──────────────────────────────────────────────────────────

def test_the_gateway_spells_its_failure_with_the_predicates_code():
    """One definition (item 1): the gateway's no-connection answer is built from
    seat_lineage.BRIDGE_DISCONNECTED, and the predicate reads it as failed."""
    import mcp_server
    gw = mcp_server.BridgeGateway(mcp_server.BridgeState())      # not connected: hung
    reply = asyncio.run(gw.read("get_bridge_state", {}))
    assert reply == {"reason": sl.BRIDGE_DISCONNECTED} and sl.read_failed(reply) == sl.BRIDGE_DISCONNECTED
    assert sl.read_failed({"error": sl.BRIDGE_DISCONNECTED}) == sl.BRIDGE_DISCONNECTED
    assert sl.BRIDGE_DISCONNECTED in sl.FAILED_READ_CODES


class _NoRulerYet(_GW):
    """The seat's own row holds no ruler at its first observe (a prebind not
    landed yet); then `fails` failed reads; then the ruler 5."""

    def __init__(self, fails):
        super().__init__(_row(primary_character_id=0), alive=True, holder=None)
        self.step_no, self.fails = 0, fails

    async def read(self, action, params=None):
        if action == "get_bridge_state":
            self.step_no += 1
            if self.step_no == 1:
                return {"sessions": [_row(primary_character_id=0)], "player_character_id": 0}
            if self.fails > 0:
                self.fails -= 1
                return {}
            return {"sessions": [_row()], "player_character_id": 0}
        return await self.answer(action, params)


@pytest.mark.parametrize("fails", [0, 1])
def test_a_seat_with_no_ruler_yet_still_gets_its_first_sighting_bound(tmp_path, fails):
    """r1 item 2 / r2 item 3: the first ruler is a `bound`, never `rebind 0->5
    primary_moved_without_heir_witness` (an unwitnessed rebind excludes the
    episode). The first observe found the row with no ruler (a prebind landing
    late): a counted `not_bound_yet` note, not character_read_refused old_id 0."""
    sink = tmp_path / "seat.jsonl"
    gw = _NoRulerYet(fails)
    prior = None
    for _ in range(fails + 2):
        prior = _step(gw, prior, sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["read_failed"] * (1 + fails) + ["bound"], rows
    assert rows[0]["reason"] == "get_bridge_state:not_bound_yet"
    assert rows[-1]["new_id"] == 5 and prior.ruler == 5 and not prior.ended


class _Pattern(_GW):
    """get_bridge_state follows `pattern` (True = fails), the date a day a step."""

    def __init__(self, pattern):
        super().__init__(_row(), alive=True, holder=None)
        self.pattern, self.day = list(pattern), 0

    async def read(self, action, params=None):
        if action == "get_bridge_state":
            self.day += 1
            if self.pattern and self.pattern.pop(0):
                return {}
        return await self.answer(action, params)

    def game(self):
        return {"tick": 100 + self.day, "date": f"867.10.{10 + self.day:02d}", "paused": False}


def test_after_a_recovery_the_bound_is_dated_at_the_first_miss_of_the_new_run(tmp_path):
    """Item 4 (kills M6c): in ONE process, two misses, a recovery, three misses:
    the unbound row is dated at the first miss AFTER the recovery (867.10.14),
    not at the first miss of the earlier run (867.10.11)."""
    sink = tmp_path / "seat.jsonl"
    gw = _Pattern([True, True, False, True, True, True])
    prior = sl.Prior(5, 9)
    for _ in range(6):
        prior = _step(gw, prior, sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == (["read_failed"] * 2 + ["reads_recovered"]
                                         + ["read_failed"] * 3 + ["seat_unbound"])
    end = rows[-1]
    assert end["reason"] == sl.R_READS_FAILED_REPEATEDLY and end["game_date"] == "867.10.14", end
    assert end["first_failed_utc"] == rows[3]["written_utc"] and prior.ended



# ── TK6 witness r2 ──────────────────────────────────────────────────────────

class _Script(_GW):
    """Each step answers the next dict of `steps` ({action: reply}; missing
    actions answer as the real world); `raise` in a step raises from its first read."""

    def __init__(self, steps, row=None, alive=True, holder=None):
        super().__init__(row or _row(), alive=alive, holder=holder)
        self.steps, self.cur, self.day = list(steps), {}, 0

    async def read(self, action, params=None):
        if action == "get_bridge_state":
            self.cur = self.steps.pop(0) if self.steps else {}
            self.day += 1
            if self.cur.get("raise"):
                raise RuntimeError("socket gone")
        if action in self.cur:
            return self.cur[action]
        return await self.answer(action, params)

    def game(self):
        return {"tick": 200 + self.day, "date": f"867.03.{self.day:02d}", "paused": False}


DEG = {"degraded": True, "fault_count": 1}


def test_N2_a_full_degraded_profile_is_the_bridges_answer(tmp_path):
    """query_handlers.cpp:972: a full profile with one faulted field stands.
    alive:true + degraded, three observes: held, no row. Red on 6027021a."""
    sink = tmp_path / "seat.jsonl"
    prof = {"id": 5, "alive": True, "primary_title_id": 9, **DEG}
    gw = _Script([{"query_character": prof}] * 3)
    prior = sl.Prior(5, 9)
    for _ in range(3):
        prior = _step(gw, prior, sink)
    assert _rows_of(sink) == [] and not prior.ended


def test_N3_a_degraded_session_list_is_read(tmp_path):
    """dispatch_router.cpp:1650 FaultScope on get_bridge_state: the list is the
    registry's, read under its lock; degraded or not, it decides. Red on 6027021a."""
    sink = tmp_path / "seat.jsonl"
    bs = {"sessions": [_row()], "player_character_id": 0, **DEG}
    gw = _Script([{"get_bridge_state": bs}] * 3)
    prior = sl.Prior(5, 9)
    for _ in range(3):
        prior = _step(gw, prior, sink)
    assert _rows_of(sink) == [] and not prior.ended


def test_N4_a_degraded_dead_profile_takes_the_signed_death_path(tmp_path):
    """alive:false present (degraded): the ruler is dead; the seat rebinds
    through the title to its holder. Red on 6027021a (read_failed instead)."""
    sink = tmp_path / "seat.jsonl"
    gw = _Script([{"query_character": {"id": 5, "alive": False, "primary_title_id": 9, **DEG}}],
                 alive=False, holder=12)
    prior = _step(gw, sl.Prior(5, 9), sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["rebind"] and rows[0]["new_id"] == 12 and not prior.ended


def test_N4b_a_degraded_title_with_its_holder_is_read(tmp_path):
    """query_handlers.cpp:3006 FaultScope: the title read carries holder_id
    (degraded): the seat rebinds to that holder. Red on 6027021a."""
    sink = tmp_path / "seat.jsonl"
    title = {"title": {"title_id": 9, "holder_id": 12}, **DEG}   # emit_title_info keys
    gw = _Script([{"query_title_hierarchy": title}], alive=False, holder=12)
    prior = _step(gw, sl.Prior(5, 9), sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["rebind"] and rows[0]["new_id"] == 12 and not prior.ended


@pytest.mark.parametrize("read,reply", [
    ("query_character", {"success": False, "action": "query_character", **DEG}),
    ("query_character", {"id": 5, "primary_title_id": 9, **DEG}),
    ("query_title_hierarchy", {"error": "title_not_found", "title_id": 9, **DEG}),
    ("query_title_hierarchy", {"title": {"id": 9}, **DEG})])
def test_degraded_without_the_load_bearing_field_is_a_failed_read(tmp_path, read, reply):
    """dispatch_router.cpp:2943 (the success:false fill-in after a fault) and a
    faulted lookup_title_obj: no `alive` / no holder to decide on."""
    sink = tmp_path / "seat.jsonl"
    gw = _Script([{read: reply}], alive=False, holder=12)
    prior = _step(gw, sl.Prior(5, 9), sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["read_failed"] and rows[0]["reason"] == f"{read}:degraded"
    assert not prior.ended


def test_N1_a_seat_that_never_reads_ends_at_the_third_failure_dated_at_the_first(tmp_path):
    """r2 item 2: a seat that never gets a good read (12 misses from its first
    observe) is `seat_unbound reads_failed_repeatedly` at the 3rd, dated at the
    1st: the bundle says "we looked and could not see", never `unwitnessed`.
    Red on 6027021a (no row at all)."""
    sink = tmp_path / "seat.jsonl"
    gw = _Script([{"get_bridge_state": {}}] * 12)
    prior = None
    for _ in range(12):
        prior = _step(gw, prior, sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["read_failed"] * 3 + ["seat_unbound"], rows
    assert rows[-1]["reason"] == sl.R_READS_FAILED_REPEATEDLY
    assert rows[-1]["game_date"] == rows[0]["game_date"] == "867.03.01" and prior.ended


def test_R3_R7_raise_raise_good_is_a_bound_never_a_swap(tmp_path):
    """r2 item 3: raises before any ruler are counted (R7's route) AND the
    first-sighting rule ignores the notes (R3's): raise, raise, good is `bound`.
    Each mutant alone and both together red this row."""
    sink = tmp_path / "seat.jsonl"
    gw = _Script([{"raise": True}, {"raise": True}, {}])
    prior = None
    for _ in range(3):
        prior = _step(gw, prior, sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["read_failed", "read_failed", "bound"], rows
    assert rows[0]["reason"] == "get_bridge_state:RuntimeError" or rows[0]["reason"].startswith("step:")
    assert rows[-1]["new_id"] == 5 and not prior.ended


def test_R9_the_listener_fails_a_read_in_flight_with_the_predicates_code():
    """r2 item 3 (R9): the listener's answer to a read in flight when the
    connection drops is spelled from seat_lineage.BRIDGE_DISCONNECTED, and the
    predicate reads it as failed on every read."""
    import mcp_server

    class _Dropped:
        async def recv(self, timeout=5.0):
            raise ConnectionError("gone")

    st = mcp_server.BridgeState()
    st._client, st.connected = _Dropped(), True

    async def go():
        fut = asyncio.get_running_loop().create_future()
        st.pending_queries["r1"] = fut
        await mcp_server._bridge_listener(st)
        return fut.result()
    reply = asyncio.run(go())
    assert not st.connected
    for read in (sl.READ_SESSIONS, sl.READ_CHARACTER, sl.READ_TITLE):
        assert sl.read_failed(reply, read) == sl.BRIDGE_DISCONNECTED, (read, reply)


@pytest.mark.parametrize("never", [False, True])
def test_N5_a_late_prebind_is_not_character_read_refused(tmp_path, never):
    """r2 item 3 (older than TK6): good reads of the seat's row with no ruler
    before it ever had one (a prebind landing late) gave `seat_unbound
    character_read_refused old_id 0` at the 2nd observe. Now each is a counted
    `not_bound_yet` note: two then the ruler -> bound (N5); three -> `seat_unbound
    never_bound` dated at the first (N5b). Red on 6027021a."""
    sink = tmp_path / "seat.jsonl"
    unbound = {"get_bridge_state": {"sessions": [_row(primary_character_id=0)],
                                    "player_character_id": 0}}
    steps = [unbound] * (3 if never else 2) + [{}]
    gw = _Script(steps)
    prior = None
    for _ in range(len(steps)):
        prior = _step(gw, prior, sink)
    rows = _rows_of(sink)
    if never:
        assert [r["kind"] for r in rows] == ["read_failed"] * 3 + ["seat_unbound"], rows
        assert rows[-1]["reason"] == sl.R_NEVER_BOUND and rows[-1]["game_date"] == "867.03.01"
        assert prior.ended
    else:
        assert [r["kind"] for r in rows] == ["read_failed", "read_failed", "bound"], rows
        assert not any(r.get("reason") == sl.R_CHARACTER_READ_REFUSED for r in rows)
        assert prior.ruler == 5 and not prior.ended


def test_a_run_mixing_not_bound_yet_and_a_failed_read_is_reads_failed_repeatedly(tmp_path):
    sink = tmp_path / "seat.jsonl"
    unbound = {"get_bridge_state": {"sessions": [_row(primary_character_id=0)],
                                    "player_character_id": 0}}
    gw = _Script([unbound, {"get_bridge_state": {}}, unbound])
    prior = None
    for _ in range(3):
        prior = _step(gw, prior, sink)
    rows = _rows_of(sink)
    assert rows[-1]["kind"] == "seat_unbound" and rows[-1]["reason"] == sl.R_READS_FAILED_REPEATEDLY


# ── TK6 witness r3 ──────────────────────────────────────────────────────────

NO_RULER = {"get_bridge_state": {"sessions": [_row(primary_character_id=0)], "player_character_id": 0}}


def test_M8_the_run_kind_survives_a_continuation(tmp_path):
    """not_bound_yet, a failed read, a continuation, not_bound_yet: the run mixes
    a failed read in, so it ends reads_failed_repeatedly, not never_bound. The
    new server rebuilds the run kind from the sink (prior_from_sink). Kills M8."""
    sink = tmp_path / "seat.jsonl"
    gw = _Script([NO_RULER, {"get_bridge_state": {}}])
    prior = None
    for _ in range(2):
        prior = _step(gw, prior, sink)
    seeded = sl.prior_from_sink(str(sink))                # the new server process
    assert seeded.consecutive_failed == 2 and seeded.run_only_not_bound is False
    prior = _step(_Script([NO_RULER]), seeded, sink)
    rows = _rows_of(sink)
    assert rows[-1]["kind"] == "seat_unbound" and rows[-1]["reason"] == sl.R_READS_FAILED_REPEATEDLY
    assert rows[-1]["game_date"] == rows[0]["game_date"] and prior.ended


@pytest.mark.parametrize("listless", [{"request_id": "x", "tick": 9, "game_date": "867.10.22"},
                                      {"request_id": "x", "tick": 9, "sessions": None}])
def test_M13_a_bound_seat_reading_no_session_list_is_not_session_row_absent(tmp_path, listless):
    """The flat_hom-3 case: a bound seat whose get_bridge_state came back with no
    session list is a failed read (a note), never the terminal
    session_row_absent. Kills M13 (the no_session_list guard removed)."""
    sink = tmp_path / "seat.jsonl"
    prior = _step(_Script([{}]), None, sink)                       # good: bound
    prior = _step(_Script([{"get_bridge_state": listless}]), prior, sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["bound", "read_failed"], rows
    assert rows[1]["reason"] == "get_bridge_state:no_session_list" and not prior.ended


def test_M14_an_unbound_reason_before_any_ruler_is_the_bridges_word_not_not_bound_yet(tmp_path):
    """The seat's own row says the bridge unbound it (heir_is_player) before it
    ever had a ruler: that is the bridge's answer, not a prebind still landing.
    No not_bound_yet note; the signed heir_is_player decides. Kills M14."""
    sink = tmp_path / "seat.jsonl"
    row = _row(primary_character_id=0, unbound_reason="heir_is_player", unbound_heir_id=7)
    told = {"get_bridge_state": {"sessions": [row], "player_character_id": 7}}
    prior = _step(_Script([told]), None, sink)
    assert _rows_of(sink) == [] and prior.ruler == 0 and not prior.ended
    prior = _step(_Script([told]), prior, sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["seat_unbound"] and rows[0]["reason"] == sl.R_HEIR_IS_PLAYER
    assert not any(r["kind"] == "read_failed" for r in rows)


def test_a_degraded_profile_whose_id_faulted_to_zero_is_a_failed_read(tmp_path):
    """build_char_profile always writes `alive`, so a profile whose id read
    faulted to 0 still carries it; without an id it is no answer about the
    ruler. Red on a443930c (character_read_refused, terminal)."""
    sink = tmp_path / "seat.jsonl"
    prof = {"id": 0, "alive": True, "primary_title_id": 9, "degraded": True, "fault_count": 1}
    prior = _step(_Script([{"query_character": prof}]), sl.Prior(5, 9), sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["read_failed"] and rows[0]["reason"] == "query_character:degraded"
    assert not prior.ended


# ── INTEG item 2 (owner ruling 2026-11-11): K's seat follows the PLAYED LINE ──
# K's seat reads the bridge's line watch (query_handlers.cpp emit_played_line:
# start_character_id, line_character_id, previous_character_id,
# carrying_title_id, primary_title_id, successions, why, status,
# pending_reason when pending, armed_date, since_date, armed_this_read), the
# decision the growth meter reads. Red on 749a3ff9: K's seat followed the
# registered title (holder 0 -> `lost`; holder X -> rebind to X).

K_FRANCE, E_HRE = 7052, 105


def _watch(line, status="following", start=5, *, carrying=0, previous=0, pending=None):
    w = {"start_character_id": start, "line_character_id": line, "previous_character_id": previous,
         "carrying_title_id": carrying, "primary_title_id": E_HRE, "successions": 1 if previous else 0,
         "why": "succession_to_heir" if previous else "armed", "status": status,
         "armed_date": "867.1.3", "since_date": "872.12.4", "armed_this_read": False,
         **({"pending_reason": pending} if pending is not None else {})}
    return {"id": start, "alive": False, "primary_title_id": 0, "played_line": w}


def _kprior():
    return sl.Prior(5, K_FRANCE, line_start=5)


def test_K_dead_registered_title_unheld_follows_the_line_not_lost():
    """The v3reg2 block-1 shape: K took e_hre, k_france went holderless; K dies
    and the line goes to e_hre's holder 12. The title rule read `lost`."""
    d = sl.decide(_kprior(), _row(), {"id": 5, "alive": False}, _title(None),
                  registered_title=K_FRANCE, watch_read=_watch(12, carrying=E_HRE, previous=5))
    assert (d.kind, d.old_id, d.new_id, d.bind_target, d.title_id, d.source, d.reason) == \
        ("rebind", 5, 12, 12, E_HRE, "explicit_control", None)


def test_K_dead_registered_title_held_by_another_follows_the_line_not_the_holder():
    d = sl.decide(_kprior(), _row(), {"id": 5, "alive": False}, _title(99),
                  registered_title=K_FRANCE, watch_read=_watch(12, carrying=E_HRE, previous=5))
    assert d.kind == "rebind" and d.bind_target == 12


def test_K_no_heir_yet_is_pending_never_lost_mid_run():
    for reason in ("primary_title_unheld", "holder_is_the_dead", "no_primary_title"):
        d = sl.decide(_kprior(), _row(primary_character_id=0), {"id": 5, "alive": False}, None,
                      registered_title=K_FRANCE,
                      watch_read=_watch(5, "successor_pending", pending=reason))
        assert (d.kind, d.reason) == ("pending", f"successor_pending:{reason}")
    d = sl.decide(_kprior(), _row(), {"id": 5, "alive": False}, None, registered_title=K_FRANCE,
                  watch_read=_watch(5))
    assert (d.kind, d.reason) == ("pending", sl.R_DIED_SINCE_TICK)


def test_K_the_bridge_says_the_line_ENDED_the_seat_is_lost():
    """Owner ruling 2026-11-12: `pending` only while the bridge has not named an
    heir; once its line watch says `line_ended`, the seat is LOST. Red on
    8cef520a (the watch reader refused the status: seat_unbound
    line_watch_unreadable:watch_malformed)."""
    ended = _watch(5, "line_ended")
    ended["played_line"]["ended_date"] = "870.1.4"
    for row in (_row(), _row(primary_character_id=0)):
        d = sl.decide(_kprior(), row, {"id": 5, "alive": False}, None, registered_title=K_FRANCE,
                      watch_read=ended)
        assert (d.kind, d.reason, d.source, d.bind_target) == ("lost", sl.R_LINE_ENDED, "bridge_handoff", 0)
    # the bridge moved the seat, but the line itself ended: still lost, never rebound off the line
    moved = sl.decide(_kprior(), _row(primary_character_id=30, heir_character_id=30),
                      {"id": 30, "alive": True}, None, registered_title=K_FRANCE, watch_read=ended)
    assert (moved.kind, moved.reason) == ("lost", sl.R_LINE_ENDED)


def test_K_step_pending_then_ended_is_lost(tmp_path):
    """(the count of such a seat as LOST is scripts/test_played_line.py's
    test_an_ended_K_seat_counts_as_a_lost_seat: seat_lineage_record does not ship)"""
    sink = tmp_path / "seat.jsonl"
    gw = _LineGW(_row(), alive=True, holder=None)
    prior = _kstep(gw, None, sink)                                   # bound, watch armed
    gw.alive = False
    gw.watch = _watch(5, "successor_pending", pending="primary_title_unheld")
    prior = _kstep(gw, prior, sink)
    assert not prior.ended
    gw.watch = _watch(5, "line_ended")
    prior = _kstep(gw, prior, sink)
    rows = _rows_of(sink)
    assert [r["kind"] for r in rows] == ["bound", "pending", "lost"]
    assert rows[-1]["reason"] == "line_ended" and prior.ended
    assert gw.writes == [], "nothing is bound for an ended line"
    assert sl.prior_from_sink(str(sink)).ended, "a resumed server keeps the seat ended"


def test_K_a_bridge_handoff_on_the_line_is_witnessed_one_off_it_is_corrected_to_the_line():
    on = sl.decide(_kprior(), _row(primary_character_id=12, heir_character_id=12),
                   {"id": 12, "alive": True}, None, registered_title=K_FRANCE,
                   watch_read=_watch(12, carrying=E_HRE, previous=5))
    assert (on.kind, on.new_id, on.source, on.reason, on.bind_target, on.title_id) == \
        ("rebind", 12, "bridge_handoff", None, 0, E_HRE)
    off = sl.decide(_kprior(), _row(primary_character_id=30, heir_character_id=30),
                    {"id": 30, "alive": True}, None, registered_title=K_FRANCE,
                    watch_read=_watch(12, carrying=E_HRE, previous=5))
    assert (off.kind, off.new_id, off.bind_target, off.source, off.reason) == \
        ("rebind", 12, 12, "explicit_control", None)
    # the line has not moved: the title rule's swap reading stands
    swap = sl.decide(_kprior(), _row(primary_character_id=30, heir_character_id=0),
                     {"id": 30, "alive": True}, None, registered_title=K_FRANCE, watch_read=_watch(5))
    assert swap.reason == sl.R_PRIMARY_MOVED


def test_K_an_unreadable_watch_is_the_instruments_never_a_guessed_heir():
    for wr, why in (({"id": 5, "alive": False}, "watch_absent"),
                    ({"error": "character_not_found"}, "watch_read:character_not_found"),
                    ({"id": 5, "alive": False, "played_line": {"status": "watch_refused",
                                                              "reason": "watch_capacity"}},
                     "watch_refused:watch_capacity")):
        d = sl.decide(_kprior(), _row(), {"id": 5, "alive": False}, _title(12),
                      registered_title=K_FRANCE, watch_read=wr)
        assert (d.kind, d.reason) == ("seat_unbound", f"{sl.R_LINE_WATCH_UNREADABLE}:{why}")
    d = sl.decide(sl.Prior(5, K_FRANCE), _row(), {"id": 5, "alive": False}, _title(12),
                  registered_title=K_FRANCE, watch_read=_watch(12))
    assert (d.kind, d.reason) == ("seat_unbound", sl.R_NO_LINE_START)


def test_K_the_line_heir_is_the_player_or_held_elsewhere_is_unbound():
    w = _watch(12, carrying=E_HRE, previous=5)
    assert sl.decide(_kprior(), _row(), {"id": 5, "alive": False}, None, registered_title=K_FRANCE,
                     watch_read=w, player_id=12).reason == sl.R_HEIR_IS_PLAYER
    assert sl.decide(_kprior(), _row(), {"id": 5, "alive": False}, None, registered_title=K_FRANCE,
                     watch_read=w, held_elsewhere=frozenset({12})).reason == sl.R_HOLDER_HELD_ELSEWHERE


def test_a_duke_keeps_the_title_rule():
    d = sl.decide(sl.Prior(5, 9), _row(), {"id": 5, "alive": False}, _title(None),
                  watch_read=_watch(12, carrying=E_HRE, previous=5))
    assert d.kind == "lost", "a duke's seat never reads the K line"


class _LineGW(_GW):
    """The fake gateway with the bridge's watch: query_character with
    `played_line` answers the profile of the start plus the watch object."""

    def __init__(self, *a, watch=None, **kw):
        super().__init__(*a, **kw)
        self.watch = watch
        self.reads = []

    async def read(self, action, params=None):
        self.reads.append((action, dict(params or {})))
        if action == "query_character" and (params or {}).get("played_line") == 1:
            cid = params["character_id"]
            w = self.watch if self.watch is not None else _watch(cid, "following", cid)
            return {**w, "id": params["character_id"], "alive": self.alive}
        return await self.answer(action, params)


def _kstep(gw, prior, sink):
    return asyncio.run(sl.step(gw, prior, sink=str(sink), seat_session_id="seat-K-5",
                               registered_title=K_FRANCE,
                               own_session_id=lambda g: "s1",
                               own_session_row=lambda bs, sid: next(
                                   (r for r in bs.get("sessions", []) if r["session_id"] == sid), None)))


def test_K_step_arms_the_watch_at_bound_and_follows_the_line_through_a_resume(tmp_path):
    sink = tmp_path / "seat.jsonl"
    gw = _LineGW(_row(), alive=True, holder=None)
    prior = _kstep(gw, None, sink)
    assert gw.reads[1] == ("query_character", {"character_id": 5, "played_line": 1}), "armed at bound"
    rows = [json.loads(ln) for ln in sink.read_text().splitlines()]
    assert rows[0]["kind"] == "bound" and rows[0]["line_start_id"] == 5 and rows[0]["line_watch_armed"] is True
    assert prior.line_start == 5
    # a resumed server knows the line's start from the sink
    prior = sl.prior_from_sink(str(sink))
    assert prior.line_start == 5
    # K dies; the registered title is unheld (holder None); the line went to 12
    gw.alive, gw.reads = False, []
    gw.watch = _watch(12, carrying=E_HRE, previous=5)
    prior = _kstep(gw, prior, sink)
    assert ("query_character", {"character_id": 5, "played_line": 1}) in gw.reads
    assert not [r for r in gw.reads if r[0] == "query_title_hierarchy"], "K's seat never reads the title"
    assert gw.writes == [("set_controlled_character", {"character_id": 12, "disable_native_ai": False})]
    rows = [json.loads(ln) for ln in sink.read_text().splitlines()]
    assert [r["kind"] for r in rows] == ["bound", "rebind"]
    assert (rows[1]["new_id"], rows[1]["title_id"], rows[1]["reason"]) == (12, E_HRE, None)
    assert prior.ruler == 12 and prior.line_start == 5 and not prior.ended


def test_K_step_a_failed_watch_read_is_no_verdict(tmp_path):
    class Flaky(_LineGW):
        async def read(self, action, params=None):
            if (params or {}).get("played_line") == 1:
                return {"error": "game_thread_timeout"}
            return await self.answer(action, params)
    sink = tmp_path / "seat.jsonl"
    prior = _kstep(Flaky(_row(), alive=False, holder=None), _kprior(), sink)
    rows = [json.loads(ln) for ln in sink.read_text().splitlines()]
    assert [r["kind"] for r in rows] == ["read_failed"]
    assert rows[0]["reason"] == "query_character:played_line:game_thread_timeout"
    assert prior.ruler == 5 and not prior.ended


# ── owner ruling 2026-11-12 (v3reg2 s3 D1 moved onto the rival king Carloman):
# a succession never moves a seat outside its registered realm; the bridge
# refuses and the seat records it `lost heir_outside_realm`. Keys from the
# emitters: session.cpp sessions_json (unbound_reason, unbound_heir_id,
# realm_root_id) and dispatch_router.cpp do_set_controlled_character's realm
# refusal (success false, reason, character_id, dead_ruler_id).

CARLOMAN = 15319


def test_the_bridge_refused_an_outside_realm_heir_the_seat_is_lost_and_recorded():
    """Red on 8cef520a: an unbound_reason other than heir_is_player fell through
    to `seat_unbound released_without_death` (an instrument verdict)."""
    row = _row(primary_character_id=0, claimed_character_ids=[], unbound_reason="heir_outside_realm",
               unbound_heir_id=CARLOMAN, realm_root_id=12015)
    d = sl.decide(sl.Prior(5, 9), row, {"id": 5, "alive": False}, None)
    assert (d.kind, d.old_id, d.new_id, d.reason, d.source, d.bind_target) == \
        ("lost", 5, CARLOMAN, "heir_outside_realm", "bridge_handoff", 0)


def test_an_explicit_rebind_the_realm_guard_refuses_is_lost_not_unbound(tmp_path):
    sink = tmp_path / "seat.jsonl"
    refusal = {"success": False, "reason": "heir_outside_realm", "character_id": CARLOMAN,
               "dead_ruler_id": 5}
    gw = _GW(_row(), alive=False, holder=CARLOMAN, bind_reply=refusal)
    prior = _step(gw, sl.Prior(5, 9), sink)
    rows = _rows_of(sink)
    assert [(r["kind"], r["reason"], r["new_id"]) for r in rows] == [("lost", "heir_outside_realm", CARLOMAN)]
    assert prior.ended and prior.ruler == 5, "never moved onto the rival"


def test_an_unresolved_realm_is_pending_and_retried(tmp_path):
    sink = tmp_path / "seat.jsonl"
    gw = _GW(_row(), alive=False, holder=12,
             bind_reply={"success": False, "reason": "realm_unresolved", "character_id": 12,
                         "dead_ruler_id": 5})
    prior = _step(gw, sl.Prior(5, 9), sink)
    assert [(r["kind"], r["reason"]) for r in _rows_of(sink)] == [("pending", "realm_unresolved")]
    assert not prior.ended
    gw.bind_reply = {"success": True}
    prior = _step(gw, prior, sink)
    assert _rows_of(sink)[-1]["kind"] == "rebind" and prior.ruler == 12
