"""cycle_cadence.py — the seat's decision cycle, measured in GAME-DAYS (v3 §7.2, §7.1).

Owner ruling 2026-11-04: a seat's cycle is every 90 game-days (`cycle.game_days`
in the conditions block, §9), the registered horizon is ten game-years
(867.01.03 → 877.01.03, §7.2; so §7.1's single constant gives X =
round(0.05 × 3650) = 183 game-days), and the cycle is HARNESS-ENFORCED in
game-days: `session(action="wait")` takes seconds only, and the budget pilot
ran at ~1.1 game-days per second with six seats, so a prompt-level "wait 60
days" overshot to ~165 days per cycle. Seconds are not the unit; the clock
rate is a property of the host.

THE ONE DEFINITION of five things, read by the server (`redesign.session_impl`
`wait`; `mcp_server.session` counts the cycle; the channel's quota is keyed on
it), the launcher (the cell's seat env, `dry_cell.build_plan`, sets the env from
the block and the episode's horizon) and the bundle's lineage reader
(`scripts/seat_lineage_record`):

  * `CYCLE_GAME_DAYS_ENV` — the registered cadence the seat's server reads; a
    `game_days` ask above it is REFUSED (`cycle_game_days_over_cadence`),
    never clamped: a clamp is a default in a new spelling. Absent = no
    registered cadence (an interactive session), and the hard ceiling alone
    bounds the ask.
  * `HORIZON_END_ENV` — the registered horizon end (§7.2, the block's
    `CK3_EPISODE_HORIZON_END`, the same spelling the bridge reads), set per
    seat by the driver. A `session(action="wait")` made when the game date is
    at or past it is REFUSED (`horizon_reached`): the harness ends the seat,
    and the seat is never told the date (owner ruling 2026-11-04) — it learns
    the horizon only by reaching it. Absent = the public behaviour, unchanged.
  * `CycleCounter` — THE seat's cycle index (v3 §5.2, §7.2: ONE cycle, not
    two). Under a registered cadence a cycle ends when the game date has
    advanced ≥ the cadence since the cycle began, across however many
    `session(action="wait")` calls it took — a seat waiting one day per call
    does not get a fresh send quota per call. Without a cadence (free play)
    every wait that waited closes a cycle, the public rule unchanged.
    `channel.CycleCounter` IS this class; every send and receipt row carries
    the index it reports.
  * `ordinal` — game-days since year 0 on the ENGINE's calendar: a fixed
    365-day year with real month lengths and no leap years
    (re/findings/re_game_calendar.md; `bridge/include/game_date.h`).
  * `plan_wait` — the next bounded bridge wait (seconds ≤ WAIT_CAP_S) from the
    days still owed and the rate measured so far: a short probe first, then
    `remaining / rate`, so the overshoot is about one second of game time
    rather than a whole call's worth. Used only when the bridge does not serve
    the pause alarm (below); the 2026.12.0 smoke measured 25 days for an ask of
    10 from its 15 s probe alone.
  * `date_from_ordinal` — the inverse of `ordinal`, in the bridge's spelling
    (`%04d.%02d.%02d`): the target date of a game-day wait. Since L20-A a
    `game_days` wait hands that target to the bridge (`unpause_game
    {"pause_at": ...}`), whose game thread pauses on the first pass at or past
    it (bridge/include/pause_alarm.h); the reply stops `alarm`.
"""
from __future__ import annotations

import math
import os
from dataclasses import dataclass
from typing import Any, Mapping, Optional

