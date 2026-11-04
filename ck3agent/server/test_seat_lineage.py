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
    prior = _step(gw, None, sink)                      # first sighting
    assert prior.ruler == 5 and prior.title_id == 9 and not sink.exists()
    gw.alive, gw.holder = False, 12
    prior = _step(gw, prior, sink)
    assert gw.writes == [("set_controlled_character", {"character_id": 12, "disable_native_ai": False})]
    rows = [json.loads(ln) for ln in sink.read_text().splitlines()]
    assert rows[0]["kind"] == "rebind" and rows[0]["old_id"] == 5 and rows[0]["new_id"] == 12
    assert rows[0]["title_id"] == 9 and rows[0]["game_date"] == "868.03.02"
    assert rows[0]["seat_session_id"] == "seat-K-5" and rows[0]["source"] == "explicit_control"
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


def test_step_never_raises_and_writes_the_failure_as_a_row(tmp_path):
    class Boom(_GW):
        async def read(self, action, params=None):
            raise RuntimeError("socket gone")
    sink = tmp_path / "seat.jsonl"
    prior = _step(Boom(_row(), True, None), sl.Prior(5, 9), sink)
    row = json.loads(sink.read_text().splitlines()[0])
    assert row["kind"] == "seat_unbound" and row["reason"].startswith("step_failed:")
    assert prior.ruler == 5


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
