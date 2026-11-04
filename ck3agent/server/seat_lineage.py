"""seat_lineage.py — §7.5 of pre-registration v3: a seat is a LINEAGE.

When a seat's ruler dies the seat continues on the primary heir of the ruler's
primary title (the king's seat follows K's REGISTERED title, a duke's its own
primary title), under the same fragments; a lineage with no landed heir or a
destroyed title ends the seat as `lost`; a rebind that fails for an INSTRUMENT
reason is `seat_unbound`.

WHERE THIS LIVES. A seat's binding is owned by the seat's own MCP server
(`mcp_server._prebind_one` claims `CK3_AGENT_CHARACTER_ID` at connect), and
the bridge already moves a control session's claim to the heir on the tick
thread (`bridge_init.cpp check_controlled_handoff`: the cached highest-tier
title's current holder, with a skip-held-heir redirect). The driver's observer
client cannot claim on a seat's behalf. So the step runs HERE, in the seat's
server, at the top of every `observe` (the seat's cycle entry), and does three
things: it WITNESSES a handoff the bridge made (a row, source `bridge_handoff`),
it makes the rebind EXPLICITLY when the bridge could not (`set_controlled_character`
on the title's current holder, source `explicit_control`), and it ENDS the seat
`lost` when the title has no holder. Every row goes to the sink
`CK3_SEAT_LINEAGE_OUT` names; the driver lifts the sinks into the bundle's
`seat_rebinds.jsonl`.

INERT UNLESS ARMED. Without `CK3_SEAT_LINEAGE_OUT` nothing here runs: an
interactive session, `ck3agent run`, and the public product behave exactly as
before. The env is set by the arm launcher (`agent_launcher.AgentSession.start`)
beside `CK3_TOOL_SURFACE_OUT`.

The decision is a PURE function of four reads (`decide`), so every branch is
tested without a bridge; `step` only performs the reads and the write.
"""
from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass, field
from typing import Any, Optional

LINEAGE_OUT_ENV = "CK3_SEAT_LINEAGE_OUT"
#: K's registered title (§7.5 "the king's seat follows K's registered title").
#: Absent for a duke: its primary title is read while it lives.
LINEAGE_TITLE_ENV = "CK3_SEAT_LINEAGE_TITLE_ID"

KIND_REBIND = "rebind"
KIND_LOST = "lost"
KIND_UNBOUND = "seat_unbound"
KIND_PENDING = "pending"          # dead, succession not yet resolved by the engine
SOURCE_BRIDGE = "bridge_handoff"
SOURCE_EXPLICIT = "explicit_control"

R_HEIR_IS_PLAYER = "heir_is_player"
R_TITLE_READ_REFUSED = "title_read_refused"
R_CHARACTER_READ_REFUSED = "character_read_refused"
R_BIND_REFUSED = "bind_refused"
R_HOLDER_HELD_ELSEWHERE = "holder_held_by_another_seat"
R_SESSION_ROW_ABSENT = "session_row_absent"
R_RELEASED = "released_without_death"
R_NO_TITLE = "no_title_to_follow"


@dataclass
class Prior:
    """What this seat last knew: its ruler and the title the lineage follows."""
    ruler: int
    title_id: Optional[int]
    ended: bool = False
    pending_cycles: int = 0
    rows: list = field(default_factory=list)