CYCLE_GAME_DAYS_ENV = "CK3_CYCLE_GAME_DAYS"
HORIZON_END_ENV = "CK3_EPISODE_HORIZON_END"
#: 2026.12.4 (cell0 gaps 5+7, PLAN §P decision 1): the registered horizon START,
#: the same spelling the bridge's episode policy reads. Its presence is what
#: makes a seat's server a CELL seat (`cell_schedule`): every
#: `session(action="wait")` then returns at the next cycle boundary, start +
#: k x cadence, capped at the horizon end, whatever `seconds` it asked for.
#: Absent = the published wait, unchanged.
HORIZON_START_ENV = "CK3_EPISODE_HORIZON_START"
#: (v4, lane v4integ: the fixed 3,650-game-day ceiling on a `game_days` ask is
#: deleted. It was v3's ten-year horizon, and v4's horizon is declared per
#: registration (up to 150 game-years for the unsteered pilot). An ask is bounded
#: by the registered cadence when one is declared (`R_OVER_CADENCE`), by the
#: horizon (`R_HORIZON_REACHED`, the cell schedule) and by MAX_WAIT_CALLS.)
#: The first wait of a loop: long enough to measure the rate, short enough
#: that a fast clock does not overshoot a 90-day cycle in the probe.
PROBE_S = 15
#: Bounded, like every loop here: 40 × 110 s is ~73 min of wall time, past
#: which a clock that has not covered the ask is a stall, not a wait.
MAX_WAIT_CALLS = 40
#: Two consecutive waits with NO date progress end the loop: the game is not
#: running, and a third sleep would measure nothing new.
MAX_NO_PROGRESS = 2

MONTH_CUM_DAYS = (0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334)
DAYS_PER_YEAR = 365

R_OVER_CADENCE = "cycle_game_days_over_cadence"
R_CADENCE_UNREADABLE = "cycle_cadence_unreadable"
R_HORIZON_REACHED = "horizon_reached"
R_HORIZON_UNREADABLE = "episode_horizon_unreadable"
STOP_REACHED = "game_days_reached"
#: L20-A: the bridge's pause alarm fired (the stop the game thread made).
STOP_ALARM = "alarm"
#: The alarm was disarmed by a pause someone else wrote before it fired.
STOP_ALARM_CLEARED = "alarm_cleared"
#: get_tick_info stopped carrying the armed alarm (a bridge restart, a reload).
STOP_ALARM_LOST = "alarm_lost"
#: An alarm wait polls the bridge this often (seconds); the stop itself is the
#: game thread's, so this bounds only how late the server notices it.
ALARM_POLL_S = 1
#: A running clock whose date has not moved for this long is not running.
ALARM_NO_PROGRESS_S = MAX_NO_PROGRESS * PROBE_S
STOP_NO_PROGRESS = "clock_not_running"
STOP_MAX_CALLS = "max_calls"
#: A cell wait's two good ends: the cycle boundary, or the horizon end (which
#: caps the last cycle; the reply then carries `horizon_reached: true`).
STOP_CYCLE_END = "cycle_end"
STOP_HORIZON = "horizon"
R_CELL_SCHEDULE_UNREADABLE = "cell_schedule_unreadable"


def ordinal(date: Any) -> Optional[int]:
    """Game-days since year 0 (engine calendar), or None when `date` is not Y.M.D."""
    if not isinstance(date, str):
        return None
    parts = date.strip().split(".")
    if len(parts) != 3:
        return None
    try:
        y, m, d = (int(x) for x in parts)
    except ValueError:
        return None
    if not (1 <= m <= 12 and 1 <= d <= 31):
        return None
    return y * DAYS_PER_YEAR + MONTH_CUM_DAYS[m - 1] + (d - 1)


def date_from_ordinal(o: int) -> str:
    """The game date `o` game-days after year 0 (engine calendar), spelled as
    the bridge spells dates. `ordinal(date_from_ordinal(o)) == o` for o >= 0."""
    y, rem = divmod(int(o), DAYS_PER_YEAR)
    m = max(i for i in range(12) if MONTH_CUM_DAYS[i] <= rem)
    return f"{y:04d}.{m + 1:02d}.{rem - MONTH_CUM_DAYS[m] + 1:02d}"


