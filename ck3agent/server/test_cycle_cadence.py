"""v3 §7.2 / §7.1 (lane s0-seats): the seat's cycle is measured in GAME-DAYS.

`session(action="wait", game_days=N)` loops bounded waits until the date has
advanced N days, planned from the measured clock rate; an ask over the
registered cadence (CK3_CYCLE_GAME_DAYS) is refused, never clamped; without
`game_days` the wait is byte-for-byte the old one. Driven through the same
fake gateway and sleep stub as test_session_wait, with a clock that advances
at a RATE per slept second — the thing seconds cannot express.

Red on 0a969f2e: `session` has no game_days; cycle_cadence does not exist.
"""
import asyncio
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import cycle_cadence as cc  # noqa: E402
import redesign  # noqa: E402


def test_ordinal_is_the_engine_calendar_and_the_registered_horizon_is_3650_days():
    assert cc.ordinal("877.01.03") - cc.ordinal("867.01.03") == 3650      # §7.2
    assert cc.ordinal("867.03.01") - cc.ordinal("867.02.01") == 28         # real month lengths
    assert cc.ordinal("868.01.01") - cc.ordinal("867.01.01") == 365        # no leap
    assert cc.ordinal("x") is None and cc.ordinal("867.13.1") is None
    # §7.1's rule at that horizon, round half UP as exclusion_rule.h does (llround).
    assert int(0.05 * 3650 + 0.5) == 183


def test_plan_wait_probes_first_then_follows_the_measured_rate_within_the_cap():
    assert cc.plan_wait(90, elapsed_s=0, advanced_days=0, cap_s=110) == cc.PROBE_S
    assert cc.plan_wait(74, elapsed_s=15, advanced_days=16, cap_s=110) == 70      # 1.067 d/s
    assert cc.plan_wait(74, elapsed_s=15, advanced_days=0, cap_s=110) == cc.PROBE_S  # stalled: probe again
    assert cc.plan_wait(900, elapsed_s=10, advanced_days=1, cap_s=60) == 60        # capped by seconds
    assert cc.plan_wait(1, elapsed_s=10, advanced_days=100, cap_s=60) == 1
    assert cc.plan_wait(5, elapsed_s=1, advanced_days=1, cap_s=3) == 3


def test_the_registered_cadence_is_read_from_the_env_and_a_malformed_one_is_an_error():
    assert cc.registered_cycle_game_days({}) == (None, None)
    assert cc.registered_cycle_game_days({cc.CYCLE_GAME_DAYS_ENV: "90"}) == (90, None)
    v, err = cc.registered_cycle_game_days({cc.CYCLE_GAME_DAYS_ENV: "ninety"})
    assert v is None and "not an integer" in err
    v, err = cc.registered_cycle_game_days({cc.CYCLE_GAME_DAYS_ENV: "0"})
    assert v is None and "positive" in err


class Clock:
    """A fake gateway whose date advances `rate` game-days per slept second while unpaused."""

    def __init__(self, rate=1.1, start="867.01.03", stall_after_s=None):
        self.rate, self.days, self.start = rate, 0.0, start
        self.paused, self.slept, self.writes = True, [], []
        self.stall_after_s = stall_after_s
        self.health = "ok"

    def date(self):
        base = cc.ordinal(self.start) + int(self.days)
        y, rem = divmod(base, 365)
        m = max(i for i in range(12) if cc.MONTH_CUM_DAYS[i] <= rem)
        return f"{y}.{m + 1}.{rem - cc.MONTH_CUM_DAYS[m] + 1}"

    async def read(self, action, params=None):
        if action == "get_tick_info":
            return {"tick": 1, "game_date": self.date(), "paused": self.paused,
                    "game_speed": 2, "speed_read_status": "ok"}
        return None

    async def write(self, action, params=None):
        self.writes.append(action)
        if action == "unpause_game":
            self.paused = False
        if action == "pause_game":
            self.paused = True
        return {"success": True}

    async def sleep(self, s):
        self.slept.append(s)
        if self.stall_after_s is not None and sum(self.slept) > self.stall_after_s:
            return
        if not self.paused:
            self.days += self.rate * s

    def game(self):
        return {"tick": 1, "date": self.date(), "paused": self.paused, "speed": 2}