@dataclass(frozen=True)
class Decision:
    kind: Optional[str]           # None = nothing to record (held / ended)
    old_id: int = 0
    new_id: int = 0
    title_id: Optional[int] = None
    source: Optional[str] = None
    reason: Optional[str] = None
    bind_target: int = 0          # >0: `step` must call set_controlled_character on it
    refresh_title: Optional[int] = None

    def row(self, *, seat_session_id: Optional[str], game_date: Any, tick: Any) -> dict:
        return {"seat_session_id": seat_session_id, "kind": self.kind,
                "old_id": self.old_id, "new_id": self.new_id,
                "title_id": self.title_id, "source": self.source, "reason": self.reason,
                "game_date": game_date if isinstance(game_date, str) else None,
                "tick": tick if isinstance(tick, int) else None,
                "written_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}


def _i(v: Any) -> int:
    return v if isinstance(v, int) and not isinstance(v, bool) else 0


def _alive(char_read: Any) -> Optional[bool]:
    """query_character's `alive` (build_char_profile), or None when the read answered nothing."""
    if not isinstance(char_read, dict) or _i(char_read.get("id")) <= 0:
        return None
    a = char_read.get("alive")
    return a if isinstance(a, bool) else None


def _primary_title(char_read: Any) -> Optional[int]:
    t = _i((char_read or {}).get("primary_title_id")) if isinstance(char_read, dict) else 0
    return t if t > 0 else None


def _holder(title_read: Any) -> Optional[int]:
    """The holder of the title `query_title_hierarchy(title_id=…)` describes.

    THE EMITTER'S SHAPE (query_handlers.cpp do_query_title_hierarchy): the
    title's own fields are NESTED under `title` (`r.key("title")` then
    `emit_title_info`, which writes `holder_id` only when the title has a
    holder), beside `sub_titles` / `sub_title_count`; a title the engine cannot
    find answers `{"error": "title_not_found", "title_id": …}` at the top
    level. So: 0 = the title exists and nobody holds it, or it is gone
    (destroyed, §7.5's `lost`); None = the read answered nothing.
    """
    if not isinstance(title_read, dict):
        return None
    if title_read.get("error") == "title_not_found":
        return 0
    title = title_read.get("title")
    if not isinstance(title, dict):
        return None
    return _i(title.get("holder_id"))


def decide(prior: Optional[Prior], row: Optional[dict], char_read: Any,
           title_read: Any, *, player_id: int = 0, held_elsewhere: frozenset = frozenset(),
           registered_title: Optional[int] = None) -> Decision:
    """ONE cycle's verdict. PURE.

    `row`        this seat's own session row (get_bridge_state.sessions[]):
                 primary_character_id, heir_character_id, unbound_reason.
    `char_read`  query_character of the ruler in question (prior's, or the row's).
    `title_read` query_title_hierarchy of the title the lineage follows.
    """
    if prior is not None and prior.ended:
        return Decision(None)
    if row is None:
        if prior is None:
            return Decision(None)
        return Decision(KIND_UNBOUND, prior.ruler, 0, prior.title_id, SOURCE_EXPLICIT,
                        R_SESSION_ROW_ABSENT)
    p = _i(row.get("primary_character_id"))
    heir = _i(row.get("heir_character_id"))
    unbound = row.get("unbound_reason") if isinstance(row.get("unbound_reason"), str) else ""
    if prior is None:
        # First sighting: learn the ruler and the title to follow. Nothing recorded.
        if p <= 0:
            return Decision(None)
        return Decision(None, refresh_title=registered_title or _primary_title(char_read))
    title = registered_title or prior.title_id
    if p == prior.ruler and p > 0:
        alive = _alive(char_read)
        if alive is True:
            # The duke's primary title may change while it lives; K's never does.
            return Decision(None, refresh_title=registered_title or _primary_title(char_read))
        if alive is None:
            return Decision(KIND_UNBOUND, p, 0, title, SOURCE_EXPLICIT, R_CHARACTER_READ_REFUSED)
        # DEAD and the bridge has not moved the claim: resolve through the title.
        return _through_title(prior, p, title, title_read, player_id, held_elsewhere)
    if p > 0 and p != prior.ruler:
        # The bridge moved this seat: a witnessed handoff (heir == new) or a swap.
        src = SOURCE_BRIDGE if heir == p else SOURCE_EXPLICIT
        return Decision(KIND_REBIND, prior.ruler, p, title, src,
                        None if heir == p else "primary_moved_without_heir_witness",
                        refresh_title=registered_title or _primary_title(char_read))
    # p == 0: the seat holds nothing.
    if unbound == R_HEIR_IS_PLAYER:
        return Decision(KIND_UNBOUND, prior.ruler, _i(row.get("unbound_heir_id")), title,
                        SOURCE_EXPLICIT, R_HEIR_IS_PLAYER)
    alive = _alive(char_read)
    if alive is False:
        return _through_title(prior, prior.ruler, title, title_read, player_id, held_elsewhere)
    if alive is None:
        return Decision(KIND_UNBOUND, prior.ruler, 0, title, SOURCE_EXPLICIT,
                        R_CHARACTER_READ_REFUSED)
    return Decision(KIND_UNBOUND, prior.ruler, 0, title, SOURCE_EXPLICIT, R_RELEASED)


def _through_title(prior: Prior, dead: int, title: Optional[int], title_read: Any,
                   player_id: int, held_elsewhere: frozenset) -> Decision:
    if not title:
        return Decision(KIND_LOST, dead, 0, None, SOURCE_EXPLICIT, R_NO_TITLE)
    holder = _holder(title_read)
    if holder is None:
        return Decision(KIND_UNBOUND, dead, 0, title, SOURCE_EXPLICIT, R_TITLE_READ_REFUSED)
    if holder == 0:
        return Decision(KIND_LOST, dead, 0, title, SOURCE_EXPLICIT, "title_has_no_holder")
    if holder == dead:
        return Decision(KIND_PENDING, dead, 0, title, SOURCE_EXPLICIT, "holder_still_the_dead_ruler")
    if player_id and (holder & 0xFFFFFF) == (player_id & 0xFFFFFF):
        return Decision(KIND_UNBOUND, dead, holder, title, SOURCE_EXPLICIT, R_HEIR_IS_PLAYER)
    if holder in held_elsewhere:
        return Decision(KIND_UNBOUND, dead, holder, title, SOURCE_EXPLICIT, R_HOLDER_HELD_ELSEWHERE)
    return Decision(KIND_REBIND, dead, holder, title, SOURCE_EXPLICIT, bind_target=holder)


def armed(environ: Optional[dict] = None) -> Optional[str]:
    env = os.environ if environ is None else environ
    out = env.get(LINEAGE_OUT_ENV)
    return out if out and not out.isspace() else None


def registered_title_from_env(environ: Optional[dict] = None) -> Optional[int]:
    env = os.environ if environ is None else environ
    raw = (env.get(LINEAGE_TITLE_ENV) or "").strip()
    try:
        v = int(raw)
    except ValueError:
        return None
    return v if v > 0 else None


def append_row(path: str, row: dict) -> bool:
    try:
        with open(os.path.expanduser(path), "a", encoding="utf-8") as fh:
            fh.write(json.dumps(row, sort_keys=True) + "\n")
        return True
    except OSError:
        return False


async def step(gw, prior: Optional[Prior], *, sink: str, seat_session_id: Optional[str],
               registered_title: Optional[int] = None, own_session_id=None,
               own_session_row=None) -> Prior:
    """One cycle: the reads, `decide`, the bind when needed, the row. Never raises.

    `own_session_id` / `own_session_row` are redesign's helpers, injected so
    this module imports nothing from redesign (which imports mcp_server's state).
    """
    try:
        bs = await gw.read("get_bridge_state", {})
        sid = own_session_id(gw) if own_session_id else None
        row = own_session_row(bs, sid) if own_session_row else None
        p = _i((row or {}).get("primary_character_id")) if isinstance(row, dict) else 0
        subject = p if (prior is None or p > 0) else prior.ruler
        char_read = await gw.read("query_character", {"character_id": subject}) if subject else None
        title = registered_title or (prior.title_id if prior else None)
        title_read = None
        if prior is not None and title and (p == 0 or (p == prior.ruler and _alive(char_read) is False)):
            title_read = await gw.read("query_title_hierarchy", {"title_id": title})
        player = _i((bs or {}).get("player_character_id")) if isinstance(bs, dict) else 0
        held = frozenset(
            _i(c) for r in ((bs or {}).get("sessions") or []) if isinstance(r, dict)
            and r.get("session_id") != sid for c in (r.get("claimed_character_ids") or []))
        d = decide(prior, row if isinstance(row, dict) else None, char_read, title_read,
                   player_id=player, held_elsewhere=held, registered_title=registered_title)
        game = gw.game() if callable(getattr(gw, "game", None)) else {}
        date, tick = (game or {}).get("date"), (game or {}).get("tick")
        if d.bind_target:
            res = await gw.write("set_controlled_character",
                                 {"character_id": d.bind_target, "disable_native_ai": False})
            ok = isinstance(res, dict) and res.get("success", True) and not res.get("reason")
            if not ok:
                d = Decision(KIND_UNBOUND, d.old_id, d.bind_target, d.title_id, SOURCE_EXPLICIT,
                             f"{R_BIND_REFUSED}:{(res or {}).get('reason') if isinstance(res, dict) else 'no_reply'}")
        if prior is None:
            if p > 0:
                prior = Prior(ruler=p, title_id=d.refresh_title)
            return prior if prior is not None else Prior(ruler=0, title_id=None)
        if d.kind is not None:
            rowd = d.row(seat_session_id=seat_session_id, game_date=date, tick=tick)
            append_row(sink, rowd)
            prior.rows.append(rowd)
        if d.kind == KIND_REBIND:
            prior.ruler, prior.title_id = d.new_id, (registered_title or d.refresh_title or d.title_id)
            prior.pending_cycles = 0
        elif d.kind in (KIND_LOST, KIND_UNBOUND):
            prior.ended = True
        elif d.kind == KIND_PENDING:
            prior.pending_cycles += 1
        elif d.refresh_title:
            prior.title_id = d.refresh_title
        return prior
    except Exception as exc:                                        # noqa: BLE001
        try:
            append_row(sink, {"seat_session_id": seat_session_id, "kind": KIND_UNBOUND,
                              "old_id": prior.ruler if prior else 0, "new_id": 0,
                              "title_id": prior.title_id if prior else None,
                              "source": SOURCE_EXPLICIT, "reason": f"step_failed:{type(exc).__name__}",
                              "game_date": None, "tick": None,
                              "written_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())})
        except Exception:                                           # noqa: BLE001
            pass
        return prior if prior is not None else Prior(ruler=0, title_id=None)