def registered_cycle_game_days(environ: Optional[dict] = None) -> tuple[Optional[int], Optional[str]]:
    """(cadence, error). (None, None) when no cadence is registered in the env;
    (None, why) when the env carries something that is not a positive integer."""
    env = os.environ if environ is None else environ
    raw = env.get(CYCLE_GAME_DAYS_ENV)
    if raw is None or not str(raw).strip():
        return None, None
    try:
        v = int(str(raw).strip())
    except ValueError:
        return None, f"{CYCLE_GAME_DAYS_ENV}={raw!r} is not an integer"
    if v <= 0:
        return None, f"{CYCLE_GAME_DAYS_ENV}={raw!r} is not a positive number of game-days"
    return v, None


def registered_horizon_end(environ: Optional[dict] = None) -> tuple[Optional[int], Optional[str], Optional[str]]:
    """(ordinal, raw, error). (None, None, None) when no horizon is registered in
    the env; (None, raw, why) when the env carries something that is not Y.M.D."""
    env = os.environ if environ is None else environ
    raw = env.get(HORIZON_END_ENV)
    if raw is None or not str(raw).strip():
        return None, None, None
    o = ordinal(str(raw).strip())
    if o is None:
        return None, str(raw), f"{HORIZON_END_ENV}={raw!r} is not a game date (Y.M.D)"
    return o, str(raw).strip(), None


@dataclass(frozen=True)
class CellSchedule:
    """The cell's cycle boundaries on the game calendar: start + k x cadence
    (k >= 1), and the horizon end, which caps the last cycle. ONE schedule for
    every seat of a cell (they share the game clock, so they share the
    boundaries: a boundary anchored on each seat's own first wait would not)."""
    start: int
    end: int
    cadence: int

    def next_boundary(self, now: int) -> int:
        """The first boundary strictly after `now`, capped at the horizon end."""
        if now < self.start:
            return min(self.start + self.cadence, self.end)
        k = (now - self.start) // self.cadence + 1
        return min(self.start + k * self.cadence, self.end)

    @property
    def n_boundaries(self) -> int:
        """The boundaries of the whole horizon, the end's included: THE count of
        cycles a cell has (the runner's plan and v3 §7.2 read it here). 3,650
        game-days at 90 is 41: forty whole cycles and a last one of 50 days."""
        return self.cycle_index(self.end)

    def cycle_index(self, at: int) -> int:
        """Boundaries reached at `at`: 0 in the first cycle, 1 after the first
        boundary, ...; the horizon end counts as the last one."""
        if at <= self.start:
            return 0
        k = (min(at, self.end) - self.start) // self.cadence
        if at >= self.end and (self.end - self.start) % self.cadence:
            k += 1
        return k


def in_cell(environ: Optional[Mapping[str, str]] = None) -> bool:
    """An experiment cell's seat: HORIZON_START_ENV is set (and non-blank). The
    one predicate for the cell wait (`cell_schedule`) and the cell's refusal of
    session(speed) (`speed_registered`); `ck3agent run` mirrors it (cli.in_cell)."""
    env = os.environ if environ is None else environ
    return bool(str(env.get(HORIZON_START_ENV) or "").strip())


def cell_schedule(environ: Optional[Mapping[str, str]] = None
                  ) -> tuple[Optional[CellSchedule], Optional[str]]:
    """(schedule, error). (None, None) when HORIZON_START_ENV is absent: not a
    cell, the published wait. With it set, the cadence and the horizon end are
    REQUIRED (refused, never defaulted): (None, why) names what is missing."""
    env = os.environ if environ is None else environ
    if not in_cell(env):
        return None, None
    raw = env.get(HORIZON_START_ENV)
    start = ordinal(str(raw).strip())
    if start is None:
        return None, f"{HORIZON_START_ENV}={raw!r} is not a game date (Y.M.D)"
    cadence, c_err = registered_cycle_game_days(env)
    if c_err or cadence is None:
        return None, c_err or f"{HORIZON_START_ENV} is set and {CYCLE_GAME_DAYS_ENV} is not"
    end, _raw, e_err = registered_horizon_end(env)
    if e_err or end is None:
        return None, e_err or f"{HORIZON_START_ENV} is set and {HORIZON_END_ENV} is not"
    if end <= start:
        return None, f"{HORIZON_END_ENV} is not after {HORIZON_START_ENV}={raw!r}"
    return CellSchedule(start=start, end=end, cadence=cadence), None


