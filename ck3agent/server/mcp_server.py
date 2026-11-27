#!/usr/bin/env python3
"""
CK3 Bridge MCP Server — 13 tools: observe, query, discover, inspect, cbs,
plan_war, succession_plan, set_phase, steer, declare_war, do, session, pass_turn.

The model reads game state, steers the native AI (personality weights) and
sends typed actions; the game's own AI runs everything
else. Every tool replies in one envelope (redesign.envelope):
{ok, tool, result, blocked, warnings, errors, bridge_health, [verdict, actor, game, ...]},
and every refusal names its gate (redesign.GATE_TABLE).

Usage:
    # Run as stdio MCP server (for Claude Code / Claude Desktop):
    python3 mcp_server.py

    # Run as HTTP MCP server (for remote access):
    python3 mcp_server.py --transport streamable-http --port 8000

    # Test without bridge (mock mode):
    python3 mcp_server.py --mock
"""

import asyncio
import json
import logging
import os
import sys
import uuid
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from typing import Annotated, Any, Optional

# This module uses bare sibling imports (`from tools import …`, `import redesign`)
# so it can run with cwd=server/ (the stdio-MCP launch path). Ensure server/ is on
# sys.path so `from server import mcp_server` (package-style import, e.g. the import
# test) resolves the siblings too.
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from mcp.server.fastmcp import FastMCP, Context
from mcp.server.fastmcp.exceptions import ToolError
from pydantic import Field, StrictBool

from tools import QUERY_DOMAINS, DISCOVER_WHATS, CONQUEST_TARGETS_DETAILS
import redesign
import bridge_client  # noqa: E402
import seat_lineage                      # the seat is a lineage (inert unless armed)
import do_intent_args
import refusal_memo                     # A game-refused write is not sent again unchanged
import army_orders as _army_orders     # A refused army order is re-asked once the army moved

#: The most domains one query(domains=[...]) call reads.
QUERY_BATCH_MAX = 5

# §8.2 — what a pipeline-mode reply actually looks like — lives in ONE
# module, and this file had zero occurrences of it. The three clients that got
# this wrong got it wrong three different ways; a fourth reading of
# `for_request_id` here would be a fourth way.
import pipeline_correlation as _pcorr
# The inter-seat
# channel. One definition of the keys, the topology rule and the ledger; this
# file only registers the tool when the seat's arm declares a topology other
# than `none`, and delivers inboxes inside `observe`.
import channel as _channel
# The goal block an experiment seat's observe carries.
import goal_block as _goal_block
import reply_fit as _fit                 # A read's reply at most 8 KB, cut with what was cut said

log = logging.getLogger("ck3-mcp")

# THE SEAT'S CHANNEL DECLARATION, READ AT BOOT. None in free play (no
# CK3_EPISODE_ARM); a typed refusal when an arm boots without a topology, so a
# seat whose arm has no key never runs with a guessed one. The refusal exits the
# process: the launcher's fresh-interpreter probe reads it as an unreadable
# surface and refuses the launch in the first second, and a seat started by
# hand has no tools rather than a defaulted channel.
try:
    _CHANNEL = _channel.boot(os.environ)
except _channel.ChannelBootRefusal as _exc:
    sys.stderr.write(json.dumps({"refused": "channel", **_exc.as_dict()}) + "\n")
    sys.stderr.flush()
    raise SystemExit(3)
_SEAT = _channel.SeatState()

# The seats' notepads, `note_read` / `note_write`, served by
# this server like msg_send and declared per arm (notepad.py is the one
# definition). None when the environment declares no notepad: the tools are
# absent, and the seat-surface pin refuses a seat whose arm declared them and
# was not shown them. A malformed declaration refuses the boot, typed.
import notepad as _notepad
try:
    _NOTEPAD = _notepad.boot(os.environ)
except _notepad.NotepadBootRefusal as _exc:
    sys.stderr.write(json.dumps({"refused": "notepad", **_exc.as_dict()}) + "\n")
    sys.stderr.flush()
    raise SystemExit(3)

# The guide, `guide_read(topic)`, served like the notepad
# and declared by the manifest's sha (guide.py is the one definition). None
# when the environment declares none: the tool is absent. A declaration whose
# content does not match its sha refuses the boot, typed. Where a guide is
# declared, `observe` also carries the pushed warnings (pushed_warnings.py),
# each naming a guide topic.
import guide as _guide
import pushed_warnings as _pw
try:
    _GUIDE = _guide.boot(os.environ)
except _guide.GuideBootRefusal as _exc:
    sys.stderr.write(json.dumps({"refused": "guide", **_exc.as_dict()}) + "\n")
    sys.stderr.flush()
    raise SystemExit(3)

# The shared notes (notes.py is the one definition): game facts and tool-use lessons,
# served by discover(what='notes'). CK3_NOTES=off turns them off for a comparison;
# any other value than on/off, or a notes file that fails its schema, refuses the boot.
import notes as _notes
try:
    _NOTES = _notes.boot(os.environ)
except _notes.NotesBootRefusal as _exc:
    sys.stderr.write(json.dumps({"refused": "notes", **_exc.as_dict()}) + "\n")
    sys.stderr.flush()
    raise SystemExit(3)
_notes.SERVED = _NOTES


def _telemetry_dir() -> Optional[str]:
    try:
        import instance as _inst
        return str(_inst.resolve(env=os.environ).telemetry_dir)
    except Exception:                                           # noqa: BLE001
        return None


#: The seat's action memory (pushed_warnings.ActionMemory): in a guided seat,
#: kept in a file beside the guide log so a resumed session remembers.
_ACTIONS = (_pw.ActionMemory(_pw.memory_path(os.environ, _telemetry_dir() or "")
                             if _telemetry_dir() else None)
            if _GUIDE is not None else None)
#: The vassal reads of the pushed warnings, once per cycle (cycle index, vassal ids).
_PUSH_CACHE: dict = {}


# ---------------------------------------------------------------------------
# The gateway the goal-shaped
# tools compose through — dispatch, single-flight serialization of the bridge's
# deferred-drain reads, and since-tick delta tracking — so the tools stay pure
# composition. It never touches the clock: only session(pause|unpause|
# speed|wait) does, on the player's call.
# ---------------------------------------------------------------------------

#: The bridge's deferred-drain reads: a mirror of `is_deferred_drain_action`
#: (bridge/src/deferred_drain.cpp), checked by
#: test_polarity_bridge_table.test_the_gateway_deferred_reads_mirror_the_bridge.
#: The handler submits to a game-thread slot and waits for its verdict, so the
#: reply comes back on the action path with the longer budget, and the bridge
#: single-flights the slot (g_deferred_drain_lock). THE one Python copy: the
#: gateway's dispatch and discover's timeout choice both read it.
DEFERRED_DRAIN_READS = frozenset({
    "query_valid_cbs", "query_cb_validity_strict", "query_cbs_for_target",
    "query_cbs_for_targets", "query_interaction_acceptance", "query_interaction",
    "query_trigger_result", "list_valid_decisions", "list_decisions_annotated",
    "query_joinable_factions", "query_interaction_check",
    "probe_interaction_cascade", "probe_gate_4_slots",
})


#: The budget a deferred-drain or game-thread read waits for its reply.
DEFERRED_READ_TIMEOUT_S = 12.0


class BridgeGateway:
    """Dispatch, single-flight, delta carry and actor-mode detection for the
    goal-shaped tools (SPEC §4/§5).

    The redesign tool impls (redesign.py) call read / write / read_many /
    confirmed_write and never touch the socket. The gateway never changes the
    pause state or the speed: every read and write the bridge serves works with
    the game paused or running (it runs game-object work on the session thread,
    in a drain that runs after every pass). A write's verdict is read back on the
    next pass in either clock state; an outcome that needs game DAYS is reported
    pending until the clock advances (confirmed_write → redesign.confirm_write),
    never forced.

    The clock is what get_tick_info says NOW (clock(), redesign.read_clock). The
    bridge pushes no tick_start or observation frame, so a cache fed by those
    reads "paused" forever; game() is the last fresh reading, taken
    at every tool's entry (_gateway_now) and by every confirmed_write.
    """

    #: Reads the bridge serves on the game thread (snapshot.cpp is_game_thread_read):
    #: they run the game's triggers under the fake random scope
    #: and leave no record in the completed ring. Checked against the
    #: bridge by test_polarity_bridge_table.
    _GAME_THREAD_READS = frozenset({"construction_check", "query_fabricate_targets",
                                    "query_create_title_cost"})

    #: Reads whose reply comes back on the action path with the 12 s budget: the
    #: deferred-drain reads (slot verdicts), the game-thread reads (queued), and
    #: scheme_action(action="check"), an intent the drain evaluates while the
    #: handler polls for its verdict (scheme_targets_impl reads it).
    _ACTION_PATH_READS = DEFERRED_DRAIN_READS | _GAME_THREAD_READS | {"scheme_action"}

    #: Commands, not reads: dispatched on the action path (full response) even
    #: when a caller routes one through read(). This set chooses the dispatch and
    #: nothing else.
    _COMMANDS = frozenset({
        "pause_game", "unpause_game", "set_speed",
        "set_controlled_character", "release_character",
        # The decision-budget governor's write surface.
        "report_usage", "set_budget_config",
    })

    def __init__(self, state: "BridgeState"):
        self.state = state
        # §4 single-flight for the deferred-drain reads: the bridge serves one
        # slot query at a time and refuses a second caller `busy_deferred` past
        # its lock budget, so the gateway queues its own instead of meeting that.
        self._strict_lock = asyncio.Lock()
        self._last_delta_tick = 0
        #: The last fresh clock reading (redesign.clock_of); paused None until one.
        self._clock = redesign.clock_of({})

    # --- clock / health ----------------------------------------------------
    async def clock(self) -> dict:
        """A FRESH clock reading from the bridge (redesign.read_clock); game()
        reports it until the next one."""
        self._clock = await redesign.read_clock(self.read)
        return dict(self._clock)

    def game(self) -> dict:
        """The envelope's game block. paused / speed are the last fresh clock
        reading (None before one: no guess); tick / date are the live stamps every
        bridge reply carries, else the reading's."""
        c = self._clock
        live_date = self.state.game_date if self.state.game_date != "unknown" else None
        return {"tick": self.state.tick_id or c["tick"] or 0,
                "date": live_date or c["date"] or "unknown",
                "paused": c["paused"], "speed": c["speed"]}

    @property
    def health(self) -> str:
        if not self.state.connected and not self.state.mock_mode:
            return "hung"
        return "ok"

    async def actor_gate(self) -> Optional[dict]:
        """What an id-less call answers (SPEC §5): the one definition, redesign.actor_gate_of."""
        return await redesign.actor_gate_of(self)

    # --- bridge calls --------------------------------------------------------
    async def _dispatch(self, action: str, params: dict, *, write: bool = False):
        """One bridge call on the path its kind needs. No clock change, no tick wait."""
        if self.health == "hung":
            return {"reason": seat_lineage.BRIDGE_DISCONNECTED}
        # Writes and commands: action path (fabricates sent_no_confirmation on a
        # timeout, never raises).
        if write or action in self._COMMANDS:
            return await _action_bridge(self.state, action, params, timeout=8.0)
        if action in self._ACTION_PATH_READS:
            try:
                if action in DEFERRED_DRAIN_READS:
                    async with self._strict_lock:
                        return await _action_bridge(self.state, action, params,
                                                    timeout=DEFERRED_READ_TIMEOUT_S)
                return await _action_bridge(self.state, action, params,
                                            timeout=DEFERRED_READ_TIMEOUT_S)
            except (TimeoutError, ConnectionError):
                return {}
        try:
            return await _query_bridge(self.state, action, params, timeout=12.0)
        except (TimeoutError, ConnectionError):
            return {}

    async def read(self, action: str, params: dict):
        """A read, served by the bridge whether the game is paused or running."""
        return await self._dispatch(action, params)

    async def write(self, action: str, params: dict):
        """A write: dispatched on the action path. It lands on the game thread within
        a frame, paused or running. A readback that needs game days goes through
        confirmed_write instead."""
        return await self._dispatch(action, params, write=True)

    async def read_many(self, requests: "list[tuple[str, dict]]") -> list:
        """Issue INDEPENDENT reads CONCURRENTLY; results in request order.

        The agent paid one flush period PER READ: observe(depth=full)
        fans out to ~10 reads awaited in turn, ~10 s each, ~100 s for one
        observation — the whole graded episode. A flush serves its entire buffer
        in one span (flush_scheduler.cpp), so reads issued together cost ONE
        period however many there are.

        MEASURED on the live bridge, four independent reads:
            sequential            20.01 s
            one call_many envelope 10.23 s
            SEPARATE envelopes issued back-to-back then collected  9.83 s
        The third is this design, and it matches the batched envelope — so no
        new client method is needed. `_send_and_wait` already mints a uuid4
        request_id per call and registers its own future, so concurrent issue
        correlates BY CONSTRUCTION, with none of the misrouting risk that made
        expensive.

        Each read keeps its own path (a deferred-drain read still takes the
        single-flight lock, so those serialize among themselves while the rest
        run beside them). A read that raised is recorded as an EMPTY result —
        one read failing must not take the batch down, and the callers'
        isinstance(x, dict) guards already treat {} as "absent".
        """
        if not requests:
            return []
        got = await asyncio.gather(*[self.read(a, p) for a, p in requests],
                                   return_exceptions=True)
        return [{} if isinstance(r, BaseException) else r for r in got]

    async def confirmed_write(self, action: str, params: dict, *, readback,
                              deadline_s: float = 12.0, max_polls: int = 8,
                              poll_interval_s: Optional[float] = None):
        """Dispatch a write, then read it back: redesign.confirm_write (the two
        waits: the drain, then the effect)."""
        if self.health == "hung":
            return {"raw": {"reason": "bridge_disconnected"}, "resolved": None,
                    "polls": 0, "drain": None, "refused": "bridge_disconnected",
                    "drains_when": None, "intent_id": "",
                    "game_date_at_submit": self.game().get("date")}

        async def _send():
            try:
                return await _action_bridge(self.state, action, params, timeout=8.0)
            except (TimeoutError, ConnectionError):
                return {"status": "sent_no_confirmation", "action": action}
        return await redesign.confirm_write(self.clock, self.read, _send, readback,
                                            deadline_s=deadline_s, max_polls=max_polls,
                                            poll_interval_s=poll_interval_s)

    async def delta_records_since_last(self) -> list:
        """Since-tick events as RECORDS: the war, scheme and council-task
        feeds since the last call, each record tagged with its `kind`. The bridge's
        war and scheme records carry no `summary` or `name`, so they are passed as
        dicts for observe's `since` filter to read, never formatted into strings.
        """
        st = self._last_delta_tick
        self._last_delta_tick = self.state.tick_id
        out: list = []
        for act, key, kind in (("query_war_declarations", "declarations", "war"),
                               ("query_scheme_inits", "scheme_inits", "scheme"),
                               ("query_council_tasks", "assignments", "council_task")):
            try:
                res = await _query_bridge(self.state, act, {"since_tick": st, "limit": 20})
            except Exception:                                    # noqa: BLE001
                continue
            if isinstance(res, dict):
                for rec in (res.get(key) or []):
                    if isinstance(rec, dict):
                        out.append({"kind": kind, **rec})
                    else:
                        out.append({"kind": kind, "unparsed": str(rec)[:200]})
        return out


def is_management_ledger_broadcast(msg: dict) -> bool:
    """The bridge's management-ledger record (built by
    management_ledger_frame.h): `type: result`, `ledger_source:
    inline_management`, the caller's id under `caller_request_id` and NO
    `request_id` (the caller's id rides under `caller_request_id` when
    the call had one). It is broadcast for the ARM relay on every control call,
    so this session never waits on it; anything else of type `result` that
    reaches the terminal else (a request_id nobody waits on, another
    ledger_source) is still an unmatched frame."""
    return (msg.get("type") == "result"
            and msg.get("ledger_source") == "inline_management"
            and not msg.get("request_id"))


def _get_gateway() -> "BridgeGateway":
    state = _get_state()
    gw = getattr(state, "_gateway", None)
    if gw is None:
        gw = BridgeGateway(state)
        state._gateway = gw
    return gw


async def _gateway_now() -> "BridgeGateway":
    """The gateway with a fresh clock reading. Every tool takes one at entry (the
    goal-shaped tools, query, discover, steer, pass_turn), so its
    envelope's game block says what get_tick_info said at the call
    (nothing else refreshes it, so no tool shows a stale reading).

    Its cost: one get_tick_info round trip per tool call, served on the bridge's
    IPC thread (bridge-internal, no game-thread wait), so it queues only behind
    another client's call on the same reader; confirmed writes add one at submit
    and one per world poll."""
    gw = _get_gateway()
    if gw.health != "hung":
        await gw.clock()
    return gw


# ---------------------------------------------------------------------------
# Shared state
# ---------------------------------------------------------------------------

@dataclass
class BridgeState:
    """Shared state between MCP handlers and the background bridge listener."""
    connected: bool = False
    tick_id: int = 0
    game_date: str = "unknown"
    latest_observation: dict = field(default_factory=dict)
    handshake_info: dict = field(default_factory=dict)
    #: This server's handshake presented a resume token and the bridge
    #: resumed that session (a seat handed over from its previous server).
    resumed_session: bool = False
    #: The predecessor's resume token this server has NOT yet presented
    #: on an acknowledged handshake (`_handshake_unusable`). Kept until a
    #: handshake is acked, so a startup handshake lost under bridge load does
    #: not lose the seat's session with it.
    pending_resume_token: Optional[str] = None
    #: Handshakes refused `session_attached` while holding that token.
    resume_attached_refusals: int = 0
    #: Frames this listener's type dispatch did not handle, counted by
    #: `type`. THE DICT IS EMPTY UNTIL ONE ARRIVES, never pre-seeded with zeroes:
    #: an absent key means none of that type was seen, and a zero would claim a
    #: reading nobody took. Before this existed the dispatch had no `result`
    #: branch and no terminal else, so a ledger record matching no pending id
    #: was discarded with NO log line and NO counter — the client could not
    #: witness an unmatched record even in principle, and a zero in a bundle
    #: could not be told from "none was ever emitted".
    unhandled_frames: dict = field(default_factory=dict)
    #: Per seat_lineage: what this seat last knew of its ruler and the
    #: title its lineage follows. None until the first armed observe.
    seat_lineage_prior: Any = None
    #: The management-ledger records, counted by `action`: the bridge
    #: broadcasts one on every set_controlled_character / release_character for
    #: the ARM relay, with the caller's id under `caller_request_id` and,
    #: deliberately, no `request_id`, so no caller is waiting on it.
    #: EXPECTED traffic, not an unmatched frame: counted here, logged at DEBUG.
    #: Empty until one arrives, like unhandled_frames.
    management_ledger_frames: dict = field(default_factory=dict)
    action_log: list = field(default_factory=list)
    pending_queries: dict = field(default_factory=dict)  # request_id -> asyncio.Future
    #: THE SECOND CONSUMER. request_id -> the flush ETA IN SECONDS that
    #: the pipeline's own acceptance ack quoted for that call. The ack is keyed
    #: `for_request_id` and never `request_id` (§8.2), so the correlation block
    #: below cannot see it and this is where it is remembered.
    #:
    #: THE ACK IS NOT THE ANSWER and is deliberately not used to resolve the
    #: future. For a write it carries no outcome — a write refused at flush
    #: time acks `accepted:true` exactly like one that lands — so settling on
    #: it would report a refusal as a success. What
    #: it is good for is the DEADLINE: `flush_eta_ms` is the bridge saying how
    #: long this call will take, and the result genuinely cannot arrive before
    #: it.
    pending_acks: dict = field(default_factory=dict)  # request_id -> eta seconds
    #: request_id -> the ticket its ack named. THE ONLY HANDLE ON A
    #: CALL WHOSE PUSH WAS LOST: `check` is keyed by ticket, not by the original
    #: request_id, so without this a missed push is unrecoverable and the call
    #: can only ever time out.
    pending_tickets: dict = field(default_factory=dict)  # request_id -> ticket
    _client: Optional[object] = None
    _listener_task: Optional[asyncio.Task] = None
    _gateway: Optional[object] = None  # BridgeGateway for the goal-shaped tools
    mock_mode: bool = False
    #: The outcome of binding CK3_AGENT_CHARACTER_ID
    #: ({character_id, ok, reason, bound_at?, also: [...], wait: {...}}), None
    #: until the bind has been attempted. Until the game is active the
    #: attempt waits in `_prebind_task` (see _prebind_when_active).
    prebind: Optional[dict] = None
    _prebind_task: Optional[asyncio.Task] = None


# ---------------------------------------------------------------------------
# Background bridge listener
# ---------------------------------------------------------------------------

