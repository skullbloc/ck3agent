"""session(action="wait") and the unpause reply read the bridge, not the cache.

Measured 2026-10-18 (persona test): Claude Code refuses a foreground `sleep` of
30 s or more, so the protocol's `sleep 60` never ran and every multi-cycle run
stopped after one cycle; and a successful `unpause` replied `paused: true`
because the gateway's cached tick_start is not refreshed by a management write.
"""

import asyncio
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(__file__))

redesign = pytest.importorskip("redesign")


class Gw:
    """get_tick_info follows the pause flag the writes set, after `lag` reads."""

    def __init__(self, paused=True, lag=0, stuck=False):
        self.paused, self.lag, self.stuck = paused, lag, stuck
        self.date = "867.1.1"
        self.speed = 1
        self.reads, self.writes = [], []
        self.health = "ok"

    async def read(self, action, params=None):
        self.reads.append(action)
        if action != "get_tick_info":
            return None
        if self.lag:
            self.lag -= 1
            return {"tick": 10, "game_date": self.date, "paused": not self.paused,
                    "game_speed": self.speed, "speed_read_status": "ok"}
        return {"tick": 10, "game_date": self.date, "paused": self.paused,
                "game_speed": self.speed, "speed_read_status": "ok"}

    async def write(self, action, params=None):
        self.writes.append(action)
        if action == "set_speed" and not self.stuck:
            self.speed = (params or {}).get("speed")   # speed_primitive stores it at once
        if action == "unpause_game" and not self.stuck:
            self.paused = False
            self.date = "867.2.1"      # the clock ran while unpaused
        if action == "pause_game":
            self.paused = True
        return {"success": True}

    def game(self):
        return {"tick": 1, "date": "stale", "paused": True, "speed": 3}


def run(coro):
    return asyncio.run(coro)


@pytest.fixture
def no_sleep(monkeypatch):
    slept = []

    async def fake(s):
        slept.append(s)
    monkeypatch.setattr(redesign, "_SLEEP", fake)
    return slept


def test_wait_unpauses_blocks_pauses_and_reports_both_dates(no_sleep):
    gw = Gw(paused=True)
    out = run(redesign.session_impl(gw, "wait", seconds=45))
    assert out["ok"] is True and out["verdict"] == "applied"
    assert gw.writes == ["unpause_game", "pause_game"]
    assert 45 in no_sleep
    r = out["result"]
    assert (r["waited_s"], r["start_date"], r["end_date"], r["paused"]) == (45, "867.1.1", "867.2.1", True)
    assert r["clock_ran"] is True
    assert out["game"]["date"] == "867.2.1" and out["game"]["paused"] is True, "fresh, not the cache"


def test_wait_is_capped_and_floored(no_sleep):
    out = run(redesign.session_impl(Gw(), "wait", seconds=5000))
    assert out["result"]["waited_s"] == redesign.WAIT_CAP_S == max(no_sleep)
    assert out["result"]["cap_s"] == redesign.WAIT_CAP_S
    out = run(redesign.session_impl(Gw(), "wait", seconds=0))
    assert out["result"]["waited_s"] == 1
    out = run(redesign.session_impl(Gw(), "wait", seconds="abc"))
    assert out["result"]["waited_s"] == 60


def test_wait_on_a_running_game_does_not_unpause_twice(no_sleep):
    gw = Gw(paused=False)
    out = run(redesign.session_impl(gw, "wait", seconds=10))
    assert out["ok"] is True and gw.writes == ["pause_game"]


def test_wait_refuses_when_the_game_will_not_unpause(no_sleep):
    gw = Gw(paused=True, stuck=True)
    out = run(redesign.session_impl(gw, "wait", seconds=30))
    assert out["ok"] is False and "did not unpause" in out["blocked"]["summary"]
    assert gw.writes == ["unpause_game"], "nothing waited, nothing paused"
    assert 30 not in no_sleep


def test_unpause_reply_reads_the_bridge_until_it_says_running(no_sleep):
    gw = Gw(paused=True, lag=2)       # the first two reads still say paused
    out = run(redesign.session_impl(gw, "unpause"))
    assert out["result"] == {"paused": False, "settled": True}
    assert out["game"]["paused"] is False and out["game"]["date"] == "867.2.1"
    assert gw.reads.count("get_tick_info") == 3


def test_unpause_reply_is_honest_when_the_bridge_never_settles(no_sleep, monkeypatch):
    gw = Gw(paused=True, stuck=True)
    monkeypatch.setattr(redesign, "_settle_pause",
                        lambda g, want, timeout_s=3.0: redesign._fresh_tick(g))
    out = run(redesign.session_impl(gw, "unpause"))
    assert out["result"] == {"paused": True, "settled": False}
    # L14 FF1 M-5: a state never read back is pending, never applied.
    assert out["verdict"] == "pending" and out["applied"] is False, out
    assert out["pending"]["drains_when"] == "next_pass", out["pending"]


class Refusing(Gw):
    """The bridge answers every write with its fault reply, in the shape it really
    sends: MEMORY_ACCESS_FAULT_REPLY = {"error":"memory_access_fault"}
    (bridge/include/bridge.h), from answer_memory_access_fault in case_set_speed /
    case_pause_game / case_unpause_game (bridge/src/dispatch_router.cpp). It
    carries no `success`. (FF1's fixture said {"success": false, ...}, a shape
    the bridge never sends, and so could not see N-MAJ-1.) The state does not
    move: the clock reads what it read before the write."""

    async def write(self, action, params=None):
        self.writes.append(action)
        return {"error": "memory_access_fault"}