#: `episode_cycle`'s why when there is no schedule at all (free play): the
#: callers decide what that means (the channel: the process's own counter; the
#: notepad and the guide log: no cycle).
CYCLE_NOT_A_CELL = "not_a_cell"


def episode_cycle(environ: Optional[Mapping[str, str]], game_date: Any
                  ) -> tuple[Optional[int], Optional[str]]:
    """THE EPISODE CYCLE at `game_date` (lane v4instr item 4a): `(index, None)`
    by the cell's one schedule (`cell_schedule(...).cycle_index`), or
    `(None, why)`: `CYCLE_NOT_A_CELL` when no schedule is declared, else what
    could not be read (the schedule, or the date). The one derivation every
    stamp reads: `channel.send_cycle` (a message's cycle) and
    `notepad.cycle_at` (a pad record's, and the guide log's) both call it, and
    test_cycle_authority checks that neither derives it another way."""
    sched, err = cell_schedule(environ)
    if sched is None and err is None:
        return None, CYCLE_NOT_A_CELL
    if err is not None:
        return None, f"the cell's schedule is unreadable: {err}"
    o = ordinal(game_date)
    if o is None:
        return None, f"the game date at the call ({game_date!r}) does not read as Y.M.D"
    return sched.cycle_index(o), None


#: `schedule_position`'s states: no cell (free play, nothing to say), a cell whose
#: schedule cannot be read (refused, named), a date that cannot be read, or read.
POS_NOT_A_CELL = "not_an_experiment_episode"
POS_SCHEDULE_UNREADABLE = R_CELL_SCHEDULE_UNREADABLE
POS_DATE_UNREAD = "game_date_unread"
POS_READ = "read"


def schedule_position(game_date: Any, environ: Optional[Mapping[str, str]] = None) -> dict:
    """Where `game_date` sits on the cell's schedule (v4 §6, lane v4sees): THE
    position both readers state, `observe`'s goal block and `ck3agent run`'s
    per-turn record. Every number comes from `cell_schedule` (the cycle
    authority the cell wait and the channel's cycle stamp use); none is
    re-derived. Keys, when `state` is `read`:

      * `cycle`: `CellSchedule.cycle_index(now)`, 0 in the first cycle (the
        index the cell wait's `cycle_index` and every ledger row carry);
      * `cycles_total`: `n_boundaries` (41 for ten years at 90 days);
      * `cycles_left`: `cycles_total - cycle` (the boundaries still to come,
        the horizon end's included);
      * `cycle_game_days`, `horizon_start`, `horizon_end` (the env's own
        spelling), `days_left` (game-days to the horizon end, never negative),
        `horizon_reached`.

    Not a cell: `{"state": "not_an_experiment_episode"}`, nothing else (no
    zeros). A cell whose schedule is unreadable or a date that is not Y.M.D:
    the state names it and carries no numbers."""
    env = os.environ if environ is None else environ
    sched, err = cell_schedule(env)
    if sched is None and err is None:
        return {"state": POS_NOT_A_CELL}
    if sched is None:
        return {"state": POS_SCHEDULE_UNREADABLE, "detail": err}
    now = ordinal(game_date)
    base = {"horizon_start": str(env.get(HORIZON_START_ENV)).strip(),
            "horizon_end": str(env.get(HORIZON_END_ENV)).strip(),
            "cycle_game_days": sched.cadence}
    if now is None:
        return {"state": POS_DATE_UNREAD, "game_date": game_date, **base}
    idx = sched.cycle_index(now)
    return {"state": POS_READ, "game_date": game_date, **base,
            "cycle": idx, "cycles_total": sched.n_boundaries,
            "cycles_left": max(0, sched.n_boundaries - idx),
            "days_left": max(0, sched.end - now), "horizon_reached": now >= sched.end}