async def _bridge_listener(state: BridgeState):
    """Background task: reads bridge messages, caches observations, auto-acks ticks."""
    client = state._client
    while True:
        try:
            msg = await client.recv(timeout=5.0)
            if msg is None:
                continue

            msg_type = msg.get("type")

            # Every bridge response stamps the LIVE tick + game_date at
            # the top level (ipc_server.cpp build_envelope_clock_fragment). Refresh
            # the cached clock from ANY message that carries it — so the envelope's
            # tick/game_date are current in both clock states (the bridge pushes no
            # tick_start). Whether the game is paused is in get_tick_info only.
            t = msg.get("tick")
            if isinstance(t, int):
                state.tick_id = t
            gd = msg.get("game_date")
            if isinstance(gd, str) and gd and gd != "unknown":
                state.game_date = gd

            # Route any message with request_id to a pending future
            req_id = msg.get("request_id", "")
            if req_id and req_id in state.pending_queries:
                state.pending_queries.pop(req_id).set_result(msg)
                state.pending_acks.pop(req_id, None)
                continue

            # ── THIS CONSUMER: THE ACK CARRIES THE DEADLINE ─────────
            #
            # The block above routes STRICTLY by `request_id`. The pipeline's
            # acceptance ack is keyed `for_request_id` and carries no
            # `request_id` at all (bridge/src/pipeline.cpp emit_accepted, §8.2),
            # so it has always fallen straight past it into the terminal else
            # and been counted as an unhandled frame.
            #
            # This is the consumer every write tool runs through:
            # BridgeGateway.write -> _action_bridge -> _send_and_wait, all in
            # this file.
            #
            # THE ACK EXTENDS THE DEADLINE; IT DOES NOT SETTLE THE CALL. A
            # write's outcome is not in the ack: a write the flush refuses is
            # `accepted: true` here and `success: false` in the settled frame,
            # so resolving on the ack would report a REFUSAL AS A SUCCESS. The
            # future still settles on the real result.
            #
            # The future is NOT resolved on the ack, as one might
            # expect, because of the hazard above. pipeline_correlation says it plainly —
            # "the result genuinely CANNOT arrive before this, so treating the
            # wait as a timeout is how a correct pipeline-mode bridge gets
            # reported broken". The module has said so; this
            # file had zero occurrences of it.
            if msg_type == "accepted":
                for pending_rid in list(state.pending_queries):
                    if _pcorr.is_our_ack(msg, pending_rid):
                        state.pending_acks[pending_rid] = \
                            _pcorr.ack_eta_seconds(msg)
                        # And the TICKET, which is the handle `check`
                        # takes. An ack with no ticket settles nothing and is
                        # not recorded as one — `ticket_of` returns None and
                        # the recovery below simply does not arm.
                        tk = _pcorr.ticket_of(msg)
                        if tk:
                            state.pending_tickets[pending_rid] = tk
                        break
                continue

            if msg_type == "tick_start":
                # Its stamps only. Whether the game is paused is read from
                # get_tick_info (BridgeGateway.clock): the bridge pushes no
                # tick_start, so a cache of this frame would say "paused" forever.
                state.tick_id = msg.get("tick_id", 0)
                state.game_date = msg.get("game_date", "unknown")

            elif msg_type == "observation":
                state.latest_observation = msg
                state.tick_id = msg.get("tick_id", state.tick_id)
                state.game_date = msg.get("game_date", state.game_date)

            elif msg_type == "handshake_ack":
                state.handshake_info = msg
                log.info("Late handshake ack received")

            elif msg_type == "notification":
                event = msg.get("event", "")
                if event == "character_died":
                    char_id = msg.get("character_id", 0)
                    log.warning("GAME OVER: Controlled character %d has died!", char_id)
                else:
                    log.info("Bridge notification: %s", event)

            elif msg_type == "ping":
                pass

            elif msg_type == "error":
                log.error("Bridge error: %s", msg.get("error"))   # ipc_server.cpp error frames

            else:
                # THE TERMINAL ELSE. Everything above names a type; a
                # frame of any other type does not reach here and vanish. The
                # ledger record is exactly that shape — `"type":"result"` — so
                # an unmatched one left no trace at all.
                #
                # A LEDGER FRAME IS LOUD AND THE REST IS NOT, deliberately. An
                # unmatched record is evidence that did not arrive where it was
                # meant to, and it is rare; an unknown type is ordinary
                # forward-compatibility noise and must not drown a log the
                # operator reads. Same counter, different volume.
                if is_management_ledger_broadcast(msg):
                    act = str(msg.get("action") or "")
                    state.management_ledger_frames[act] = (
                        state.management_ledger_frames.get(act, 0) + 1)
                    log.debug("management ledger record (action=%r, caller_request_id=%r): "
                              "the relay's, not a reply", act, msg.get("caller_request_id"))
                    continue
                state.unhandled_frames[msg_type] = (
                    state.unhandled_frames.get(msg_type, 0) + 1)
                if msg.get("ledger_source"):
                    log.warning(
                        "UNMATCHED LEDGER FRAME (type=%r, ledger_source=%r, "
                        "action=%r): it carried no request_id this session is "
                        "waiting on, so it was not delivered to any caller. "
                        "This is a management record that reached the client "
                        "and went nowhere. Total unhandled of this type: %d",
                        msg_type, msg.get("ledger_source"), msg.get("action"),
                        state.unhandled_frames[msg_type])
                else:
                    log.debug("unhandled bridge frame type=%r (total %d)",
                              msg_type, state.unhandled_frames[msg_type])

        except ConnectionError:
            log.error("Bridge disconnected")
            state.connected = False
            for req_id, future in list(state.pending_queries.items()):
                if not future.done():
                    future.set_result({"error": seat_lineage.BRIDGE_DISCONNECTED})
            state.pending_queries.clear()
            state.pending_acks.clear()
            state.pending_tickets.clear()
            break
        except asyncio.CancelledError:
            break
        except Exception as e:
            log.error("Listener error: %s", e)
            await asyncio.sleep(1)


# ---------------------------------------------------------------------------
# Mock mode (testing without CK3)
# ---------------------------------------------------------------------------

def _make_mock_state() -> BridgeState:
    """Create a BridgeState with realistic mock data for testing."""
    state = BridgeState(connected=True, mock_mode=True)
    state.tick_id = 42
    state.game_date = "1066.09.15"
    state.latest_observation = {
        "type": "observation",
        "tick_id": 42,
        "game_date": "1066.09.15",
        "ruler": {
            "id": 12345,
            "name": "Harold",
            "house_name": "Godwinson",
            "dynasty_name": "Godwin",
            "gold": 245.5,
            "prestige": 1200.0,
            "piety": 350.0,
            "domain_size": 4,
            "domain_limit": 5,
            "realm_size": 12,
            "military_strength": 4500,
            "dread": 15.0,
            "stress": 25,
            "health": 6.2,
            "age": 44,
            "traits": ["brave", "ambitious", "just", "diligent"],
            "personality": {
                "boldness": 50, "compassion": 20, "energy": 60,
                "greed": 30, "honor": 70, "rationality": 40,
                "sociability": 35, "vengefulness": 25, "zeal": 45,
            },
            "is_at_war": True,
            "primary_title": "k_england",
            "vassal_count": 8,
            "liege_id": 0,
            "spouse_id": 11111,
            "heir_id": 12346,
        },
    }
    return state


# ---------------------------------------------------------------------------
# Lifespan: connect to bridge on startup
# ---------------------------------------------------------------------------

MOCK_MODE = "--mock" in sys.argv


@asynccontextmanager
async def bridge_lifespan(server: FastMCP):
    """Connect to bridge on startup, run listener, disconnect on shutdown."""
    if MOCK_MODE:
        log.info("Running in MOCK mode (no bridge connection)")
        state = _make_mock_state()
        yield state
        return

    from async_bridge_client import AsyncBridgeClient

    client = AsyncBridgeClient()
    state = BridgeState()
    state._client = client
    # Read the predecessor's session BEFORE the wrapper below
    # overwrites the record with this server's own session.
    handover = _handover_resume_token()
    _record_session_at_handshake(client)   # Before the pre-bind claims anything

    EXPECTED_BRIDGE_VERSION = "v8-2026-03-20"

    state.pending_resume_token = handover
    connected = await client.connect()
    if connected:
        state.connected = True
        ack = await _handshake_and_bind(client, state, resume_token=handover)
        if await _handshake_unusable(state, ack, handover):
            ack = None
        if ack:
            state.handshake_info = ack
            # "bridge_version" is the release version; the wire protocol tag moved to "bridge_protocol".
            bv = ack.get("bridge_protocol") or ack.get("bridge_version", "unknown")
            bid = ack.get("binary_build_id", "unknown")
            orev = ack.get("offset_revision", "unknown")
            odate = ack.get("offset_date", "unknown")
            log.info("Bridge version: %s, binary build ID: %s, "
                     "offset revision: %s (%s)", bv, bid, orev, odate)
            if bv != EXPECTED_BRIDGE_VERSION:
                log.warning(
                    "Bridge version mismatch! Expected %s, got %s. "
                    "Offsets may be stale — proceed with caution.",
                    EXPECTED_BRIDGE_VERSION, bv,
                )
        if not state.connected:
            log.warning("no usable bridge session at start: the first tool call reconnects "
                        "(and presents the predecessor's token if there is one)")
        elif state._listener_task is None or state._listener_task.done():   # a pre-bind starts it
            state._listener_task = asyncio.create_task(_bridge_listener(state))
            log.info("Bridge connected, listener started")
    else:
        log.warning("Bridge not available — tools will return errors. "
                     "Start CK3 with bridge DLL and reconnect.")

    _arm_exit_signals(state)            # Opt-in, see _release_on_exit
    try:
        yield state
    finally:
        # A pre-bind still waiting for the game must not land in
        # the middle of the release below, so it is stopped first.
        await _cancel_prebind_wait(state)
        await _release_on_exit(state)
        if state._listener_task:
            state._listener_task.cancel()
            try:
                await state._listener_task
            except asyncio.CancelledError:
                pass
        if state._client:
            await client.close()
        log.info("Bridge disconnected")
        if _EXIT_SIGNAL:                # the stdin reader thread would hold exit until EOF
            os._exit(128 + _EXIT_SIGNAL[0])


# ---------------------------------------------------------------------------
# MCP Server
# ---------------------------------------------------------------------------

def unknown_arguments(input_schema: dict, arguments: Any) -> list[str]:
    """The argument names a call sent that the tool's registered schema does not
    declare, sorted. One definition: the FastMCP refusal below and its test read it."""
    if not isinstance(arguments, dict):
        return []
    declared = set((input_schema or {}).get("properties") or {})
    return sorted(k for k in arguments if k not in declared)


def raised_reply(tool: str, err: BaseException) -> dict[str, Any]:
    """The envelope for a tool call that raised instead of answering. The
    exception's own text never reaches the reply. A schema refusal (pydantic's
    ValidationError under FastMCP's ToolError) is invalid_argument naming the
    fields and what each one takes; anything else is server_error naming the
    exception's type, with what is unknown said: the call raised partway, so
    whether a write was sent is not known."""
    cause = err.__cause__ if err.__cause__ is not None else err
    errors = getattr(cause, "errors", None)
    try:
        state = _get_state()
    except Exception:                       # no request context (a direct call)
        state = None
    if type(cause).__name__ == "ValidationError" and callable(errors):
        fields = []
        for row in errors() or []:
            loc = [str(x) for x in (row.get("loc") or ()) if not isinstance(x, int)]
            fields.append({"field": ".".join(loc), "expected": str(row.get("type") or "")})
        names = sorted({f["field"] for f in fields if f["field"]})
        return _blocked(tool, state, "invalid_argument",
                        f"{tool}: {', '.join(names) or 'an argument'} has a value this call does not "
                        "take; the call was not run",
                        fix={"rejected_fields": names}, measured={"fields": fields})
    return _blocked(tool, state, "server_error",
                    f"{tool} raised {type(cause).__name__} inside the server before it answered; "
                    "whether anything was sent to the game is not known: read the state before "
                    "calling again",
                    raw_reason=type(cause).__name__)


#: The `_meta` key of a call a harness makes for itself on a seat's server (a harness spells the same string)
BENCH_READ_META = "ck3bench_own_read"


class _StrictArgumentsFastMCP(FastMCP):
    """Every tool refuses an argument its schema does not declare.

    FastMCP's argument model ignores an extra key, so a call carrying one ran as
    if the key were absent: pass_turn(character_id=...) on 174 pilot calls (fixed
    on that tool as the explicit case) and steer's flat axes once. Refuse rather
    than default: the refusal is one place, before the tool runs, with the typed
    gate `unknown_argument` naming each unknown key and the declared ones.

    What this does NOT cover, said so a reader does not assume it:
      * A caller that dispatches tool calls its own way (redesign.*_impl,
        directly) never reaches this call_tool, so an unknown key it sends is
        not refused here; the MCP server is the product surface.
      * Only top-level keys are checked. Keys nested inside a dict argument
        (do's `args`, declare_war's `declare_args`, set_phase's `overrides`) are
        each tool body's to read; set_phase refuses an unknown axis itself."""

    def _bench_read(self) -> bool:
        """The request's `_meta` carries BENCH_READ_META true: a read a harness makes for itself on the seat's
        server (a metric, a witness), never shown to the seat, so it leaves the seat's kept pages as they were."""
        try:
            meta = self.get_context().request_context.meta
        except (LookupError, ValueError, AttributeError):
            return False
        extra = getattr(meta, "model_extra", None) or {}
        return extra.get(BENCH_READ_META) is True

    async def call_tool(self, name: str, arguments: dict[str, Any]):
        tool = self._tool_manager.get_tool(name)
        if tool is not None:
            unknown = unknown_arguments(tool.parameters, arguments)
            if unknown:
                declared = sorted((tool.parameters or {}).get("properties") or {})
                try:
                    state = _get_state()
                except ValueError:          # no request context (a direct call)
                    state = None
                env = _blocked(name, state, "unknown_argument",
                               f"{name} takes no argument named {', '.join(unknown)}; "
                               "the call was not run",
                               fix={"rejected_fields": unknown, "accepted_fields": declared},
                               measured={"unknown_arguments": unknown})
                return tool.fn_metadata.convert_result(env)
        try:
            if tool is None:
                return await super().call_tool(name, arguments)
            raw = await self._tool_manager.call_tool(name, arguments, context=self.get_context(),
                                                     convert_result=False)
            if _TP.prefix():
                # The host's tool-name prefix on every hint in the reply (tool_prefix.py)
                names = [t.name for t in self._tool_manager.list_tools()]
                raw = _TP.apply(raw, names)
            # The cap on the bytes the seat receives, after the prefix and the shim's wrapper (reply_fit)
            return tool.fn_metadata.convert_result(_fit.delivered(raw, name, arguments,
                                                                  store=not self._bench_read()))
        except ToolError as e:
            # declare_war(cb_title_id="c_athlone") came back
            # as "Error executing tool declare_war: invalid literal for int() ...". No
            # raw exception text reaches a reply: an argument the schema refuses is
            # invalid_argument naming the field; anything else the tool raised is
            # server_error naming only the exception's type.
            if tool is None:
                raise
            return tool.fn_metadata.convert_result(raised_reply(name, e))


import tool_text as _TT                 # Every tool's description, one module
import map_view as _MV                  # inspect(subject="map")
import tool_prefix as _TP               # The client's tool-name prefix in hints


mcp = _StrictArgumentsFastMCP(
    "CK3 Bridge",
    instructions=(
        # What the tools do, not a role for the caller.
        "This server connects to a running Crusader Kings 3 game via an "
        "injected bridge library. Its tools read game state and write changes "
        "to it: personality weights and typed actions "
        "such as interactions, council tasks, laws and war declarations. The "
        "game's own AI continues to run everything not written through them.\n\n"
        "Each cycle: observe first; read deeper with query, inspect, discover or cbs; "
        "plan with plan_war, succession_plan or set_phase; act with do, declare_war "
        "or steer; " + ("end your turn to let time pass (the environment owns the clock).\n"
                        if redesign.clock_owned_by_env() else
                        "let time pass with session(action='wait').\n") +
        "Every reply is one envelope: {ok, tool, result, blocked, warnings, errors, "
        "bridge_health}, plus verdict (applied | rejected | pending) on writes. "
        "When ok is false, blocked.gate names why and blocked.next, when present, "
        "says what to call instead.\n" + _NOTES.instructions_line()
    ),
    lifespan=bridge_lifespan,
)


# ---------------------------------------------------------------------------
# Resources (read-only game state)
# ---------------------------------------------------------------------------

@mcp.resource("ck3://state/formatted")
def get_formatted_state() -> str:
    """Human-readable game state: ruler stats, wars, diplomacy."""
    ctx = mcp.get_context()
    state: BridgeState = ctx.request_context.lifespan_context

    if state.latest_observation:
        from prompts import format_observation
        return format_observation(state.latest_observation)

    status = "connected" if state.connected else "disconnected"
    return (
        f"Bridge is {status}. No cached observation available.\n"
        "Call observe for the situation report."
    )


@mcp.resource("ck3://state/overview")
def get_overview() -> dict[str, Any]:
    """Connection status, tick ID, game date (structured data, not a JSON string)."""
    ctx = mcp.get_context()
    state: BridgeState = ctx.request_context.lifespan_context
    return overview_payload(state)


def overview_payload(state) -> dict:
    """ck3://state/overview's body. The handshake is the ack less the withheld
    keys (bridge_client.HANDSHAKE_ACK_WITHHELD): the resume token is an
    authorisation over this session's claims and a resource body lands in the
    client's transcript."""
    return {
        "connected": state.connected,
        "mock_mode": state.mock_mode,
        "tick_id": state.tick_id,
        "game_date": state.game_date,
        "handshake": bridge_client.ack_for_disk(state.handshake_info) if state.handshake_info else state.handshake_info,
    }


# ---------------------------------------------------------------------------
# Helper functions
# ---------------------------------------------------------------------------

def _get_state() -> BridgeState:
    ctx = mcp.get_context()
    return ctx.request_context.lifespan_context


def _extract_resolved_id(data: Any) -> int | None:
    """Lift the resolved character_id the bridge already echoed in an inner section.

    observe()/steer()/query() with no character_id silently resolve to the
    bridge PRIMARY controlled char. The bridge handlers DO compute and echo the
    resolved id as a nested `character_id` (masked to 24 bits), but it sits at a
    different path per tool and the agent never asserts against it -> silent
    wrong-target. This helper finds that id from any of the sections the bridge
    populates, so the envelope can surface ONE top-level `resolved_character_id`.

    Pure best-effort: returns None if no section carries it (never fabricate). We
    only trust a section that LOOKS resolved — its own `character_id` is a positive
    int — and we ignore error stubs.
    """
    if not isinstance(data, dict):
        return None
    # Sections (in priority order) the bridge handlers emit a resolved character_id on.
    for key in ("character", "personality", "realm", "succession",
                "council", "traits", "claims", "relations", "opinion"):
        section = data.get(key)
        if isinstance(section, dict):
            cid = section.get("character_id")
            if isinstance(cid, int) and cid > 0:
                return cid
    # Some single-domain query results put character_id at the top level of `data`.
    cid = data.get("character_id")
    if isinstance(cid, int) and cid > 0:
        return cid
    return None


def _state_game(state: BridgeState) -> dict[str, Any]:
    """The `game` block for a tool that reads the bridge directly (query, discover,
    steer, pass_turn): the gateway's, so it carries the fresh clock reading those
    tools take at entry (_gateway_now), the same block every other tool's envelope
    has. Before any reading paused / speed are None, never a guess."""
    if state is None:
        return {"tick": None, "date": None, "paused": None, "speed": None}
    gw = getattr(state, "_gateway", None)
    if gw is not None:
        return gw.game()
    return {"tick": state.tick_id, "date": state.game_date, "paused": None, "speed": None}


def _state_health(state: BridgeState) -> str:
    if state is None or (not state.connected and not state.mock_mode):
        return "hung"
    return "ok"


def _ok(tool: str, state: BridgeState, data: Any = None,
        resolved_id: int | None = None, *, errors: Optional[list] = None,
        warnings: Optional[list] = None) -> dict[str, Any]:
    """A successful read in the one envelope (redesign.envelope).

    `result` is always a parsed object: a JSON string is parsed back, any other
    non-dict is carried as {"value": ...}. The character the bridge actually
    resolved (character_id=0 means the bound ruler) is echoed as
    actor.character_id, lifted from the reply when the caller does not pass it.
    """
    if isinstance(data, str):
        try:
            data = json.loads(data)
        except (json.JSONDecodeError, ValueError):
            pass
    if resolved_id is None:
        resolved_id = _extract_resolved_id(data)
    result = data if isinstance(data, dict) else ({} if data is None else {"value": data})
    actor = ({"character_id": resolved_id}
             if isinstance(resolved_id, int) and resolved_id > 0 else None)
    return redesign.envelope(tool, result=result, actor=actor, game=_state_game(state),
                             warnings=warnings, errors=errors,
                             bridge_health=_state_health(state))


def _blocked(tool: str, state: BridgeState, gate: str, summary: str,
             **why) -> dict[str, Any]:
    """A refusal in the one envelope, carrying a named gate (redesign.GATE_TABLE)."""
    return redesign.envelope(tool, ok=False,
                             blocked=redesign.why_blocked(gate, summary=summary, **why),
                             game=_state_game(state), bridge_health=_state_health(state))


def _refused(tool: str, state: BridgeState, blocked: dict) -> dict[str, Any]:
    """A refusal in the one envelope from a why_blocked already built."""
    return redesign.envelope(tool, ok=False, blocked=blocked, game=_state_game(state),
                             bridge_health=_state_health(state))


def _declined(reply: Any, action: str = "") -> dict[str, Any] | None:
    """The why_blocked for a reply the bridge DECLINED, or None when it answered.

    A handler that returns false comes back as {"success": false, "action": ...},
    with `reason` (or `error`) when the handler named why; the bridge's own fault
    and timeout replies carry no `success` at all. Either is a refusal, never a
    reading: shown as ok=true it reads like data. The rule is redesign.read_refusal,
    the read twin of the write predicate: a game-thread read that
    did not run in time is read_timeout, retryable."""
    name = str((reply.get("action") if isinstance(reply, dict) else "") or action or "the read")
    return redesign.read_refusal(reply, name)


def _read_failure(name: str, exc: BaseException) -> dict[str, Any]:
    """One `errors[]` row for a read that did not come back: a timeout is
    `read_timeout`, anything else on the wire is `bridge_disconnected`."""
    code = "read_timeout" if isinstance(exc, (TimeoutError, asyncio.TimeoutError)) \
        else "bridge_disconnected"
    return {"code": code, "detail": f"{name}: {exc}" if str(exc) else name}