@pytest.mark.parametrize("action,kw", [("speed", {"speed": 2}), ("pause", {}), ("unpause", {})])
def test_a_refused_clock_write_is_rejected_not_applied(no_sleep, action, kw):
    """L14 FF2 N-MAJ-1 (the witness's run 2): the bridge's fault reply is
    write_blocked, verdict rejected, with the bridge's reason. RED on b84e3634
    for speed (answered applied)."""
    gw = Refusing(paused=(action == "unpause"))
    gw.speed = 2 if action == "speed" else 1   # even a speed that already reads 2
    out = run(redesign.session_impl(gw, action, **kw))
    assert out["ok"] is False and out["verdict"] == "rejected", out
    assert out.get("applied") is False, out
    assert out["blocked"]["raw_reason"] == "memory_access_fault", out["blocked"]


def test_speed_reads_the_speed_back(no_sleep):
    """L14 FF2 M-5: speed settles like pause/unpause; the reply is the reading."""
    gw = Gw(paused=True)
    out = run(redesign.session_impl(gw, "speed", speed=2))
    assert out["verdict"] == "applied" and out["result"]["settled"] is True, out
    assert out["result"]["speed"] == 2 and out["game"]["speed"] == 2, out
    assert "get_tick_info" in gw.reads


def test_a_speed_that_never_reads_back_is_pending(no_sleep, monkeypatch):
    """The write was not refused and get_tick_info never reads the speed asked
    for: pending next_pass, never applied. RED on b84e3634 (applied, no read)."""
    monkeypatch.setattr(redesign, "_settle_tick",
                        lambda g, settled, timeout_s=3.0: redesign._fresh_tick(g))
    gw = Gw(paused=True, stuck=True)
    out = run(redesign.session_impl(gw, "speed", speed=2))
    assert out["verdict"] == "pending" and out["applied"] is False, out
    assert out["result"]["settled"] is False and out["result"]["speed"] == 1, out
    assert out["pending"]["drains_when"] == "next_pass", out["pending"]


def test_a_wait_whose_closing_pause_never_reads_paused_is_pending(no_sleep, monkeypatch):
    """The wait's closing pause is a write like any other: a pause not read back
    is pending, never applied (N-MAJ-1 sweep). RED on b84e3634."""
    class NoPause(Gw):
        async def write(self, action, params=None):
            self.writes.append(action)
            if action == "unpause_game":
                self.paused = False
            return {"success": True}
    monkeypatch.setattr(redesign, "_settle_pause",
                        lambda g, want, timeout_s=3.0: redesign._fresh_tick(g))
    out = run(redesign.session_impl(NoPause(paused=True), "wait", seconds=5))
    assert out["verdict"] == "pending" and out["result"]["paused"] is False, out


def test_a_wait_whose_closing_pause_is_REFUSED_is_rejected_not_pending(no_sleep):
    """L14 FF3 N3-2: the closing pause answered with the bridge's fault reply
    (MEMORY_ACCESS_FAULT_REPLY, bridge/include/bridge.h, from
    answer_memory_access_fault in case_pause_game, dispatch_router.cpp), the clock
    still running. Nothing will drain: rejected, gate bridge_fault, never ok:true
    pending next_pass. RED on 293c0e00 (ok True, pending, next_pass)."""
    class PauseFaults(Gw):
        async def write(self, action, params=None):
            self.writes.append(action)
            if action == "pause_game":
                return {"error": "memory_access_fault"}
            self.paused, self.date = False, "867.2.1"
            return {"success": True}
    gw = PauseFaults(paused=True)
    out = run(redesign.session_impl(gw, "wait", seconds=5))
    assert out["ok"] is False and out["verdict"] == "rejected", out
    assert out.get("pending") is None and out.get("applied") is False, out
    assert out["blocked"]["gate"] == "bridge_fault", out["blocked"]
    assert out["blocked"]["raw_reason"] == "memory_access_fault", out["blocked"]
    assert "still running" in out["blocked"]["summary"], out["blocked"]
    assert out["result"]["paused"] is False and out["result"]["end_date"] == "867.2.1", out
    assert gw.writes == ["unpause_game", "pause_game"]


def test_pause_reads_the_bridge_back(no_sleep):
    """pause settles like unpause: the reply is the reading, not a literal."""
    gw = Gw(paused=False, lag=2)
    out = run(redesign.session_impl(gw, "pause"))
    assert out["verdict"] == "applied" and out["result"] == {"paused": True, "settled": True}
    assert gw.reads.count("get_tick_info") == 3


def test_a_pause_that_never_reads_paused_is_pending(no_sleep, monkeypatch):
    class Stuck(Gw):
        async def write(self, action, params=None):
            self.writes.append(action)
            return {"success": True}
    monkeypatch.setattr(redesign, "_settle_pause",
                        lambda g, want, timeout_s=3.0: redesign._fresh_tick(g))
    out = run(redesign.session_impl(Stuck(paused=False), "pause"))
    assert out["verdict"] == "pending" and out["result"]["settled"] is False, out


def test_wait_is_a_listed_session_action():
    import mcp_server
    sess = next(t for t in mcp_server.registered_tool_definitions() if t["name"] == "session")
    props = sess["input_schema"]["properties"]
    assert "wait" in props["action"]["enum"] and "seconds" in props