def _wait(gw, monkeypatch, **kw):
    monkeypatch.setattr(redesign, "_SLEEP", gw.sleep)
    return asyncio.run(redesign.session_impl(gw, "wait", **kw))


def test_wait_in_game_days_covers_the_ask_with_bounded_calls_and_little_overshoot(monkeypatch):
    monkeypatch.delenv(cc.CYCLE_GAME_DAYS_ENV, raising=False)
    gw = Clock(rate=1.1)
    out = _wait(gw, monkeypatch, seconds=60, game_days=90)
    r = out["result"]
    assert out["ok"] is not False and out["verdict"] == "applied"
    assert r["game_days_requested"] == 90 and 90 <= r["game_days_advanced"] <= 93
    assert r["stopped"] == cc.STOP_REACHED and r["calls"] == len(gw.slept) >= 2
    assert gw.slept[0] == cc.PROBE_S and all(1 <= s <= 60 for s in gw.slept)
    assert r["cycle_start_date"] == "867.1.3" and r["end_date"] == gw.date() and gw.paused is True
    assert r["cadence_game_days"] is None
    # the prompt-level shape this replaces: 60 s at 1.1 d/s is 66 days per call, not 60
    assert cc.ordinal(r["end_date"]) - cc.ordinal(r["cycle_start_date"]) == r["game_days_advanced"]


def test_wait_without_game_days_is_the_old_single_wait(monkeypatch):
    gw = Clock(rate=1.1)
    out = _wait(gw, monkeypatch, seconds=45)
    r = out["result"]
    assert gw.slept == [45] and r["waited_s"] == 45 and "game_days_requested" not in r
    assert out["verdict"] == "applied"


def test_an_ask_over_the_registered_cadence_is_refused_never_clamped(monkeypatch):
    monkeypatch.setenv(cc.CYCLE_GAME_DAYS_ENV, "90")
    gw = Clock()
    out = _wait(gw, monkeypatch, seconds=60, game_days=120)
    assert out["ok"] is False and out["blocked"]["gate"] == cc.R_OVER_CADENCE
    assert out["blocked"]["measured"] == {"game_days": 120, "cadence_game_days": 90}
    assert gw.slept == [] and gw.writes == []
    out = _wait(gw, monkeypatch, seconds=60, game_days=90)
    assert out["verdict"] == "applied" and out["result"]["cadence_game_days"] == 90
    monkeypatch.setenv(cc.CYCLE_GAME_DAYS_ENV, "lots")
    out = _wait(Clock(), monkeypatch, seconds=60, game_days=10)
    assert out["ok"] is False and out["blocked"]["gate"] == cc.R_CADENCE_UNREADABLE


def test_a_bad_game_days_argument_is_invalid_and_a_stalled_clock_stops_the_loop(monkeypatch):
    monkeypatch.delenv(cc.CYCLE_GAME_DAYS_ENV, raising=False)
    for bad in (-1, cc.HARD_GAME_DAYS_CEILING + 1, "ninety"):
        out = _wait(Clock(), monkeypatch, seconds=60, game_days=bad)
        assert out["ok"] is False and out["blocked"]["gate"] == "invalid_argument"
    gw = Clock(rate=1.0, stall_after_s=20)
    out = _wait(gw, monkeypatch, seconds=60, game_days=200)
    r = out["result"]
    assert out["ok"] is False and out["blocked"]["gate"] == "cycle_not_reached"
    assert r["stopped"] == cc.STOP_NO_PROGRESS and r["game_days_advanced"] < 200
    assert len(gw.slept) <= cc.MAX_WAIT_CALLS