async def _try_reconnect(state: BridgeState) -> bool:
    """Attempt to reconnect to the bridge if not connected."""
    if state.connected or state.mock_mode:
        return True
    log.info("Attempting bridge reconnect...")
    try:
        from async_bridge_client import AsyncBridgeClient
        old_client = state._client
        resume = _reconnect_resume_token(old_client)
        if old_client:
            try:
                await old_client.close()
            except Exception:
                pass
        # A token this server never presented on an acked handshake (its
        # startup handshake was lost, or refused `session_attached`) is the
        # seat's session; it is presented before any other.
        resume = getattr(state, "pending_resume_token", None) or resume
        state._client = AsyncBridgeClient()
        # The new client's session is recorded like the first
        # one, or CK3_SESSION_OUT keeps naming a session that no longer holds
        # the claims and `ck3agent run` resumes the wrong one after a SIGKILL.
        _record_session_at_handshake(state._client)
        connected = await state._client.connect()
        if connected:
            state.connected = True
            # A reconnect mints a NEW session (session.cpp:819 —
            # "RELEASED and the session ERASED; the client must handshake"), so
            # the claim does NOT survive it. Re-bind here or the agent carries
            # on unbound for the rest of the episode with nothing saying so.
            # Under release-on-exit the old token is presented first: where the
            # bridge kept the session (claims pinned) the claims stay on one
            # session; where it destroyed it the bridge answers unknown_token
            # and mints a fresh one, and the re-bind below claims again.
            ack = await _handshake_and_bind(state._client, state, resume_token=resume)
            if await _handshake_unusable(state, ack, resume):
                return False
            if ack:
                state.handshake_info = ack
            if state._listener_task is None or state._listener_task.done():
                state._listener_task = asyncio.create_task(_bridge_listener(state))
            log.info("Bridge reconnected successfully")
            return True
    except Exception as e:
        log.error("Reconnect failed: %s: %s", type(e).__name__, e)
    return False


def _handover_resume_token() -> Optional[str]:
    """The resume token of the session this seat's PREVIOUS server
    held, read from the session record (CK3_SESSION_OUT) before this server's
    first handshake overwrites it. None unless release-on-exit is armed
    (`ck3agent run`) and the record names a token.

    WHY: `ck3agent run` resumes a cell
    seat's claude when it ends early, and the resumed claude starts a NEW
    server. In an experiment episode the bridge PINS a dropped control
    session's claims (ipc_server.cpp release_slot_locked, R3) and refuses to
    release a control session's last claim (dispatch_router.cpp
    do_release_character, last_claim_of_control_session), so the old session
    kept the seat's ruler and the new server's fresh session was refused
    `owned_by_other` on every bind: K played 94 game-days unbound. The seat is
    HANDED OVER instead: the new server presents the old session's token and
    the bridge resumes that session, claims intact (session.cpp
    resume_by_token). A session still attached to a live connection is not
    resumable (`session_attached`): the bridge mints a fresh session and a
    bind of a ruler that session holds is still refused `owned_by_other`, so
    a live second server never takes a seat."""
    if not os.environ.get(RELEASE_ON_EXIT_ENV):
        return None
    out = os.environ.get(SESSION_OUT_ENV)
    if not out:
        return None
    try:
        with open(os.path.expanduser(out), encoding="utf-8") as fh:
            rec = json.loads(fh.read() or "{}")
    except (OSError, ValueError):
        return None
    tok = rec.get("resume_token") if isinstance(rec, dict) else None
    return tok if isinstance(tok, str) and tok else None


#: How many handshakes may be refused `session_attached` (the
#: predecessor's server still holds its socket) before this server keeps the
#: fresh session it was given. Each is one tool call that answers
#: `bridge_disconnected` and is retried on the next.
RESUME_ATTACHED_RETRIES = 5


def _resume_retry_due(state, ack, resume_token: Optional[str]) -> bool:
    """True when a handshake that presented the seat's token was refused only
    because the old session is still attached: the predecessor's server has not
    let go yet, and a later handshake can still resume it. Bounded by
    RESUME_ATTACHED_RETRIES."""
    if not resume_token or not isinstance(ack, dict) or ack.get("resumed") is True:
        return False
    if ack.get("resume_status") != "session_attached":
        return False
    return int(getattr(state, "resume_attached_refusals", 0)) < RESUME_ATTACHED_RETRIES


async def _handshake_unusable(state, ack, resume_token: Optional[str]) -> bool:
    """A recorded case (COUNCIL D2 and D3): True when this connection must
    not be used, after closing it and marking the server disconnected so the
    next tool call reconnects (_ensure_connected).

    THE HAZARD. Under six seats the bridge's one reader thread fell behind and
    a seat server's startup handshake got no ack within its 10 s. The server
    kept the socket as connected anyway: no pre-bind, no session record
    (CK3_SESSION_OUT), the predecessor's token dropped, and the session the
    bridge minted for that socket when it caught up was one the server could
    not name. The agent's own `session(control)` then claimed its ruler on that
    nameless session; at the claude's exit the bridge pinned it (a declared
    seat), nobody could resume it, and every later server of the seat was
    refused `owned_by_other` (D2 from turn 2, D3 from turn 1, to the end).

    Unusable: (a) no ack (None, or not a dict); (b) the seat's token refused
    `session_attached` while retries remain (_resume_retry_due). The token is
    kept in `state.pending_resume_token` until an acked handshake presented it;
    a resumed or freshly bound session clears it."""
    usable = isinstance(ack, dict) and bool(ack)
    retry = usable and _resume_retry_due(state, ack, resume_token)
    if usable and not retry:
        state.pending_resume_token = None
        return False
    if retry:
        state.resume_attached_refusals = int(getattr(state, "resume_attached_refusals", 0)) + 1
        log.warning("the seat's session is still attached to its previous server: this "
                    "connection is dropped and the resume retried on the next call (%d/%d)",
                    state.resume_attached_refusals, RESUME_ATTACHED_RETRIES)
    else:
        log.warning("no handshake ack: this connection is dropped and the next tool call "
                    "reconnects%s", " presenting the seat's token" if resume_token else "")
    state.pending_resume_token = resume_token or getattr(state, "pending_resume_token", None)
    state.connected = False
    state.handshake_info = {}
    client = getattr(state, "_client", None)
    if client is not None:
        try:
            await client.close()
        except Exception:                                          # noqa: BLE001
            pass
    return True


def _reconnect_resume_token(old_client) -> Optional[str]:
    """The previous session's resume token, presented on a reconnect ONLY when
    release-on-exit is armed (`ck3agent run`). An experiment episode, which
    never sets it, reconnects exactly as before."""
    if not os.environ.get(RELEASE_ON_EXIT_ENV) or old_client is None:
        return None
    try:
        ack = old_client.handshake_ack
    except Exception:                                           # noqa: BLE001
        return None
    tok = ack.get("resume_token") if isinstance(ack, dict) else None
    return tok if isinstance(tok, str) and tok else None


async def _ensure_connected(state: BridgeState, tool: str) -> dict[str, Any] | None:
    """None when the bridge is connected (reconnecting once if not); otherwise the
    `bridge_disconnected` refusal for `tool`."""
    if state.mock_mode:
        return None
    if not state.connected:
        await _try_reconnect(state)
    if not state.connected:
        return _blocked(tool, state, "bridge_disconnected",
                        "the bridge is not connected: is CK3 running with the bridge library loaded?")
    return None


# ═══════════════════════════════════════════════════════════════════════════
# ARRIVE BOUND, AND SAY WHO YOU ARE
# ═══════════════════════════════════════════════════════════════════════════

#: The character this agent plays, plumbed in by the harness. ABSENT MEANS NO
#: CLAIM — never a default and never 0. An interactive or observing MCP session
#: sets nothing and behaves exactly as before; only an episode that declares a
#: character gets pre-bound. That gating is what keeps this out of the null arm (which
#: runs NullAgent and no MCP server at all) and out of anyone's ad-hoc session.
AGENT_CHARACTER_ENV = "CK3_AGENT_CHARACTER_ID"

#: This server's game-refused writes (refusal_memo.py).
_REFUSALS = refusal_memo.Memo()


#: The most rulers one pre-bind list may name. The bridge's cap is
#: 10 claims across every session (MAX_CONTROLLED_WATCHLIST); a longer list
#: cannot all be bound, so it is refused whole rather than bound in part.
MAX_PREBIND_RULERS = 10

#: How long the pre-bind waits for the game to become ACTIVE
#: before it is attempted anyway. `ck3agent run` can start while the save is
#: still loading (in one run `launch` returned rc 3, both ids were
#: refused `character_not_found` and nothing retried). Seconds; 0 attempts at
#: once as before.
PREBIND_WAIT_ENV = "CK3_AGENT_PREBIND_WAIT_S"
PREBIND_WAIT_DEFAULT_S = 120.0
#: Seconds between two reads of the bridge's session_state while waiting.
PREBIND_POLL_S = 2.0


def _prebind_wait_s() -> float:
    """The wait budget: CK3_AGENT_PREBIND_WAIT_S, or the default when unset or
    not a non-negative number (logged, never a crash at connect)."""
    raw = os.environ.get(PREBIND_WAIT_ENV)
    if raw is None or not str(raw).strip():
        return PREBIND_WAIT_DEFAULT_S
    try:
        val = float(str(raw).strip())
    except (TypeError, ValueError):
        val = -1.0
    if val < 0 or val != val:            # negative, or NaN
        log.warning("%s=%r is not a non-negative number of seconds; using %.0f",
                    PREBIND_WAIT_ENV, raw, PREBIND_WAIT_DEFAULT_S)
        return PREBIND_WAIT_DEFAULT_S
    return val


def _agent_character_ids() -> list[int]:
    """The declared characters, in order (the first is the primary), or [].

    `CK3_AGENT_CHARACTER_ID` takes one id or a comma-separated list
    ("<id1>,<id2>"). Refuses rather than guesses: one malformed entry, or more
    than MAX_PREBIND_RULERS, claims NOTHING (a partial list would bind rulers
    the declaration did not mean as a set). A repeated id is bound once.
    """
    raw = os.environ.get(AGENT_CHARACTER_ENV)
    if raw is None or not str(raw).strip():
        return []
    out: list[int] = []
    for part in str(raw).split(","):
        try:
            cid = int(part.strip())
        except (TypeError, ValueError):
            log.error("%s=%r: %r is not an integer — NOT claiming any character",
                      AGENT_CHARACTER_ENV, raw, part)
            return []
        if cid <= 0:
            log.error("%s=%r: %d is not a character id — NOT claiming any character",
                      AGENT_CHARACTER_ENV, raw, cid)
            return []
        if cid in out:
            log.warning("%s=%r names %d twice; binding it once", AGENT_CHARACTER_ENV, raw, cid)
            continue
        out.append(cid)
    if len(out) > MAX_PREBIND_RULERS:
        log.error("%s=%r names %d rulers; the bridge holds at most %d — NOT claiming any",
                  AGENT_CHARACTER_ENV, raw, len(out), MAX_PREBIND_RULERS)
        return []
    return out


def _agent_character_id() -> Optional[int]:
    """The declared PRIMARY character (the first of the list), or None."""
    ids = _agent_character_ids()
    return ids[0] if ids else None


async def _handshake_and_bind(client, state,
                              resume_token: Optional[str] = None) -> Optional[dict]:
    """Handshake at v2, then CLAIM the declared character before the first turn.

    WHY THE CLAIM IS HERE AND NOT IN THE PROMPT (the experiment's
    rule). The registered prompt says "You ARE Harold Godwinson" — present
    tense, already him — while the driver expects the agent to seize control of
    itself as an early act, and the `session` tool that does it is described as
    "you rarely call this". The materials disagree about whether the agent
    arrives bound. Binding is SETUP, a precondition to playing rather than a
    strategic act under study, so it is repaired in machinery and NOT by editing
    a word the agent reads — the registered text is the treatment.

    Measured motivation: on the graded run the agent stated intent to act, loaded
    the action tools, and was cut off mid-orientation having never claimed;
    across 44 bundles every run with a claim had acts > 0 and no run with
    acts = 0 had a roster. Claiming tracked how far the agent got. Pre-binding
    removes that opening entirely.

    THE CLAIM IS ON THIS SESSION. Ownership is per-session (an id claimed by the
    harness's session is not held by the agent's), which is why it cannot be
    done driver-side.

    Rides here: passing client_name/mode promotes the frame to v2. A
    BARE handshake omits protocol_version, the bridge assumes legacy v1, and the
    session is logged as client='unknown'. v1 fails PERMISSIVE
    measured a bare handshake returning session_mode "control" — so this is not
    what blocked the claim, but an observe-only request at v1 silently yields
    CONTROL, and a session that cannot say what it asked for cannot be audited.
    """
    # Local import to match this module's style for bridge-client symbols and
    # to keep import order independent of the MCP framework's.
    from bridge_client import (DEFAULT_CLIENT_NAME, MODE_CONTROL,
                               seat_declaration_kwargs)
    # DECLARE THE SEAT. This session is what an arm's agent acts
    # through, so it is a seat; the driver's steer-probe and speed-pin helpers
    # are control sessions that never claim and must not declare.
    # A reconnect under release-on-exit presents the old token.
    resume = {"resume_token": resume_token} if resume_token else {}
    ack = await client.handshake(client_name=DEFAULT_CLIENT_NAME,
                                 mode=MODE_CONTROL,
                                 **seat_declaration_kwargs(), **resume)
    if not ack:
        return ack
    if resume_token:
        log.info("handshake presented a resume token: resumed=%s resume_status=%s session=%s",
                 ack.get("resumed"), ack.get("resume_status"), ack.get("session_id"))
        if ack.get("resumed") is not True:
            log.warning("the session the token names was not resumed (%s): this server "
                        "has a fresh session, and a ruler the old one holds stays refused "
                        "owned_by_other", ack.get("resume_status"))
    # A handed-over session already holds the seat's rulers.
    state.resumed_session = bool(resume_token) and ack.get("resumed") is True
    if _resume_retry_due(state, ack, resume_token):
        return ack                      # No bind on a session about to be dropped
    ids = _agent_character_ids()
    if not ids:
        return ack                      # nothing declared: unchanged behaviour
    # READ THE PRE-BIND'S REPLY. The listener is what routes a reply
    # to its waiting call, and at connect it does not start only AFTER this
    # function returned: every pre-bind waited out its timeout and came back
    # `sent_no_confirmation`, which the `success`-defaults-true read below took
    # for a bind. Measured with the fake bridge: 5 s per declared ruler, and an
    # added ruler's `added: true` never seen. Start it now, after the handshake
    # has consumed its own ack.
    if getattr(state, "_client", None) is client and not getattr(state, "mock_mode", False):
        task = getattr(state, "_listener_task", None)
        if task is None or task.done():
            state._listener_task = asyncio.create_task(_bridge_listener(state))
    # A reconnect's bind replaces any wait the old session left.
    await _cancel_prebind_wait(state)
    # BIND WHEN THE GAME IS ACTIVE, NOT WHEN THE SERVER IS. The
    # ack says what the bridge is attached to. `active` (or an ack that does
    # not say — an older bridge or a test fake) binds now, as before. Any
    # other state (`dormant` on the main menu, a save still loading) would be
    # refused `character_not_found` at once and never retried, so the bind is
    # deferred to a task that polls get_tick_info's session_state. A TASK, not
    # a wait here: this runs inside the lifespan, which FastMCP enters before
    # it answers `initialize`, and Claude Code gives a server far less than
    # the 120 s wait to come up. The tools are usable meanwhile.
    ack_state = ack.get("session_state") if isinstance(ack, dict) else None
    if ack_state and ack_state != "active":
        state.prebind = None
        state._prebind_task = asyncio.create_task(
            _prebind_when_active(state, client, ids, str(ack_state)))
        return ack
    await _prebind_now(state, ids, wait={"waited_s": 0.0, "polls": 0,
                                         "game_active": True if ack_state else None})
    return ack


async def _prebind_now(state, ids: list[int], wait: dict) -> None:
    """Claim every declared ruler now, record the outcome on `state.prebind` and
    in the session record. `wait` says how long the game was waited for
    (`waited_s`, `polls`) and whether it was seen active (`game_active`: True,
    False after a timeout, None when nothing said)."""
    if getattr(state, "resumed_session", False) and len(ids) > 1:
        ids = await _resumed_primary_first(state, ids)
    state.prebind = await _prebind_one(state, ids[0], add=False)
    if len(ids) > 1:
        # Every further ruler is an ADDED claim (`add: true`), so the
        # first stays the primary. A bridge that predates `add` ignores the key
        # and SWITCHES away from the first; its reply then lacks `added`, which
        # _prebind_one records as a refusal rather than a bind.
        state.prebind["also"] = [await _prebind_one(state, x, add=True) for x in ids[1:]]
    state.prebind["wait"] = dict(wait)
    _record_prebind(state.prebind)


async def _resumed_primary_first(state, ids: list[int]) -> list[int]:
    """On a RESUMED session, the ruler it already has as its primary
    goes first. The first pre-bind is a switch (no `add`), and a switch from the
    session's primary to another ruler it also holds drops the primary
    (session.cpp swap_character); the carried list is in id order, not primary
    order. Unchanged when the read fails or the primary is not in the list."""
    try:
        bs = await _query_bridge(state, "get_bridge_state", {}, timeout=5.0)
    except Exception as exc:                                   # noqa: BLE001
        log.debug("resumed pre-bind: get_bridge_state failed: %s", exc)
        return ids
    sid = getattr(state._client, "session_id", None)
    # session.cpp Session::to_json: session_id, primary_character_id
    row = next((r for r in (bs or {}).get("sessions") or []
                if isinstance(r, dict) and r.get("session_id") == sid), None) \
        if isinstance(bs, dict) else None
    p = (row or {}).get("primary_character_id")
    if isinstance(p, int) and p in ids:
        return [p] + [x for x in ids if x != p]
    return ids


async def _game_active(state) -> Optional[bool]:
    """One read of the bridge's session_state: True when `active`, False for any
    other named state, None when the read failed or named none."""
    try:
        res = await _query_bridge(state, "get_tick_info", {}, timeout=5.0)
    except Exception as exc:                                   # noqa: BLE001
        log.debug("pre-bind wait: get_tick_info failed: %s", exc)
        return None
    st = res.get("session_state") if isinstance(res, dict) else None
    return (st == "active") if st else None


def _agent_claims(state) -> int:
    """How many set_controlled_character calls this server has sent."""
    return sum(1 for e in state.action_log if e.get("action") == "set_controlled_character")


async def _prebind_when_active(state, client, ids: list[int], first_state: str) -> None:
    """Poll the bridge until the game is active, then bind; on the deadline bind
    anyway, so a game that never comes up yields the bridge's own refusal
    (`character_not_found`), exactly what the run prompt already recovers from.
    Stops without binding if the connection was replaced or exit began, and
    records `superseded_by_agent_claim` if the agent bound for itself while the
    wait ran (the bind would otherwise switch or re-claim under it)."""
    loop = asyncio.get_running_loop()
    budget = _prebind_wait_s()
    t0 = loop.time()
    deadline = t0 + budget
    log.warning("game not active (session_state=%s): the pre-bind of %s waits up to %.0f s "
                "for it (%s)", first_state, ids, budget, PREBIND_WAIT_ENV)
    claims_before = _agent_claims(state)
    polls = 0
    active: Optional[bool] = False
    try:
        while True:
            if _EXIT_SIGNAL or state._client is not client:
                log.warning("pre-bind of %s abandoned: %s", ids,
                            "exit requested" if _EXIT_SIGNAL else "connection replaced")
                return
            active = await _game_active(state)
            polls += 1
            if active:
                break
            if loop.time() >= deadline:
                log.error("game still not active after %.0f s (%d polls): attempting the "
                          "pre-bind of %s anyway", loop.time() - t0, polls, ids)
                active = False
                break
            await asyncio.sleep(max(0.0, min(PREBIND_POLL_S, deadline - loop.time())))
        waited = round(loop.time() - t0, 3)
        wait = {"waited_s": waited, "polls": polls, "game_active": active}
        if _agent_claims(state) > claims_before:
            log.warning("the agent claimed for itself while the pre-bind of %s waited; "
                        "the pre-bind is not attempted", ids)
            state.prebind = {"character_id": ids[0], "ok": False,
                             "reason": "superseded_by_agent_claim",
                             "also": [{"character_id": x, "ok": False,
                                       "reason": "superseded_by_agent_claim"} for x in ids[1:]],
                             "wait": wait}
            _record_prebind(state.prebind)
            return
        if _EXIT_SIGNAL:                 # exit began during the last poll: nothing is claimed
            log.warning("pre-bind of %s abandoned: exit requested", ids)
            return
        if active:
            log.info("game active after %.1f s (%d polls): pre-binding %s", waited, polls, ids)
        await _prebind_now(state, ids, wait)
    except asyncio.CancelledError:
        raise
    except Exception as exc:                                   # noqa: BLE001
        log.error("pre-bind wait for %s failed: %s", ids, exc)


async def _cancel_prebind_wait(state) -> None:
    """Stop a pre-bind that is still waiting for the game, if any."""
    task = getattr(state, "_prebind_task", None)
    if task is None or task.done():
        return
    task.cancel()
    try:
        await task
    except (asyncio.CancelledError, Exception):               # noqa: BLE001
        pass


async def _prebind_one(state, cid: int, add: bool) -> dict:
    """Claim one declared ruler; return {character_id, ok, reason} plus, when it
    landed, `bound_at` {tick, date} from the reply's clock stamp. Never raises."""
    params = {"character_id": cid, "disable_native_ai": False}
    if add:
        params["add"] = True
    try:
        res = await _action_bridge(state, "set_controlled_character", params)
    except Exception as exc:                                   # noqa: BLE001
        log.error("pre-bind to character %d FAILED to send: %s", cid, exc)
        return {"character_id": cid, "ok": False, "reason": str(exc)}
    # THE RESULT IS INSPECTED, NOT ASSUMED (the same file's lesson:
    # a write whose reply is never read leaves the client believing something
    # that did not happen). A refusal is recorded and the agent still runs —
    # it can still claim for itself, exactly as it does today.
    ok = bool(isinstance(res, dict) and res.get("success", True)
              and not res.get("reason"))
    reason = (res or {}).get("reason") if isinstance(res, dict) else None
    if ok and add and not (isinstance(res, dict) and res.get("added") is True):
        ok, reason = False, "add_unsupported_by_bridge"
    out = {"character_id": cid, "ok": ok, "reason": reason}
    if ok:
        # WHEN it landed. Every bridge frame stamps the live
        # clock (ipc_server.cpp build_envelope_clock_fragment); a reply without
        # one (a fake) records None, not a guess.
        tick = res.get("tick")
        date = res.get("game_date")
        out["bound_at"] = {"tick": tick if isinstance(tick, int) else None,
                           "date": date if isinstance(date, str) and date and date != "unknown" else None}
        log.info("pre-bound to character %d before first turn%s (tick %s, %s)", cid,
                 " (added)" if add else "", out["bound_at"]["tick"], out["bound_at"]["date"])
    else:
        log.error("pre-bind to character %d REFUSED: %s — the agent starts "
                  "without it and must claim for itself", cid, reason)
    return out


