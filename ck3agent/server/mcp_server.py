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
# test / agent.py) resolves the siblings too.
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from mcp.server.fastmcp import FastMCP, Context
from pydantic import Field, StrictBool

from tools import QUERY_DOMAINS, DISCOVER_WHATS
import redesign
import seat_lineage                      # v3 §7.5: the seat is a lineage (inert unless armed)
import do_intent_args

#: The most domains one query(domains=[...]) call reads.
QUERY_BATCH_MAX = 5

# ck-2r9k. §8.2 — what a pipeline-mode reply actually looks like — lives in ONE
# module, and this file had zero occurrences of it. The three clients that got
# this wrong got it wrong three different ways; a fourth reading of
# `for_request_id` here would be a fourth way.
import pipeline_correlation as _pcorr
# Study 0 (channel lane), pre-registration v3 section 5.2: the inter-seat
# channel. One definition of the keys, the topology rule and the ledger; this
# file only registers the tool when the seat's arm declares a topology other
# than `none`, and delivers inboxes inside `observe`.
import channel as _channel

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


# ---------------------------------------------------------------------------
# Redesign (SPEC docs/notes/mcp-redesign/SPEC.md): the gateway the goal-shaped
# tools compose through — dispatch, single-flight serialization of the bridge's
# deferred-drain reads, and since-tick delta tracking — so the tools stay pure
# composition. It never touches the clock (L13): only session(pause|unpause|
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
    reads "paused" forever (witness W1); game() is the last fresh reading, taken
    at every tool's entry (_gateway_now) and by every confirmed_write.
    """

    #: Reads the bridge serves on the game thread (snapshot.cpp is_game_thread_read):
    #: they run the game's triggers under the fake random scope (lane l6, witness #2)
    #: and leave no record in the completed ring (witness #3). Checked against the
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
        # ck-6kvq.13 (R6): the decision-budget governor's write surface.
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
        bridge reply carries (#123), else the reading's."""
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
            return {"reason": "bridge_disconnected"}
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

        ck-p3vd. The agent paid one flush period PER READ: observe(depth=full)
        fans out to ~10 reads awaited in turn, ~10 s each, ~100 s for one
        observation — the whole graded episode. A flush serves its entire buffer
        in one span (flush_scheduler.cpp), so reads issued together cost ONE
        period however many there are.

        MEASURED by ck-zgh4 on the live bridge, four independent reads:
            sequential            20.01 s
            one call_many envelope 10.23 s
            SEPARATE envelopes issued back-to-back then collected  9.83 s
        The third is this design, and it matches the batched envelope — so no
        new client method is needed. `_send_and_wait` already mints a uuid4
        request_id per call and registers its own future, so concurrent issue
        correlates BY CONSTRUCTION, with none of the misrouting risk that made
        ck-iu11 expensive.

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
        waits, L13 contract D5 amendment 3)."""
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
        """Since-tick events as RECORDS (ck-s8vr): the war, scheme and council-task
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
    """The bridge's management-ledger record (ck-324r, built by
    management_ledger_frame.h): `type: result`, `ledger_source:
    inline_management`, the caller's id under `caller_request_id` and NO
    `request_id` (ck-h4pn; the caller's id rides under `caller_request_id` when
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
    envelope's game block says what get_tick_info said at the call (witness W1:
    nothing else refreshes it; m-16: no tool shows a stale reading).

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
    #: ck-ey8p. Frames this listener's type dispatch did not handle, counted by
    #: `type`. THE DICT IS EMPTY UNTIL ONE ARRIVES, never pre-seeded with zeroes:
    #: an absent key means none of that type was seen, and a zero would claim a
    #: reading nobody took. Before this existed the dispatch had no `result`
    #: branch and no terminal else, so a ledger record matching no pending id
    #: was discarded with NO log line and NO counter — the client could not
    #: witness an unmatched record even in principle, and a zero in a bundle
    #: could not be told from "none was ever emitted".
    unhandled_frames: dict = field(default_factory=dict)
    #: v3 §7.5 (seat_lineage): what this seat last knew of its ruler and the
    #: title its lineage follows. None until the first armed observe.
    seat_lineage_prior: Any = None
    #: ck-324r's management-ledger records, counted by `action`: the bridge
    #: broadcasts one on every set_controlled_character / release_character for
    #: the ARM relay, with the caller's id under `caller_request_id` and,
    #: deliberately, no `request_id` (ck-h4pn), so no caller is waiting on it.
    #: EXPECTED traffic, not an unmatched frame: counted here, logged at DEBUG.
    #: Empty until one arrives, like unhandled_frames.
    management_ledger_frames: dict = field(default_factory=dict)
    action_log: list = field(default_factory=list)
    pending_queries: dict = field(default_factory=dict)  # request_id -> asyncio.Future
    #: ck-2r9k, THE SECOND CONSUMER. request_id -> the flush ETA IN SECONDS that
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
    #: ck-2r9k. request_id -> the ticket its ack named. THE ONLY HANDLE ON A
    #: CALL WHOSE PUSH WAS LOST: `check` is keyed by ticket, not by the original
    #: request_id, so without this a missed push is unrecoverable and the call
    #: can only ever time out.
    pending_tickets: dict = field(default_factory=dict)  # request_id -> ticket
    _client: Optional[object] = None
    _listener_task: Optional[asyncio.Task] = None
    _gateway: Optional[object] = None  # BridgeGateway for the goal-shaped tools
    mock_mode: bool = False
    #: lane-multi / lane-prebind: the outcome of binding CK3_AGENT_CHARACTER_ID
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

            # #123: every bridge response now stamps the LIVE tick + game_date at
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

            # ── ck-2r9k, THIS CONSUMER: THE ACK CARRIES THE DEADLINE ─────────
            #
            # The block above routes STRICTLY by `request_id`. The pipeline's
            # acceptance ack is keyed `for_request_id` and carries no
            # `request_id` at all (bridge/src/pipeline.cpp emit_accepted, §8.2),
            # so it has always fallen straight past it into the terminal else
            # and been counted as an unhandled frame.
            #
            # ck-2r9k fixed that in agent.py. IT DID NOT REACH HERE, and this is
            # the consumer every write tool runs through: BridgeGateway.write ->
            # _action_bridge -> _send_and_wait, all in this file. Two clients,
            # one protocol, one of them taught.
            #
            # THE ACK EXTENDS THE DEADLINE; IT DOES NOT SETTLE THE CALL. A
            # write's outcome is not in the ack: a write the flush refuses is
            # `accepted: true` here and `success: false` in the settled frame,
            # so resolving on the ack would report a REFUSAL AS A SUCCESS. The
            # future still settles on the real result.
            #
            # BOTH CONSUMERS NOW DO THIS, and the history is worth the three
            # lines because it is the argument. ck-2r9k made agent.py resolve on
            # the ack; ck-xksn (4859d957) found that was the defect above and
            # made it extend-and-wait, which is what this does. So the comment
            # that used to stand here — "agent.py resolves the future on the
            # ack" — went stale at 77d4d01b and is corrected rather than
            # deleted: a reader who finds the two clients agreeing should know
            # they once did not, and why. pipeline_correlation says it plainly —
            # "the result genuinely CANNOT arrive before this, so treating the
            # wait as a timeout is how a correct pipeline-mode bridge gets
            # reported broken". The module has said so since ck-6kvq.18; this
            # file had zero occurrences of it.
            if msg_type == "accepted":
                for pending_rid in list(state.pending_queries):
                    if _pcorr.is_our_ack(msg, pending_rid):
                        state.pending_acks[pending_rid] = \
                            _pcorr.ack_eta_seconds(msg)
                        # ck-2r9k: and the TICKET, which is the handle `check`
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
                # tick_start, so a cache of this frame said "paused" forever (W1).
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
                # ck-ey8p: THE TERMINAL ELSE. Everything above names a type; a
                # frame of any other type used to reach here and vanish. The
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
                    future.set_result({"error": "bridge_disconnected"})
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
    _record_session_at_handshake(client)   # rc2 B7: before the pre-bind claims anything

    EXPECTED_BRIDGE_VERSION = "v8-2026-03-20"

    connected = await client.connect()
    if connected:
        state.connected = True
        ack = await _handshake_and_bind(client, state)
        if ack:
            state.handshake_info = ack
            # rc2: "bridge_version" is the release version; the wire protocol tag moved to "bridge_protocol".
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
        if state._listener_task is None or state._listener_task.done():   # a pre-bind starts it
            state._listener_task = asyncio.create_task(_bridge_listener(state))
        log.info("Bridge connected, listener started")
    else:
        log.warning("Bridge not available — tools will return errors. "
                     "Start CK3 with bridge DLL and reconnect.")

    _arm_exit_signals(state)            # rc2 B7: opt-in, see _release_on_exit
    try:
        yield state
    finally:
        # lane-prebind: a pre-bind still waiting for the game must not land in
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


class _StrictArgumentsFastMCP(FastMCP):
    """L18 ruling (a): every tool refuses an argument its schema does not declare.

    FastMCP's argument model ignores an extra key, so a call carrying one ran as
    if the key were absent: pass_turn(character_id=...) on 174 pilot calls (fixed
    on that tool as the explicit case) and steer's flat axes once. Refuse rather
    than default: the refusal is one place, before the tool runs, with the typed
    gate `unknown_argument` naming each unknown key and the declared ones.

    What this does NOT cover, said so a reader does not assume it:
      * agent.py, the in-repo autonomous loop, is uncovered. It dispatches tool
        calls its own way (redesign.*_impl, directly) and never reaches this
        call_tool, so an unknown key it
        sends is not refused here. It does not ship (release/manifest.toml
        excludes it: "agent.py, the retired loop, which does not ship"), so the
        product surface is all of this class's.
      * Only top-level keys are checked. Keys nested inside a dict argument
        (do's `args`, declare_war's `declare_args`, set_phase's `overrides`) are
        each tool body's to read; set_phase refuses an unknown axis itself."""

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
        return await super().call_tool(name, arguments)


mcp = _StrictArgumentsFastMCP(
    "CK3 Bridge",
    instructions=(
        # ck-zgh4 deploy-51: "the LLM acts as a 'personality tuner'" told the
        # caller what its ROLE was. It describes what the tools do instead.
        "This server connects to a running Crusader Kings 3 game via an "
        "injected bridge library. Its tools read game state and write changes "
        "to it: personality weights and typed actions "
        "such as interactions, council tasks, laws and war declarations. The "
        "game's own AI continues to run everything not written through them.\n\n"
        "Each cycle: observe first; read deeper with query, inspect, discover or cbs; "
        "plan with plan_war, succession_plan or set_phase; act with do, declare_war "
        "or steer; let time pass with session(action='wait').\n"
        "Every reply is one envelope: {ok, tool, result, blocked, warnings, errors, "
        "bridge_health}, plus verdict (applied | rejected | pending) on writes. "
        "When ok is false, blocked.gate names why and blocked.next, when present, "
        "says what to call instead."
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
    return {
        "connected": state.connected,
        "mock_mode": state.mock_mode,
        "tick_id": state.tick_id,
        "game_date": state.game_date,
        "handshake": state.handshake_info,
    }


# ---------------------------------------------------------------------------
# Helper functions
# ---------------------------------------------------------------------------

def _get_state() -> BridgeState:
    ctx = mcp.get_context()
    return ctx.request_context.lifespan_context


def _extract_resolved_id(data: Any) -> int | None:
    """Lift the resolved character_id the bridge already echoed in an inner section.

    #94 fix: observe()/steer()/query() with no character_id silently resolve to the
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
    non-dict is carried as {"value": ...}. #94: the character the bridge actually
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
    the read twin of the write predicate (L14 FF3 N3-1): a game-thread read that
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
        state._client = AsyncBridgeClient()
        # rc2 B7 (cli-4): the new client's session is recorded like the first
        # one, or CK3_SESSION_OUT keeps naming a session that no longer holds
        # the claims and `ck3agent run` resumes the wrong one after a SIGKILL.
        _record_session_at_handshake(state._client)
        connected = await state._client.connect()
        if connected:
            state.connected = True
            # ck-mxlt: a reconnect mints a NEW session (session.cpp:819 —
            # "RELEASED and the session ERASED; the client must handshake"), so
            # the claim does NOT survive it. Re-bind here or the agent carries
            # on unbound for the rest of the episode with nothing saying so.
            # Under release-on-exit the old token is presented first: where the
            # bridge kept the session (claims pinned) the claims stay on one
            # session; where it destroyed it the bridge answers unknown_token
            # and mints a fresh one, and the re-bind below claims again.
            ack = await _handshake_and_bind(state._client, state, resume_token=resume)
            if ack:
                state.handshake_info = ack
            if state._listener_task is None or state._listener_task.done():
                state._listener_task = asyncio.create_task(_bridge_listener(state))
            log.info("Bridge reconnected successfully")
            return True
    except Exception as e:
        log.error("Reconnect failed: %s: %s", type(e).__name__, e)
    return False


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
# ck-mxlt / ck-hlpb: ARRIVE BOUND, AND SAY WHO YOU ARE
# ═══════════════════════════════════════════════════════════════════════════

#: The character this agent plays, plumbed in by the harness. ABSENT MEANS NO
#: CLAIM — never a default and never 0. An interactive or observing MCP session
#: sets nothing and behaves exactly as before; only an episode that declares a
#: character gets pre-bound. That gating is what keeps this out of arm E0 (which
#: runs NullAgent and no MCP server at all) and out of anyone's ad-hoc session.
AGENT_CHARACTER_ENV = "CK3_AGENT_CHARACTER_ID"


#: lane-multi: the most rulers one pre-bind list may name. The bridge's cap is
#: 10 claims across every session (MAX_CONTROLLED_WATCHLIST); a longer list
#: cannot all be bound, so it is refused whole rather than bound in part.
MAX_PREBIND_RULERS = 10

#: lane-prebind: how long the pre-bind waits for the game to become ACTIVE
#: before it is attempted anyway. `ck3agent run` can start while the save is
#: still loading (lane-multi run 1: `launch` returned rc 3, both ids were
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

    lane-multi: `CK3_AGENT_CHARACTER_ID` takes one id or a comma-separated list
    ("13902,12145"). Refuses rather than guesses: one malformed entry, or more
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

    WHY THE CLAIM IS HERE AND NOT IN THE PROMPT (ck-mxlt, e0's ruling
    2026-09-21). The registered prompt says "You ARE Harold Godwinson" — present
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

    ck-hlpb rides here: passing client_name/mode promotes the frame to v2. A
    BARE handshake omits protocol_version, the bridge assumes legacy v1, and the
    session is logged as client='unknown'. v1 fails PERMISSIVE — ck-zgh4
    measured a bare handshake returning session_mode "control" — so this is not
    what blocked the claim, but an observe-only request at v1 silently yields
    CONTROL, and a session that cannot say what it asked for cannot be audited.
    """
    # Local import to match this module's style for bridge-client symbols and
    # to keep import order independent of the MCP framework's.
    from bridge_client import (DEFAULT_CLIENT_NAME, MODE_CONTROL,
                               seat_declaration_kwargs)
    # ck-j6lk: DECLARE THE SEAT. This session is what an arm's agent acts
    # through, so it is a seat; the driver's steer-probe and speed-pin helpers
    # are control sessions that never claim and must not declare.
    # rc2 B7 (cli-4): a reconnect under release-on-exit presents the old token.
    resume = {"resume_token": resume_token} if resume_token else {}
    ack = await client.handshake(client_name=DEFAULT_CLIENT_NAME,
                                 mode=MODE_CONTROL,
                                 **seat_declaration_kwargs(), **resume)
    if not ack:
        return ack
    if resume_token:
        log.info("reconnect presented its resume token: resumed=%s resume_status=%s session=%s",
                 ack.get("resumed"), ack.get("resume_status"), ack.get("session_id"))
    ids = _agent_character_ids()
    if not ids:
        return ack                      # nothing declared: unchanged behaviour
    # lane-multi: READ THE PRE-BIND'S REPLY. The listener is what routes a reply
    # to its waiting call, and at connect it used to start only AFTER this
    # function returned: every pre-bind waited out its timeout and came back
    # `sent_no_confirmation`, which the `success`-defaults-true read below took
    # for a bind. Measured with the fake bridge: 5 s per declared ruler, and an
    # added ruler's `added: true` never seen. Start it now, after the handshake
    # has consumed its own ack.
    if getattr(state, "_client", None) is client and not getattr(state, "mock_mode", False):
        task = getattr(state, "_listener_task", None)
        if task is None or task.done():
            state._listener_task = asyncio.create_task(_bridge_listener(state))
    # lane-prebind: a reconnect's bind replaces any wait the old session left.
    await _cancel_prebind_wait(state)
    # lane-prebind: BIND WHEN THE GAME IS ACTIVE, NOT WHEN THE SERVER IS. The
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
    state.prebind = await _prebind_one(state, ids[0], add=False)
    if len(ids) > 1:
        # lane-multi: every further ruler is an ADDED claim (`add: true`), so the
        # first stays the primary. A bridge that predates `add` ignores the key
        # and SWITCHES away from the first; its reply then lacks `added`, which
        # _prebind_one records as a refusal rather than a bind.
        state.prebind["also"] = [await _prebind_one(state, x, add=True) for x in ids[1:]]
    state.prebind["wait"] = dict(wait)
    _record_prebind(state.prebind)


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
    # THE RESULT IS INSPECTED, NOT ASSUMED (ck-tygb is the same file's lesson:
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
        # lane-prebind: WHEN it landed. Every bridge frame stamps the live
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


#: ck-2r9k. Added to the ack's own `flush_eta_ms` before it is used as a
#: deadline. The ETA is the time until the flush is DUE; serving the call and
#: getting the frame back across the socket happens after that, so a deadline of
#: exactly the ETA expires just as the work begins. Deliberately generous and
#: deliberately NOT tuned: this number decides whether a completed call is
#: reported as unconfirmed, and the cost of it being too large is a slower
#: refusal on a genuinely dead bridge, while the cost of it being too small is a
#: silent false negative in the episode record.
ACK_ETA_MARGIN_S = 5.0

#: ck-9rto. The bridge's flush cadence, MIRRORED from
#: `bridge/include/flush_scheduler.h` (`period_ms = 10000`, a member
#: initialiser with no getenv) and CHECKED against it by
#: `test_mcp_confirmation.test_C5_the_flush_period_mirror_matches_the_header`.
#:
#: WHY A MIRROR AND NOT A READ. The pre-registration registers
#: `CK3_EPISODE_FLUSH_PERIOD_MS` at 10000 with the note "withdrawn until a
#: reader exists". Making this client that reader would un-withdraw a
#: registered variable, which is the pre-registration's call and not an
#: engineering one. So the number is mirrored and the mirror is tested — the
#: same shape as `registered_speed.py` and `bridge/include/registered_speed.h`,
#: and for the same reason: a mirror nobody checks is two homes wearing one name.
FLUSH_PERIOD_S = 10.0


class BridgeTimeout(TimeoutError):
    """A call that was not confirmed, CARRYING WHAT WAS OBSERVED ABOUT IT.

    ck-9rto. `_action_bridge` used to catch every TimeoutError and return one
    sentence — "did not confirm within 8.0s" — for three outcomes that have
    three different repairs. A reader of `actions.jsonl` could not tell a
    bridge that never answered from one that ACCEPTED the call, minted a
    ticket and was still working on it, and the d43 census spent a day on
    exactly that question with the evidence already thrown away at this line.

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

    THE DEFECT THIS CLOSES. In pipeline mode a TICK_WRITE is buffered and served
    on a flush. `flush_scheduler.h` holds `period_ms = 10000` as a member
    initialiser with no getenv, and the pre-registration registers 10,000 ms
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

    # ck-9rto: AND A WRITE CAN MISS THE FLUSH IT WAS QUOTED. The eta is the time
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
    # state the ck-zgh4 readiness probe was in when it declared a live world
    # dead. So ask early, once."
    #
    # A check is ANY-polarity and served INLINE, so asking costs a socket round
    # trip and not a flush. And the reply carries the ticket's OWN remaining
    # eta, so the poll interval corrects itself to the real period immediately:
    # two round trips in the worst case, not a poll storm. My first version
    # waited the full eta + margin before asking, which made a lost push cost
    # five seconds even when the ack had quoted 50 ms.
    push_wait = min(eta + _pcorr.PUSH_GRACE_S, _pcorr.FIRST_CHECK_S)
    try:
        return await asyncio.wait_for(asyncio.shield(future), timeout=push_wait)
    except asyncio.TimeoutError:
        pass

    # ── ck-2r9k: THE PUSH HAD ITS CHANCE. ASK. ──────────────────────────────
    #
    # Waiting alone is not a settlement protocol; it is a hope —
    # pipeline_correlation's words, written when the node's readiness probe read
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

    # ck-9rto: THE CHECKS ARE EXHAUSTED; THE BUDGET IS NOT. This tail used to be
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


#: ck-2r9k. How many times a still-running ticket is re-asked before giving up.
#: BOUNDED, and small: a check reply for a live ticket carries that ticket's own
#: remaining `flush_eta_ms`, so the interval corrects itself to the real flush
#: period after the first round trip. Two round trips is the expected worst
#: case; this is the cap that stops a poll storm if the bridge stops quoting.
CHECK_ATTEMPTS = 4
#: Per-check reply budget. A check is served INLINE, so it is a socket
#: round trip and not a flush wait; a second is generous.
CHECK_REPLY_S = 2.0

#: ck-9rto. The poll interval for a ticket the bridge says is LIVE while
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
        # during the LIVE wait and never through this path. The guard I had was
        # in the wrong place.
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
                # ck-9rto: LIVE AND QUOTING NOTHING IS OVERDUE, NOT IMMINENT.
                # A missing eta and an eta of 0 are the same statement here —
                # the bridge cannot say when this ticket's flush will come —
                # and the old code read both as `CHECK_POLL_S`, which spent the
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
    # `message` wrapper (L15 A2 deleted the unwrap that looked for one)
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
        # ck-9rto: SAY WHICH OF THE THREE THINGS HAPPENED, AND WHAT WAS SEEN.
        # This used to flatten every timeout into one sentence quoting
        # `timeout`, which was not even the budget actually waited — the ack
        # extension spends `timeout + extra`. Two facts were thrown away at
        # this line: whether the bridge ACCEPTED the call, and the ticket it
        # minted. Both are what a reader of a sealed bundle needs to tell "the
        # bridge never heard me" from "the bridge took my write and I stopped
        # listening", and the d43 census could not tell them apart.
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

@mcp.tool()
async def query(
    domain: Annotated[
        str,
        Field(
            description="Which game domain to query (single).",
            json_schema_extra={"enum": QUERY_DOMAINS},
        ),
    ] = "",
    domains: Annotated[
        list[str],
        Field(
            description="Batch query: up to 5 domains. Mutually exclusive with domain.",
            json_schema_extra={"items": {"type": "string", "enum": QUERY_DOMAINS}},
            max_length=QUERY_BATCH_MAX,
        ),
    ] = None,
    character_id: int = 0, target_id: int = 0,
    title_id: int = 0, trait_id: int = 0,
    include_vassals: bool = False,
    war_id: int = 0, culture_id: int = 0,
    dynasty_id: int = 0, filter: str = "", sort: str = "",
    sort_order: str = "", max_results: int = 0, court_of: int = 0,
    female: StrictBool | None = None,
    since_tick: int = 0, limit: int = 0,
    require_in_range: bool = True, verbose: bool = False,
    scope: str = "", hops: int = 0, max_nodes: int = 0,
    target_county: int = 0, target_county_key: str = "",
    counties: str = "", detail: str = "") -> dict[str, Any]:
    """Deep-dive query for a specific game domain. Supports batch queries via
    'domains' (max 5) with partial failure: each domain's reply sits under its own
    key, and a domain that failed is an errors[] row naming it (ok is then false).
    A read the bridge declined is refused with the gate its reason names.

    Args:
        domain: Which game domain to query (single).
        domains: Batch query: up to 5 domains (event_log and relative_power
            included). Pass domain or domains, not both: both is refused.
        character_id: Character context (0 = controlled).
        target_id: The other character for opinion, opinion_breakdown,
            diplomatic_range and relative_power.
        title_id: Title context for titles, de_jure, election, claimants,
            title_history, title_succession, neighbors, adjacency, development.
            neighbors: title_id is a COUNTY. With title_id=0 the character is used
            (character_id, 0 = the bound ruler): its capital county's neighbours, plus
            bordering_realms[] = every realm whose counties touch that character's realm
            (own + vassal counties), with a border_counties count each. Each neighbour row
            carries title_id, holder_id and realm_ruler_id (the holder's top liege), or
            realm_ruler_unresolved when that liege walk could not be made.
        trait_id: Trait ID for trait_compatibility.
        include_vassals: Include vassal claims in claims domain.
        war_id: Specific war for war_score domain (0 = all).
        culture_id: Culture ID for innovation/fascination/research (0 = character's).
        dynasty_id: Dynasty ID for dynasty domain (0 = character's).
        filter: For domain="characters": all|alive|dead|ruler|independent|ai_controlled.
        sort: For domain="characters": military|gold|realm_size|realm_counties|tier|age|
              diplomacy|martial|stewardship|intrigue|learning|prowess.
              Row fields: realm_size = titles the character holds DIRECTLY, all tiers,
              baronies included (same number as held_title_count); it drops when the AI
              grants titles to vassals and is not territory. realm_counties = counties
              held by the character or anyone in its vassal tree (for an independent ruler,
              the same as query(domain="realm").realm_total_counties): use it for realm
              size. is_ai = has an AI controller and is not the human player (the same
              definition as domain="character"; true for every ruler in an observer game).
        sort_order: For domain="characters": desc (default) | asc.
        court_of: For domain="characters": keep only characters whose immediate
            liege is this ruler (its unlanded courtiers and its landed direct
            vassals); 0 (default) = no court filter. The reply echoes court_of.
            "Who is at my court": court_of=<your ruler id>, filter="alive".
            Each row carries is_female and dynasty_name (when the character has one).
        female: For domain="characters": true keeps women, false keeps men; unset
            (default) keeps both. The reply echoes it.
        max_results: Row cap for domain="characters" (default 50; the bridge caps it
            at 500 lean, 5000 verbose) and domain="marriage_candidates" (default 30).
            marriage_candidates also takes `sort` (default military).
        since_tick: For the feed domains (war_declarations, scheme_inits,
            council_tasks, event_log): only records after this tick. Pass the
            current_tick of the previous reply.
        limit: For the feed domains: at most this many records (feeds default 64,
            event_log 100); for title_history, at most this many holders.
        require_in_range: For domain="marriage_candidates": true (default) keeps
            candidates in diplomatic range; false browses globally.
        scope: For domain="map_graph": realm (default) | target | explicit.
        hops: For domain="map_graph": breadth of the window in county hops (default 2).
        max_nodes: For domain="map_graph": cap on counties (default 60).
        target_county: For domain="map_graph", scope=target: the county title id.
        target_county_key: For domain="map_graph", scope=target: the county key
            (e.g. "c_york"), instead of target_county.
        counties: For domain="map_graph", scope=explicit: comma-separated county keys.
        detail: For domain="map_graph": "brief" drops distances and the live overlay.
        verbose: Opt-in to full detail for characters/wars/council/armies/realm/claims/
            conquest_targets (whose default lists 25 counties outside your realm,
            conquerable first, with max_results for more).
            Default False returns lean payloads (drops per-row skills, government, age,
            trait names, war participant id arrays, score_breakdown, and per-army
            secondary status fields) — all of which the agent's default formatters do not read. Set True
            only when you specifically need those extra fields.
    """
    state = _get_state()
    err = await _ensure_connected(state, "query")
    if err:
        return err
    gw = await _gateway_now()             # the envelope's game block: a fresh clock reading
    if domain and domains:
        return _blocked("query", state, "invalid_argument",
                        "pass domain=<one domain> or domains=[...], not both")

    params = {
        "character_id": character_id, "target_id": target_id,
        "title_id": title_id, "trait_id": trait_id,
        "include_vassals": include_vassals,
        "war_id": war_id, "culture_id": culture_id,
        "dynasty_id": dynasty_id,
        # ck-leg: character-browser params (domain="characters" → search_characters).
        "filter": filter, "sort": sort,
        "sort_order": sort_order, "max_results": max_results,
        "require_in_range": 1 if require_in_range else 0,
    }
    # L1b: only thread verbose through when explicitly requested, so the lean
    # bridge-side default applies otherwise. The param builders in tools.py emit
    # "verbose" only when present in p.
    if verbose:
        params["verbose"] = True
    # since_tick/limit thread through to the feed domains (war_declarations,
    # scheme_inits, council_tasks, event_log) only when explicitly set.
    if court_of:
        params["court_of"] = court_of
    # The caller's value, uncoerced (L14 m-10): StrictBool refuses a non-boolean
    # ("yes", 1) at the MCP boundary, and a value that reaches here another way
    # still meets the bridge's own female_must_be_boolean refusal.
    if female is not None:
        params["female"] = female
    if since_tick:
        params["since_tick"] = since_tick
    if limit:
        params["limit"] = limit
    # map_graph scoping params (domain="map_graph" → query_map_graph). Thread only
    # when explicitly set so the bridge-side defaults (scope=realm, hops=2, etc.) apply.
    if scope:
        params["scope"] = scope
    if hops:
        params["hops"] = hops
    if max_nodes:
        params["max_nodes"] = max_nodes
    if target_county:
        params["target_county"] = target_county
    if target_county_key:
        params["target_county_key"] = target_county_key
    if counties:
        params["counties"] = counties
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
        resolved = resolve_query(d, params)
        if resolved is None:
            return None, [], redesign.why_blocked(
                "unknown_domain", summary=f"unknown domain '{d}'; valid: {', '.join(QUERY_DOMAINS)}")
        action, bridge_params = resolved
        # L14 finding 6: a domain about the bound ruler, with none bound, answers
        # the refusal every actor-bearing tool answers.
        unbound = await redesign.unbound_refusal(gw, "query", action, bridge_params)
        if unbound is not None:
            return None, [], unbound["blocked"]
        try:
            reply = await _query_bridge(state, action, bridge_params)
        except Exception as e:                           # noqa: BLE001
            f = _read_failure(d, e)
            return None, [], redesign.why_blocked(f["code"], summary=f["detail"])
        declined = _declined(reply, action)
        if declined is not None:
            return None, [], declined
        if d == "conquest_targets" and not verbose:
            from tools import compact_conquest_targets
            reply = compact_conquest_targets(reply, max_results)
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
        return _ok("query", state, data, errors=errors)

    if not domain:
        return _blocked("query", state, "missing_required_argument",
                        "pass domain=<one domain> or domains=[up to "
                        f"{QUERY_BATCH_MAX}]; valid: {', '.join(QUERY_DOMAINS)}")
    got, errs, refused = await one(domain)
    if refused is not None:
        return _refused("query", state, refused)
    return _ok("query", state, got, errors=errs)


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
    largest = max([float(f.get("power_pct_of_liege") or 0) for f in (ft.get("factions") or [])], default=0.0)
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
            # lane-namekey: a row the bridge has not localized carries no name
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
            # read — report the gate as unmeasured, loudly. L16 R9: the bridge
            # answers a clamp or a skipped lookup as an error (title_count_clamped,
            # title_lookup_failed); L15 witness n6 stays as the second line: an
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
        # L15 A2: read by the keys emit_title_info writes (query_handlers.cpp:
        # `key` :2910, `tier` :2935, `de_jure_parent_key` :2964; the list is
        # `held_titles` :3046). The reader used to take "titles", "title_key" and
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


@mcp.tool()
async def discover(
    what: Annotated[
        str,
        Field(description="What to discover (see the list in the description).",
              json_schema_extra={"enum": DISCOVER_WHATS}),
    ] = "",
    interaction_type: str = "",
    target_id: int = 0, actor_id: int = 0,
    interaction: str = "", cb: str = "",
    title: str = "", action: str = "", title_id: int = 0,
    building: str = "", location_id: int = 0, slot: Optional[int] = None,
    max_results: int = 0,
    character_id: int = 0, decision_type: str = "",
    decisions: list = None, scheme: str = "",
    candidates: list = None) -> dict[str, Any]:
    """Pre-action validation: list interaction and decision types, check
    acceptance, validate casus belli, enumerate valid targets. Use it before do()
    or declare_war() to check that an action is possible.

    Args:
        what: interactions | decisions (light name list) | decisions_annotated /
              valid_decisions (pass decisions=[names]: each annotated with
              shown/valid/cost_ok/available, unmet-requirement keys, cost, and
              the character's current gold/piety/prestige; valid_decisions keeps
              only the available ones) | acceptance | valid_cbs | valid_cbs_strict |
              trigger_result | trigger_slots | war_validity | interaction_validity |
              construction_check | war_targets | imprisonment | targets |
              decision_why_blocked. Enumerations are do's: mercenaries,
              fabricate-claim targets and factions are do(action_class=
              'mercenaries' | 'fabricate_targets' | 'factions_to_join').
              construction_check (building, location_id = a barony title id
              of a holding; slot optional, read from the holding when omitted):
              the game's own can-build verdict for actor_id with its reasons
              (engine_reasons: the build button's tooltip lines, gold and
              innovations included), the price and the actor's gold.
              war_targets: every target+title+CB the actor can attack. CANDIDATES
              ONLY: `declarable` means no relation gate (kin, alliance, truce,
              imprisonment, already at war) blocks the row, not that the game
              accepts the war; cbs(target_id) or what=valid_cbs_strict is the
              engine's check and wins when the two disagree.
              imprisonment: the grounds the game gives actor_id against
              target_id (has_imprisonment_reason and its modifiers); not whether
              the game takes an imprisonment: that is targets with
              action=imprison.
              targets: the valid targets for action=war|imprison|execute|banish|
              revoke_title|claim (claim needs title_id). action=imprison is the
              game's own send check per vassal, the rows
              do(action_class='imprison_targets') gives: offered only when the
              game would take the send, each with the target's answer (yes /
              maybe / no) and score at check time; execute|banish list your
              vassals with the game's grounds against each.
              decision_why_blocked: per-clause pass/fail for
              decision_type=negotiate_the_danelaw|found_kingdom from structural reads.
              scheme_types: every scheme in the game's database with its target
              type (character|title), category (hostile|personal|political|
              contract), is_secret and max_breaches.
              scheme_targets: whom the actor can start `scheme` against, by the
              game's own check (the scheme's allow and valid triggers, an
              existing scheme, the cooldown). target_id checks one character;
              candidates=[ids] checks those; neither checks your court and
              vassals, your liege and his court, the holders of titles you claim and their courts. Stops at max_results (default
              20) valid. A refused row lists the game's reasons, one per failing
              check, each {check, summary}: the game's words; for a failing
              allow/valid trigger, that the bridge does not read the game's
              words for this check; for another check with none, that the
              game gives no detail.
        scheme: For what=scheme_targets: the scheme key (from scheme_types).
        candidates: For what=scheme_targets: character ids to check.
        max_results: For what=scheme_targets: stop at this many valid (default 20).
        action: For what=targets: the action to enumerate targets for.
        title_id: For what=targets+action=claim (and claimant lookups).
        interaction_type: For what=acceptance / trigger_result: the interaction
                          to check (full *_interaction key).
        target_id: Target character for acceptance/valid_cbs/interaction_validity.
        actor_id: Acting character (0 = controlled).
        interaction: For what=interaction_validity: short interaction name
                     (e.g. 'declare_war'); the wrapper appends '_interaction'
                     where the bridge expects it.
        cb: For what=interaction_validity + interaction='declare_war':
            CB name to scope the verdict to (e.g. 'claim_cb').
        title: For what=interaction_validity + interaction='declare_war':
               title that owns the claim (informational; e.g. 'd_sistan').
        decision_type: For what=decision_why_blocked.
        building / location_id / slot: For what=construction_check: the building
              key (e.g. 'castle_02'), the holding's barony or county title id,
              and the building slot. Answers {buildable, building_index}.
        character_id: For what=decisions / decisions_annotated / valid_decisions
              / scheme_targets: the character (0 = controlled; actor_id is
              read when character_id is 0).
        decisions: For what=decisions_annotated / valid_decisions: the decision
              keys or name-hashes (from what=decisions) to evaluate.
    A reply the bridge declined is refused with the gate its reason names.
    """
    state = _get_state()
    err = await _ensure_connected(state, "discover")
    if err:
        return err
    gw = await _gateway_now()             # the envelope's game block: a fresh clock reading
    if not what:
        return _blocked("discover", state, "missing_required_argument",
                        f"what is required; valid: {', '.join(DISCOVER_WHATS)}")
    if what in ("targets", "decision_why_blocked"):
        # L14 finding 6: about the bound ruler when no actor is named
        unbound = await redesign.unbound_refusal(
            gw, "discover", what, {"actor_id": actor_id or character_id})
        if unbound is not None:
            return unbound

    # Legibility capstone: what="targets" — "who are the valid targets for
    # action X right now". Some actions resolve in one call; others enumerate a
    # candidate pool then annotate each entry with per-target validity.
    if what == "targets":
        return await _discover_targets(state, action, actor_id, title_id)

    # L14 (finding 8): one door per read. An enumeration discover once had is
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

    if what == "scheme_types":
        return await redesign.scheme_types_impl(await _gateway_now())
    if what == "scheme_targets":
        return await redesign.scheme_targets_impl(
            await _gateway_now(), scheme, character_id=actor_id or character_id,
            target_id=target_id, candidates=candidates, max_results=max_results)

    from tools import resolve_discover, shape_discover_response
    resolver_params = {
        "what": what, "interaction_type": interaction_type,
        "target_id": target_id, "actor_id": actor_id,
        "interaction": interaction, "cb": cb, "title": title,
        "building": building, "location_id": location_id, "slot": slot,
        "max_results": max_results,
        "character_id": character_id,
        # #64: for what=decisions_annotated / valid_decisions — annotate ONLY these
        # decisions (key names and/or name-hashes from what="decisions").
        "decisions": decisions or [],
    }
    resolved = resolve_discover(resolver_params)
    if resolved is None:
        return _blocked("discover", state, "unknown_subject",
                        f"unknown discover target '{what}'; valid: {', '.join(DISCOVER_WHATS)}")

    action, bridge_params = resolved
    if what in ("decisions_annotated", "valid_decisions") and not decisions:
        # L18-B: without a list the bridge serves the light list, which is
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
    shaped = shape_discover_response(resolver_params, result)
    return _ok("discover", state, shaped)


# ---------------------------------------------------------------------------
# steer
# ---------------------------------------------------------------------------

@mcp.tool()
async def steer(character_id: int = 0, personality: dict = None) -> dict[str, Any]:
    """Read or adjust the AI's personality weights. No args = read current values.
    Partial updates supported: the axes you pass are written and locked, the
    others keep their values. The axes change what the game's AI chooses for this
    character; prefer set_phase, which writes a whole named posture.

    A write answers like every write: `verdict` applied (the axes read back as
    written, in `result.readback`), rejected (with `blocked`), or pending (with
    `pending`; read it back with steer()).

    Args:
        character_id: Character to steer (0 = controlled).
        personality: Personality axes to set. Keys: honor, greed, rationality, energy,
            boldness, zeal, vengefulness, compassion, sociability. Values: integers
            -100 to +100. Any other key or value is refused and nothing is written.
    """
    err = await _ensure_connected(_get_state(), "steer")
    if err:
        return err
    return await redesign.steer_impl(await _gateway_now(), character_id=character_id,
                                     personality=personality)


# ---------------------------------------------------------------------------
# pass_turn
# ---------------------------------------------------------------------------

@mcp.tool()
async def pass_turn(reason: str = "", character_id: int = 0) -> dict[str, Any]:
    """Explicitly choose not to act this cycle. Provide a reason for the decision log.

    Args:
        reason: Brief explanation for inaction.
        character_id: the ruler who passes (0 = the primary). With several rulers,
            one pass per ruler that does not act; the log and the reply name him.
    """
    state = _get_state()
    await _gateway_now()                  # the envelope's game block: a fresh clock reading
    # L18-B: pass_turn was the one tool without character_id while the protocol
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
# The goal-shaped tools (SPEC docs/notes/mcp-redesign/SPEC.md). Each is a thin
# wrapper over redesign.py composition + the BridgeGateway (dispatch and
# single-flight §4, actor explicitness §5). The bridge actions each one composes are the ones its
# redesign.py implementation sends; there is no separate hand-kept list of them.
# ===========================================================================

@mcp.tool()
async def observe(
    character_id: int = 0,
    depth: Annotated[
        str,
        Field(description="brief: de_jure, standing, wars, money, succession, posture, since. "
                          "full: also self vitals, war and council briefs and the AI's "
                          "evaluations.",
              json_schema_extra={"enum": list(redesign.OBSERVE_DEPTHS)}),
    ] = "brief") -> dict[str, Any]:
    """Your situation report: call it first every cycle. Returns de-jure county counts
    for the titles nearest and widest around your holdings (`de_jure`), your current
    `standing` (gold, debt, stress, strongest faction percentage, succession ETA,
    whether you are at war), the personality genome (`posture`), and what changed
    since the last call (`since`; `delta` is your own share of it). `de_jure` counts
    counties and `self.counties_held` counts yours.

    Also folds in a `succession` block with succession_plan's verdict, computed the
    same way: `outcome` (intact, splits, or undetermined when the reads name no heir
    who takes a title), `passes_intact` (true, false, null when undetermined), the
    `breaker` (the first other heir who takes titles, and which), `eligible_heir_count`. The
    per-title map and the levers are succession_plan's.

    Args:
        character_id: ruler to reason about (0 = controlled primary; REQUIRED in multi-actor).
        depth: 'brief' or 'full' (adds self vitals, war/council brief and the AI's
            evaluations). Any other value is refused.
    """
    err = await _ensure_connected(_get_state(), "observe")
    if err:
        return err
    gw = await _gateway_now()
    # v3 §7.5: the seat is a LINEAGE. At the seat's cycle entry, witness a
    # handoff the bridge made, make the rebind explicitly when it could not,
    # or end the seat `lost`; rows go to CK3_SEAT_LINEAGE_OUT. INERT unless
    # that env is set (the public product sets nothing).
    _sink = seat_lineage.armed()
    if _sink:
        _st = _get_state()
        _st.seat_lineage_prior = await seat_lineage.step(
            gw, _st.seat_lineage_prior, sink=_sink,
            seat_session_id=os.environ.get("CK3_SESSION_ID"),
            registered_title=seat_lineage.registered_title_from_env(),
            own_session_id=redesign.own_session_id, own_session_row=redesign.own_session_row)
    env = await redesign.observe_impl(gw, character_id=character_id, depth=depth)
    if _CHANNEL is not None and _CHANNEL.enabled:
        env = await _deliver_inbox(gw, env)
    return env


async def _deliver_inbox(gw, env: dict[str, Any]) -> dict[str, Any]:
    """Section 5.2 delivery: the inbox rides on the recipient's next observe as
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
        env.setdefault("result", {})["messages"] = _channel.deliver(
            _CHANNEL, ledger, game_date=game.get("date"), tick=game.get("tick"),
            cycle=_SEAT.cycles.cycle)
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


@mcp.tool()
async def plan_war(character_id: int = 0, goal_scoped: bool = True,
                   ally_free_only: bool = True, exclude_straddling: bool = True,
                   max_results: int = 8, toward: str = "") -> dict[str, Any]:
    """"What is my single best next war?" Returns a RANKED list of targets that are
    ALREADY declarable, ally-free, and goal-advancing — each row carries a `declare_args`
    object you pass to declare_war VERBATIM. You never name a casus_belli string or hunt
    a claimant. Each row passed the game's own declare-war check for that target. A claim
    CB is offered only on a pressed claim (`claim_pressed: true`, with the claimed title
    and the claimant); an unpressed claim is listed in `excluded` as claim_not_pressed.
    Empty list ⇒ `empty_reason` names the commonest
    `excluded` gate with its count (all gates in `measured.by_gate`), and in `fix` the
    parameter that would admit those rows (exclude_straddling=False with
    confirm_straddle on the declare; ally_free_only=False) when one would.
    `empty_reason.times` counts the consecutive calls that gave this ruler the same
    reason for the same arguments, and `unchanged_since` is the game date of the first.

    Each row carries `strength` (your and the target's military strength, with allies,
    and their ratio, read from the game), `score` (de-jure counties gained x that ratio,
    capped at 3), `truce`, and `affordability` (the CB's base cost; the game scales the
    real price by target, so it is an estimate).

    Args:
        character_id: aggressor (0 = controlled primary; REQUIRED in multi-actor).
        goal_scoped: True = claims, duchies you partly hold and the `toward` title.
            False = also every duchy bordering your realm.
        ally_free_only: exclude targets that pull allies you'd fight.
        exclude_straddling: exclude every row while the ruler is old enough that a war
            would run across his succession (the age declare_war's guard uses).
        max_results: top-N by score.
        toward: a de-jure title key (county to empire, e.g. "k_france") to conquer
            toward, anywhere on the map; its duchies are listed first.
    """
    err = await _ensure_connected(_get_state(), "plan_war")
    if err:
        return err
    return await redesign.plan_war_impl(await _gateway_now(), character_id=character_id,
                                         goal_scoped=goal_scoped, ally_free_only=ally_free_only,
                                         exclude_straddling=exclude_straddling, max_results=max_results,
                                         toward=toward)


@mcp.tool()
async def declare_war(character_id: int = 0, declare_args: dict = None,
                      target_id: int = 0, cb: str = "", cb_title_id: int = 0,
                      claimant_id: Optional[int] = 0, confirm_straddle: bool = False) -> dict[str, Any]:
    """Declare ONE war. Pass a plan_war row's `declare_args` verbatim (or the fields
    explicitly). The only irreversible high-stakes act with its own verb so it cannot
    fire by accident. Reads the war list back after dispatch: the bridge runs the
    declaration on the game's next pass, paused or running, and the war shows within
    seconds. Not read in that time: verdict:pending, drains_when "next_pass" (read it
    by pending.intent_id). Ran but not listed: drains_when "clock_advances" (the game
    settles it on a later day). It never pauses or unpauses. A `valid` CB does NOT guarantee a landed war; this re-checks
    pressed-claim + target-state up front. Evaporated war ⇒ verdict:rejected with the
    exact gate (e.g. target_truced); invisible so far ⇒ verdict:pending, verify with
    inspect(subject='wars') before declaring again. A straddles_succession war
    REQUIRES confirm_straddle=true.

    Args:
        character_id: aggressor (0 = controlled primary).
        declare_args: a plan_war row's declare_args dict (preferred path).
        target_id / cb / cb_title_id / claimant_id: escape-hatch fields if no declare_args.
            claimant_id null (as a plan_war row writes it for a CB without a claimant)
            is the same as 0: no claimant.
        confirm_straddle: HARD GUARD — refuses a near-death-straddling war unless true.
    """
    err = await _ensure_connected(_get_state(), "declare_war")
    if err:
        return err
    return await redesign.declare_war_impl(await _gateway_now(), character_id=character_id,
                                            declare_args=declare_args, target_id=target_id,
                                            cb=cb, cb_title_id=cb_title_id, claimant_id=claimant_id or 0,
                                            confirm_straddle=confirm_straddle)


@mcp.tool()
async def succession_plan(character_id: int = 0) -> dict[str, Any]:
    """"Will my realm pass through my heir's succession INTACT?" Returns one verdict:
    `outcome` intact (passes_intact true), splits (false, with the `breaker`: the heir
    who takes titles away) or undetermined (null, with `undetermined_reason` and the
    `succession_undetermined` warning: no heir was read, or the reads say it does not
    pass intact and name no breaker), the eligible heir count, and the per-title map (which heir takes which
    title, under which law). observe's succession block and the live-state hook
    state the same verdict from the same function. `levers` lists what this ruler can pull, each with its
    do() call and its read-back: designate_heir (available, the children it can
    name, the prestige cost), disinherit (only the dynasty head; sent only at your
    own descendants), grant_title and revoke (revoke_allowed_by: the realm law that
    allows revoking), title_law and marriage. Fewer eligible heirs means fewer
    titles split.

    Args:
        character_id: the ruler whose death you plan around (0 = controlled primary).
    """
    err = await _ensure_connected(_get_state(), "succession_plan")
    if err:
        return err
    return await redesign.succession_plan_impl(await _gateway_now(), character_id=character_id)


@mcp.tool()
async def set_phase(
    phase: Annotated[
        Optional[str],
        Field(description="The posture to assert, or null to read the current genome and the phase it matches.",
              json_schema_extra={"enum": list(redesign.PHASES) + [None]}),
    ] = None,
    character_id: int = 0,
    overrides: dict = None) -> dict[str, Any]:
    """Assert your strategic posture by NAME instead of hand-tuning nine integers.
    phase=None reads the current genome and names the phase whose genome the engine
    holds (result.phase; null when none matches). Nothing re-applies it after a succession:
    call it again for the heir. `result.lock_state` is the bridge's word on the write
    (live: held against the AI's rewrites; quarantined or none: not held, and a
    warning says so); observe carries the same field under posture.genome.

    Each call writes, for the ruler: the personality axes, and set_character_ai,
    re-enabling the native AI tick. No phase arms an experimental flag.

    Args:
        phase: the posture name, or None to read the current genome.
        character_id: who to pin (0 = controlled primary; REQUIRED in multi-actor to assert).
        overrides: escape hatch — per-axis int overrides, -100 to 100 (discouraged). A key
            that is not an axis or a value out of range is refused and nothing is written.
    """
    err = await _ensure_connected(_get_state(), "set_phase")
    if err:
        return err
    return await redesign.set_phase_impl(await _gateway_now(), phase=phase,
                                          character_id=character_id, overrides=overrides)


@mcp.tool()
async def do(
    intent: Annotated[
        Optional[str],
        Field(description="The one action to execute; null with action_class enumerates.",
              json_schema_extra={"enum": list(redesign.INTENTS) + [None]}),
    ] = None,
    action_class: Annotated[
        Optional[str],
        Field(description="What to enumerate (enumerate mode only).",
              json_schema_extra={"enum": list(redesign.ACTION_CLASSES) + [None]}),
    ] = None,
    candidates: list = None,
    character_id: int = 0,
    args: Annotated[
        Optional[dict],
        Field(description="Intent-specific keys. " + do_intent_args.table_text()),
    ] = None) -> dict[str, Any]:
    """Perform one concrete game action that is NOT a war (wars use declare_war), OR
    enumerate what you can do. Two modes:
     • Enumerate — do(action_class="council_tasks") returns available actions, each
       with the EXACT do_call to run it (copy the row; never guess a type string or
       English key) plus a blocked-why for each unavailable one. For decisions you MUST
       pass a bounded `candidates` list (an unbounded engine eval stalls the game's thread).
     • Execute — do(intent="council_task", args={...}) runs ONE action. Every write
       returns verdict:applied (re-read confirms), verdict:rejected with a named gate,
       or verdict:pending. The bridge runs a queued write on the game's next pass,
       paused or running, and its verdict is read back in this call; pending names
       the wait: pending.drains_when "next_pass" (the verdict was not read in time)
       or "clock_advances" (it ran; the outcome needs game days, which pass only
       while the game runs). A pending reply carries pending.intent_id; inspect(subject='pending')
       lists what is still queued and the outcomes of what drained, and
       do(intent='cancel_pending', args={'request_id': <intent_id>}) withdraws one the
       bridge has not yet taken to run (a "clock_advances" pending already ran:
       result.sent is true, and it cannot be withdrawn).
       council_task / fabricate warn if the AI will REVERT them. appoint is an intent
       here, not a discoverable interaction.

    Args:
        intent: one of the enum. The war intents, for a war's leader or an ally:
                make_peace (args={"war_id", "terms": white_peace | surrender |
                enforce_demands}) sends the peace from your side; the enemy's own
                acceptance decides white peace and enforced demands. A surrender is
                always taken and is your DEFEAT: the game charges the casus belli's
                penalty (prestige, often gold; 2026.11 pilots: 15-350 prestige, up to
                590 gold), and the reply warns surrender_is_your_defeat. A stalled war
                that costs nothing is cheaper left open. applied means the war ended,
                with its outcome and your gold/prestige before and after. call_ally (args={"war_id", "ally_id"})
                calls an ally into a war you lead; join_war (args={"war_id",
                "leader_id"}) joins an ally's war on the side he leads (leader_id
                defaults to the ally who leads it); applied means the joiner is in the
                war's participant list. A refusal names the gate: recipient_refuses
                (they would decline), interaction_invalid (the game's own check),
                not_war_leader, war_not_found.
        action_class: formable_titles lists the titles you could create now, each with
                      its de-jure county count and a ready mint_title do_call (pass
                      `candidates=[...]` to test a custom key set). mercenaries: free
                      companies, nearest first, each labelled with home county and men,
                      plus regiments; cost is null (the game prices a hire per hirer; the
                      hire refuses what you cannot afford). men_at_arms: MaA types with
                      recruit cost and fielded_regiments; 30 rows, fielded first then
                      cheapest, or pass candidates=[types]. laws: the other laws in each
                      law group you have; whether a change is allowed now is decided when
                      change_law runs. fabricate_targets: the titles the court
                      chaplain's task_fabricate_claim can take (the door for a claim on a
                      title; the generate_claim scheme is refused); an empty list warns
                      fabricate_targets_empty, naming the seat it used and the bridge's
                      counts. council_task takes args.seat optionally; a task
                      belongs to ONE seat (task_fabricate_claim is the court chaplain's), a
                      seat that cannot take it is refused, and every reply names the seat
                      and councillor used. buildings: the game's building types (40
                      rows, or pass candidates=[keys]); pass args={"location_id": <barony
                      title id of a holding you hold>} and each row is the game's verdict
                      for you there (available, or blocked with the game's reasons or the
                      price against your gold) with a complete do_call. A build costs its
                      price at once and takes its construction_days;
                      inspect(subject='county') holdings[].construction shows it.
        candidates: BOUNDED list for action_class="decisions"; for "men_at_arms", the
                    MaA type keys to look up; for "imprison_targets", the character
                    ids to check (default: your first 40 vassals). An imprison row's
                    answer is the target's at check time: check right before sending.
        character_id: actor (0 = controlled primary; REQUIRED in multi-actor).
        args: intent-specific keys, listed per intent under ARGUMENTS BY INTENT in
              this parameter's description (read from the code). An enumerate row's
              do_call carries them ready to send; a call missing one is refused
              with missing_required_argument and fix.rejected_fields names it.
              cancel_pending takes args.request_id (a pending.intent_id).
    """
    err = await _ensure_connected(_get_state(), "do")
    if err:
        return err
    return await redesign.do_impl(await _gateway_now(), intent=intent, action_class=action_class,
                                   candidates=candidates, character_id=character_id, args=args)


@mcp.tool()
async def inspect(
    subject: Annotated[
        str,
        Field(description="The subject to read.",
              json_schema_extra={"enum": list(redesign.INSPECT_SUBJECTS)}),
    ] = "",
    character_id: int = 0, target_id: int = 0,
    title_id: int = 0, war_id: int = 0, title_key: str = "",
    dynasty_id: int = 0,
    max_results: int = 20) -> dict[str, Any]:
    """Deep, typed read of ONE subject. Whether the next succession passes intact is
    succession_plan's. `heir_plan` lists each child and eligible heir with martial, traits, education
    focus, and the levers that exist for them (education focus / guardian / marriage /
    designate heir); guardian and projected_adult_martial read NULL because no bridge
    read exposes them. `guardian_candidates` lists in-court guardian prospects for a
    child with their martial, and the global high-martial pool to invite. `war_stance` is each war's side, stance and occupation
    with a stall verdict; an attacker inferred not sieging carries the limitation
    war_stalled_no_lever (the AI sieges only above 1.33x the enemy's strength, which no
    axis or phase moves; calling allies is the lever), and `exit_lever.surrender_cost`
    says a surrender is your defeat. `war_score` without war_id is your own wars
    (get_active_wars), with war_id that one war. `pending` lists the writes still queued in the bridge and the
    outcomes of the ones that drained (match a do reply's pending.intent_id): the
    bridge keeps the verdicts of the last 128 drained writes (reads are never kept), a
    129th drops the oldest (that token is then found only by a read-back), and the
    newest max_results of them are listed (default 20, at most 64).
    dynasty_id is null for landed rulers by design. A title's de-jure position is
    inspect(subject='title') or query(domain='de_jure'); a realm's borders are
    query(domain='neighbors').

    Args:
        subject: the subject to read (the enum).
        character_id / target_id / title_id / war_id / dynasty_id: per-subject ids.
        title_key: for the 'title' (de-jure) subject: a title key such as "k_england".
        max_results: cap.
    """
    err = await _ensure_connected(_get_state(), "inspect")
    if err:
        return err
    return await redesign.inspect_impl(await _gateway_now(), subject=subject, character_id=character_id,
                                        target_id=target_id, title_id=title_id, war_id=war_id,
                                        title_key=title_key, dynasty_id=dynasty_id,
                                        max_results=max_results)


@mcp.tool()
async def cbs(target_id: int = 0, character_id: int = 0) -> dict[str, Any]:
    """"Show me EVERY casus belli I can use against THIS target, with tradeoffs." Returns,
    for the (actor, target) pair, every CB the game accepts now plus every CB of the war
    families (invasion — the Varangian/Viking adventures included —, conquest, holy war,
    claim, de jure, subjugation, vassalization) with the game's verdict. Each option: its
    `family`, `valid` (the game's own CB check), `scope`, what it `grants` on victory where
    known, why it is unmet (`requirements_unmet`), `tradeoffs`, its base cost, and the
    dispatch hint (`needs_cb_title_id` / `needs_claimant` / `dispatch_supported`).
    `valid_options` is the declarable subset, `by_family` groups it, `best` is the first
    declarable one. Re-point a plan_war row's declare_args.cb at any option here.
    An engine read on the game thread; works paused or running.

    Args:
        target_id: the defender character id (REQUIRED).
        character_id: aggressor (0 = controlled primary; REQUIRED in multi-actor).
    """
    err = await _ensure_connected(_get_state(), "cbs")
    if err:
        return err
    return await redesign.cbs_impl(await _gateway_now(), target_id=target_id, character_id=character_id)


@mcp.tool()
async def session(
    action: Annotated[
        str,
        Field(description="The session operation.",
              json_schema_extra={"enum": list(redesign.SESSION_ACTIONS)}),
    ] = "",
    character_id: int = 0, speed: int = redesign.SPEED_MAX,
    enabled: Optional[bool] = None, disable_native_ai: bool = False,
    flag: str = "", seconds: int = 60, game_days: int = 0) -> dict[str, Any]:
    """Game-flow, identity, and liveness control — and the ONLY tool that changes the
    clock: no other tool pauses, unpauses or sets speed, and every other tool works with
    the game paused or running. Use it to: switch which ruler you control after a
    succession (control); confirm the controlled ruler is alive+landed (whoami — control
    can silently drift); check bridge liveness if calls time out (health — 'hung' = STOP
    looping); set speed/pause; or let time pass (wait). Speed runs 1 to 3: above 3 is
    REFUSED while at war (speed_refused_at_war) and otherwise set to 3, below 1 is
    refused. The human player can be a target but never an actor.

    wait: the way to let time pass between cycles. Unpauses if paused, blocks for
    `seconds` (1-110; longer waits are several calls), pauses again, and returns
    start_date/end_date. Do not use Bash `sleep` for this: Claude Code refuses sleeps
    of 30 s or more. With `game_days` > 0 the wait is measured in GAME-DAYS instead:
    the server repeats bounded waits (each at most `seconds`, planned from the clock
    rate it measures) until the game date has advanced that many days since the call
    began, and reports game_days_advanced, calls and why it stopped; seconds are not
    the unit, because the clock's rate depends on the host and the seat count. Under
    a registered cadence (CK3_CYCLE_GAME_DAYS) an ask above it is refused
    (cycle_game_days_over_cadence), never clamped. Under a registered horizon
    (CK3_EPISODE_HORIZON_END) a wait made at or past it is refused (horizon_reached):
    the episode is over; pause and report.

    With no ruler bound, whoami returns ok=false, blocked.gate='no_ruler_bound',
    controlled=null, and blocked.measured.unbound_reason says why ('heir_is_player':
    the human inherited; '': no reason recorded, as after a reconnect). So do
    observe, plan_war, declare_war, succession_plan, set_phase, do, inspect, cbs,
    steer, and query and discover for anything about the ruler, when character_id
    is omitted; a read that names no character (the character browser) answers.
    'actor_unresolved' means the read failed, not that nothing is bound. A seat whose
    ruler became the human's character gets 'cannot_act_as_player'. control needs
    character_id: 0 or omitted is refused (missing_character_id), never an unbind.

    Several rulers in one session: add claims one more ruler and keeps the ones held
    (the first stays the primary, the one a call without character_id acts as);
    control SWITCHES the primary to another ruler. whoami's my_claims lists every
    ruler held. With several, pass character_id on EVERY call. release frees one;
    the last one is freed when the session ends. CK3_AGENT_CHARACTER_ID pre-binds
    one id or a comma-separated list ("13902,12145"); the first is the primary.

    experimental_flags lists the experimental engine flags, all OFF at boot;
    session(action="set_experimental", flag=<name>, enabled=<bool>) arms or disarms ONE
    (enabled is required: a call without it is refused). Nothing else arms them;
    off_goal_veto is process-wide (every controlled character of every session) until
    disarmed or the game exits. The bridge has no readback: each flag's `live` is null
    and `last_written_here` is this server's last confirmed write (another seat's
    write is not seen; null does not mean off).

    Args:
        action: the operation (the enum).
        character_id: for control and add (required, > 0) and release.
        speed: for action=speed, 1-3.
        enabled: REQUIRED for set_experimental (arm/disarm the flag; no default:
            a call without it is refused, invalid_argument).
        disable_native_ai: for control and add.
        flag: for set_experimental — one of session(action='experimental_flags').
        seconds: for wait — how long to let the clock run, 1-110 (default 60); with
            game_days, the longest single wait of the loop.
        game_days: for wait — let the clock run until the date has advanced this many
            game-days (0 = wait `seconds` once). Capped by the registered cycle
            cadence when one is set.
    """
    err = await _ensure_connected(_get_state(), "session")
    if err:
        return err
    env = await redesign.session_impl(await _gateway_now(), action=action, character_id=character_id,
                                      speed=speed, enabled=enabled, disable_native_ai=disable_native_ai,
                                      flag=flag, seconds=seconds, game_days=game_days)
    # Study 0 (channel): the seat's cycle, the unit the send quota is keyed on
    # (cycle_cadence.CycleCounter: the registered cadence in game-days across
    # the waits that waited, or every such wait in free play). Counted whether
    # or not a channel is registered, so the count is the same fact in every arm.
    _SEAT.cycles.note(action, env)
    return env


# ---------------------------------------------------------------------------
# msg_send — pre-registration v3 section 5.2 (Study 0, channel lane)
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
                        "this seat's arm declares no channel")
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
    row = _channel.attempt(_CHANNEL, ledger, to=str(to or ""), text=str(text or ""),
                           game_date=game.get("date"), tick=game.get("tick"),
                           cycle=_SEAT.cycles.cycle)
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


# REGISTERED ONLY UNDER A DECLARED TOPOLOGY OTHER THAN `none` (section 5.1): in
# E0, K1 and free play the name is absent from list_tools, not present-and-
# refused (the ruling the deleted arm.py recorded, G5 option A2, carried).
if _CHANNEL is not None and _CHANNEL.enabled:
    mcp.add_tool(msg_send)


def registered_tool_definitions() -> list[dict]:
    """What this server registers, read from FastMCP's tools/list handler.

    One reader of the surface for everyone who needs it as data: the autonomous
    loop (agent.py) hands these to the Anthropic API, scripts/gen_tool_reference.py
    renders the reference from them, and scripts/tool_surface.py's probe takes the
    names. Each row is {name, description, input_schema}. Run in a thread so a
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
    """ck-jpej. THE SERVER IS THE ONLY AUTHORITY ON WHAT IT REGISTERED.

    ck-3ron graded the arm's tool surface from the CLI's `init` event, and in
    registered-d45 slot 2 that event listed 15 tools where the seat had 17 —
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
    # rc2 B7: what the MODEL is served, not just the names: FastMCP.list_tools()
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


# rc2 B7: RELEASE THIS SESSION'S OWN CLAIMS WHEN THE SERVER STOPS. The bridge
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
    """lane-multi: add the pre-bind outcome to the session record (CK3_SESSION_OUT),
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
    # lane-prebind: how long the game was waited for before the bind, and
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
    parser.add_argument("--mock", action="store_true",
                        help="Run with mock game state (no bridge needed)")
    args, _ = parser.parse_known_args()

    if args.transport == "streamable-http":
        mcp.run(transport="streamable-http", port=args.port)
    else:
        mcp.run(transport="stdio")
