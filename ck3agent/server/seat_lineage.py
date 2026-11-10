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
it writes a `bound` row at the first step that holds a ruler (2026.12.4: a
seat whose ruler never dies is witnessed, not absent), it makes the rebind EXPLICITLY when the bridge could not (`set_controlled_character`
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
import logging
import os
import time
from dataclasses import dataclass, field
from typing import Any, Optional

_log = logging.getLogger("ck3-mcp.seat_lineage")

LINEAGE_OUT_ENV = "CK3_SEAT_LINEAGE_OUT"
#: K's registered title (§7.5 "the king's seat follows K's registered title").
#: Absent for a duke: its primary title is read while it lives.
LINEAGE_TITLE_ENV = "CK3_SEAT_LINEAGE_TITLE_ID"
#: The seat's session id, written into every row as `seat_session_id`. THE
#: spelling: mcp_server reads it here, `scripts/dry_cell.py` and
#: `scripts/agent_launcher.py` set it, and `ck3agent run` (cli.SESSION_ID_ENV,
#: the shipped mirror) sets it; test_the_runs_env_spellings_are_the_servers and
#: scripts/test_dry_cell.py check the mirrors.
SESSION_ID_ENV = "CK3_SESSION_ID"

#: The first step that holds a ruler (2026.12.4, cell0 gap 2): the seat's
#: opening witness. Without it a seat whose ruler never dies wrote no row, so its
#: sink never existed and the bundle read it `unwitnessed` (cell0: n_at_end 0).
KIND_BOUND = "bound"
KIND_REBIND = "rebind"
KIND_LOST = "lost"
KIND_UNBOUND = "seat_unbound"
KIND_PENDING = "pending"          # dead, succession not yet resolved by the engine
#: 2026.12.6 (owner rulings 2026-11-10): NON-TERMINAL notes. `read_failed` is a
#: step that gave no verdict because a read failed (`read_failed`, below); it
#: carries the read, why, and how many such steps are consecutive. The first
#: verdict-bearing step after them writes `reads_recovered`. The sink carries
#: them so a resumed seat's NEW server (prior_from_sink) keeps the count: a
#: seat's server often makes only one or two observes before its claude ends,
#: so a count that reset with each process could never reach the bound.
KIND_READ_FAILED = "read_failed"
KIND_READS_RECOVERED = "reads_recovered"
#: 2026.12.7 (owner ruling 2026-11-10; v3pilot5 witness): a NON-TERMINAL note.
#: The seat asked to claim a character the cell reserved for another seat (or
#: while the reservation could not be read) and the server refused it before
#: any bridge call (ck3agent/server/seat_reservations.py). The row names the
#: attempting seat and the target (`character_id`) so the bundle witnesses the
#: attempt: in v3pilot5 the king's takes showed only through its held_at_stop
#: and the dukes' owned_by_other. It changes no status, no ruler and no
#: failed-read count, and it is not part of the lineage a resumed server seeds
#: from (`prior_from_sink` skips it).
KIND_CLAIM_REFUSED = "claim_refused"
#: 2026.12.7 (witness r1 F1 on 1eaab8ac): a NON-TERMINAL note beside a `bound`
#: or `rebind` row whose ruler is another seat's DECLARED character (in this
#: seat's CK3_RESERVED_FOR_OTHER_SEATS). The lineage rebind, the bridge's heir
#: handoff and a continuation's pre-bind of carried rulers are not refused by
#: the reservations (only session add/control are); a seat that comes to hold
#: another seat's ruler that way is witnessed here, not silent. The rebind row
#: alone names the ruler but not that he is reserved. Changes nothing else.
KIND_HOLDS_RESERVED = "holds_reserved"
#: The notes that are not part of the lineage a resumed server seeds from.
_NOT_LINEAGE = (KIND_CLAIM_REFUSED, KIND_HOLDS_RESERVED)
SOURCE_BRIDGE = "bridge_handoff"
SOURCE_EXPLICIT = "explicit_control"

R_HEIR_IS_PLAYER = "heir_is_player"
R_TITLE_READ_REFUSED = "title_read_refused"
R_CHARACTER_READ_REFUSED = "character_read_refused"
R_BIND_REFUSED = "bind_refused"
R_HOLDER_HELD_ELSEWHERE = "holder_held_by_another_seat"
R_SESSION_ROW_ABSENT = "session_row_absent"
#: 2026.12.6 (owner, verbatim: "If there are something like multiple timeouts in
#: a row, that might be fair to call an issue."): the bound on failed reads. The
#: MAX_CONSECUTIVE_FAILED_READS-th consecutive failed step ends the seat
#: `seat_unbound` with this reason, DATED at the first of them (game date and
#: tick; `first_failed_utc` beside), so the unbound span runs from when the seat
#: stopped seeing.
R_READS_FAILED_REPEATEDLY = "reads_failed_repeatedly"
MAX_CONSECUTIVE_FAILED_READS = 3
#: TK6 witness r2 item 3: the seat's own session row was READ and holds no
#: ruler, MAX_CONSECUTIVE_FAILED_READS observes in a row, before the seat ever
#: had one (a prebind that never landed). Counted with the failed reads; the
#: terminal says `never_bound` when every step of the run was this, and
#: `reads_failed_repeatedly` when any read in it failed.
NOT_BOUND_YET = "not_bound_yet"
R_NEVER_BOUND = "never_bound"
R_RELEASED = "released_without_death"
#: a rebind this seat's server records when the primary moved with NO heir witness
#: (heir != the new primary): a swap, not a lineage rebind (v3 §7.5 as amended by the
#: owner's ruling 2026-11-09). scripts/seat_lineage_record.py imports this one name.
R_PRIMARY_MOVED = "primary_moved_without_heir_witness"
R_NO_TITLE = "no_title_to_follow"


@dataclass
class Prior:
    """What this seat last knew: its ruler and the title the lineage follows."""
    ruler: int
    title_id: Optional[int]
    ended: bool = False
    pending_cycles: int = 0
    rows: list = field(default_factory=list)
    #: 2026.12.6: consecutive steps with a failed read (no verdict), and the
    #: first of them (game_date, tick, utc); rebuilt from the sink by a new server.
    consecutive_failed: int = 0
    first_failed: Optional[dict] = None
    #: whether every step of the current run was NOT_BOUND_YET (no read failed)
    run_only_not_bound: bool = True


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
                        None if heir == p else R_PRIMARY_MOVED,
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


def session_rows(reply: Any) -> Optional[list]:
    """`get_bridge_state`'s session list, or None when the reply carries none.

    2026.12.6 (v3 pilot flat_hom-3: D4 and D5 written `seat_unbound
    session_row_absent` at 867.10.22/23 while both held their rulers on the same
    bridge session to the horizon). A read that did not deliver the list says
    nothing about the seat's row: the gateway answers `{}` for a read that timed
    out or lost its connection and `{"reason": "bridge_disconnected"}` when hung
    (mcp_server.BridgeGateway._dispatch), and the bridge answers a faulted read
    `{"error": "memory_access_fault"}` (bridge.h MEMORY_ACCESS_FAULT_REPLY). The
    bridge builds the list from its registry under one lock, detached and lost
    sessions included (session.cpp sessions_json), so only a reply that CARRIES
    the list can say a session is absent. The same rule as the CLI's
    `_bridge._session_rows` (test_seat_lineage checks the two agree); the
    step's failed-read predicate for all three reads is `read_failed`."""
    rows = reply.get("sessions") if isinstance(reply, dict) else None
    if not isinstance(rows, list):
        return None
    return [r for r in rows if isinstance(r, dict)]


#: THE FAILED-READ CODES, defined ONCE (TK6 witness r1 item 1). The server's
#: gateway and its listener spell their own answers with these constants
#: (mcp_server.BridgeGateway._dispatch, mcp_server._bridge_listener), so a new
#: spelling cannot slip past the predicate below:
#:   BRIDGE_DISCONNECTED   the gateway with no connection ({"reason": ...}) and
#:                         the listener failing a read IN FLIGHT when the
#:                         connection drops ({"error": ...})
#:   GAME_THREAD_TIMEOUT   ipc_server.cpp serve_on_game_thread ("the game-thread
#:                         drain did not run this read in time ... Nothing was read.")
#:   MEMORY_ACCESS_FAULT   bridge.h MEMORY_ACCESS_FAULT_REPLY (the read faulted)
BRIDGE_DISCONNECTED = "bridge_disconnected"
GAME_THREAD_TIMEOUT = "game_thread_timeout"
MEMORY_ACCESS_FAULT = "memory_access_fault"
FAILED_READ_CODES = frozenset({BRIDGE_DISCONNECTED, GAME_THREAD_TIMEOUT, MEMORY_ACCESS_FAULT})
#: `"degraded": true` marks a reply one of whose memory reads faulted. Where it
#: is set (TK6 witness r2 item 1):
#:   query_handlers.cpp:972         emit_degraded on a FULL query_character
#:                                  profile (id, alive, ...): it "stands
#:                                  without" the faulted fields
#:                                  (action_dispatch_internal.h:157)
#:   action_dispatch_internal.h:213-215  FaultScope -> mark_reply_degraded on any
#:                                  reply of get_bridge_state
#:                                  (dispatch_router.cpp:1650) and of
#:                                  query_title_hierarchy (query_handlers.cpp:3006)
#:   dispatch_router.cpp:2943       the {"success": false, "action": ...} fill-in
#:                                  when the handler returned false after a fault
#: So degraded is a FAILED read only when the reply also lacks the field the
#: lineage decides on (LOAD_BEARING): the session list; a character's `alive`
#: (and a degraded success:false is the lookup itself faulting); a title's
#: holder (a degraded title_not_found is lookup_title_obj faulting). A degraded
#: reply that carries the field is the bridge's answer and is decided on.
DEGRADED = "degraded"
READ_SESSIONS = "get_bridge_state"
READ_CHARACTER = "query_character"
READ_TITLE = "query_title_hierarchy"


def _load_bearing_missing(reply: dict, read: Optional[str]) -> bool:
    """Whether `reply` lacks the field the step decides on, for `read`."""
    if read == READ_SESSIONS:
        return session_rows(reply) is None
    if read == READ_CHARACTER:
        # `alive` is always written by build_char_profile, so a profile whose id
        # read faulted to 0 still carries it: the id is load-bearing too (TK6
        # witness r3 item 3; _alive needs id > 0)
        return reply.get("success") is False or not isinstance(reply.get("alive"), bool) \
            or _i(reply.get("id")) <= 0
    if read == READ_TITLE:
        title = reply.get("title")
        return reply.get("error") == "title_not_found" or not (
            isinstance(title, dict) and "holder_id" in title)
    return True                         # an unnamed read: no field is known to stand


def read_failed(reply: Any, read: Optional[str] = None) -> Optional[str]:
    """Why `reply` to `read` is a FAILED read (no verdict, counted toward the
    bound), or None when the bridge answered.

    2026.12.6 (owner rulings 2026-11-10: "a failed or list-less read is
    retried, not counted as an unbind", bounded at MAX_CONSECUTIVE_FAILED_READS).
    ONE predicate for the step's three reads (READ_SESSIONS, READ_CHARACTER,
    READ_TITLE):
      * nothing came back: None, not an object, or `{}` (the gateway's answer to
        a read that timed out or lost its connection, BridgeGateway._dispatch;
        a bridge reply is never `{}`: ipc_server injects request_id, tick and
        game_date into every reply);
      * a FAILED_READ_CODES code under `error` or `reason`;
      * for the session read, no session list;
      * `"degraded": true` AND the read's load-bearing field missing (DEGRADED).
    Anything else is a READ: for query_character a refusal is a non-degraded
    `{"success": false, "action": "query_character"}` (do_query_character
    returned false: dispatch_router.cpp dispatch_action_with_response fills it
    in), the signed `character_read_refused`; for query_title_hierarchy a
    non-degraded `{"error": "title_not_found"}` is the title gone (`lost`)."""
    if reply is None:
        return "no_reply"
    if not isinstance(reply, dict):
        return "not_an_object"
    if not reply:
        return "empty_reply"
    for key in ("error", "reason"):
        code = reply.get(key)
        if isinstance(code, str) and code in FAILED_READ_CODES:
            return code
    if read == READ_SESSIONS and session_rows(reply) is None:
        return "no_session_list"
    if reply.get(DEGRADED) is True and _load_bearing_missing(reply, read):
        return DEGRADED
    return None


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


def prior_from_sink(path: Optional[str]) -> Optional[Prior]:
    """The seat's Prior as its sink left it, or None when the sink holds no row.

    2026.12.4 (L23 witness r2 item 1): a cell's `ck3agent run` resumes a seat in
    a NEW claude, so a new server whose in-memory Prior starts at None. Seeded
    from the sink, the resumed server knows the seat's lineage: the last
    `bound`/`rebind` row's ruler and title (a dead original is not the ruler
    any more), an ended seat (`lost`, `seat_unbound`) stays ended, and the
    first step writes no second `bound` row."""
    if not path:
        return None
    rows: list = []
    try:
        with open(os.path.expanduser(path), encoding="utf-8") as fh:
            for line in fh:
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                if isinstance(r, dict) and r.get("kind") not in _NOT_LINEAGE:
                    rows.append(r)
    except OSError:
        return None
    if not rows:
        return None
    prior = Prior(ruler=0, title_id=None, rows=rows)
    for r in rows:
        k = r.get("kind")
        if k in (KIND_BOUND, KIND_REBIND):
            prior.ruler = _i(r.get("new_id"))
            prior.title_id = r.get("title_id") if isinstance(r.get("title_id"), int) else prior.title_id
            prior.pending_cycles = 0
        elif k in (KIND_LOST, KIND_UNBOUND):
            prior.ended = True
        elif k == KIND_PENDING:
            prior.pending_cycles += 1
        if k == KIND_READ_FAILED:
            prior.consecutive_failed += 1
            if prior.consecutive_failed == 1:
                prior.first_failed = {"game_date": r.get("game_date"), "tick": r.get("tick"),
                                      "utc": r.get("written_utc")}
                prior.run_only_not_bound = True
            if not str(r.get("reason") or "").endswith(":" + NOT_BOUND_YET):
                prior.run_only_not_bound = False
        else:                       # any other row is a step that gave a verdict
            prior.consecutive_failed, prior.first_failed = 0, None
            prior.run_only_not_bound = True
    return prior


def append_row(path: str, row: dict) -> bool:
    try:
        with open(os.path.expanduser(path), "a", encoding="utf-8") as fh:
            fh.write(json.dumps(row, sort_keys=True) + "\n")
        return True
    except OSError:
        return False


def _verdict_rows(rows: list) -> list:
    """The rows that are a lineage's verdicts: every row but the non-terminal
    notes (KIND_READ_FAILED, KIND_READS_RECOVERED)."""
    return [r for r in rows if not (isinstance(r, dict)
                                    and r.get("kind") in (KIND_READ_FAILED, KIND_READS_RECOVERED)
                                    + _NOT_LINEAGE)]


def _reserved_ids() -> frozenset:
    """This seat's reservations (empty when none or unreadable)."""
    try:
        from . import seat_reservations as _resv               # imported as ck3agent.server.*
    except ImportError:
        import seat_reservations as _resv                      # imported from the server dir
    return _resv.read().ids


def _note_if_reserved(sink: str, rowd: dict) -> Optional[dict]:
    """After a bound/rebind row: the KIND_HOLDS_RESERVED note when its ruler is
    another seat's declared character. Never raises."""
    try:
        cid = _i(rowd.get("new_id"))
        if cid <= 0 or cid not in _reserved_ids():
            return None
        note = {"seat_session_id": rowd.get("seat_session_id"), "kind": KIND_HOLDS_RESERVED,
                "character_id": cid, "via_kind": rowd.get("kind"), "via_source": rowd.get("source"),
                "old_id": rowd.get("old_id"), "game_date": rowd.get("game_date"),
                "tick": rowd.get("tick"),
                "written_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        append_row(sink, note)
        _log.warning("seat lineage: this seat now holds %d, another seat's declared ruler (%s)",
                     cid, rowd.get("kind"))
        return note
    except Exception:                                          # noqa: BLE001
        return None


def record_claim_refused(sink: Optional[str], *, seat_session_id: Optional[str],
                         seat_label: Optional[str], character_id: Any, action: str,
                         reason: str, game: Optional[dict] = None) -> Optional[dict]:
    """Append the KIND_CLAIM_REFUSED note for one refused claim; the row, or
    None when the sink is not armed (nothing is written outside a cell)."""
    if not sink:
        return None
    g = game if isinstance(game, dict) else {}
    row = {"seat_session_id": seat_session_id, "seat_label": seat_label,
           "kind": KIND_CLAIM_REFUSED, "action": action, "character_id": _i(character_id),
           "reason": reason, "game_date": g.get("date"), "tick": g.get("tick"),
           "written_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    if not append_row(sink, row):
        _log.error("seat lineage: could not write the claim_refused row to %s", sink)
    return row


def _game_of(gw) -> dict:
    try:
        g = gw.game() if callable(getattr(gw, "game", None)) else {}
    except Exception:                                          # noqa: BLE001
        g = {}
    return g if isinstance(g, dict) else {}


def _no_verdict(prior: Prior, read: str, why: Optional[str], *, gw, sink: str,
                seat_session_id: Optional[str]) -> Prior:
    """A failed read (2026.12.6): a non-terminal `read_failed` row; on the
    MAX_CONSECUTIVE_FAILED_READS-th in a row, the terminal `seat_unbound
    reads_failed_repeatedly`, dated at the first of them."""
    game = _game_of(gw)
    utc = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    prior.consecutive_failed += 1
    if prior.consecutive_failed == 1 or prior.first_failed is None:
        prior.first_failed = {"game_date": game.get("date"), "tick": game.get("tick"), "utc": utc}
        prior.run_only_not_bound = True
    if why != NOT_BOUND_YET:
        prior.run_only_not_bound = False
    note = {"seat_session_id": seat_session_id, "kind": KIND_READ_FAILED,
            "old_id": prior.ruler, "new_id": 0, "title_id": prior.title_id, "source": None,
            "reason": f"{read}:{why}", "consecutive": prior.consecutive_failed,
            "game_date": game.get("date"), "tick": game.get("tick"), "written_utc": utc}
    append_row(sink, note)
    prior.rows.append(note)
    _log.warning("seat lineage: %s failed (%s); no verdict (%d of %d in a row)", read, why,
                 prior.consecutive_failed, MAX_CONSECUTIVE_FAILED_READS)
    if prior.consecutive_failed >= MAX_CONSECUTIVE_FAILED_READS:
        first = prior.first_failed or {}
        end = {"seat_session_id": seat_session_id, "kind": KIND_UNBOUND,
               "old_id": prior.ruler, "new_id": 0, "title_id": prior.title_id,
               "source": SOURCE_EXPLICIT,
               "reason": R_NEVER_BOUND if prior.run_only_not_bound else R_READS_FAILED_REPEATEDLY,
               "game_date": first.get("game_date"), "tick": first.get("tick"),
               "first_failed_utc": first.get("utc"), "consecutive": prior.consecutive_failed,
               "written_utc": utc}
        append_row(sink, end)
        prior.rows.append(end)
        prior.ended = True
    return prior


def _recovered(prior: Prior, *, gw, sink: str, seat_session_id: Optional[str]) -> None:
    """The first verdict-bearing step after failed reads: say so, reset the count."""
    if not prior.consecutive_failed:
        return
    game = _game_of(gw)
    note = {"seat_session_id": seat_session_id, "kind": KIND_READS_RECOVERED,
            "old_id": prior.ruler, "new_id": prior.ruler, "title_id": prior.title_id,
            "source": None, "reason": None, "after": prior.consecutive_failed,
            "game_date": game.get("date"), "tick": game.get("tick"),
            "written_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    append_row(sink, note)
    prior.rows.append(note)
    prior.consecutive_failed, prior.first_failed = 0, None
    prior.run_only_not_bound = True


async def step(gw, prior: Optional[Prior], *, sink: str, seat_session_id: Optional[str],
               registered_title: Optional[int] = None, own_session_id=None,
               own_session_row=None) -> Prior:
    """One cycle: the reads, `decide`, the bind when needed, the row. Never raises.

    `own_session_id` / `own_session_row` are redesign's helpers, injected so
    this module imports nothing from redesign (which imports mcp_server's state).
    """
    try:
        bs = await gw.read(READ_SESSIONS, {})
        sid = own_session_id(gw) if own_session_id else None
        # 2026.12.6 (owner rulings 2026-11-10; TK6 witness r1, r2): EVERY seat not
        # ended counts, whether or not it has had a ruler, so a seat that never
        # gets a good read ends `reads_failed_repeatedly` ("we looked and could
        # not see"), never `unwitnessed`. A FAILED read of any of the three reads
        # (read_failed, one predicate), no own session id, a step that raised,
        # and (before any ruler) a good read finding no ruler yet are NO VERDICT:
        # a non-terminal `read_failed` row and the next observe reads again; the
        # MAX_CONSECUTIVE_FAILED_READS-th in a row ends the seat, dated at the
        # first (_no_verdict). A read the bridge answered keeps the signed
        # reasons: a list without this seat's row is `session_row_absent`, a
        # refused character or title read `character_read_refused` /
        # `title_read_refused` (decide, below). Note rows are invisible to the
        # first-sighting rule (_verdict_rows): no false swap.
        counting = prior is None or not prior.ended
        miss = read_failed(bs, READ_SESSIONS) or (None if sid else "no_own_session_id")
        if counting and miss:
            return _no_verdict(prior or Prior(ruler=0, title_id=None), READ_SESSIONS, miss,
                               gw=gw, sink=sink, seat_session_id=seat_session_id)
        row = own_session_row(bs, sid) if own_session_row else None
        p = _i((row or {}).get("primary_character_id")) if isinstance(row, dict) else 0
        no_ruler_yet = prior is None or (prior.ruler == 0 and not prior.ended)
        if no_ruler_yet and isinstance(row, dict) and p == 0 \
                and not (isinstance(row.get("unbound_reason"), str) and row.get("unbound_reason")):
            # the row was read and holds no ruler, and the seat never had one
            # (a prebind landing late, or never): not a verdict on a lineage
            # that does not exist yet (TK6 witness r2 item 3)
            return _no_verdict(prior or Prior(ruler=0, title_id=None), READ_SESSIONS,
                               NOT_BOUND_YET, gw=gw, sink=sink, seat_session_id=seat_session_id)
        subject = p if (prior is None or p > 0) else prior.ruler
        char_read = await gw.read(READ_CHARACTER, {"character_id": subject}) if subject else None
        if counting and subject and read_failed(char_read, READ_CHARACTER):
            return _no_verdict(prior or Prior(ruler=0, title_id=None), READ_CHARACTER,
                               read_failed(char_read, READ_CHARACTER), gw=gw, sink=sink,
                               seat_session_id=seat_session_id)
        title = registered_title or (prior.title_id if prior else None)
        title_read = None
        if prior is not None and title and (p == 0 or (p == prior.ruler and _alive(char_read) is False)):
            title_read = await gw.read(READ_TITLE, {"title_id": title})
            if counting and read_failed(title_read, READ_TITLE):
                return _no_verdict(prior, READ_TITLE, read_failed(title_read, READ_TITLE),
                                   gw=gw, sink=sink, seat_session_id=seat_session_id)
        if prior is not None and not prior.ended and prior.ruler > 0:
            _recovered(prior, gw=gw, sink=sink, seat_session_id=seat_session_id)
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
        if prior is None or (prior.ruler == 0 and p > 0 and not prior.ended
                             and not _verdict_rows(prior.rows)):
            # First sighting (or the first step that finds a ruler after steps
            # that held none): the `bound` row opens the seat's sink, so a seat
            # whose ruler lives to the horizon is witnessed `bound`, not absent.
            if p > 0:
                prior = Prior(ruler=p, title_id=d.refresh_title)
                rowd = Decision(KIND_BOUND, 0, p, d.refresh_title).row(
                    seat_session_id=seat_session_id, game_date=date, tick=tick)
                append_row(sink, rowd)
                prior.rows.append(rowd)
                _note_if_reserved(sink, rowd)
            return prior if prior is not None else Prior(ruler=0, title_id=None)
        if d.kind is not None:
            rowd = d.row(seat_session_id=seat_session_id, game_date=date, tick=tick)
            append_row(sink, rowd)
            prior.rows.append(rowd)
            if d.kind == KIND_REBIND:
                _note_if_reserved(sink, rowd)
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
        # 2026.12.6 (owner rulings 2026-11-10): a step that raised is a failed
        # read like the others (a `read_failed` row, bounded the same way). It
        # used to write `seat_unbound step_failed:<exc>` at once (a reason the
        # signed rule does not list).
        if prior is None or not prior.ended:
            try:
                return _no_verdict(prior or Prior(ruler=0, title_id=None), "step",
                                   type(exc).__name__, gw=gw, sink=sink,
                                   seat_session_id=seat_session_id)
            except Exception:                                    # noqa: BLE001
                return prior
        return prior if prior is not None else Prior(ruler=0, title_id=None)