async def _send_and_wait(state: BridgeState, action: str, params: dict,
                          timeout: float = 15.0) -> dict:
    """Send a bridge action with request_id and wait for the response.

    Returns the raw bridge response dict.
    """
    if state.mock_mode:
        from mock_game_data import get_mock_query_result
        raw = get_mock_query_result(action, params)
        if isinstance(raw, str):
            try:
                return json.loads(raw)
            except (json.JSONDecodeError, TypeError):
                return {"data": raw}
        return raw if isinstance(raw, dict) else {"data": raw}

    request_id = str(uuid.uuid4())
    msg = {"action": action, "params": params, "request_id": request_id}

    future: asyncio.Future = asyncio.get_running_loop().create_future()
    state.pending_queries[request_id] = future

    try:
        await state._client.send_actions(state.tick_id, [msg])
    except Exception as e:
        state.pending_queries.pop(request_id, None)
        state.pending_acks.pop(request_id, None)
        state.pending_tickets.pop(request_id, None)
        raise ConnectionError(f"Failed to send: {e}") from e

    try:
        return await _await_with_ack_extension(state, request_id, future,
                                               timeout, action)
    finally:
        state.pending_acks.pop(request_id, None)
        state.pending_tickets.pop(request_id, None)


#: Added to the ack's own `flush_eta_ms` before it is used as a
#: deadline. The ETA is the time until the flush is DUE; serving the call and
#: getting the frame back across the socket happens after that, so a deadline of
#: exactly the ETA expires just as the work begins. Deliberately generous and
#: deliberately NOT tuned: this number decides whether a completed call is
#: reported as unconfirmed, and the cost of it being too large is a slower
#: refusal on a genuinely dead bridge, while the cost of it being too small is a
#: silent false negative in the episode record.
ACK_ETA_MARGIN_S = 5.0

#: The bridge's flush cadence, MIRRORED from
#: `bridge/include/flush_scheduler.h` (`period_ms = 10000`, a member
#: initialiser with no getenv) and CHECKED against it by
#: `test_mcp_confirmation.test_C5_the_flush_period_mirror_matches_the_header`.
#:
#: WHY A MIRROR AND NOT A READ. The experiment design registers
#: `CK3_EPISODE_FLUSH_PERIOD_MS` at 10000 with the note "withdrawn until a
#: reader exists". Making this client that reader would un-withdraw a
#: registered variable, which is the experiment design's call and not an
#: engineering one. So the number is mirrored and the mirror is tested — the
#: same shape as `registered_speed.py` and `bridge/include/registered_speed.h`,
#: and for the same reason: a mirror nobody checks is two homes wearing one name.
FLUSH_PERIOD_S = 10.0


class BridgeTimeout(TimeoutError):
    """A call that was not confirmed, CARRYING WHAT WAS OBSERVED ABOUT IT.

    `_action_bridge` used to catch every TimeoutError and return one
    sentence — "did not confirm within 8.0s" — for three outcomes that have
    three different repairs. A reader of `actions.jsonl` could not tell a
    bridge that never answered from one that ACCEPTED the call, minted a
    ticket and was still working on it: a census of the recorded calls could
    not answer that question with the evidence already thrown away at this line.

    The facts live here rather than in the message because a message is read by
    a person and these are read by a census. `_send_and_wait`'s `finally`
    clears `pending_acks` and `pending_tickets` before the caller can look, so
    they have to travel with the exception or not at all.
    """

    def __init__(self, message: str, *, reason_code: str, ack_seen: bool,
                 ticket: Optional[str] = None, ack_eta_s: Optional[float] = None,
                 waited_s: Optional[float] = None):
        super().__init__(message)
        #: One of NO_ACK / ACCEPTED_UNSETTLED / ACCEPTED_NO_RESULT. A closed
        #: vocabulary, because a census groups on it.
        self.reason_code = reason_code
        self.ack_seen = ack_seen
        self.ticket = ticket
        self.ack_eta_s = ack_eta_s
        self.waited_s = waited_s


#: The three ways an accepted-or-not call can fail to confirm. NOT spelled
#: `reason`: §1.1's `blocked` envelope has no `reason` field and
#: `arm_tool_invocation` already reads `gate`/`summary`/`raw_reason` off
#: refusals, so a fourth meaning for that one name is how a field stops being
#: readable. See [a-field-name-is-not-owned-by-one-emitter].
REASON_NO_ACK = "no_ack"
REASON_ACCEPTED_UNSETTLED = "accepted_unsettled"
REASON_ACCEPTED_NO_RESULT = "accepted_no_result"


async def _await_with_ack_extension(state: BridgeState, request_id: str,
                                    future: asyncio.Future,
                                    timeout: float, action: str = "") -> dict:
    """Wait for the settled frame, EXTENDING the deadline by what the bridge said.

    THE HAZARD THIS GUARDS AGAINST. In pipeline mode a TICK_WRITE is buffered and served
    on a flush. `flush_scheduler.h` holds `period_ms = 10000` as a member
    initialiser with no getenv, and the experiment design registers 10,000 ms
    with the note "no reader exists" — so the period is ten seconds in every
    episode, and episode_driver measured exactly 10.0 s/call against it. The
    deadline on this path is 8.0 s. A deadline below the period of the event it
    waits on cannot help firing, and the bridge's own §8.2 comment states the
    contract it is breaking: a ticket-ignorant client "just experiences the call
    as slower, BOUNDED BY THE FLUSH PERIOD".

    It is not every call, and the honest claim is narrower than "a write
    times out". `due_locked` has four early triggers besides the cadence —
    all-ANY, all-sessions-submitted, the declared reader's own submission, and
    the size bound — so a busy six-session arm flushes often and well inside
    8 s. What cannot be confirmed is a call that lands in a QUIET window: no
    other session submitting, no privileged-reader submission, the buffer below
    its size bound. That state is ordinary and reachable, and it is what a
    `sent_no_confirmation` on an accepted post looks like from here.

    SO THE DEADLINE IS TAKEN FROM THE BRIDGE AND NOT FROM THIS FILE. A literal
    here would be this client's guess at another process's cadence — the same
    shape as a validator owning the constant it judges against. `flush_eta_ms`
    is that process's own answer, per call, and it is already on the wire.

    THE BASE TIMEOUT STILL BOUNDS A SILENT BRIDGE. Nothing is extended unless an
    ack actually arrived, so a legacy-mode bridge, a disconnected one, or one
    that never accepted the call fails in `timeout` seconds exactly as before.
    """
    try:
        # SHIELDED: a plain wait_for CANCELS the future it was waiting on, so
        # the second wait below would be on a dead object and a result already
        # in flight would be dropped — which is the bug this function exists to
        # stop, reintroduced by the fix for it.
        return await asyncio.wait_for(asyncio.shield(future), timeout=timeout)
    except asyncio.TimeoutError:
        pass

    eta = state.pending_acks.get(request_id)
    if eta is None:
        # NOBODY ACCEPTED THIS CALL. Not an extension case: a bridge that never
        # acked has told us nothing about when to expect an answer, and waiting
        # longer on no evidence is how a dead bridge reads as a slow one.
        state.pending_queries.pop(request_id, None)
        if not future.done():
            future.cancel()
        raise BridgeTimeout(f"Bridge did not respond within {timeout}s",
                            reason_code=REASON_NO_ACK, ack_seen=False,
                            waited_s=timeout)

    # The ack's ETA is measured from when the BRIDGE sent it, and we have
    # already spent `timeout` seconds since then — so the remainder is what is
    # left of it, never the whole ETA again.
    # THE OVERALL BUDGET is still the bridge's own eta plus the margin: the
    # result cannot physically arrive before the flush, so that is how long a
    # correct bridge may take.
    ticket = state.pending_tickets.get(request_id)
    extra = max(0.0, eta - timeout) + ACK_ETA_MARGIN_S

    # AND A WRITE CAN MISS THE FLUSH IT WAS QUOTED. The eta is the time
    # until the next flush is DUE, not a promise that THIS write is served on
    # it: `flush_scheduler` caps tick calls per flush and a read-lane flush
    # hands them back, so an accepted write can be deferred to the following
    # period. An ack quoting eta 0 — the flush is due now — then yields a
    # budget of just the margin, which cannot cover one deferral, and the call
    # reports `sent_no_confirmation` for a write that lands seconds later.
    #
    # So once the bridge has given us a TICKET, the floor is a whole period
    # plus the margin. Gated on the ticket deliberately: a call nobody accepted
    # is still bounded by `timeout` exactly as before, and an inline ANY query
    # never reaches this.
    if ticket:
        extra = max(extra, FLUSH_PERIOD_S + ACK_ETA_MARGIN_S - timeout)
    deadline = asyncio.get_running_loop().time() + extra

    # BUT THE FIRST CHECK DOES NOT WAIT FOR IT, and that is pipeline_correlation's
    # rule rather than a choice of mine: `push_deadline = clock() + min(eta +
    # PUSH_GRACE_S, FIRST_CHECK_S)`, with the reason written beside it — "a
    # flush period is seconds, and a client that simply waits it out is a client
    # that cannot tell 'not flushed yet' from 'my push was lost', which is the
    # state the readiness probe was in when it declared a live world
    # dead. So ask early, once."
    #
    # A check is ANY-polarity and served INLINE, so asking costs a socket round
    # trip and not a flush. And the reply carries the ticket's OWN remaining
    # eta, so the poll interval corrects itself to the real period immediately:
    # two round trips in the worst case, not a poll storm. Waiting the full
    # eta + margin before asking would make a lost push cost
    # five seconds even when the ack had quoted 50 ms.
    push_wait = min(eta + _pcorr.PUSH_GRACE_S, _pcorr.FIRST_CHECK_S)
    try:
        return await asyncio.wait_for(asyncio.shield(future), timeout=push_wait)
    except asyncio.TimeoutError:
        pass

    # ── THE PUSH HAD ITS CHANCE. ASK. ──────────────────────────────
    #
    # Waiting alone is not a settlement protocol; it is a hope —
    # pipeline_correlation's words, written when the test host's readiness probe read
    # one frame out of phase and reported a live world dead. The result frame is
    # BROADCAST from the game thread, and a client can miss it: a reconnect
    # between the ack and the flush, a recv budget shorter than the eta, a
    # listener restarted mid-call. `check` is ANY-polarity, is served inline and
    # never waits for a flush, so it cannot deadlock behind the very flush we
    # are waiting on.
    if ticket:
        settled = await _settle_by_check(state, request_id, future, ticket,
                                         action, deadline)
        if settled is not None:
            return settled

    # THE CHECKS ARE EXHAUSTED; THE BUDGET IS NOT. This tail is not
    # the `else` of the branch above — reachable only when there was NO ticket —
    # so the one case where the bridge had told us most about the call was the
    # one case we stopped listening in. `_settle_by_check` returns None when it
    # ran out of ATTEMPTS, not when the ticket is dead: every None it returns is
    # a live-or-unknown ticket, and the push may still be in flight. Raising
    # here threw away up to several seconds of a budget we had already decided
    # was legitimate, and the result then arrived and was dropped on the floor.
    #
    # An ack without a ticket settles nothing either, and the same reasoning
    # applies, so the two paths share one tail instead of disagreeing.
    left = deadline - asyncio.get_running_loop().time()
    if left > 0:
        try:
            return await asyncio.wait_for(asyncio.shield(future), timeout=left)
        except asyncio.TimeoutError:
            pass

    state.pending_queries.pop(request_id, None)
    if not future.done():
        future.cancel()
    # THE THREE OUTCOMES ARE SPELLED DIFFERENTLY, because they have three
    # different repairs: nothing accepted this call; it was accepted and we
    # could not settle it; it was accepted and is still running.
    if ticket:
        raise BridgeTimeout(
            f"Bridge ACCEPTED {action} as ticket {ticket} (flush eta "
            f"{eta:.1f}s), the push never arrived, `check` did not settle it, "
            f"and nothing landed within {timeout + extra:.1f}s",
            reason_code=REASON_ACCEPTED_UNSETTLED, ack_seen=True,
            ticket=ticket, ack_eta_s=eta, waited_s=timeout + extra)
    raise BridgeTimeout(
        f"Bridge ACCEPTED the call (flush eta {eta:.1f}s) and did not "
        f"deliver its result within {timeout + extra:.1f}s",
        reason_code=REASON_ACCEPTED_NO_RESULT, ack_seen=True,
        ticket=None, ack_eta_s=eta, waited_s=timeout + extra)


#: How many times a still-running ticket is re-asked before giving up.
#: BOUNDED, and small: a check reply for a live ticket carries that ticket's own
#: remaining `flush_eta_ms`, so the interval corrects itself to the real flush
#: period after the first round trip. Two round trips is the expected worst
#: case; this is the cap that stops a poll storm if the bridge stops quoting.
CHECK_ATTEMPTS = 4
#: Per-check reply budget. A check is served INLINE, so it is a socket
#: round trip and not a flush wait; a second is generous.
CHECK_REPLY_S = 2.0

#: The poll interval for a ticket the bridge says is LIVE while
#: quoting no time to its flush — eta 0, or no eta at all.
#:
#: Read eta 0 literally and it means "ask again in no time", so the four
#: attempts burned a single second and the recovery gave up while the flush it
#: was waiting for had not come round. But eta 0 on a ticket that is still
#: pending does not mean imminent: it means the flush is ALREADY DUE and this
#: write was not served on it — deferred by the per-flush tick-call cap, or by
#: a read-lane flush handing tick calls back. The next opportunity is up to a
#: whole period away.
#:
#: So the attempts are spread across one flush period instead of one second:
#: FLUSH_PERIOD_S / CHECK_ATTEMPTS. The cap then bounds a POLL STORM, which is
#: what it was for, rather than bounding the wait.
OVERDUE_POLL_S = FLUSH_PERIOD_S / CHECK_ATTEMPTS


async def _settle_by_check(state: BridgeState, request_id: str,
                           future: asyncio.Future, ticket: str,
                           action: str, deadline: float) -> Optional[dict]:
    """Ask the bridge to settle one ticket. The rebuilt result frame, or None.

    EVERY RULE HERE IS `pipeline_correlation`'S, NOT A SECOND READING OF §8.2:
    the envelope, which ticket states are terminal and which are live, the poll
    interval, and the reconstruction of the result frame all come from that
    module. What is local is the CONCURRENCY SHAPE, and only because it has to
    be: `pipeline_correlation.settle()` owns its own `recv` loop, and this
    process drains ONE socket through a background listener on behalf of many
    in-flight calls. A `settle()` here would steal frames belonging to other
    callers. So the loop is this file's and the vocabulary is the module's.

    THE PUSH IS STILL PREFERRED ALL THE WAY THROUGH. The caller's future stays
    live and is checked on every pass: if the broadcast lands late — the common
    case when a flush simply ran long — that frame wins, and it carries the
    attribution spine verbatim rather than reconstructed.
    """
    loop = asyncio.get_running_loop()
    for _ in range(CHECK_ATTEMPTS):
        if future.done():
            return future.result()
        # BOUNDED BY THE BUDGET AND BY THE ATTEMPT CAP, both. The cap alone
        # would let a bridge that answers every check slowly outlive the
        # caller's deadline; the budget alone would let a fast-answering bridge
        # that never settles spin.
        if loop.time() >= deadline:
            return None

        check_rid = str(uuid.uuid4())
        check_future: asyncio.Future = asyncio.get_running_loop().create_future()
        state.pending_queries[check_rid] = check_future
        envelope = _pcorr.build_check_envelope(ticket, check_rid)
        try:
            await state._client.send_actions(state.tick_id, envelope["actions"])
        except Exception:                                      # noqa: BLE001
            state.pending_queries.pop(check_rid, None)
            return None
        try:
            reply = await asyncio.wait_for(check_future, timeout=CHECK_REPLY_S)
        except asyncio.TimeoutError:
            state.pending_queries.pop(check_rid, None)
            # The check itself went unanswered. Not a verdict about the ticket —
            # it is a verdict about the socket, and the caller's future may yet
            # resolve, so the loop tries again rather than concluding.
            continue

        entry = _pcorr.check_entry_for(reply, ticket)
        if entry is None:
            # A `check` reply that does not carry OUR ticket. The bridge
            # broadcasts, so this is somebody else's; ask again.
            continue
        st = entry.get("state")
        # THE PUSH IS ASKED ABOUT AGAIN HERE, and this is not belt-and-braces.
        # A broadcast can land while the check reply is in flight. Taking the
        # terminal entry without looking would CANCEL a future that already
        # holds the real frame and hand the caller a reconstruction instead —
        # both correct, but the push carries the §2.8 attribution spine
        # verbatim rather than rebuilt from the fields `check` happens to
        # report. A surviving mutant found this: removing the loop-top
        # `future.done()` guard changed nothing, because R5's push arrived
        # during the LIVE wait and never through this path. A guard only at the
        # loop's top is in the wrong place.
        if future.done():
            return future.result()
        if st in _pcorr.TERMINAL_STATES:
            # SETTLED. Rebuilt into the shape the push would have carried, so
            # no consumer downstream needs a second code path — and stamped
            # `settled_via: "check"` by the module, so a run can count how often
            # the push was missed instead of guessing.
            if isinstance(entry.get("result"), dict):
                frame = _pcorr.result_from_check_entry(entry, request_id, action)
            else:
                # A ticket that ended with no payload is a TYPED FAILURE and not
                # silence. Returning None here would send the caller to the
                # timeout path and report an expired ticket as an unanswered
                # bridge.
                frame = _pcorr.refusal_from_check_entry(entry, request_id, action)
            state.pending_queries.pop(request_id, None)
            if not future.done():
                future.cancel()
            return frame
        if st in _pcorr.LIVE_STATES:
            # STILL RUNNING, so the call is fine and the flush has not come
            # round. Wait the ticket's OWN remaining eta when it quotes one —
            # the bridge correcting our poll interval to its real period — and
            # the module's floor when it does not.
            eta_ms = entry.get("flush_eta_ms")
            quoted = (isinstance(eta_ms, (int, float))
                      and not isinstance(eta_ms, bool) and float(eta_ms) > 0)
            if quoted:
                wait = max(_pcorr.CHECK_POLL_S, float(eta_ms) / 1000.0)
            else:
                # LIVE AND QUOTING NOTHING IS OVERDUE, NOT IMMINENT.
                # A missing eta and an eta of 0 are the same statement here —
                # the bridge cannot say when this ticket's flush will come —
                # and the superseded code read both as `CHECK_POLL_S`, which spent the
                # attempt cap in one second on a write that had been deferred
                # to a flush up to ten seconds away.
                wait = OVERDUE_POLL_S
            wait = min(wait, max(0.0, deadline - loop.time()))
            if wait <= 0:
                return None
            try:
                return await asyncio.wait_for(asyncio.shield(future), timeout=wait)
            except asyncio.TimeoutError:
                continue
        # A state this client does not know. NOT read as terminal and not read
        # as live: ask again, and let the attempt cap end it. Guessing either way
        # would invent an outcome for a ticket the bridge described in a
        # vocabulary newer than this reader's.
    return None


async def _query_bridge(state: BridgeState, action: str, params: dict,
                         timeout: float = 15.0) -> dict | str:
    """Send a query to the bridge and return its reply."""
    # the reply as the bridge wrote it: no bridge reply carries a `data` or
    # `message` wrapper (so there is no unwrap that looks for one)
    return await _send_and_wait(state, action, params, timeout)


async def _action_bridge(state: BridgeState, action: str, params: dict,
                          timeout: float = 5.0) -> dict | str:
    """Send an action to the bridge and return the response."""
    state.action_log.append({"tick": state.tick_id, "action": action, "params": params})
    if len(state.action_log) > 100:
        state.action_log = state.action_log[-50:]

    if state.mock_mode:
        return {"status": "sent", "action": action, "current_tick": state.tick_id}

    try:
        result = await _send_and_wait(state, action, params, timeout)
        return result
    except TimeoutError as exc:
        # SAY WHICH OF THE THREE THINGS HAPPENED, AND WHAT WAS SEEN.
        # This used to flatten every timeout into one sentence quoting
        # `timeout`, which was not even the budget actually waited — the ack
        # extension spends `timeout + extra`. Two facts were thrown away at
        # this line: whether the bridge ACCEPTED the call, and the ticket it
        # minted. Both are what a reader of a sealed bundle needs to tell "the
        # bridge never heard me" from "the bridge took my write and I stopped
        # listening", and a census without them cannot tell the two apart.
        #
        # `status` is UNCHANGED. `arm_tool_invocation` keys the unconfirmed
        # state on it and a census of sealed episodes compares across deploys;
        # renaming it would silently re-bucket every historical row. The new
        # facts are additive.
        waited = getattr(exc, "waited_s", None)
        out = {
            "status": "sent_no_confirmation",
            "action": action,
            "current_tick": state.tick_id,
            "reason_code": getattr(exc, "reason_code", "unknown"),
            "ack_seen": getattr(exc, "ack_seen", None),
            "ticket": getattr(exc, "ticket", None),
            "ack_eta_s": getattr(exc, "ack_eta_s", None),
            "waited_s": waited,
            "warning": str(exc) or (
                f"Action sent but bridge did not confirm within {timeout}s."),
        }
        return out


# ---------------------------------------------------------------------------
# query
# ---------------------------------------------------------------------------

#: A default query reply's size target (8 KB, measured on recorded states), and the
#: cap for a reply whose rows the caller asked for (max_results / limit), well under what a seat's client
#: refuses to pass (125,588 characters lost)
QUERY_REPLY_CAP = _TT.T.QUERY_REPLY_KB * 1024
QUERY_REPLY_CAP_ASKED = 32768
#: The bridge parameters of a query that name a character (tools.QUERY_DOMAIN_MAP's builders), each read alive
#: before the domain is read; and the domains a dead character's id still answers in full (his profile, his
#: memories, his dynasty)
QUERY_CHARACTER_KEYS = ("character_id", "target_id", "actor_id", "character_id_a", "character_id_b")
QUERY_DEAD_READ_OK = frozenset({"character", "memories", "dynasty"})


