"""goal_block.py — what an experiment seat's `observe` says about the goal (v4 §6, lane v4sees).

THE DEFECT (seat review of set v3-registered-2, problem 5). The goal was not
operational in anything a seat could observe: no score, no end date, no cycle
count. s3's king read the de-jure fraction as realm size ("42 of 47") while the
meter read 44, down from 89.

THE BLOCK. `observe` carries `result.goal` (first in `result`):

  * `R`: the county count of the realm of the PLAYED LINE, now and at the open,
    by THE meter's definition (scripts/meter_lane.py `G_DEFINITION`):
      - the line is the bridge's decision (`query_character(character_id=<line
        start>, played_line=1)`, `emit_played_line`), read through
        `seat_lineage.read_watch`, the shipped checked mirror of
        scripts/played_line.py `read_watch`; this module re-derives none of it;
      - R is read only while the watch says `following` and the line character
        read alive (`read_alive`, a mirror of played_line.read_alive);
      - R is `query_realm(character_id=<line>).realm_total_counties`, graded by
        `grade_line_realm`, a mirror of meter_lane.grade_line_realm (a living,
        readable, landless line character reads 0, measured);
      - both mirrors are checked reply by reply in scripts/test_played_line.py;
      - R at the open is NOT re-read: it is the meter's own open sample, which
        the cell passes to the seat (`ENV_R_OPEN`, `ENV_OPEN_DATE`,
        `ENV_LINE_START`; ck3bench/runner/cell.py `goal_open_env`).
  * `horizon` and `cycle`: `cycle_cadence.schedule_position`, the cycle
    authority (`cell_schedule`); nothing re-derived.
  * `own_counties` (a duke seat only): his own realm's county count, the same
    read and grade as R applied to the seat's own ruler.

OUTSIDE AN EXPERIMENT EPISODE (no horizon start declared) the block is
`{"state": "not_an_experiment_episode"}`: stated absent, never zeros.

Keys read, by emitter (bridge/src/query_handlers.cpp): `query_character`
build_char_profile `id` (:496) and `alive` (:513); `emit_played_line` (:969-998);
`do_query_realm` `character_id` (:1469), `has_realm` (:1476 false / :1484 true),
`realm_total_counties` (:1540), `realm_total_counties_unresolved` (:1543, only
when > 0).
"""
from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Any, Mapping, Optional

try:                                    # imported as ck3agent.server.goal_block (scripts)
    from . import cycle_cadence as _cc
    from . import seat_lineage as _sl
except ImportError:                     # imported flat (the server)
    import cycle_cadence as _cc         # type: ignore[no-redef]
    import seat_lineage as _sl          # type: ignore[no-redef]

#: The cell's open sample, handed to every seat (one spelling: the cell imports these).
ENV_LINE_START = "CK3_EPISODE_LINE_START_ID"
ENV_R_OPEN = "CK3_EPISODE_R_OPEN"
ENV_OPEN_DATE = "CK3_EPISODE_OPEN_DATE"
#: Which seat this process is (channel.ENV_SEAT; mirrored, checked by test).
ENV_SEAT = "CK3_SEAT_LABEL"
KING = "K"

ID_MASK = 0xFFFFFF

# ── the meter's grade, mirrored (scripts/meter_lane.py; checked) ───────────
STATUS_OK = "ok"
STATUS_PARTIAL = "partial"
STATUS_UNAVAILABLE = "unavailable"
REALM_LANDLESS = "line_character_holds_no_realm"
REALM_NO_REALM_UNATTRIBUTED = "holder_has_no_realm"

R_DEFINITION = ("R = query_realm(line).realm_total_counties, line = the played line (the registered "
                "title's holder at the open; him while he lives; at his death the heir the bridge's "
                "line watch decides); 0 when the line character is alive and holds no realm. The "
                "score is R at the horizon end minus R at the open. Not the de-jure block.")