def test_a_wait_at_or_past_the_registered_horizon_is_refused_and_before_it_is_not(monkeypatch):
    """v3 §7.2 (Study 0 round 2, signing blocker c): the seat is never told
    the horizon; a wait made at or past it is `horizon_reached`, nothing is
    waited, and without the env the public behaviour is unchanged."""
    monkeypatch.delenv(cc.CYCLE_GAME_DAYS_ENV, raising=False)
    monkeypatch.setenv(cc.HORIZON_END_ENV, "877.01.03")
    gw = Clock(rate=1.0, start="877.01.03")
    for kw in ({"seconds": 30}, {"seconds": 30, "game_days": 10}):
        out = _wait(gw, monkeypatch, **kw)
        assert out["ok"] is False and out["blocked"]["gate"] == cc.R_HORIZON_REACHED, out
        assert out["blocked"]["measured"] == {"game_date": "877.1.3"}
        assert gw.slept == [] and gw.writes == []           # nothing was waited
    assert cc.R_HORIZON_REACHED in redesign.GATE_TABLE
    # the day before the horizon: the wait runs and carries the clock past it
    gw = Clock(rate=1.0, start="877.01.02")
    out = _wait(gw, monkeypatch, seconds=5)
    assert out["verdict"] == "applied" and gw.slept == [5]
    # ...and the next call is refused: the harness ends the seat, the seat does not count down
    assert _wait(gw, monkeypatch, seconds=5)["blocked"]["gate"] == cc.R_HORIZON_REACHED
    # a horizon the env cannot spell is an instrument fault, never a pass
    monkeypatch.setenv(cc.HORIZON_END_ENV, "soon")
    out = _wait(Clock(), monkeypatch, seconds=5)
    assert out["ok"] is False and out["blocked"]["gate"] == cc.R_HORIZON_UNREADABLE
    # no env: the public behaviour, byte for byte (no extra read either)
    monkeypatch.delenv(cc.HORIZON_END_ENV, raising=False)
    gw = Clock(rate=1.0, start="900.01.01")
    assert _wait(gw, monkeypatch, seconds=5)["verdict"] == "applied"
    assert cc.horizon_reached("877.1.3", None) is None and cc.horizon_reached("x", 5) is None


def _wait_env(start, end, **res):
    return {"ok": True, "verdict": "applied", "result": {"waited_s": 10, "start_date": start,
                                                          "end_date": end, **res}}


def test_the_cycle_is_the_registered_cadence_in_game_days_across_waits_not_any_wait_that_waited():
    """v3 §5.2 / §7.2, signing blocker f: ONE cycle. A seat waiting one day
    per call does not get a fresh send quota per call."""
    c = cc.CycleCounter.from_env({cc.CYCLE_GAME_DAYS_ENV: "90"})
    assert c.cadence_days == 90 and c.cycle == 0
    assert c.note("wait", _wait_env("867.01.03", "867.01.04")) == 0        # one day: no boundary
    assert c.note("wait", _wait_env("867.01.04", "867.02.03")) == 0        # 31 days in
    assert c.note("wait", _wait_env("867.02.03", "867.04.02")) == 0        # 89 days in
    assert c.note("wait", _wait_env("867.04.02", "867.04.03")) == 1        # day 90: the boundary
    assert c.cycle_start_ordinal == cc.ordinal("867.01.03") + 90
    # a game_days wait reports cycle_start_date (its first inner wait) and end_date
    assert c.note("wait", _wait_env("867.06.01", "867.07.02", cycle_start_date="867.04.03",
                                    game_days_advanced=90)) == 2
    # one wait that covered two boundaries moves two
    assert c.note("wait", _wait_env("867.07.02", "868.01.03")) == 4
    # a wait that did not wait, or whose dates cannot be read, moves nothing
    assert c.note("wait", {"ok": False, "blocked": {"gate": "horizon_reached"}, "result": {}}) == 4
    assert c.note("wait", {"ok": True, "result": {"waited_s": 5, "start_date": None, "end_date": None}}) == 4
    assert c.note("observe", _wait_env("868.01.03", "869.01.03")) == 4
    # free play (no cadence): every wait that waited closes a cycle, the public rule
    f = cc.CycleCounter.from_env({})
    assert f.cadence_days is None
    assert f.note("wait", _wait_env("867.01.03", "867.01.04")) == 1
    assert f.note("wait", {"ok": False, "verdict": "rejected", "result": {"waited_s": 1}}) == 2
    assert f.note("wait", {"ok": False, "blocked": {"gate": "game_did_not_unpause"}, "result": {}}) == 2