def fit_reply(reply, cap: int):
    """A reply over `cap` bytes (compact JSON) loses rows from the end of its biggest list (at any depth),
    again until it fits; `truncated` names each list cut, the rows shown and the rows there were. Under
    the cap it is unchanged."""
    import json as _json

    def size(o):
        return len(_json.dumps(o, default=str))
    if not isinstance(reply, dict):
        return reply
    try:
        first = size(reply)
    except (TypeError, ValueError):
        return reply
    if first <= cap:
        return reply
    out = _json.loads(_json.dumps(reply, default=str))
    cuts: dict = {}

    def lists(o, path=()):
        items = o.items() if isinstance(o, dict) else enumerate(o)
        for k, v in items:
            if isinstance(v, list):
                if v:
                    yield path + (k,), o, k, v
                yield from lists(v, path + (k,))
            elif isinstance(v, dict):
                yield from lists(v, path + (k,))
    room = cap - 800                        # the `truncated` block's own bytes
    for _ in range(64):
        if size(out) <= room:
            break
        def weight(t):
            # a list whose bulk is one row of its own is cut inside that row, not by dropping it
            whole = size(t[3])
            return 0 if len(t[3]) > 0 and max(size(r) for r in t[3]) * 2 > whole and \
                isinstance(max(t[3], key=size), (dict, list)) and len(t[3]) > 1 or \
                (len(t[3]) == 1 and isinstance(t[3][0], (dict, list))) else whole
        found = max(lists(out), key=weight, default=None)
        if found is None or weight(found) == 0:
            found = max(lists(out), key=lambda t: size(t[3]), default=None)
        if found is None:
            break
        path, holder, k, rows = found
        name = ".".join(str(p) for p in path)
        n = cuts.get(name, {}).get("of", len(rows))
        budget = size(rows) - (size(out) - room)
        lo, hi = 0, len(rows) - 1          # the most rows whose list fits the budget
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if size(rows[:mid]) <= budget:
                lo = mid
            else:
                hi = mid - 1
        keep = lo
        holder[k] = rows[:keep]
        cuts[name] = {"list": name, "shown": max(keep, 0), "of": n}
    out["truncated"] = {"lists": list(cuts.values()), "why": f"the reply was {first} bytes; at most {cap} are sent",
                        "narrow": "max_results, filter or a title_id/character_id narrows it"}
    if len(cuts) == 1:
        out["truncated"].update(next(iter(cuts.values())))
    return out


@mcp.tool(description=_TT.QUERY)
async def query(
    domain: Annotated[str, Field(description="One domain.", json_schema_extra={"enum": QUERY_DOMAINS})] = "",
    domains: Annotated[
        list[str],
        Field(description=f"Up to {QUERY_BATCH_MAX} domains at once; not with domain.",
              json_schema_extra={"items": {"type": "string", "enum": QUERY_DOMAINS}},
              max_length=QUERY_BATCH_MAX),
    ] = None,
    character_id: Annotated[int, Field(description="The character read (0 = yours).")] = 0,
    target_id: Annotated[int, Field(description="The other character: opinion, opinion_breakdown, "
                                                "diplomatic_range, relative_power.")] = 0,
    title_id: Annotated[int | str, Field(description="The title (a key or an id): titles, de_jure, "
                                                     "election, claimants, title_history, title_succession, "
                                                     "neighbors (a county), adjacency, development, buildings; "
                                                     "conquest_targets: conquer toward it.")] = 0,
    trait_id: Annotated[int, Field(description="trait_compatibility.")] = 0,
    include_vassals: Annotated[bool, Field(description="claims: your vassals' claims too.")] = False,
    war_id: Annotated[int, Field(description="war_score: one war (0 = every war).")] = 0,
    culture_id: Annotated[int, Field(description="innovation_progress, fascination_progress, "
                                                 "research_speed (0 = yours).")] = 0,
    dynasty_id: Annotated[int, Field(description="dynasty (0 = yours).")] = 0,
    filter: Annotated[str, Field(description="characters: " + " | ".join(_TT.T.CHARACTER_FILTERS)
                                             + " (default alive); event_log: a comma list of war, "
                                             "scheme, council_task; buildings: a name part.")] = "",
    sort: Annotated[str, Field(description="characters and marriage_candidates: the order (default "
                                           "military). realm_size = titles held directly; "
                                           "realm_counties = counties in the realm.",
                               json_schema_extra={"enum": [""] + list(_TT.T.CHARACTER_SORTS)})] = "",
    sort_order: Annotated[str, Field(description="characters (default desc).",
                                     json_schema_extra={"enum": ["", "desc", "asc"]})] = "",
    max_results: Annotated[int, Field(description="Row cap: characters, marriage_candidates, "
                                                  "conquest_targets.")] = 0,
    court_of: Annotated[int, Field(description="characters: only those whose liege is this ruler "
                                               "(his courtiers and direct vassals).")] = 0,
    female: Annotated[StrictBool | None, Field(description="characters: true women, false men, unset "
                                                           "both.")] = None,
    since_tick: Annotated[int, Field(description="Feeds: only records after this tick.")] = 0,
    limit: Annotated[int, Field(description="Feeds: at most this many records; title_history: holders.")] = 0,
    require_in_range: Annotated[bool, Field(description="marriage_candidates: only in diplomatic range "
                                                        "(false: anywhere).")] = True,
    verbose: Annotated[bool, Field(description="The bridge's full reply (more fields per row) for "
                                               "characters, wars, council, armies, realm, claims, "
                                               "conquest_targets.")] = False,
    detail: Annotated[str, Field(description="map_graph: brief drops distances and the live overlay; "
                                             "conquest_targets: full gives every field per row.",
                                 json_schema_extra={"enum": ["", "brief", "full"]})] = "",
) -> dict[str, Any]:
    state = _get_state()
    err = await _ensure_connected(state, "query")
    if err:
        return err
    gw = await _gateway_now()             # the envelope's game block: a fresh clock reading
    if domain and domains:
        return _blocked("query", state, "invalid_argument",
                        "pass domain=<one domain> or domains=[...], not both")
    # The bridge reads an unknown sort as military and an unknown
    # filter as alive; refused here instead of answered about something else.
    _browse = {domain} | set(domains or [])
    if sort and sort not in _TT.T.CHARACTER_SORTS and _browse & {"characters", "marriage_candidates"}:
        return _blocked("query", state, "invalid_argument",
                        f"sort={sort!r} is not one of {', '.join(_TT.T.CHARACTER_SORTS)}; nothing was read")
    if filter and "characters" in _browse and filter not in _TT.T.CHARACTER_FILTERS:
        return _blocked("query", state, "invalid_argument",
                        f"filter={filter!r} is not one of {', '.join(_TT.T.CHARACTER_FILTERS)}; nothing was read")
    title_echo: list = []
    if isinstance(title_id, str):           # a title key names the title
        import title_refs as _trefs
        title_id, _row, _blk = await _trefs.resolve(gw, title_id, "title_id")
        if _blk is not None:
            return _refused("query", state, _blk)
        title_id = title_id or 0
        title_echo = [_row] if _row else []

    params = {
        "character_id": character_id, "target_id": target_id,
        "title_id": title_id, "trait_id": trait_id,
        "include_vassals": include_vassals,
        "war_id": war_id, "culture_id": culture_id,
        "dynasty_id": dynasty_id,
        # Character-browser params (domain="characters" → search_characters).
        "filter": filter, "sort": sort,
        "sort_order": sort_order, "max_results": max_results,
        "require_in_range": 1 if require_in_range else 0,
    }
    # Only thread verbose through when explicitly requested, so the lean
    # bridge-side default applies otherwise. The param builders in tools.py emit
    # "verbose" only when present in p.
    if verbose:
        params["verbose"] = True
    # since_tick/limit thread through to the feed domains (war_declarations,
    # scheme_inits, council_tasks, event_log) only when explicitly set.
    if court_of:
        params["court_of"] = court_of
    # The caller's value, uncoerced: StrictBool refuses a non-boolean
    # ("yes", 1) at the MCP boundary, and a value that reaches here another way
    # still meets the bridge's own female_must_be_boolean refusal.
    if female is not None:
        params["female"] = female
    if since_tick:
        params["since_tick"] = since_tick
    if limit:
        params["limit"] = limit
    if detail:
        params["detail"] = detail

    from tools import resolve_query

    async def one(d: str):
        """(data, errors, refusal): one domain's reply, the rows a composed domain
        could not read, and the why_blocked when the domain was not answered."""
        if d == "event_log":
            data, errs = await _query_event_log(state, since_tick, limit, filter)
            return data, errs, None
        if d == "relative_power":
            unbound = await redesign.unbound_refusal(gw, "query", "relative_power",
                                                      {"character_id": character_id})
            if unbound is not None:
                return None, [], unbound["blocked"]
            return await _query_relative_power(state, character_id, target_id)
        if d == "buildings":
            # (125,588 characters, the whole building database whatever the title)
            import buildings_read as _br
            me = character_id
            if title_id:
                actor, err = await redesign._resolve_actor(gw, character_id, "query")
                if err is not None:
                    return None, [], err.get("blocked")
                me = redesign._as_int((actor or {}).get("id")) or character_id
            return (*(await _br.buildings_reply(gw, me, title_id or 0, filter, max_results)), None)
        if d == "conquest_targets" and detail not in CONQUEST_TARGETS_DETAILS:
            return None, [], redesign.why_blocked(
                "invalid_argument",
                summary=f"detail='{detail}' is not a conquest_targets detail; "
                        "pass detail='full' or leave it out")
        resolved = resolve_query(d, params)
        if resolved is None:
            return None, [], redesign.why_blocked(
                "unknown_domain", summary=f"unknown domain '{d}'; valid: {', '.join(QUERY_DOMAINS)}")
        action, bridge_params = resolved
        # A domain about the bound ruler, with none bound, answers
        # the refusal every actor-bearing tool answers.
        unbound = await redesign.unbound_refusal(gw, "query", action, bridge_params)
        if unbound is not None:
            return None, [], unbound["blocked"]
        if d not in QUERY_DEAD_READ_OK:
            # A character this read names who is dead is said first, before any other check (a marriage read of
            # a son the news had reported dead answered actor_capital_unresolved)
            named = {f"{k}": v for k, v in bridge_params.items()
                     if k in QUERY_CHARACTER_KEYS and redesign._as_int(v)}

            async def _read_char(a, p):
                return await _query_bridge(state, a, p)
            dead = await redesign.dead_among(_read_char, named) if named else None
            if dead is not None:
                return None, [], redesign.dead_character_blocked(dead["id"], dead["sect"], dead["field"])
        try:
            reply = await _query_bridge(state, action, bridge_params)
        except Exception as e:                           # noqa: BLE001
            f = _read_failure(d, e)
            return None, [], redesign.why_blocked(f["code"], summary=f["detail"])
        declined = _declined(reply, action)
        if declined is not None:
            return None, [], declined
        if d == "character":
            reply = await _reread_pending_name(state, action, bridge_params, reply)
        if d == "marriage_candidates":
            async def _read(a, p):
                return await _query_bridge(state, a, p)
            reply = await redesign.marriage_rows(reply, _read)   # A pair per row
        if d == "alliances":
            async def _read(a, p):
                return await _query_bridge(state, a, p)
            reply = await redesign.alliance_rows(reply, _read)   # A vassal or liege among them, marked
        if d == "conquest_targets" and not verbose:
            from tools import compact_conquest_targets
            reply = compact_conquest_targets(reply, max_results, detail)
        if d == "wars":
            reply = redesign.wars_with_totals(reply)
        if d == "council_tasks":
            reply = redesign.council_task_rows(reply, (gw.game() or {}).get("date"))   # the council tasks' targets and dates
        if d == "vassal_loyalty":
            reply = _fit.compact_vassal_loyalty(reply)              # All 30 rows in 8 KB
        # No domain's reply grows past the size a seat's client passes it whole
        reply = fit_reply(reply, QUERY_REPLY_CAP_ASKED if (max_results or limit or verbose) else QUERY_REPLY_CAP)
        return reply, [], None

    # Batch mode: every domain's reply under its own key; a domain that failed is
    # an errors[] row naming it, so a partial batch is ok=false, never a success.
    if domains:
        if len(domains) > QUERY_BATCH_MAX:
            return _blocked("query", state, "invalid_argument",
                            f"a batch is limited to {QUERY_BATCH_MAX} domains; got {len(domains)}")
        data = {}
        errors = []
        for d in domains:
            got, errs, refused = await one(d)
            errors.extend({"code": e["code"], "detail": f"{d}: {e['detail']}"} for e in errs)
            if refused is not None:
                errors.append({"code": refused["gate"], "detail": f"{d}: {refused['summary']}"})
            else:
                data[d] = got
        import title_refs as _trefs
        return _trefs.attach(_ok("query", state, data, errors=errors), title_echo)

    if not domain:
        return _blocked("query", state, "missing_required_argument",
                        "pass domain=<one domain> or domains=[up to "
                        f"{QUERY_BATCH_MAX}]; valid: {', '.join(QUERY_DOMAINS)}")
    got, errs, refused = await one(domain)
    if refused is not None:
        return _refused("query", state, refused)
    import title_refs as _trefs
    return _trefs.attach(_ok("query", state, got, errors=errs), title_echo)


#: The pauses before each re-read of a character whose
#: name the bridge has queued for the game thread (name_tables.cpp: a cache miss
#: answers name_key + name_pending and queues the id; the drain after the next
#: game-thread pass, paused or running, resolves it). ~1.5 s in all.
NAME_PENDING_REREADS_S = (0.25, 0.25, 0.5, 0.5)
NAME_PENDING_NOTE = ("the game has not localized this name yet (the bridge queued it for the "
                     "game thread); read the character again in a moment")


async def _reread_pending_name(state, action: str, bridge_params: dict, reply):
    """query(domain="character") for an id the bridge has not named yet: re-read it
    while the drain resolves the name (12015 and 11329 came back with
    name_key + name_pending on the first read of the session, and `name` absent,
    while the same ids were named by later reads). A name still pending after the
    re-reads is said: `name` null beside name_pending and a note, never a key."""
    def pending(r):
        return isinstance(r, dict) and r.get("name_pending") and not r.get("name")
    if not pending(reply):
        return reply
    for pause in NAME_PENDING_REREADS_S:
        await asyncio.sleep(pause)
        try:
            again = await _query_bridge(state, action, bridge_params)
        except Exception:                                # noqa: BLE001
            break
        if _declined(again, action) is not None or not isinstance(again, dict):
            break
        reply = again
        if not pending(reply):
            return reply
    return {**reply, "name": None, "name_note": NAME_PENDING_NOTE}


async def _query_event_log(state, since_tick: int, limit: int, filter: str):
    """query(domain="event_log"): a synthetic "what happened since I last looked"
    feed. It merges the bridge's since_tick hook buffers (war declarations, scheme
    inits, council tasks) into one stream, newest first. Call it once per cycle with
    the previous reply's current_tick as since_tick; `filter` is a comma list of
    kinds (war, scheme, council_task). Returns (data, errors): a feed that did not
    answer is an errors[] row and the others still report."""
    lim = limit if limit > 0 else 100
    st = since_tick
    kinds = {k.strip() for k in filter.split(",")} if filter else None
    feeds = [
        ("query_war_declarations", "declarations", "war"),
        ("query_scheme_inits",     "scheme_inits", "scheme"),
        ("query_council_tasks",    "assignments",  "council_task"),
    ]
    events = []
    errors = []
    cur_tick = 0
    for action, list_key, etype in feeds:
        if kinds and etype not in kinds:
            continue
        try:
            res = await _query_bridge(state, action, {"since_tick": st, "limit": lim})
        except Exception as e:                           # noqa: BLE001
            errors.append(_read_failure(etype, e))
            continue
        declined = _declined(res, action)
        if declined is not None:
            errors.append({"code": declined["gate"], "detail": f"{etype}: {declined['summary']}"})
            continue
        if not isinstance(res, dict):
            continue
        cur_tick = max(cur_tick, int(res.get("current_tick") or 0))
        for rec in (res.get(list_key) or []):
            if isinstance(rec, dict):
                e = dict(rec)
                e["kind"] = etype
                events.append(e)
    events.sort(key=lambda r: int(r.get("tick") or 0), reverse=True)
    truncated = len(events) > lim
    events = events[:lim]
    return {
        "action": "event_log",
        "current_tick": cur_tick,
        "since_tick": st,
        "event_count": len(events),
        "truncated": truncated,
        "events": events,
    }, errors


async def _query_relative_power(state, character_id: int, target_id: int):
    """query(domain="relative_power"): my strength (+allies) against a target
    (+their allies), and against my largest faction, from query_alliances and
    query_faction_threat. Returns (data, errors, refusal): without my own
    strength there is no comparison, so that read failing is the refusal."""
    try:
        me = await _query_bridge(state, "query_alliances", {"character_id": character_id})
    except Exception as e:                               # noqa: BLE001
        f = _read_failure("query_alliances", e)
        return None, [], redesign.why_blocked(f["code"], summary=f["detail"])
    declined = _declined(me, "query_alliances")
    if declined is not None:
        return None, [], declined
    me = me if isinstance(me, dict) else {}
    my_str = float(me.get("own_military_strength") or 0)
    my_callable = float(me.get("total_callable_strength") or my_str)
    out = {"action": "relative_power", "character_id": character_id,
           "my_strength": my_str, "my_callable_strength": my_callable}
    errors = []
    if target_id:
        try:
            t = await _query_bridge(state, "query_alliances", {"character_id": target_id})
        except Exception as e:                           # noqa: BLE001
            errors.append(_read_failure("query_alliances(target)", e))
            t = {}
        t = t if isinstance(t, dict) else {}
        t_str = float(t.get("own_military_strength") or 0)
        t_callable = float(t.get("total_callable_strength") or t_str)
        ratio = my_callable / t_callable if t_callable > 0 else float("inf")
        out.update({"target_id": target_id, "target_strength": t_str,
                    "target_callable_strength": t_callable,
                    "offensive_ratio": round(ratio, 2),
                    "offensive_verdict": ("winnable_on_tempo" if ratio >= 1.5
                                          else "parity_defense_only" if ratio >= 0.9 else "unfavorable")})
    try:
        ft = await _query_bridge(state, "query_faction_threat", {"character_id": character_id})
    except Exception as e:                               # noqa: BLE001
        errors.append(_read_failure("query_faction_threat", e))
        ft = {}
    ft = ft if isinstance(ft, dict) else {}
    import faction_letters as _fl  # noqa: PLC0415 - the one faction power (the game's, threshold 80)
    largest = max([_fl.faction_power(f) or 0.0 for f in (ft.get("factions") or [])], default=0.0)
    internal = (100.0 / largest) if largest > 0 else float("inf")
    out.update({"largest_faction_power_pct": largest,
                "internal_ratio": (round(internal, 2) if internal != float("inf") else None),
                "internal_safe": internal >= 1.25,
                "hint": "offensive_ratio>=1.5 means winnable; internal_ratio<1.25 means suspend external wars"})
    return out, errors, None


# ---------------------------------------------------------------------------
# discover
# ---------------------------------------------------------------------------

async def _discover_targets(state, action: str, actor_id: int,
                            title_id: int) -> dict[str, Any]:
    """Execute a discover(what='targets', action=...) plan: enumerate the valid
    targets for an action, annotating per-target validity where available."""
    from tools import resolve_targets_plan
    plan = resolve_targets_plan(action, {"actor_id": actor_id, "title_id": title_id})

    if plan.get("mode") == "error":
        return _blocked("discover", state, plan.get("gate", "unknown_verb"),
                        plan.get("reason", "unsupported action"))

    try:
        if plan["mode"] == "imprison_targets":
            # The one imprison judge: the game's send check per target, the rows
            # do(action_class='imprison_targets') gives (redesign.imprison_target_rows).
            gw = _get_gateway()
            cid = actor_id
            if not cid:
                # The check is the actor's: resolve "the controlled ruler" first.
                actor, err = await redesign._resolve_actor(gw, 0, "discover")
                if err is not None:
                    return err
                cid = int((actor or {}).get("id") or 0)
            available, unavailable, refused = await redesign.imprison_target_rows(gw, cid)
            if refused is not None:
                return _refused("discover", state, refused)
            return _ok("discover", state, {
                "action": action, "label": plan.get("label"),
                "target_count": len(available) + len(unavailable),
                "available": available, "unavailable": unavailable,
            })
        if plan["mode"] == "single":
            result = await _query_bridge(state, plan["bridge_action"], plan["params"])
            declined = _declined(result, plan["bridge_action"])
            if declined is not None:
                return _refused("discover", state, declined)
            return _ok("discover", state, {"action": action, **(result if isinstance(result, dict) else {"result": result})})

        # pool_annotate: enumerate the candidate pool, then annotate each entry.
        pool = plan["pool"]
        pool_resp = await _query_bridge(state, pool["bridge_action"], pool["params"])
        declined = _declined(pool_resp, pool["bridge_action"])
        if declined is not None:
            return _refused("discover", state, declined)
        items = (pool_resp or {}).get(pool["list_key"], []) if isinstance(pool_resp, dict) else []
        annotate = plan.get("annotate")
        targets = []
        # Bound the per-target fan-out to keep the call responsive.
        cap = 40
        for entry in items[:cap]:
            tid = entry.get(pool["id_key"])
            out = {"target_id": tid, "name": entry.get(pool.get("name_key", "name"))}
            # A row the bridge has not localized carries no name
            # but `name_key` + `name_pending`; pass the state on, never the key as a name.
            if entry.get("name_pending") and not out["name"]:
                out["name_pending"] = True
                out["name_key"] = entry.get("name_key")
            if annotate and tid:
                ann_params = dict(annotate["params_keys"])
                ann_params[annotate["target_key"]] = tid
                try:
                    ann = await _query_bridge(state, annotate["bridge_action"], ann_params)
                    if isinstance(ann, dict):
                        for k in annotate["keep"]:
                            if k in ann:
                                out[k] = ann[k]
                except (ConnectionError, TimeoutError):
                    out["annotate_error"] = True
            targets.append(out)
        return _ok("discover", state, {
            "action": action,
            "label": plan.get("label"),
            "target_count": len(targets),
            "truncated": len(items) > cap,
            "targets": targets,
        })
    except Exception as e:                               # noqa: BLE001
        f = _read_failure(f"discover(what='targets', action='{action}')", e)
        return _blocked("discover", state, f["code"], f["detail"])


