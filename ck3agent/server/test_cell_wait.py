"""2026.12.4 (cell0 gaps 5+7; PLAN §P decisions 1 and 4): a CELL seat's wait.

cell0 ran three seats taught `session(action="wait", seconds=60)`: a seconds
wait is not held to CK3_CYCLE_GAME_DAYS, so the decision cadence was the clock
rate times the seconds asked, and the horizon was `ck3agent run --until-date`,
which overshot six days because nothing capped an in-flight wait. Under a cell
(CK3_EPISODE_HORIZON_START set beside the cadence and the horizon end) every
wait now returns at the next cycle boundary of ONE schedule the seats share,
start + k x cadence, capped at the horizon end; `seconds` is recorded, never the
target. Without HORIZON_START the published wait is unchanged.

Driven through test_cycle_cadence's AlarmClock (the emitters' reply keys).
Red on 08bb8f92: cycle_cadence has no cell_schedule and a seconds wait sleeps
its seconds.
"""
import asyncio
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import cycle_cadence as cc  # noqa: E402
import redesign  # noqa: E402
from test_cycle_cadence import AlarmClock, Clock, _wait  # noqa: E402

START, END = "867.01.03", "867.07.02"          # cell0's horizon: 2 x 90 game-days
#: The spelling, literal here so the base tree's behaviour is what reds (not a
#: missing constant); test_the_start_is_the_bridge_s_spelling pins the module's.
START_ENV = "CK3_EPISODE_HORIZON_START"


@pytest.fixture
def cell(monkeypatch):
    monkeypatch.setenv(START_ENV, START)
    monkeypatch.setenv(cc.HORIZON_END_ENV, END)
    monkeypatch.setenv(cc.CYCLE_GAME_DAYS_ENV, "90")


def test_the_start_is_the_bridge_s_spelling():
    """episode_policy.cpp reads CK3_EPISODE_HORIZON_START; one spelling."""
    assert cc.HORIZON_START_ENV == START_ENV
    src = (Path(__file__).resolve().parents[2] / "bridge" / "src" / "episode_policy.cpp").read_text()
    assert f'env_str("{START_ENV}")' in src


def test_the_schedule_is_start_plus_whole_cadences_capped_at_the_horizon():
    s = cc.CellSchedule(start=cc.ordinal(START), end=cc.ordinal(START) + 200, cadence=90)
    o = s.start
    assert [s.next_boundary(x) for x in (o, o + 1, o + 89, o + 90, o + 179, o + 180, o + 199)] == \
        [o + 90, o + 90, o + 90, o + 180, o + 180, o + 200, o + 200]
    assert [s.cycle_index(x) for x in (o, o + 89, o + 90, o + 180, o + 199, o + 200, o + 400)] == \
        [0, 0, 1, 2, 2, 3, 3]
    assert s.next_boundary(o - 5) == o + 90


def test_the_schedule_is_read_only_when_the_start_is_set_and_then_refuses_what_is_missing():
    assert cc.cell_schedule({cc.HORIZON_END_ENV: END, cc.CYCLE_GAME_DAYS_ENV: "90"}) == (None, None)
    s, err = cc.cell_schedule({cc.HORIZON_START_ENV: START, cc.HORIZON_END_ENV: END,
                               cc.CYCLE_GAME_DAYS_ENV: "90"})
    assert err is None and (s.start, s.end, s.cadence) == (cc.ordinal(START), cc.ordinal(END), 90)
    for env in ({cc.HORIZON_START_ENV: START, cc.HORIZON_END_ENV: END},
                {cc.HORIZON_START_ENV: START, cc.CYCLE_GAME_DAYS_ENV: "90"},
                {cc.HORIZON_START_ENV: "soon", cc.HORIZON_END_ENV: END, cc.CYCLE_GAME_DAYS_ENV: "90"},
                {cc.HORIZON_START_ENV: END, cc.HORIZON_END_ENV: START, cc.CYCLE_GAME_DAYS_ENV: "90"}):
        s, err = cc.cell_schedule(env)
        assert s is None and err, env