# ── block states ────────────────────────────────────────────────────────────
STATE_PRESENT = "present"
R_MEASURED = "measured"
R_UNREAD = "unread"
#: why R (or a duke's own count) was not read; each names the leg
WHY_NO_LINE_START = "line_start_undeclared"
WHY_LINE_PENDING = "line_successor_pending"
WHY_CHAR_UNREAD = "line_character_unread"
WHY_CHAR_DEAD = "line_character_read_dead"
WHY_REALM = "realm_read"
WHY_NO_ACTOR = "seat_ruler_unknown"


@dataclass
class RealmRead:
    status: str
    detail: str = ""
    total: Any = ""
    unresolved: Any = ""


def _refusal(reply: Any) -> Optional[str]:
    if not isinstance(reply, dict):
        return "response_not_an_object"
    for key in ("error", "reason"):
        if reply.get(key):
            return str(reply[key])
    if reply.get("success") is False:
        return "success_false"
    return None


def _int(v: Any) -> Optional[int]:
    return v if isinstance(v, int) and not isinstance(v, bool) else None


def read_alive(reply: Any, character_id: int) -> tuple:
    """MIRROR of scripts/played_line.py `read_alive`."""
    why = _refusal(reply)
    if why is not None:
        return None, why
    alive = reply.get("alive")
    if not isinstance(alive, bool):
        return None, "alive_absent"
    rid = _int(reply.get("id"))
    if rid is None or (rid & ID_MASK) != (character_id & ID_MASK):
        return None, "profile_id_mismatch"
    return alive, ""


def grade_realm(reply: Any) -> RealmRead:
    """MIRROR of scripts/meter_lane.py `grade_realm`."""
    refusal = _refusal(reply) if isinstance(reply, dict) else "response_not_an_object"
    if refusal is not None:
        return RealmRead(STATUS_UNAVAILABLE, f"realm_refused:{refusal}")
    if reply.get("has_realm") is False:
        return RealmRead(STATUS_PARTIAL, "holder_has_no_realm")
    total = reply.get("realm_total_counties")
    if not isinstance(total, int):
        return RealmRead(STATUS_PARTIAL, "realm_total_counties_absent")
    unresolved = reply.get("realm_total_counties_unresolved")
    if unresolved is None:
        unresolved = 0            # the emitter omits the key at 0
    elif not isinstance(unresolved, int):
        return RealmRead(STATUS_PARTIAL, "realm_total_counties_unresolved_not_an_integer")
    return RealmRead(STATUS_OK, "", total=total, unresolved=unresolved)


def grade_line_realm(reply: Any, character_id: int) -> RealmRead:
    """MIRROR of scripts/meter_lane.py `grade_line_realm` (owner ruling
    2026-11-11: alive, readable and landless is a real zero)."""
    if _refusal(reply) is None and reply.get("has_realm") is False:
        rid = reply.get("character_id")
        if not isinstance(rid, int) or isinstance(rid, bool):
            return RealmRead(STATUS_PARTIAL, f"{REALM_NO_REALM_UNATTRIBUTED}:character_id_absent")
        if (rid & ID_MASK) != (int(character_id) & ID_MASK):
            return RealmRead(STATUS_PARTIAL, f"{REALM_NO_REALM_UNATTRIBUTED}:character_id_mismatch")
        return RealmRead(STATUS_OK, REALM_LANDLESS, total=0, unresolved=0)
    return grade_realm(reply)


def _count(rr: RealmRead) -> dict:
    if rr.status == STATUS_OK:
        out = {"read": R_MEASURED, "count": rr.total}
        if rr.unresolved:
            out["unresolved"] = rr.unresolved
        if rr.detail:
            out["detail"] = rr.detail
        return out
    return {"read": R_UNREAD, "count": None, "why": WHY_REALM, "detail": f"{rr.status}:{rr.detail}"}


def _env_int(env: Mapping[str, str], key: str) -> Optional[int]:
    raw = str(env.get(key) or "").strip()
    try:
        return int(raw) if raw else None
    except ValueError:
        return None