async def _discover_decision_why_blocked(state, decision: str, actor_id: int,
                                         title_id: int) -> dict[str, Any]:
    """Per-clause structural why-blocked for a SHIELD decision (additive, crash-safe).

    Gathers cheap structural reads (realm, character, and — for the danelaw gate —
    per-duchy de-jure control of the kingdom's duchies) and runs the pure
    clause-checker in tools.shield_decision_why_blocked. Never the hanging engine
    eval. Works PAUSED. Degrades to a partial verdict if any read fails."""
    from tools import (shield_decision_why_blocked, SHIELD_DECISIONS,
                       _DANELAW_KINGDOM_KEY)
    if not decision.endswith("_decision"):
        decision = decision + "_decision"
    if decision not in SHIELD_DECISIONS:
        return _blocked("discover", state, "unknown_subject",
                        f"decision_why_blocked supports {', '.join(SHIELD_DECISIONS)}; got '{decision}'")

    cid = actor_id or 0
    try:
        realm = await _query_bridge(state, "query_realm", {"character_id": cid})
        character = await _query_bridge(state, "query_character", {"character_id": cid})
    except Exception as e:                               # noqa: BLE001
        f = _read_failure("query_realm/query_character", e)
        return _blocked("discover", state, f["code"], f["detail"])
    realm = realm if isinstance(realm, dict) else {}
    character = character if isinstance(character, dict) else {}

    # Enumerate duchy control for the danelaw gate: held DUCHY titles (tier 3)
    # whose de-jure-above = k_england, each scored by per-duchy de-jure control.
    # Bounded + every read defensive — a failure degrades to "unmeasured", never raises.
    duchy_control = None
    if decision == "negotiate_the_danelaw_decision":
        # D1 fix: the action is query_title_hierarchy — "query_titles" never
        # existed in the bridge (invented in 2691c3a; hung paused callers and
        # the error dict was consumed as an empty title list).
        try:
            titles = await _query_bridge(state, "query_title_hierarchy",
                                         {"character_id": cid})
        except (ConnectionError, TimeoutError) as e:
            titles = {"error": f"bridge_unreachable: {e}"}
        held = titles.get("held_titles") if isinstance(titles, dict) else None
        if not redesign.hierarchy_was_read(titles, realm):
            # D1-B: never fabricate a confident empty verdict from a failed
            # read — report the gate as unmeasured, loudly. The bridge
            # answers a clamp or a skipped lookup as an error (title_count_clamped,
            # title_lookup_failed); the second check stays as the second line: an
            # empty list while query_realm counts held titles is not "holds none"
            # (redesign.hierarchy_was_read).
            reason = (titles.get("error")
                      or ("no held_titles in the reply" if not isinstance(held, list)
                          else "held_titles empty while query_realm counts held titles (clamped)")
                      if isinstance(titles, dict)
                      else f"unexpected_reply:{type(titles).__name__}")
            log.warning("duchy-control read failed (%s) — reporting unmeasured",
                        reason)
            # duchy_control=None routes to the shield's existing UNMEASURED
            # clause (a hint, not a false fail) instead of a confident empty.
            verdict = shield_decision_why_blocked(
                decision, realm=realm, character=character, duchy_control=None)
            return _ok("discover", state, verdict)
        # Read by the keys emit_title_info writes (query_handlers.cpp:
        # `key` :2910, `tier` :2935, `de_jure_parent_key` :2964; the list is
        # `held_titles` :3046). The reader does not take "titles", "title_key" and
        # "de_jure_kingdom", none of which the bridge writes, so the kingdom
        # filter never ran and every held duchy anywhere counted toward k_england.
        # A held duchy whose de-jure parent did not read back, or whose meter read
        # failed, is UNMEASURED: it is neither counted nor dropped.
        complete, candidates, unmeasured = [], [], []
        for t in held[:40]:
            if not isinstance(t, dict):
                continue
            key = t.get("key")
            key = key if isinstance(key, str) else ""
            tier = t.get("tier")
            if tier != 3:          # the emitter derives tier from a d_ key itself
                continue
            row = {"title_id": t.get("title_id"), "key": key or None}
            parent = t.get("de_jure_parent_key")
            if not key or not isinstance(parent, str) or not parent:
                row["unmeasured"] = ("no title key" if not key
                                     else "no de_jure_parent_key: the duchy's kingdom did not read back")
                unmeasured.append(row)
                continue
            if parent != _DANELAW_KINGDOM_KEY:
                continue
            try:
                prog = await _query_bridge(state, "query_dejure_progress",
                                           {"character_id": cid, "title_key": key})
            except (ConnectionError, TimeoutError) as e:
                prog = {"error": f"bridge_unreachable: {e}"}
            if not isinstance(prog, dict):
                prog = {"error": f"unexpected_reply:{type(prog).__name__}"}
            held_c, total_c = prog.get("held"), prog.get("total")
            if prog.get("error") or not isinstance(held_c, int) \
                    or not isinstance(total_c, int) or total_c <= 0:
                row["unmeasured"] = ("query_dejure_progress: "
                                     + str(prog.get("error") or "no held/total"))
                unmeasured.append(row)
                continue
            row["held"] = held_c; row["total"] = total_c
            (complete if held_c >= total_c else candidates).append(row)
        duchy_control = {"complete_duchies": complete, "candidate_duchies": candidates,
                         "unmeasured_duchies": unmeasured}

    verdict = shield_decision_why_blocked(
        decision, realm=realm, character=character, duchy_control=duchy_control)
    return _ok("discover", state, verdict)


@mcp.tool(description=_TT.DISCOVER + (_TT.DISCOVER_NOTES if _NOTES.enabled else ""))
async def discover(
    what: Annotated[str, Field(description="What to check or list.",
                               json_schema_extra={"enum": [w for w in DISCOVER_WHATS
                                                           if w != "notes" or _NOTES.enabled]})] = "",
    interaction_type: Annotated[str, Field(description="acceptance, trigger_result: the full "
                                                       "*_interaction key.")] = "",
    target_id: Annotated[int, Field(description="The other character.")] = 0,
    actor_id: Annotated[int, Field(description="The acting character (0 = yours).")] = 0,
    interaction: Annotated[str, Field(description="interaction_validity: the short name "
                                                  "(e.g. declare_war).")] = "",
    cb: Annotated[str, Field(description="interaction_validity for declare_war: the casus belli.")] = "",
    title: Annotated[str, Field(description="interaction_validity for declare_war: the claimed title "
                                            "(informational).")] = "",
    action: Annotated[str, Field(description="targets: the action.",
                                 json_schema_extra={"enum": [""] + sorted(_TT.T.TARGETS_ACTIONS)})] = "",
    title_id: Annotated[int | str, Field(description="targets with action=claim: the title.")] = 0,
    building: Annotated[str, Field(description="construction_check: the building key.")] = "",
    location_id: Annotated[int | str, Field(description="construction_check: the holding's barony or "
                                                        "county.")] = 0,
    slot: Annotated[Optional[int], Field(description="construction_check: the building slot (read "
                                                     "from the holding when omitted).")] = None,
    max_results: Annotated[int, Field(description="scheme_targets: stop at this many valid.")] = 0,
    character_id: Annotated[int, Field(description="decisions, decisions_annotated, valid_decisions, "
                                                   "scheme_targets: the character (0 = yours).")] = 0,
    decision_type: Annotated[str, Field(description="decision_why_blocked: the two it models.",
                                        json_schema_extra={"enum": [""] + [x for d in _TT.T.SHIELD_DECISIONS
                                                                          for x in (d, d.removesuffix("_decision"))]})] = "",
    decisions: Annotated[list, Field(description="decisions_annotated, valid_decisions: keys to "
                                                 "evaluate; decisions: name fragments.")] = None,
    scheme: Annotated[str, Field(description="scheme_targets: the scheme key (from scheme_types).")] = "",
    candidates: Annotated[list, Field(description="scheme_targets: character ids to check.")] = None,
    topic: Annotated[str, Field(description="notes: the topic (empty: the index).",
                                json_schema_extra={"enum": [""] + _NOTES.topics})] = "",
) -> dict[str, Any]:
    state = _get_state()
    if what == "notes" and _NOTES.enabled:     # static content: no game needed
        if not topic:
            return redesign.envelope("discover", result=_NOTES.index_reply(),
                                     game=_state_game(state), bridge_health=_state_health(state))
        body = _NOTES.topic_reply(topic)
        if body is None:
            return _blocked("discover", state, "invalid_argument",
                            f"no notes topic named {topic!r}",
                            fix={"accepted": _NOTES.topics})
        return redesign.envelope("discover", result=body, game=_state_game(state),
                                 bridge_health=_state_health(state))
    err = await _ensure_connected(state, "discover")
    if err:
        return err
    gw = await _gateway_now()             # the envelope's game block: a fresh clock reading
    if not what:
        return _blocked("discover", state, "missing_required_argument",
                        f"what is required; valid: {', '.join(DISCOVER_WHATS)}")
    if isinstance(title_id, str) or isinstance(location_id, str):
        import title_refs as _trefs
        _named, _echo, _blk = await _trefs.resolve_args(
            gw, {"title_id": title_id, "location_id": location_id}, ("title_id", "location_id"))
        if _blk is not None:
            return _refused("discover", state, _blk)
        title_id, location_id = _named["title_id"] or 0, _named["location_id"] or 0
    if what in ("targets", "decision_why_blocked"):
        # About the bound ruler when no actor is named
        unbound = await redesign.unbound_refusal(
            gw, "discover", what, {"actor_id": actor_id or character_id})
        if unbound is not None:
            return unbound

    # Legibility capstone: what="targets" — "who are the valid targets for
    # action X right now". Some actions resolve in one call; others enumerate a
    # candidate pool then annotate each entry with per-target validity.
    if what == "targets":
        return await _discover_targets(state, action, actor_id, title_id)

    # One door per read. An enumeration discover once had is
    # do's; the refusal names that door and reads nothing.
    from tools import DISCOVER_MOVED_TO_DO
    if what in DISCOVER_MOVED_TO_DO:
        door = DISCOVER_MOVED_TO_DO[what]
        return _blocked("discover", state, "unknown_subject",
                        f"discover(what='{what}') is do(action_class='{door}'), which lists "
                        "each row with its do_call",
                        fix={"tool": "do", "args": {"action_class": door}})

    # Legibility (re/findings/re_shield_robustness.md (D)): per-clause pass/fail for
    # the SHIELD decisions (negotiate_the_danelaw / found_kingdom) from CHEAP
    # STRUCTURAL READS — the SAFE alternative to the hanging engine eval
    # (decisions_annotated). Pure reads → works PAUSED; additive; crash-safe.
    if what == "decision_why_blocked":
        return await _discover_decision_why_blocked(
            state, decision_type or interaction_type or "negotiate_the_danelaw_decision",
            actor_id or character_id, title_id)

    if what == "acceptance":
        # `interaction` is taken as the same key (the live walk sent it and
        # was answered interaction_not_found under ok true); the send's own checks.
        return await redesign.acceptance_impl(gw, interaction_type or interaction, target_id,
                                              actor_id or character_id)
    if what == "war_targets":
        # Each row judged by the same casus-belli read cbs uses.
        return _fit.sent(await redesign.war_targets_impl(gw, actor_id or character_id), "discover", {"what": what})
    if what == "scheme_types":
        return _fit.sent(await redesign.scheme_types_impl(await _gateway_now()), "discover", {"what": what})
    if what == "scheme_targets":
        return await redesign.scheme_targets_impl(
            await _gateway_now(), scheme, character_id=actor_id or character_id,
            target_id=target_id, candidates=candidates, max_results=max_results)

    from tools import resolve_discover, shape_discover_response
    resolver_params = {
        "what": what, "interaction_type": interaction_type,
        # sweep: character_id names the actor as actor_id does (the
        # description says either); it was dropped for every what below.
        "target_id": target_id, "actor_id": actor_id or character_id,
        "interaction": interaction, "cb": cb, "title": title,
        "building": building, "location_id": location_id, "slot": slot,
        "max_results": max_results,
        "character_id": character_id,
        # For what=decisions_annotated / valid_decisions — annotate ONLY these
        # decisions (key names and/or name-hashes from what="decisions").
        "decisions": decisions or [],
    }
    resolved = resolve_discover(resolver_params)
    if resolved is None:
        return _blocked("discover", state, "unknown_subject",
                        f"unknown discover target '{what}'; valid: {', '.join(DISCOVER_WHATS)}")

    action, bridge_params = resolved
    if what in ("decisions_annotated", "valid_decisions") and not decisions:
        # Without a list the bridge serves the light list, which is
        # what="decisions"; an annotated read of nothing is a missing argument.
        return _blocked("discover", state, "missing_required_argument",
                        f"what='{what}' evaluates the decisions you name: pass "
                        "decisions=[<key>, ...] (what='decisions' lists the keys)")
    unbound = await redesign.unbound_refusal(gw, "discover", action, bridge_params)
    if unbound is not None:
        return unbound

    # A deferred-drain read submits to a game-thread slot and waits for its
    # verdict, so it takes the longer-budget action path. DEFERRED_DRAIN_READS is
    # the one Python copy of the bridge's is_deferred_drain_action.
    try:
        if action in DEFERRED_DRAIN_READS:
            result = await _action_bridge(state, action, bridge_params,
                                          timeout=DEFERRED_READ_TIMEOUT_S)
        elif action in BridgeGateway._GAME_THREAD_READS:
            # A game-thread read (the game's triggers run under the random scope),
            # through the gateway's action path.
            result = await _get_gateway().read(action, bridge_params)
        else:
            result = await _query_bridge(state, action, bridge_params)
    except Exception as e:                               # noqa: BLE001
        f = _read_failure(f"discover('{what}')", e)
        return _blocked("discover", state, f["code"], f["detail"])
    if isinstance(result, dict) and result.get("status") == "sent_no_confirmation":
        return _blocked("discover", state, "read_timeout",
                        f"discover('{what}') was sent and the bridge did not answer within "
                        f"{result.get('waited_s') or DEFERRED_READ_TIMEOUT_S}s",
                        raw_reason=str(result.get("reason_code") or ""))
    declined = _declined(result, action)
    if declined is not None:
        return _refused("discover", state, declined)
    if what in ("decisions_annotated", "valid_decisions"):
        rows, refused = redesign.decision_eval_rows(result, decisions)
        if refused is not None:
            return _refused("discover", state, refused)
        result = dict(result, decisions=rows)
    if what in ("decisions", "interactions") and isinstance(result, dict):
        # 428 decisions were 28 KB live. Names only, filtered by the
        # fragments in `decisions`, capped (redesign.NAME_LIST_CAP), the rest counted.
        key = "decisions" if what == "decisions" else "interactions"
        rows = [r.get("name") or r.get("type") for r in (result.get(key) or []) if isinstance(r, dict)]
        names, more = redesign.filter_names([n for n in rows if isinstance(n, str)], decisions)
        result = {k: v for k, v in result.items() if k not in (key, "count", "request_id")}
        result.update({key: names, "count": len(rows)})
        if more:
            result["truncated"] = more
    shaped = shape_discover_response(resolver_params, result)
    return _fit.sent(_ok("discover", state, shaped), "discover", {"what": what})


# ---------------------------------------------------------------------------
# steer
# ---------------------------------------------------------------------------

if not _NOTES.enabled:
    # notes off: `topic` leaves the schema too, so the strict-argument check refuses it
    mcp._tool_manager.get_tool("discover").parameters["properties"].pop("topic", None)


@mcp.tool(description=_TT.STEER)
async def steer(character_id: Annotated[int, Field(description="The ruler (0 = yours).")] = 0,
                personality: Annotated[dict, Field(description="{axis: integer}; omit to read.")] = None
                ) -> dict[str, Any]:
    err = await _ensure_connected(_get_state(), "steer")
    if err:
        return err
    return await redesign.steer_impl(await _gateway_now(), character_id=character_id,
                                     personality=personality)


# ---------------------------------------------------------------------------
# pass_turn
# ---------------------------------------------------------------------------

@mcp.tool(description=(_TT.PASS_TURN_CLOCKLESS if redesign.clock_owned_by_env() else _TT.PASS_TURN))
async def pass_turn(reason: Annotated[str, Field(description="Why you do not act.")] = "",
                    character_id: Annotated[int, Field(description="The ruler who passes (0 = the "
                                                                   "primary); one pass per ruler.")] = 0
                    ) -> dict[str, Any]:
    state = _get_state()
    await _gateway_now()                  # the envelope's game block: a fresh clock reading
    # pass_turn was the one tool without character_id while the protocol
    # says to pin it on every call; 174 pilot calls passed it and FastMCP dropped
    # it without a word, so a several-ruler log could not say who passed.
    who = {"character_id": int(character_id)} if character_id else {}
    state.action_log.append({
        "tick": state.tick_id,
        "action": {"type": "pass_turn", "reason": reason, **who},
    })
    return _ok("pass_turn", state, {
        "action": "pass_turn",
        "reason": reason or "maintaining stability",
        **who,
    })


# ===========================================================================
# The goal-shaped tools. Each is a thin
# wrapper over redesign.py composition + the BridgeGateway (dispatch and
# single-flight §4, actor explicitness §5). The bridge actions each one composes are the ones its
# redesign.py implementation sends; there is no separate hand-kept list of them.
# ===========================================================================

@mcp.tool(description=_TT.OBSERVE)
async def observe(
    character_id: Annotated[int, Field(description="The ruler (0 = your primary).")] = 0,
    depth: Annotated[
        str,
        Field(description="brief; full adds self, war, council, AI evaluations; news: only news, "
                          "since the last news read.",
              json_schema_extra={"enum": list(redesign.OBSERVE_DEPTHS)}),
    ] = "brief") -> dict[str, Any]:
    err = await _ensure_connected(_get_state(), "observe")
    if err:
        return err
    gw = await _gateway_now()
    if depth == "news":
        # A news-only read for a per-turn hand-off: not the seat's cycle entry
        # (no lineage step, no inbox delivery, no goal or pushed blocks), its own baseline
        return _fit.sent(await redesign.observe_impl(gw, character_id=character_id, depth=depth), "observe")
    # the seat is a LINEAGE. At the seat's cycle entry, witness a
    # handoff the bridge made, make the rebind explicitly when it could not,
    # or end the seat `lost`; rows go to CK3_SEAT_LINEAGE_OUT. INERT unless
    # that env is set (the public product sets nothing).
    _sink = seat_lineage.armed()
    if _sink:
        _st = _get_state()
        if _st.seat_lineage_prior is None:
            # A resumed seat's new server: the lineage so far.
            _st.seat_lineage_prior = seat_lineage.prior_from_sink(_sink)
        _st.seat_lineage_prior = await seat_lineage.step(
            gw, _st.seat_lineage_prior, sink=_sink,
            seat_session_id=os.environ.get(seat_lineage.SESSION_ID_ENV),
            registered_title=seat_lineage.registered_title_from_env(),
            own_session_id=redesign.own_session_id, own_session_row=redesign.own_session_row)
    facts: dict = {}
    # the raw reads the pushed warnings judge, asked for only in a guided seat
    env = await (redesign.observe_impl(gw, character_id=character_id, depth=depth, facts_out=facts)
                 if _GUIDE is not None else redesign.observe_impl(gw, character_id=character_id, depth=depth))
    if _CHANNEL is not None and _CHANNEL.enabled:
        env = await _deliver_inbox(gw, env)
    if isinstance(env.get("result"), dict) and not env.get("blocked"):
        env = await _attach_goal(gw, env)
        if _GUIDE is not None:
            env = await _attach_pushed(gw, env, facts)
    return _fit.sent(env, "observe", {"depth": depth})


async def _attach_pushed(gw, env: dict[str, Any], facts: dict) -> dict[str, Any]:
    """`result.warnings`, after `goal`: the pushed warnings
    (pushed_warnings.compose), each from a trigger of its table and naming a
    guide topic. The vassal reads (ONE batched query_realm for every direct
    vassal, pushed_warnings.vassal_read_request) are taken once per cycle. A
    read that fails leaves its trigger silent and a diagnostic in the
    envelope's `warnings`, never an invented line."""
    res = env["result"]
    goal = res.get("goal") if isinstance(res.get("goal"), dict) else {}
    idx = (goal.get("cycle") or {}).get("index") if isinstance(goal.get("cycle"), dict) else None
    realm = facts.get("realm") if isinstance(facts.get("realm"), dict) else {}
    vids = _pw.vassal_ids(realm)
    key = (idx, tuple(vids))
    if idx is not None and _PUSH_CACHE.get("key") == key:
        vassal_realms, unread = _PUSH_CACHE["vassal_realms"], _PUSH_CACHE.get("unread")
    else:
        unread = None
        try:
            if vids:
                vassal_realms, unread = _pw.vassal_realms_of(vids, await gw.read(*_pw.vassal_read_request(vids)))
            else:
                vassal_realms = {}
        except Exception as exc:                                # noqa: BLE001
            vassal_realms = {}
            unread = {"code": _pw.W_VASSAL_REALMS_UNREAD, "unread": len(vids), "of": len(vids),
                      "vassal_ids": list(vids), "detail": f"{type(exc).__name__}: {exc}"}
        _PUSH_CACHE.update(key=key, vassal_realms=vassal_realms, unread=unread)
    if unread:
        env.setdefault("warnings", []).append(dict(unread))
    briefing, why = _pw.read_briefing(os.environ)
    if why:
        env.setdefault("warnings", []).append({"code": _pw.W_BRIEFING_UNREAD, "detail": why})
    items = _pw.compose(succession=res.get("succession"), character=facts.get("character"), realm=realm,
                        vassal_realms=vassal_realms, memory=_ACTIONS, briefing=briefing, cycle_index=idx)
    rest = {k: v for k, v in res.items() if k not in ("goal", "warnings")}
    env["result"] = {**({"goal": res["goal"]} if "goal" in res else {}), "warnings": items, **rest}
    return env