def test_a_seconds_wait_returns_at_the_cycle_boundary_and_records_the_seconds(cell, monkeypatch):
    gw = AlarmClock(rate=1.6, start="867.01.20")                  # mid-cycle: 17 days in
    out = _wait(gw, monkeypatch, seconds=60)
    r = out["result"]
    assert out["verdict"] == "applied", out
    assert r["stopped"] == cc.STOP_CYCLE_END and r["horizon_reached"] is False
    assert r["end_date"] == "867.4.3" and r["game_days_advanced"] == 73   # start + 90, not 60 s x rate
    assert r["cycle_index"] == 1 and r["requested_seconds"] == 60 and r["wait_mode"] == "cycle"
    assert ("unpause_game", {"pause_at": "0867.04.03"}) in gw.params
    assert "pause_game" not in gw.writes                         # free running: no closing pause
    assert "waited_s" in r and cc.wait_completed("wait", out)     # a wait that waited
    # the seat's counter adopts the schedule's index
    c = cc.CycleCounter(cadence_days=90)
    assert c.note("wait", out) == 1


def test_the_last_cycle_ends_at_the_horizon_and_the_next_wait_is_refused(cell, monkeypatch):
    gw = AlarmClock(rate=1.6, start="867.04.03")                  # at the first boundary
    out = _wait(gw, monkeypatch, seconds=30, game_days=10)
    r = out["result"]
    assert out["verdict"] == "applied" and r["stopped"] == cc.STOP_HORIZON
    assert r["horizon_reached"] is True and r["end_date"] == "867.7.2" and r["cycle_index"] == 2
    assert r["requested_game_days"] == 10 and r["game_days_advanced"] == 90   # game_days is not the target
    again = _wait(gw, monkeypatch, seconds=60)
    assert again["ok"] is False and again["blocked"]["gate"] == cc.R_HORIZON_REACHED


def test_a_seat_whose_boundary_another_seat_already_rearmed_past_does_not_sleep_a_cycle(cell, monkeypatch):
    """Two seats, one clock, one alarm: seat A's wait returned at the boundary
    and A re-armed the next one before seat B's poll saw `fired`. B's wait ends
    at its own boundary (the date is past it), not one cycle later."""
    gw = AlarmClock(rate=1.6, start="867.03.20")
    rearmed = []

    async def other_seat_rearms(s, _orig=gw.sleep):
        await _orig(s)
        if gw.alarm.get("state") == "fired" and not rearmed:
            rearmed.append(True)
            await gw.write("unpause_game", {"pause_at": "0867.07.02"})   # seat A's next wait
            await _orig(1)
    gw.sleep = other_seat_rearms
    out = _wait(gw, monkeypatch, seconds=60)
    r = out["result"]
    assert rearmed and out["verdict"] == "applied" and r["stopped"] == cc.STOP_CYCLE_END
    assert cc.ordinal(r["end_date"]) >= cc.ordinal("867.04.03")
    assert cc.ordinal(r["end_date"]) < cc.ordinal("867.05.01") and r["cycle_index"] == 1


def test_a_pause_another_seat_writes_does_not_end_a_cell_wait_short(cell, monkeypatch):
    """Free running: a seat's session(pause) clears the alarm; a cell wait arms
    its boundary again and runs on, rather than returning cycle_not_reached."""
    gw = AlarmClock(rate=1.0, start="867.03.20")

    async def pause_midway(s, _orig=gw.sleep):
        await _orig(s)
        if len(gw.slept) == 3:
            await gw.write("pause_game", {})
            gw.paused = True
    gw.sleep = pause_midway
    out = _wait(gw, monkeypatch, seconds=60)
    assert out["verdict"] == "applied" and out["result"]["stopped"] == cc.STOP_CYCLE_END
    assert out["result"]["calls"] == 2


def test_an_incomplete_cell_env_is_refused_and_nothing_is_written(monkeypatch):
    monkeypatch.setenv(START_ENV, START)
    monkeypatch.delenv(cc.CYCLE_GAME_DAYS_ENV, raising=False)
    monkeypatch.setenv(cc.HORIZON_END_ENV, END)
    gw = AlarmClock(rate=1.0, start="867.02.01")
    out = _wait(gw, monkeypatch, seconds=60)
    assert out["ok"] is False and out["blocked"]["gate"] == "cell_schedule_unreadable"
    assert gw.params == [] and gw.slept == []


