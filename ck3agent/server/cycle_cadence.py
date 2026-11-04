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
it), the launcher (`episode_driver.seat_fleet_environment` sets the env from
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
#: §7.2's horizon is 3,650 game-days; an ask beyond one horizon is not a cycle.
HARD_GAME_DAYS_CEILING = 3650
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