async def _attach_goal(gw, env: dict[str, Any]) -> dict[str, Any]:
    """The goal block FIRST in `result`
    (goal_block.goal_block). Outside a scheduled run it is stated absent
    (`{"state": "no_scheduled_run"}`), never zeros."""
    actor = env.get("actor") if isinstance(env.get("actor"), dict) else {}
    try:
        goal = await _goal_block.goal_block(gw, actor_id=actor.get("id"),
                                            game_date=gw.game().get("date"))
    except Exception as exc:                                    # noqa: BLE001
        goal = {"state": "unread", "detail": f"{type(exc).__name__}: {exc}"}
    env["result"] = {"goal": goal, **{k: v for k, v in env["result"].items() if k != "goal"}}
    return env


async def _deliver_inbox(gw, env: dict[str, Any]) -> dict[str, Any]:
    """The channel's delivery: the inbox rides on the recipient's next observe as
    `result.messages[]`, and this observe's clock stamps each one read. THE
    ONLY CALLER OF channel.deliver. When the ledger cannot be reached the
    field is ABSENT and a warning names why — never an empty list, which would
    read as "nothing was sent"."""
    try:
        if _CHANNEL.seat_refusal():
            raise RuntimeError(f"{_CHANNEL.seat_refusal()}: this process has no "
                               f"{_channel.ENV_SEAT}, so it cannot be a recipient")
        ledger = await _seat_ledger(gw)
        if ledger is None:
            raise RuntimeError("no episode id from the environment or the bridge")
        game = gw.game()
        read_cycle, _why = _channel.send_cycle(os.environ, game.get("date"), _SEAT.cycles)
        env.setdefault("result", {})["messages"] = _channel.deliver(
            _CHANNEL, ledger, game_date=game.get("date"), tick=game.get("tick"),
            cycle=read_cycle)
    except Exception as exc:                                    # noqa: BLE001
        env.setdefault("warnings", []).append(
            {"code": _channel.W_UNREAD,
             "detail": f"the channel ledger could not be read: {type(exc).__name__}: {exc}"})
    return env


async def _seat_ledger(gw) -> Optional["_channel.Ledger"]:
    """This seat's ledger for the running episode, or None when no episode id
    can be resolved (the environment's, else get_tick_info's pipeline.episode_id,
    the id `actions.jsonl` rows carry)."""
    eid = _SEAT.episode_id
    if not eid:
        eid = _SEAT.resolve_episode_id(os.environ, await gw.read("get_tick_info", {}))
    if not eid:
        return None
    return _channel.Ledger(_channel.ledger_path(eid, os.environ), episode_id=eid,
                           instance_id=_instance_id())


def _instance_id() -> str:
    try:
        import instance as _inst
        return _inst.current_id(os.environ)
    except Exception:                                           # noqa: BLE001
        return ""


@mcp.tool(description=_TT.PLAN_WAR)
async def plan_war(character_id: Annotated[int, Field(description="The aggressor (0 = your primary).")] = 0,
                   goal_scoped: Annotated[bool, Field(description="true: claims, duchies you partly hold "
                                                                  "and toward; false adds every bordering "
                                                                  "duchy.")] = True,
                   ally_free_only: Annotated[bool, Field(description="Leave out targets whose allies you "
                                                                     "would fight.")] = True,
                   exclude_straddling: Annotated[bool, Field(description="Leave out every row while a war "
                                                                         "would run across your "
                                                                         "succession.")] = True,
                   max_results: Annotated[int, Field(description="Top rows by score.")] = 8,
                   toward: Annotated[str, Field(description="A de-jure title key to conquer toward "
                                                            "(e.g. k_scotland).")] = "") -> dict[str, Any]:
    err = await _ensure_connected(_get_state(), "plan_war")
    if err:
        return err
    env = await redesign.plan_war_impl(await _gateway_now(), character_id=character_id,
                                       goal_scoped=goal_scoped, ally_free_only=ally_free_only,
                                       exclude_straddling=exclude_straddling, max_results=max_results,
                                       toward=toward)
    return _fit.sent(withhold_refused_offers(env, character_id, _REFUSALS), "plan_war")


def withhold_refused_offers(env: Any, character_id: int, memo: "refusal_memo.Memo") -> Any:
    """A plan_war row whose declaration the game refused at this moment, nothing changed since
    (the memo, keyed as declare_war keys it; the moment is the game tick, not the month) is not
    offered again: it moves to `excluded` with the gate refused_unchanged and the earlier refusal's gate.
    In one recorded run plan_war offered William 31808 / 7127, 30162 / 7808 and
    63899 / 7808 after the game had refused each (cb_invalid_for_target)."""
    if not isinstance(env, dict) or not isinstance(env.get("result"), dict):
        return env
    res = env["result"]
    date = env.get("game") if isinstance(env.get("game"), dict) else None
    keep, moved = [], []
    for row in res.get("targets") or []:
        args = row.get("declare_args") if isinstance(row, dict) else None
        earlier = memo.earlier(refusal_memo.declare_key(character_id, args), date) \
            if isinstance(args, dict) else None
        if earlier is None:
            keep.append(row)
            continue
        gate = ((earlier["env"].get("blocked") or {}).get("gate"))
        moved.append({**{k: row.get(k) for k in ("declare_args", "target_name", "title") if k in row},
                      "blocked": redesign.why_blocked(
                          refusal_memo.GATE_REFUSED_UNCHANGED,
                          summary=f"the game refused this declaration on {earlier['game_date']} ({gate}); "
                                  "it is offered again after a game day or a write of yours"),
                      "repeat_of": {"game_date": earlier["game_date"], "gate": gate}})
    if moved:
        res["targets"] = keep
        res["excluded"] = list(res.get("excluded") or []) + moved
    return env


@mcp.tool(description=_TT.DECLARE_WAR)
async def declare_war(character_id: Annotated[int, Field(description="The aggressor (0 = your primary).")] = 0,
                      declare_args: Annotated[dict, Field(description="A plan_war row's declare_args, "
                                                                      "unchanged.")] = None,
                      target_id: Annotated[int, Field(description="Without declare_args: the defender.")] = 0,
                      cb: Annotated[str, Field(description="Without declare_args: the casus belli key.")] = "",
                      cb_title_id: Annotated[int | str, Field(description="Without declare_args: the "
                                                                          "title fought for (key or id).")] = 0,
                      claimant_id: Annotated[Optional[int], Field(description="A claim casus belli's "
                                                                              "claimant; null or 0 = "
                                                                              "none.")] = 0,
                      confirm_straddle: Annotated[bool, Field(description="Required true for a war across "
                                                                          "your succession.")] = False
                      ) -> dict[str, Any]:
    err = await _ensure_connected(_get_state(), "declare_war")
    if err:
        return err
    gw = await _gateway_now()
    fields = dict(declare_args) if isinstance(declare_args, dict) else \
        {"target_id": target_id, "cb": cb, "cb_title_id": cb_title_id, "claimant_id": claimant_id}
    key = refusal_memo.declare_key(character_id, fields)
    again = _REFUSALS.repeat_reply(key, _state_game(_get_state()))
    if again is not None:
        return again                        # Refused at this tick, nothing changed; not sent
    env = await redesign.declare_war_impl(gw, character_id=character_id,
                                          declare_args=declare_args, target_id=target_id,
                                          cb=cb, cb_title_id=cb_title_id, claimant_id=claimant_id or 0,
                                          confirm_straddle=confirm_straddle)
    _REFUSALS.note(key, env, env.get("game") if isinstance(env, dict) else None)
    return env


@mcp.tool(description=_TT.SUCCESSION_PLAN)
async def succession_plan(character_id: Annotated[int, Field(description="The ruler whose death you plan "
                                                                         "around (0 = your primary).")] = 0
                          ) -> dict[str, Any]:
    err = await _ensure_connected(_get_state(), "succession_plan")
    if err:
        return err
    return _fit.sent(await redesign.succession_plan_impl(await _gateway_now(), character_id=character_id),
                    "succession_plan")


@mcp.tool(description=_TT.SET_PHASE)
async def set_phase(
    phase: Annotated[
        Optional[str],
        Field(description=_TT.phase_meanings(),
              json_schema_extra={"enum": list(redesign.PHASES) + [None]}),
    ] = None,
    character_id: Annotated[int, Field(description="The ruler (0 = your primary).")] = 0,
    overrides: Annotated[dict, Field(description="{axis: integer} written over the phase's genome; an "
                                                 "unknown axis or a value out of range is refused.")] = None
) -> dict[str, Any]:
    err = await _ensure_connected(_get_state(), "set_phase")
    if err:
        return err
    return await redesign.set_phase_impl(await _gateway_now(), phase=phase,
                                          character_id=character_id, overrides=overrides)


@mcp.tool(description=_TT.DO)
async def do(
    intent: Annotated[
        Optional[str],
        Field(description="The action to run; null with action_class lists.",
              json_schema_extra={"enum": list(redesign.INTENTS) + [None]}),
    ] = None,
    action_class: Annotated[
        Optional[str],
        Field(description="What to list.",
              json_schema_extra={"enum": list(redesign.ACTION_CLASSES) + [None]}),
    ] = None,
    candidates: Annotated[list, Field(description="decisions, formable_titles: names (required); "
                                                  "fabricate_targets: title keys; men_at_arms: types; "
                                                  "imprison_targets: ids.")] = None,
    character_id: Annotated[int, Field(description="The actor (0 = your primary).")] = 0,
    args: Annotated[
        Optional[dict],
        Field(description=_TT.DO_ARGS + do_intent_args.table_text()),
    ] = None) -> dict[str, Any]:
    err = await _ensure_connected(_get_state(), "do")
    if err:
        return err
    gw = await _gateway_now()
    key = refusal_memo.do_key(character_id, intent, args) if intent else None
    if key is not None:
        _now = _state_game(_get_state())
        again = _REFUSALS.repeat_reply(key, _now)
        if again is not None and intent in refusal_memo.MOVING_INTENTS \
                and await _army_orders.moved_since(gw, _REFUSALS.earlier(key, _now)["env"]):
            again = None                    # The army moved since; the game is asked again
        if again is not None:
            return again                    # Refused at this tick, nothing changed; not sent
    env = await redesign.do_impl(gw, intent=intent, action_class=action_class,
                                 candidates=candidates, character_id=character_id, args=args)
    if key is not None:
        _REFUSALS.note(key, env, env.get("game") if isinstance(env, dict) else None)
    # A listing's reply is cut to the cap (a write's reply is sent whole)
    return _fit.sent(env, "do", {"action_class": action_class, "intent": intent})


@mcp.tool(description=_TT.INSPECT)
async def inspect(
    subject: Annotated[str, Field(description="The subject.",
                                  json_schema_extra={"enum": list(redesign.INSPECT_SUBJECTS)})] = "",
    character_id: Annotated[int, Field(description="The character (0 = yours).")] = 0,
    target_id: Annotated[int, Field(description="opinion, opinion_breakdown: the other character.")] = 0,
    title_id: Annotated[int | str, Field(description="title_hierarchy, title_succession, claimants, "
                                                     "development: the title (key or id).")] = 0,
    war_id: Annotated[int, Field(description="war_score: one war.")] = 0,
    title_key: Annotated[str, Field(description="title, map: the de-jure title, e.g. k_scotland.")] = "",
    dynasty_id: Annotated[int, Field(description="dynasty: the dynasty (0 = yours).")] = 0,
    max_results: Annotated[int, Field(description="Row cap.")] = redesign.INSPECT_MAX_RESULTS_DEFAULT,
    layers: Annotated[list[str], Field(description="map: columns (default holders,cb,mil,str,adj).",
                                       json_schema_extra={"items": {"type": "string", "enum": list(_MV.LAYERS)}})] = None
) -> dict[str, Any]:
    if subject == _fit.MORE_SUBJECT and _fit.MORE_SUBJECT in redesign.INSPECT_SUBJECTS:
        return _fit.next_page()             # The next page of the last paged reply (no game read)
    err = await _ensure_connected(_get_state(), "inspect")
    if err:
        return err
    return _fit.sent(await redesign.inspect_impl(await _gateway_now(), subject=subject, character_id=character_id,
                                                 target_id=target_id, title_id=title_id, war_id=war_id,
                                                 title_key=title_key, dynasty_id=dynasty_id,
                                                 max_results=max_results, layers=layers),
                    "inspect", {"subject": subject})


@mcp.tool(description=_TT.CBS)
async def cbs(target_id: Annotated[int, Field(description="The defender: an independent ruler (required).")] = 0,
              character_id: Annotated[int, Field(description="The aggressor (0 = yours).")] = 0,
              include_invalid: Annotated[bool, Field(description="Also list every invalid option whole.")] = False,
              ) -> dict[str, Any]:
    err = await _ensure_connected(_get_state(), "cbs")
    if err:
        return err
    return _fit.sent(await redesign.cbs_impl(await _gateway_now(), target_id=target_id, character_id=character_id,
                                            include_invalid=include_invalid), "cbs")


@mcp.tool(description=_TT.session_text(redesign.clock_owned_by_env()))
async def session(
    action: Annotated[
        str,
        Field(description="The operation.",
              json_schema_extra={"enum": list(redesign.SESSION_ACTIONS)}),
    ] = "",
    character_id: Annotated[int, Field(description="control and add (required), release.")] = 0,
    speed: Annotated[int, Field(description=f"speed: {redesign.SPEED_MIN} to {redesign.SPEED_MAX}.")] = redesign.SPEED_MAX,
    enabled: Annotated[Optional[bool], Field(description="set_experimental: arm or disarm (required).")] = None,
    disable_native_ai: Annotated[bool, Field(description="control and add: also switch the game's own "
                                                         "AI off for the ruler (it then neither declares "
                                                         "wars nor sieges; leave it false).")] = False,
    flag: Annotated[str, Field(description="set_experimental: the flag (from experimental_flags).")] = "",
    seconds: Annotated[int, Field(description=f"wait: seconds (1 to {redesign.WAIT_CAP_S}); with "
                                              "game_days, the longest single wait.")] = 60,
    game_days: Annotated[int, Field(description="wait: run until the date has advanced this many "
                                                "game days (0 = wait seconds once).")] = 0) -> dict[str, Any]:
    err = await _ensure_connected(_get_state(), "session")
    if err:
        return err
    env = await redesign.session_impl(await _gateway_now(), action=action, character_id=character_id,
                                      speed=speed, enabled=enabled, disable_native_ai=disable_native_ai,
                                      flag=flag, seconds=seconds, game_days=game_days)
    # The seat's cycle, the unit the send quota is keyed on
    # (cycle_cadence.CycleCounter: the registered cadence in game-days across
    # the waits that waited, or every such wait in free play). Counted whether
    # or not a channel is registered, so the count is the same fact in every arm.
    _SEAT.cycles.note(action, env)
    return env


# ---------------------------------------------------------------------------
# msg_send — the seats' message channel
# ---------------------------------------------------------------------------

async def msg_send(to: str, text: str) -> dict[str, Any]:
    """Send a short message to another steered ruler's seat. `to` is a seat label
    (`K` for the king, `D1`..`Dn` for the dukes). Who you may write to is fixed
    for this game: a recipient outside the table is refused `topology_denied`.
    You may send at most the declared number of messages per cycle (a cycle ends
    when your session(action='wait') calls have carried the game date the
    registered cadence past the cycle's start); the next is refused `quota_exceeded`. A text
    over the declared byte bound is refused `text_too_long`, never truncated.
    Every attempt, delivered or refused, is written to the episode's ledger.
    Messages reach the recipient inside its next observe (`result.messages[]`).
    There is no broadcast: to reach several seats, send several messages.

    Args:
        to: the recipient's seat label.
        text: the message, at most the declared number of UTF-8 bytes.
    """
    state = _get_state()
    err = await _ensure_connected(state, "msg_send")
    if err:
        return err
    gw = await _gateway_now()
    if _CHANNEL is None or not _CHANNEL.enabled:
        return _blocked("msg_send", state, _channel.GATE_UNAVAILABLE,
                        "this seat's run declares no channel")
    if _CHANNEL.seat_refusal():
        return _blocked("msg_send", state, _channel.GATE_UNAVAILABLE,
                        f"{_CHANNEL.seat_refusal()}: this process has no "
                        f"{_channel.ENV_SEAT}, so it is nobody's seat and nothing "
                        "was sent", measured={"reason": _CHANNEL.seat_refusal()})
    ledger = await _seat_ledger(gw)
    if ledger is None:
        return _blocked("msg_send", state, _channel.GATE_UNAVAILABLE,
                        "no episode id: neither CK3_EPISODE_ID nor the bridge's "
                        "get_tick_info pipeline.episode_id names the episode, so "
                        "there is no ledger to write to; nothing was sent")
    game = gw.game()
    # The cycle comes from the episode's authority (channel.send_cycle: in a
    # cell, the schedule over the bridge's date at this call), never from this
    # process's counter, which a resumed session starts at 0 (seat-review
    # problem 3). An authority that cannot answer is a refusal, not cycle 0.
    cycle, why = _channel.send_cycle(os.environ, game.get("date"), _SEAT.cycles)
    if cycle is None:
        return _blocked("msg_send", state, _channel.R_CYCLE_UNKNOWN,
                        f"the cycle this send falls in cannot be established ({why}); "
                        "nothing was sent and no quota was spent",
                        measured={"reason": _channel.R_CYCLE_UNKNOWN, "game_date": game.get("date")})
    row = _channel.attempt(_CHANNEL, ledger, to=str(to or ""), text=str(text or ""),
                           game_date=game.get("date"), tick=game.get("tick"),
                           cycle=cycle)
    shown = {k: row[k] for k in ("msg_id", "sender", "recipient", "cycle", "bytes",
                                 "text_sha256", "verdict", "game_date", "tick")}
    if row["verdict"] == _channel.V_DELIVERED:
        return redesign.envelope("msg_send", verdict="applied", result=shown,
                                 game=game, bridge_health=gw.health)
    detail = {
        _channel.V_TOPOLOGY_DENIED: (
            f"{_CHANNEL.seat} may not write to {row['recipient']!r} under the "
            f"{_CHANNEL.topology} topology (seats: {', '.join(_CHANNEL.seats)})"),
        _channel.V_TEXT_TOO_LONG: (
            f"{row['bytes']} bytes exceeds the declared bound of "
            f"{_CHANNEL.max_text_bytes}; nothing was sent and nothing truncated"),
        _channel.V_QUOTA_EXCEEDED: (
            f"{_CHANNEL.quota_per_cycle} messages were already delivered in cycle "
            f"{row['cycle']}; the quota resets when your session(action='wait') calls have "
            "carried the game date past this cycle's boundary"),
    }[row["verdict"]]
    return redesign.envelope("msg_send", ok=False, verdict="rejected", result=shown,
                             blocked=redesign.why_blocked(row["verdict"], summary=detail),
                             game=game, bridge_health=gw.health)


# REGISTERED ONLY UNDER A DECLARED TOPOLOGY OTHER THAN `none`: in
# Null mode, single-seat runs and free play the name is absent from list_tools, not present-and-
# refused.
if _CHANNEL is not None and _CHANNEL.enabled:
    mcp.add_tool(msg_send)


# ---------------------------------------------------------------------------
# note_read / note_write
# ---------------------------------------------------------------------------

async def _notepad_call(tool: str):
    """(state, gw, store, game, cycle) for a notepad call, or (refusal, ...)
    when the call cannot reach a log: no declaration, no seat label, no
    episode id. The cycle is the cycle authority's at the bridge's date
    (`notepad.cycle_at`); None is decided by the log (`notepad_cycle_unknown`)."""
    state = _get_state()
    err = await _ensure_connected(state, tool)
    if err:
        return err, None, None, None, None
    gw = await _gateway_now()
    if _NOTEPAD is None or not _NOTEPAD.enabled:
        return _blocked(tool, state, _notepad.GATE_UNAVAILABLE,
                        "this seat's run declares no notepad"), None, None, None, None
    if _NOTEPAD.seat_refusal():
        return _blocked(tool, state, _notepad.GATE_UNAVAILABLE,
                        f"{_NOTEPAD.seat_refusal()}: this process has no {_notepad.ENV_SEAT}, "
                        "so it is nobody's seat and has no pad",
                        measured={"reason": _NOTEPAD.seat_refusal()}), None, None, None, None
    eid = _SEAT.episode_id or _SEAT.resolve_episode_id(os.environ, await gw.read("get_tick_info", {}))
    if not eid:
        return _blocked(tool, state, _notepad.GATE_UNAVAILABLE,
                        "no episode id: neither CK3_EPISODE_ID nor the bridge's get_tick_info "
                        "pipeline.episode_id names the episode, so there is no notepad log; "
                        "nothing was read or written"), None, None, None, None
    store = _notepad.Store(_notepad.log_path(eid, os.environ), episode_id=eid,
                           instance_id=_instance_id())
    game = gw.game()
    return None, gw, store, game, _notepad.cycle_at(game.get("date"))


_NOTE_REFUSAL_DETAIL = {
    _notepad.V_PAD_UNKNOWN: "pad must be 'private' or 'council'",
    _notepad.V_PAD_NOT_DECLARED: "this run declares no such pad for its seats",
    _notepad.V_MODE_INVALID: "mode must be 'replace' or 'append'",
    _notepad.V_CYCLE_UNKNOWN: ("the episode's cycle could not be read from the cycle schedule "
                               "at the bridge's game date; nothing was done"),
    _notepad.V_WRITE_TOO_LONG: "the text exceeds the declared bytes per write; nothing was written or truncated",
    _notepad.V_VERSION_REQUIRED: "a replace names the pad version you last read (note_read's `version`)",
    _notepad.V_VERSION_STALE: ("the pad changed since the version you read; note_read it again and "
                               "write against the new version (or append)"),
    _notepad.V_PAD_FULL: "the pad would exceed its declared size; nothing was written or truncated",
}