async def read_line_R(gw, start_id: int) -> dict:
    """R now: the watch's line, his life, his realm. Every failure names its leg."""
    reply = await gw.read("query_character", _sl.watch_request(start_id))
    watch, why_not = _sl.read_watch(reply)
    if watch is None:
        return {"read": R_UNREAD, "count": None, "why": _sl.R_LINE_WATCH_UNREADABLE, "detail": why_not}
    line = _int(watch.get("line_character_id")) & ID_MASK
    base = {"line_character_id": line, "line_status": watch.get("status")}
    if watch.get("armed_this_read") is True:
        base["watch_armed_by_this_read"] = True       # the meter had not armed it: worth seeing
    if watch.get("status") == _sl.WATCH_PENDING:
        return {"read": R_UNREAD, "count": None, "why": WHY_LINE_PENDING,
                "detail": str(watch.get("pending_reason") or ""), **base}
    profile = reply if line == (start_id & ID_MASK) else \
        await gw.read("query_character", {"character_id": line})
    alive, why = read_alive(profile, line)
    if alive is None:
        return {"read": R_UNREAD, "count": None, "why": WHY_CHAR_UNREAD, "detail": why, **base}
    if not alive:
        return {"read": R_UNREAD, "count": None, "why": WHY_CHAR_DEAD, **base}
    realm = await gw.read("query_realm", {"character_id": line})
    return {**_count(grade_line_realm(realm, line)), **base}


async def goal_block(gw, *, actor_id: Optional[int], game_date: Any,
                     environ: Optional[Mapping[str, str]] = None) -> dict:
    """`result.goal` for this seat (see the module docstring)."""
    env = os.environ if environ is None else environ
    pos = _cc.schedule_position(game_date, env)
    if pos["state"] == _cc.POS_NOT_A_CELL:
        return {"state": _cc.POS_NOT_A_CELL}
    if pos["state"] == _cc.POS_SCHEDULE_UNREADABLE:
        return {"state": pos["state"], "detail": pos.get("detail")}
    seat = str(env.get(ENV_SEAT) or "").strip() or None
    out: dict = {"state": STATE_PRESENT, "seat": seat}
    horizon = {"end_date": pos["horizon_end"]}
    cycle: dict = {"game_days": pos["cycle_game_days"]}
    if pos["state"] == _cc.POS_READ:
        horizon.update(days_left=pos["days_left"], reached=pos["horizon_reached"])
        cycle.update(index=pos["cycle"], total=pos["cycles_total"], left=pos["cycles_left"])
    else:
        horizon.update(days_left=None, why=pos["state"])
        cycle.update(index=None, total=None, left=None, why=pos["state"])
    start = _env_int(env, ENV_LINE_START)
    r_open = _env_int(env, ENV_R_OPEN)
    if start is None:
        R = {"read": R_UNREAD, "count": None, "why": WHY_NO_LINE_START,
             "detail": f"{ENV_LINE_START} is not set: the cell passed no open sample"}
    else:
        R = await read_line_R(gw, start)
    R = {"now": R.pop("count"), **R,
         "at_open": r_open, "open_date": str(env.get(ENV_OPEN_DATE) or "").strip() or None,
         "line_start_id": start}
    R["change"] = (R["now"] - r_open) if isinstance(R["now"], int) and r_open is not None else None
    if r_open is None:
        R["at_open_why"] = f"{ENV_R_OPEN} is not set: the cell passed no open sample"
    R["definition"] = R_DEFINITION
    out.update(R=R, horizon=horizon, cycle=cycle)
    if seat != KING:
        if not isinstance(actor_id, int) or actor_id <= 0:
            out["own_counties"] = {"read": R_UNREAD, "count": None, "why": WHY_NO_ACTOR}
        else:
            realm = await gw.read("query_realm", {"character_id": actor_id})
            out["own_counties"] = {"character_id": actor_id,
                                   **_count(grade_line_realm(realm, actor_id))}
    return out


def open_env(sample: Mapping[str, Any]) -> dict[str, str]:
    """The env the cell hands each seat from the meter's open sample (the row
    `meter_lane.realm_ok_sample` accepted): line start, R at the open, its date."""
    return {ENV_LINE_START: str(int(sample["line_character_id"])),
            ENV_R_OPEN: str(int(sample["realm_total_counties"])),
            ENV_OPEN_DATE: str(sample["game_date"])}