def horizon_reached(game_date: Any, horizon_ordinal: Optional[int]) -> Optional[bool]:
    """True when `game_date` is at or past the registered horizon; False when
    before it; None when there is no horizon or the date cannot be read (a
    reader must not treat None as "not reached")."""
    if horizon_ordinal is None:
        return None
    o = ordinal(game_date)
    return None if o is None else o >= horizon_ordinal


def plan_wait(remaining_days: int, *, elapsed_s: float, advanced_days: int,
              cap_s: int, probe_s: int = PROBE_S) -> int:
    """Seconds for the next bounded wait. PURE.

    No measurement yet (nothing elapsed) → the probe. A measured rate → the
    seconds the remaining days should take at it, rounded up, clamped to
    [1, cap_s]. A measured ZERO rate (elapsed, nothing advanced) → the probe
    again, so a paused or stalled clock costs probes, not caps.
    """
    cap = max(1, int(cap_s))
    if elapsed_s <= 0 or advanced_days <= 0:
        return max(1, min(probe_s, cap))
    rate = advanced_days / elapsed_s                  # game-days per second
    want = math.ceil(max(remaining_days, 1) / rate)
    return max(1, min(want, cap))


# ── the cycle (v3 §5.2 / §7.2: one definition) ──────────────────────────────

def wait_completed(action: str, envelope: Any) -> bool:
    """Did this `session` reply WAIT? A boundary candidate is a wait that
    waited — the server slept and the clock was given the chance to run
    (`result.waited_s` is present: applied, pending or a rejected closing
    pause alike). A wait refused before anything was waited (the game did not
    unpause, the horizon was reached) is not one: counting it would let a
    seat reset its quota without time passing. Wall time is never consulted.
    """
    if action != "wait" or not isinstance(envelope, Mapping):
        return False
    result = envelope.get("result")
    return isinstance(result, Mapping) and "waited_s" in result


@dataclass
class CycleCounter:
    """This seat's cycle index: 0 before the first boundary. In-process by
    design — the sender's own server is the only process that makes the
    sender's waits.

    `cadence_days` set (the registered cadence, `CYCLE_GAME_DAYS_ENV`): the
    cycle began at the first wait's start date; it ends, and the index moves,
    when a wait's end date is ≥ `cadence_days` past that (several boundaries
    at once if one wait covered them). A wait whose dates cannot be read moves
    nothing: a cycle is a fact about the game date. `cadence_days` None: every
    wait that waited closes a cycle (free play, the public rule).
    """
    cadence_days: Optional[int] = None
    cycle: int = 0
    cycle_start_ordinal: Optional[int] = None

    @classmethod
    def from_env(cls, environ: Optional[Mapping[str, str]] = None) -> "CycleCounter":
        cadence, _err = registered_cycle_game_days(environ)
        return cls(cadence_days=cadence)

    def note(self, action: str, envelope: Any) -> int:
        if not wait_completed(action, envelope):
            return self.cycle
        idx = (envelope.get("result") or {}).get("cycle_index")
        if isinstance(idx, int) and not isinstance(idx, bool):
            # A cell wait (2026.12.4): the schedule's index is THE index, the
            # same for every seat of the cell; the counter never runs backwards.
            self.cycle = max(self.cycle, idx)
            return self.cycle
        if self.cadence_days is None:
            self.cycle += 1
            return self.cycle
        res = envelope.get("result") or {}
        start = ordinal(res.get("cycle_start_date") or res.get("start_date"))
        end = ordinal(res.get("end_date"))
        if start is None or end is None:
            return self.cycle
        if self.cycle_start_ordinal is None:
            self.cycle_start_ordinal = start
        while end - self.cycle_start_ordinal >= self.cadence_days:
            self.cycle += 1
            self.cycle_start_ordinal += self.cadence_days
        return self.cycle