async def note_read(
    pad: Annotated[str, Field(description="'private' or 'council'.",
                              json_schema_extra={"enum": list(_notepad.PADS)})],
) -> dict[str, Any]:
    """Read one of your notepads, whole. `pad` is `private` (yours alone; it
    persists across your turns and resumes) or `council` (one pad shared by
    every seat of the council, where your arm has one). The reply carries the
    text, its `version` (pass it to note_write to replace the pad), its bytes
    and the declared maximum. Every read is logged.

    Args:
        pad: 'private' or 'council'.
    """
    err, gw, store, game, cycle = await _notepad_call("note_read")
    if err:
        return err
    rec, view = _notepad.note_read_attempt(_NOTEPAD, store, pad=str(pad or ""), game_date=game.get("date"),
                              tick=game.get("tick"), cycle=cycle)
    shown = {k: rec.get(k) for k in ("pad", "seat", "cycle", "game_date", "verdict")}
    if view is not None:
        shown.update(text=view["text"], version=view["version"], bytes=view["bytes"],
                     max_bytes=_NOTEPAD.max_bytes(rec["pad"]), updated_by=view["updated_by"],
                     updated_game_date=view["updated_game_date"])
        return redesign.envelope("note_read", result=shown, game=game, bridge_health=gw.health)
    return redesign.envelope("note_read", ok=False, verdict="rejected", result=shown,
                             blocked=redesign.why_blocked(rec["verdict"],
                                                          summary=_NOTE_REFUSAL_DETAIL[rec["verdict"]]),
                             game=game, bridge_health=gw.health)


async def note_write(
    pad: Annotated[str, Field(description="'private' or 'council'.",
                              json_schema_extra={"enum": list(_notepad.PADS)})],
    text: str,
    mode: Annotated[str, Field(description="'append' or 'replace' (no default).",
                               json_schema_extra={"enum": list(_notepad.MODES)})],
    version: Optional[int] = None,
) -> dict[str, Any]:
    """Write to one of your notepads. `mode` is required: `append` adds `text`
    to the end of the pad exactly as given (start it with a newline if you want
    one); `replace` sets the pad to `text` and must name the `version` you last
    read, and is refused `notepad_version_stale` if the pad changed since
    (another seat wrote to the council pad): read again and retry. A write
    over the declared bytes per write, or one that would take the pad over its
    declared size, is refused, never truncated. Every write is logged.

    Args:
        pad: 'private' or 'council'.
        text: the text to append, or the pad's new whole text.
        mode: 'append' or 'replace'.
        version: for replace, the version note_read returned.
    """
    err, gw, store, game, cycle = await _notepad_call("note_write")
    if err:
        return err
    rec = _notepad.note_write_attempt(_NOTEPAD, store, pad=str(pad or ""), text=str(text or ""), mode=str(mode or ""),
                         version=version if isinstance(version, int) and not isinstance(version, bool) else None,
                         game_date=game.get("date"), tick=game.get("tick"), cycle=cycle)
    shown = {k: rec.get(k) for k in ("pad", "seat", "mode", "cycle", "game_date", "bytes", "verdict",
                                     "version_before", "version_after", "pad_bytes_after")}
    if rec["verdict"] == _notepad.V_APPLIED:
        return redesign.envelope("note_write", verdict="applied", result=shown, game=game,
                                 bridge_health=gw.health)
    return redesign.envelope("note_write", ok=False, verdict="rejected", result=shown,
                             blocked=redesign.why_blocked(rec["verdict"],
                                                          summary=_NOTE_REFUSAL_DETAIL[rec["verdict"]]),
                             game=game, bridge_health=gw.health)


# REGISTERED ONLY WHEN THE ARM DECLARES A PAD: absent otherwise, never
# present-and-refused (msg_send's rule).
if _NOTEPAD is not None and _NOTEPAD.enabled:
    mcp.add_tool(note_read)
    mcp.add_tool(note_write)


# ---------------------------------------------------------------------------
# guide_read — the experiment's guide
# ---------------------------------------------------------------------------

async def guide_read(
    topic: Annotated[str, Field(description="A topic from the guide's index; an unknown topic returns the index.")] = "index",
) -> dict[str, Any]:
    """Read one page of the guide. `topic` is a name from the index (your play
    file lists each with when to open it); a page may name deeper topics to
    read next. An unknown topic returns the index. Every read is logged.

    Args:
        topic: the page to read, or 'index'.
    """
    state = _get_state()
    err = await _ensure_connected(state, "guide_read")
    if err:
        return err
    gw = await _gateway_now()
    if _GUIDE is None:
        return _blocked("guide_read", state, _guide.GATE_UNAVAILABLE, "this seat declares no guide")
    eid = _SEAT.episode_id or _SEAT.resolve_episode_id(os.environ, await gw.read("get_tick_info", {}))
    if not eid:
        return _blocked("guide_read", state, _guide.GATE_UNAVAILABLE,
                        "no episode id: neither CK3_EPISODE_ID nor the bridge's get_tick_info "
                        "pipeline.episode_id names the episode, so there is no guide log; nothing was read")
    game = gw.game()
    cycle, why = _guide.cycle_at(game.get("date"))
    rec, text = _guide.read_attempt(_GUIDE, topic=topic, game_date=game.get("date"), tick=game.get("tick"),
                                    cycle=cycle, cycle_why=why, episode_id=eid, instance_id=_instance_id(),
                                    log=_guide.log_path(eid, os.environ))
    shown = {k: rec.get(k) for k in ("topic_asked", "topic_served", "verdict", "bytes", "cycle", "game_date")}
    if text is None:
        return redesign.envelope("guide_read", ok=False, verdict="rejected", result=shown,
                                 blocked=redesign.why_blocked(rec["verdict"], summary=rec.get("detail") or ""),
                                 game=game, bridge_health=gw.health)
    shown["text"] = text
    return redesign.envelope("guide_read", result=shown, game=game, bridge_health=gw.health)


# REGISTERED ONLY WHEN THE SEAT'S ENVIRONMENT DECLARES A GUIDE (msg_send's rule).
if _GUIDE is not None:
    mcp.add_tool(guide_read)


def _remember_actions() -> None:
    """Every action tool's call goes through the seat's
    action memory (the `refusal_repeated` trigger). Wrapped at the tool
    manager so the tools themselves are untouched."""
    import functools
    for name in _pw.ACTION_TOOLS:
        tool = mcp._tool_manager.get_tool(name)
        if tool is None:
            continue

        def wrap(fn, tool_name):
            @functools.wraps(fn)
            async def recorded(**kwargs):
                env = await fn(**kwargs)
                try:
                    _ACTIONS.record(tool_name, kwargs, env)
                except Exception:                               # noqa: BLE001
                    pass
                return env
            return recorded
        tool.fn = wrap(tool.fn, name)


if _ACTIONS is not None:
    _remember_actions()


def _strip_titles(node: Any) -> None:
    """Drop pydantic's generated `title` strings ("Character Id") from a schema: they
    repeat the property name and cost a seat's context on every listing. A property
    NAMED title (discover's) is a dict under `properties` and is kept."""
    if isinstance(node, dict):
        if isinstance(node.get("title"), str):
            del node["title"]
        for v in node.values():
            _strip_titles(v)
    elif isinstance(node, list):
        for v in node:
            _strip_titles(v)


def _shape_registered_schemas() -> None:
    """The served schemas, after every tool is registered. The titles go; and
    with CK3_CLOCK_OWNER=env (redesign.clock_owned_by_env) session lists no clock
    action and no clock argument (a call with one is refused unknown_argument, an
    action refused clock_owned_by_env)."""
    for t in mcp._tool_manager.list_tools():
        _strip_titles(t.parameters)
        fn = globals().get(t.name)
        if callable(fn):
            fn.__doc__ = t.description      # __doc__ and the served description agree
    if redesign.clock_owned_by_env():
        props = mcp._tool_manager.get_tool("session").parameters["properties"]
        props["action"]["enum"] = [a for a in props["action"]["enum"]
                                   if a not in redesign.SESSION_CLOCK_ACTIONS]
        for k in ("speed", "seconds", "game_days"):
            props.pop(k, None)


_shape_registered_schemas()


def registered_tool_definitions() -> list[dict]:
    """What this server registers, read from FastMCP's tools/list handler.

    One reader of the surface for everyone who needs it as data: the generated
    tool reference renders from them, and the surface probe takes the names. Each row is {name, description, input_schema}. Run in a thread so a
    caller's running event loop cannot block or refuse it.
    """
    import threading as _th
    box: list = []
    err: list = []

    def _run():
        try:
            box.append(asyncio.run(mcp.list_tools()))
        except Exception as e:                                  # noqa: BLE001
            err.append(e)

    t = _th.Thread(target=_run, daemon=True)
    t.start()
    t.join(30)
    if err:
        raise err[0]
    if not box:
        raise RuntimeError("FastMCP list_tools did not return within 30 s")
    return [{"name": x.name, "description": x.description or "",
             "input_schema": x.inputSchema or {"type": "object", "properties": {}}}
            for x in box[0]]


def _write_tool_surface_record() -> None:
    """THE SERVER IS THE ONLY AUTHORITY ON WHAT IT REGISTERED.

    A tool surface graded from the CLI's `init` event can come up short: that
    event once listed two tools fewer than the seat had registered —
    the two absent being the ones registered LAST (the CLI defers schemas, so
    its init list can be short of the real surface). All six seats were refused
    `arm_tools_absent` for tools they had. A false refusal of a functioning seat
    costs a slot and leaves the seats running.

    So the surface is written down by the process that decided it. The witness
    reads this and keeps the init list beside it as a second, weaker reading.

    NEVER FATAL, AND NEVER A FALSE PASS. Any failure here leaves no file, and an
    absent record makes the witness say so rather than assume either way — the
    safe direction, because a seat whose surface cannot be confirmed must not be
    certified as the registered instrument. Enumeration degrades the same way:
    if a FastMCP upgrade moves the registry, this writes `enumerated_via:
    unavailable` instead of a short list that would read as missing tools.
    """
    out = os.environ.get("CK3_TOOL_SURFACE_OUT")
    if not out:
        return
    names: list = []
    via = "unavailable"
    try:
        tm = getattr(mcp, "_tool_manager", None)
        listed = tm.list_tools() if tm is not None and hasattr(tm, "list_tools") else None
        if listed:
            names = sorted(str(getattr(t, "name", t)) for t in listed)
            via = "tool_manager.list_tools"
        else:
            reg = getattr(tm, "_tools", None)
            if isinstance(reg, dict):
                names = sorted(str(k) for k in reg)
                via = "tool_manager._tools"
    except Exception:                                           # noqa: BLE001
        names, via = [], "unavailable"
    # What the MODEL is served, not just the names: FastMCP.list_tools()
    # is the tools/list handler, so its output IS the surface (name, description,
    # inputSchema). Run in a thread so an importer's running loop cannot block it.
    schemas, schemas_via = None, "unavailable"
    try:
        import threading as _th
        box: list = []
        t = _th.Thread(target=lambda: box.append(asyncio.run(mcp.list_tools())), daemon=True)
        t.start()
        t.join(10)
        if box:
            schemas = sorted((x.model_dump(mode="json", by_alias=True, exclude_none=True)
                              for x in box[0]), key=lambda d: d.get("name", ""))
            schemas_via = "fastmcp.list_tools"
    except Exception:                                           # noqa: BLE001
        schemas, schemas_via = None, "unavailable"
    try:
        import datetime as _dt
        import hashlib as _hl
        doc = {
            "criterion": "ck-jpej",
            "written_utc": _dt.datetime.now(_dt.timezone.utc).strftime(
                "%Y-%m-%dT%H:%M:%SZ"),
            "arm": (os.environ.get("CK3_EPISODE_ARM") or "").strip().lower(),
            "enumerated_via": via,
            "tools": names,
            "schemas_via": schemas_via,
            "schemas": schemas,
            "schemas_sha256": (_hl.sha256(json.dumps(schemas, sort_keys=True).encode()).hexdigest()
                               if schemas is not None else None),
        }
        pth = os.path.expanduser(out)
        os.makedirs(os.path.dirname(pth) or ".", exist_ok=True)
        with open(pth, "w", encoding="utf-8") as fh:
            fh.write(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    except Exception:                                           # noqa: BLE001
        return


# RELEASE THIS SESSION'S OWN CLAIMS WHEN THE SERVER STOPS. The bridge
# refuses a release from any session but the owner (not_owned_by_session), so
# `ck3agent run`'s teardown cannot do it after the fact. Opt-in: the CLI sets
# CK3_RELEASE_ON_EXIT to a record path. Never set in an experiment episode,
# whose claims are pinned across a dropped socket by design.
RELEASE_ON_EXIT_ENV = "CK3_RELEASE_ON_EXIT"
#: Set beside it by the CLI: where the server writes its session id and resume
#: token, so `ck3agent run` can resume THIS session and release what is left
#: when the server is killed first (Claude Code SIGKILLs it 500 ms after SIGINT).
SESSION_OUT_ENV = "CK3_SESSION_OUT"
_EXIT_SIGNAL: list = []
_EXIT_TASK: list = []          # (loop, main task, state), set by _arm_exit_signals
_RELEASE: list = []            # the one release task; the lifespan's finally awaits it
_STOPPER: list = []            # the task that releases, then cancels the main task


def _on_exit_signal(signum, _frame):
    """The first SIGINT/SIGTERM/SIGHUP starts the release AT ONCE on the running
    loop, and only then is the main task cancelled. Claude Code sends SIGINT,
    SIGTERM 100 ms later and SIGKILL 400 ms after that, so the release cannot
    wait for the server to unwind first. Later signals are ignored."""
    if not _EXIT_SIGNAL:
        _EXIT_SIGNAL.append(int(signum))
        if _EXIT_TASK:
            _EXIT_TASK[0][0].call_soon_threadsafe(_begin_exit)


def _begin_exit() -> None:
    """On the loop: release first, then cancel the main task."""
    loop, task, state = _EXIT_TASK[0]

    async def _release_then_stop():
        await _release_once(state)
        task.cancel()

    _STOPPER.append(loop.create_task(_release_then_stop()))    # held: the loop keeps only weak refs


def _install_exit_signals() -> None:
    """At import, so a signal during startup is held rather than fatal."""
    import signal
    import threading
    if not os.environ.get(RELEASE_ON_EXIT_ENV) or threading.current_thread() is not threading.main_thread():
        return
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, _on_exit_signal)


def _arm_exit_signals(state: "BridgeState") -> None:
    """In the lifespan, before it yields: from here a signal starts the release.
    A signal held during startup starts it now."""
    if os.environ.get(RELEASE_ON_EXIT_ENV) and not _EXIT_TASK:
        _EXIT_TASK.append((asyncio.get_running_loop(), asyncio.current_task(), state))
        log.info("release on exit armed")
        if _EXIT_SIGNAL:
            _begin_exit()


def _record_session_at_handshake(client) -> None:
    """Wrap client.handshake so each new session's id and resume token are on
    disk (0600) before anything is claimed through it. _try_reconnect wraps
    its new client too, so the file always names the current session."""
    out = os.environ.get(SESSION_OUT_ENV)
    if not out or not os.environ.get(RELEASE_ON_EXIT_ENV):
        return
    inner = client.handshake

    async def handshake(*a, **kw):
        ack = await inner(*a, **kw)
        if isinstance(ack, dict) and ack.get("session_id") and ack.get("resume_token"):
            import datetime as _dt
            rec = {"session_id": ack["session_id"], "resume_token": ack["resume_token"],
                   "pid": os.getpid(),
                   "written_utc": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")}
            # Every session this seat's servers have held in this run,
            # oldest first, carried from the record this one replaces: the
            # run attributes action-log rows by it (ck3agent run, actions.jsonl).
            pth0 = os.path.expanduser(out)
            try:
                with open(pth0, encoding="utf-8") as fh:
                    prev = json.loads(fh.read() or "{}")
            except (OSError, ValueError):
                prev = {}
            seen = [x for x in (prev.get("sessions") or []) if isinstance(x, str)] \
                if isinstance(prev, dict) else []
            if isinstance(prev, dict) and isinstance(prev.get("session_id"), str) \
                    and prev["session_id"] not in seen:
                seen.append(prev["session_id"])
            if ack["session_id"] not in seen:
                seen.append(ack["session_id"])
            rec["sessions"] = seen
            if kw.get("resume_token"):
                # A token was presented (a hand-over or a reconnect):
                # whether the bridge resumed that session, and its word why not.
                rec["resumed"] = ack.get("resumed") is True
                rec["resume_status"] = ack.get("resume_status")
            pth = os.path.expanduser(out)
            tmp = f"{pth}.{os.getpid()}.tmp"
            try:
                fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
                with os.fdopen(fd, "w", encoding="utf-8") as fh:
                    fh.write(json.dumps(rec) + "\n")
                os.replace(tmp, pth)
            except OSError as exc:
                log.warning("could not record the session for release: %s", exc)
        return ack

    client.handshake = handshake


def _record_prebind(prebind: Optional[dict]) -> None:
    """Add the pre-bind outcome to the session record (CK3_SESSION_OUT),
    so `ck3agent run` can list in its manifest every ruler bound at start. Same
    file, same 0600 atomic replace; never raises."""
    out = os.environ.get(SESSION_OUT_ENV)
    if not out or not os.environ.get(RELEASE_ON_EXIT_ENV) or not prebind:
        return
    pth = os.path.expanduser(out)
    try:
        with open(pth, encoding="utf-8") as fh:
            rec = json.loads(fh.read() or "{}")
    except (OSError, ValueError):
        return                       # no session record: nothing to annotate
    if not isinstance(rec, dict):
        return
    rec["prebind"] = [{k: p.get(k) for k in ("character_id", "ok", "reason", "bound_at")
                       if k != "bound_at" or "bound_at" in p}
                      for p in [prebind, *(prebind.get("also") or [])]]
    # How long the game was waited for before the bind, and
    # whether it was seen active; absent when the server predates the wait.
    if isinstance(prebind.get("wait"), dict):
        rec["prebind_wait"] = dict(prebind["wait"])
    tmp = f"{pth}.{os.getpid()}.tmp"
    try:
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(json.dumps(rec) + "\n")
        os.replace(tmp, pth)
    except OSError as exc:
        log.warning("could not record the pre-bind: %s", exc)


# Handlers first: the CLI and its tests take the tool surface file as "the
# server is up", and a SIGINT after that must not be fatal.
_install_exit_signals()
_write_tool_surface_record()


async def _release_once(state: "BridgeState") -> None:
    """Run the release at most once; a second caller waits for the first."""
    import anyio
    if not _RELEASE:
        _RELEASE.append(asyncio.ensure_future(_release_on_exit_now(state)))
    with anyio.CancelScope(shield=True):
        await asyncio.shield(_RELEASE[0])


async def _release_on_exit(state: "BridgeState") -> None:
    """From the lifespan's finally: the release, unless the signal path ran it."""
    if not os.environ.get(RELEASE_ON_EXIT_ENV) or state.mock_mode:
        return
    await _release_once(state)


#: Snapshot rounds. A claim still in flight at the first snapshot shows up in a
#: later one, so there are always two; after that a round that finds nothing new
#: ends the loop. What a server killed mid-loop leaves, `ck3agent run` releases
#: by resuming this session.
RELEASE_ROUNDS = 5


async def _release_on_exit_now(state: "BridgeState") -> None:
    """Release every character THIS session holds; write what happened. Never raises."""
    out = os.environ.get(RELEASE_ON_EXIT_ENV)
    if not out or state.mock_mode:
        return
    import anyio
    import signal
    sid = getattr(state._client, "session_id", None) if state._client else None
    rec: dict = {"trigger": (signal.Signals(_EXIT_SIGNAL[0]).name if _EXIT_SIGNAL else "shutdown"),
                 "session_id": sid, "claims": None, "released": [], "refused": {},
                 "held_after": None, "rounds": 0, "error": None}
    tried: list = []
    with anyio.CancelScope(shield=True), anyio.move_on_after(12) as scope:
        try:
            if not (state.connected and sid):
                raise ConnectionError("no bridge session")
            while True:
                bs = await _send_and_wait(state, "get_bridge_state", {}, timeout=4.0)
                row = next((r for r in bs.get("sessions") or []
                            if isinstance(r, dict) and r.get("session_id") == sid), None)
                if row is None:
                    raise LookupError(f"session {sid} not in get_bridge_state.sessions")
                held = [int(c) for c in row.get("claimed_character_ids") or []]
                rec["rounds"] += 1
                rec["held_after"] = held
                new = [c for c in held if c not in tried]
                if (not new and rec["rounds"] >= 2) or rec["rounds"] > RELEASE_ROUNDS:
                    break
                for cid in new:
                    tried.append(cid)
                    r = await _send_and_wait(state, "release_character", {"character_id": cid},
                                             timeout=4.0)
                    if isinstance(r, dict) and r.get("success") is True:
                        rec["released"].append(cid)
                    else:
                        rec["refused"][str(cid)] = (r.get("reason") or "no success") \
                            if isinstance(r, dict) else repr(r)
            rec["claims"] = tried
        except Exception as exc:                                # noqa: BLE001
            rec["error"] = f"{type(exc).__name__}: {exc}"
            rec["claims"] = tried or None
    if scope.cancelled_caught:
        rec["error"] = "timed out after 12s"
    try:
        with open(os.path.expanduser(out), "w", encoding="utf-8") as fh:
            fh.write(json.dumps(rec, indent=1, sort_keys=True) + "\n")
    except OSError:
        pass
    log.info("release on exit: %s", rec)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import argparse
    import os

    # Add file-based debug logging
    _log_path = os.path.join(os.environ.get('HOME', '/tmp'), '.ck3-mcp-debug.log')
    _debug_fh = logging.FileHandler(_log_path)
    _debug_fh.setLevel(logging.DEBUG)
    _debug_fh.setFormatter(logging.Formatter('%(asctime)s [%(levelname)s] %(name)s: %(message)s'))
    logging.getLogger('ck3-mcp').addHandler(_debug_fh)
    logging.getLogger('bridge_client').addHandler(_debug_fh)

    # Configure logging to stderr (critical for stdio transport)
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        stream=sys.stderr,
    )

    parser = argparse.ArgumentParser(description="CK3 Bridge MCP Server")
    parser.add_argument("--transport", default="stdio",
                        choices=["stdio", "streamable-http"],
                        help="MCP transport type")
    parser.add_argument("--port", type=int, default=8000,
                        help="Port for HTTP transport")
    args, _ = parser.parse_known_args()     # --mock is read from sys.argv at import (MOCK_MODE)

    if args.transport == "streamable-http":
        mcp.run(transport="streamable-http", port=args.port)
    else:
        mcp.run(transport="stdio")
