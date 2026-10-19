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
        self.reads, self.writes = [], []
        self.health = "ok"

    async def read(self, action, params=None):
        self.reads.append(action)
        if action != "get_tick_info":
            return None
        if self.lag:
            self.lag -= 1
            return {"tick": 10, "game_date": self.date, "paused": not self.paused}
        return {"tick": 10, "game_date": self.date, "paused": self.paused}

    async def write(self, action, params=None):
        self.writes.append(action)
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


def test_wait_is_a_listed_session_action():
    import tools
    sess = next(t for t in tools._redesign_tool_definitions() if t["name"] == "session")
    props = sess["input_schema"]["properties"]
    assert "wait" in props["action"]["enum"] and "seconds" in props