def test_without_the_start_the_published_wait_is_unchanged(monkeypatch):
    """The control: the cadence and the horizon end alone (Study 0's seats) keep
    the published wait: a seconds wait sleeps its seconds once, a game_days ask
    over the cadence is refused, and no reply carries the cell's keys."""
    monkeypatch.delenv(START_ENV, raising=False)
    monkeypatch.setenv(cc.HORIZON_END_ENV, END)
    monkeypatch.setenv(cc.CYCLE_GAME_DAYS_ENV, "90")
    gw = Clock(rate=1.1, start="867.02.01")
    out = _wait(gw, monkeypatch, seconds=45)
    assert gw.slept == [45] and out["verdict"] == "applied"
    assert not {"cycle_index", "requested_seconds", "horizon_reached"} & set(out["result"])
    out = _wait(Clock(start="867.02.01"), monkeypatch, seconds=45, game_days=91)
    assert out["blocked"]["gate"] == cc.R_OVER_CADENCE


def test_the_registered_horizon_has_41_boundaries_and_the_last_cycle_is_50_days():
    """L23 witness item 10: v3 §7.2 said "40 cycles (3,650 / 90) is exactly the
    number of boundaries". It is 41: forty whole cycles, then the horizon end
    caps a 50-day last one. One count, the schedule's (the runner reads it)."""
    s, err = cc.cell_schedule({cc.HORIZON_START_ENV: "867.01.03", cc.HORIZON_END_ENV: "877.01.03",
                               cc.CYCLE_GAME_DAYS_ENV: "90"})
    assert err is None and s.end - s.start == 3650
    assert getattr(s, "n_boundaries", None) == 41 == s.cycle_index(s.end)
    assert s.end - (s.start + 40 * 90) == 50 and s.next_boundary(s.start + 40 * 90) == s.end


def test_an_unreadable_end_date_is_an_unknown_advance_not_zero_days(cell, monkeypatch):
    """L23 witness item 6: the closing read of a cell wait that cannot be read
    gave game_days_advanced 0 and horizon_reached false. Both are unknown (null);
    a wait whose loop read the date at the horizon keeps horizon_reached true."""
    import asyncio as _a

    class LosesTheDate(AlarmClock):
        """The loop's read sees the date at the boundary once; every read after
        it (the closing read) answers the bridge's "unknown"."""
        seen = False

        async def read(self, action, params=None):
            out = await super().read(action, params)
            if action == "get_tick_info" and self.alarm.get("state") == "fired":
                if self.seen:
                    out["game_date"] = "unknown"
                self.seen = True
            return out

    gw = LosesTheDate(rate=1.6, start="867.03.20")
    monkeypatch.setattr(redesign, "_SLEEP", gw.sleep)
    out = _a.run(redesign.session_impl(gw, "wait", seconds=60))
    r = out["result"]
    assert r["stopped"] == cc.STOP_CYCLE_END and r["end_date"] in (None, "unknown")
    assert r["game_days_advanced"] is None and r["horizon_reached"] is None and r["cycle_index"] is None

    gw = LosesTheDate(rate=1.6, start="867.06.20")          # the last cycle: ends at the horizon
    monkeypatch.setattr(redesign, "_SLEEP", gw.sleep)
    out = _a.run(redesign.session_impl(gw, "wait", seconds=60))
    r = out["result"]
    assert r["stopped"] == cc.STOP_HORIZON and r["horizon_reached"] is True
    assert r["game_days_advanced"] is None


def test_a_cell_seat_cannot_change_the_game_speed(cell, monkeypatch):
    """L23 witness item 3: the registered speed (v3 §7.3) is written and read back
    by the cell's runner; a seat's session(speed) on the shared clock would change
    it for every seat. Refused `speed_registered`, nothing written."""
    gw = AlarmClock(rate=1.0, start="867.02.01")
    monkeypatch.setattr(redesign, "_SLEEP", gw.sleep)
    out = asyncio.run(redesign.session_impl(gw, "speed", speed=3))
    assert out["ok"] is False and out["blocked"]["gate"] == "speed_registered"
    assert gw.params == [] and gw.writes == []


def test_outside_a_cell_the_speed_is_the_seats(monkeypatch):
    """The control: without the start, session(speed) writes as published."""
    monkeypatch.delenv(START_ENV, raising=False)
    gw = AlarmClock(rate=1.0, start="867.02.01")
    monkeypatch.setattr(redesign, "_SLEEP", gw.sleep)
    out = asyncio.run(redesign.session_impl(gw, "speed", speed=2))
    assert ("set_speed", {"speed": 2}) in gw.params
