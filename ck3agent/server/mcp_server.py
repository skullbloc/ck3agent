#!/usr/bin/env python3
"""
CK3 Bridge MCP Server — 11 consolidated tools.

The LLM steers one AI character by reading game state and manipulating
AI personality weights and subsystem toggles. The native AI handles ALL
execution (wars, marriages, schemes, etc.). No action injection.

Usage:
    # Run as stdio MCP server (for Claude Code / Claude Desktop):
    python3 mcp_server.py

    # Run as HTTP MCP server (for remote access):
    python3 mcp_server.py --transport streamable-http --port 8000

    # Test without bridge (mock mode):
    python3 mcp_server.py --mock
"""

import asyncio
import enum
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
from pydantic import Field

from tools import QUERY_DOMAINS

# ck-6kvq.12 (R4 / G5): the arm gate for the shared board. One authority for
# "is there a board", read by the tool registration below and by
# tools.get_tool_definitions().
from arm import board_enabled as _board_enabled

# ck-2r9k. §8.2 — what a pipeline-mode reply actually looks like — lives in ONE
# module, and this file had zero occurrences of it. The three clients that got
# this wrong got it wrong three different ways; a fourth reading of
# `for_request_id` here would be a fourth way.
import pipeline_correlation as _pcorr

log = logging.getLogger("ck3-mcp")


# ---------------------------------------------------------------------------
# Redesign (SPEC docs/notes/mcp-redesign/SPEC.md): the gateway that owns the
# pause-flip (§4), single-flight serialization of deferred queries (§4), and
# since-tick delta tracking — so the 8 redesign tools stay pure composition.
# ---------------------------------------------------------------------------

class BridgeGateway:
    """Owns pause-state polarity, single-flight serialization, delta carry, and
    actor-mode detection for the 8 redesign tools (SPEC §4/§5).

    The redesign tool impls (redesign.py) call read/write/read_unpaused and never
    touch the socket or the pause state directly — this is the layer that satisfies
    the per-call pause matrix automatically (kills E1/E2/E3 from the agent's view).
    """

    def __init__(self, state: "BridgeState"):
        self.state = state
        self._strict_lock = asyncio.Lock()  # §4 single-flight for deferred/strict-CB
        self._last_delta_tick = 0

    # --- clock / health ----------------------------------------------------
    def game(self) -> dict:
        ts = self.state.latest_tick_start or {}
        return {"tick": self.state.tick_id, "date": self.state.game_date,
                "paused": bool(ts.get("paused", True)),
                # ck-k57a: null means the bridge read no speed. `game_speed_legacy`
                # is the documented compat integer (0 == no reading).
                "speed": (ts.get("game_speed")
                          if ts.get("game_speed") is not None
                          else ts.get("game_speed_legacy", 3))}

    @property
    def health(self) -> str:
        if not self.state.connected and not self.state.mock_mode:
            return "hung"
        return "ok"

    async def multi_actor(self) -> bool:
        """Multi-actor mode = >1 controlled char or a campaign overlay (SPEC §5).
        Best-effort: reads the controlled roster; defaults to single-controlled."""
        try:
            roster = await _query_bridge(self.state, "get_controlled_roster", {})
            if isinstance(roster, dict):
                chars = roster.get("characters") or roster.get("controlled") or []
                return isinstance(chars, list) and len(chars) > 1
        except Exception:
            pass
        return False

    # ──────────────────────────────────────────────────────────────────────
    # ★ UNIFIED SCOPED PAUSE-STATE (_HARDENING_PLAN.md FIX 3, supersedes #95)
    #
    # One mechanism owns pause polarity for EVERY bridge call the 8 tools compose:
    # RECORD the live {paused, speed}, FLIP to the polarity the action needs,
    # EXECUTE, then in a `finally` RESTORE the recorded {paused, speed}. Callers
    # (redesign.py) never specify pause-state and the game tempo is never left
    # changed (the lingering-unpause / speed-drift bug). The polarity table below
    # is the SINGLE place pause logic is encoded; drive it off the per-action
    # classification + the bridge `needs_unpaused` typed-reason signal.
    # ──────────────────────────────────────────────────────────────────────

    class Polarity(enum.Enum):
        PAUSED = "paused"            # TIER-2 object-graph read; rejected fast when running
        UNPAUSED_TICK = "unpaused"   # deferred game-thread query / write; needs a live tick
        ANY = "any"                  # unpaused-safe; served inline regardless of pause

    # Deferred game-thread reads (strict-CB family + cb-capability + goal-ladder).
    # These are serviced on the game thread (g_cb_query slot / drain_and_execute_intents)
    # and BLOCK while paused on a bridge that doesn't self-reject — so the gateway must
    # FLIP them UNPAUSED. The bridge's `{status:"needs_unpaused"}` self-classification
    # (emit_needs_unpaused) is the authoritative override, handled in _call() below; this
    # static set is the fallback classification for OLD bridges that don't emit it.
    #
    # query_conquest_targets runs cb_query_strict_full_sync per holder (the same deferred
    # game-thread slot the strict-CB family uses) — the _HEALTH_MATRIX P0 HANG when paused
    # was this query routed as a plain PAUSED read. Classify it UNPAUSED so it drains on a
    # tick instead of blocking the full client timeout.
    # query_dejure_progress is the goal metric. It is a PURE safe_read meter (no engine
    # calls, never hangs, PAUSED-safe — it does NOT emit needs_unpaused). Forcing it
    # UNPAUSED made the de-jure children vector be read MID-TICK while the engine
    # clears-then-repopulates it (de-jure drift / title create/destroy) → SUB_TITLE_COUNT
    # (0x264) transiently 0 → total=0 → the agent-blocking "0/0 sub-title unresolved" /
    # false mint reject (re_dejure_meter_and_mint.md bug #1). Route it PAUSED (the
    # query_/get_/list_ default below) so the tree is read on a STOPPED clock → stable
    # held/total. (On any old bridge that would hang it paused, the bridge's authoritative
    # needs_unpaused self-classification still flips it — see _call().)
    _UNPAUSED_READS = frozenset({
        "query_conquest_targets",
        "query_cbs_for_target", "query_cb_validity_strict",
        "list_valid_decisions", "list_decisions_annotated",
    })

    # Writes (tier-3 / B*) — need UNPAUSED + a live tick before dispatch (the E3 fix).
    _WRITE_ACTIONS = frozenset({
        "create_title", "send_decision", "send_interaction", "set_character_faith",
        "set_character_culture", "set_doctrine", "change_realm_law", "set_title_law",
        "set_council_task", "appoint_councillor", "set_lifestyle_focus",
        "construct_building", "raise_armies", "raise_raiders", "maa_recruit",
        "maa_station", "maa_unstation", "hire_mercenary", "move_capital",
        "set_obligation_level", "respond_event", "scheme_action", "faction_action",
        "declare_war", "call_war_create_core", "set_cb_selector_override",
        "set_ai_personality", "set_character_subsystems",
        # REMOVED: move_army deleted per design (agent does not drive army micro).
    })

    # Tempo / liveness / identity controls — served inline, NEVER scoped (they ARE
    # the tempo, or are unpaused-safe readbacks used mid-poll without disturbing a flip).
    # The COMMAND subset dispatches via _action_bridge (full response); the readback
    # subset (get_active_wars / get_bridge_state / rosters) via _query_bridge (.data).
    _ANY_COMMANDS = frozenset({
        "pause_game", "unpause_game", "set_speed", "set_auto_respond_events",
        "set_controlled_character", "release_character",
        # ck-6kvq.13 (R6): the decision-budget governor's write surface. Pure
        # bridge state (a mutex-protected ledger), so ANY polarity in the
        # bridge's own table (snapshot.cpp) — listed here so the two tables
        # AGREE rather than disagreeing loudly on every sync (§2.2).
        "report_usage", "set_budget_config",
    })
    _ANY_READS = frozenset({
        "get_active_wars", "get_bridge_state", "who_holds_the_lock",
        "get_offset_validation", "get_tick_info", "get_controlled_roster",
        "get_budget_state",
    })
    _ANY_ACTIONS = _ANY_COMMANDS | _ANY_READS

    _polarity_warned: set = set()

    # ── ck-6kvq.9 / §2.2: THE BRIDGE IS THE POLARITY AUTHORITY ───────────────
    # The table above is a CACHED HINT used to pick a timeout, not an authority.
    # The bridge publishes the required clock state of every registered action
    # (`get_action_polarity`), derived from its own classifiers — the same ones
    # the flush loop partitions a buffer by. Two tables in two languages is a
    # defect waiting for a new handler, and they ALREADY disagree in at least
    # one place (query_conquest_targets is an is_deferred_drain_action in the
    # bridge and sits in _UNPAUSED_READS here), so a disagreement is reported
    # LOUDLY rather than silently mis-routed.
    #
    # Wire names come from the bridge's polarity_name() (flush_scheduler.cpp).
    # The bridge splits the tick class into tick_read / tick_write (reads must
    # drain before the writes of the same flush); the gateway has one
    # UNPAUSED_TICK for both, which is correct for its purpose — it only picks a
    # pause flip and a timeout.
    _WIRE_TO_POLARITY = {
        "any": Polarity.ANY,
        "paused": Polarity.PAUSED,
        "tick_read": Polarity.UNPAUSED_TICK,
        "tick_write": Polarity.UNPAUSED_TICK,
        "tick": Polarity.UNPAUSED_TICK,
        "unknown": Polarity.PAUSED,
    }

    def _bridge_polarity_table(self) -> dict:
        return getattr(self, "_bridge_polarity", {}) or {}

    def sync_polarity_table(self, published: dict) -> list:
        """Adopt the bridge's published polarity table and report disagreements.

        `published` is {action_name: wire_polarity} straight off
        `get_action_polarity`. Returns a list of
        {action, client, bridge} disagreement records — EMPTY means the two
        tables agree. Every disagreement is logged at ERROR: the bridge wins
        (it is the authority and the thing that actually routes the call), but
        it is never allowed to be a silent correction, because a silent
        correction is exactly how a new handler ends up mis-routed for months.
        """
        adopted: dict = {}
        disagreements: list = []
        for action, wire in (published or {}).items():
            pol = self._WIRE_TO_POLARITY.get(wire)
            if pol is None:
                log.error("BridgeGateway: bridge published unknown polarity %r for %r; "
                          "keeping the client classification", wire, action)
                continue
            adopted[action] = pol
            client_pol = self._classify_local(action)
            if client_pol is not pol:
                disagreements.append({"action": action,
                                      "client": client_pol.value,
                                      "bridge": pol.value})
        self._bridge_polarity = adopted
        for d in disagreements:
            log.error("POLARITY DISAGREEMENT: %s — client says %s, bridge says %s. "
                      "The bridge is the authority (ck-6kvq.9 §2.2); the client table "
                      "is only a timeout hint and should be corrected.",
                      d["action"], d["client"], d["bridge"])
        if disagreements:
            log.error("POLARITY DISAGREEMENT: %d action(s) disagree between the "
                      "gateway table and the bridge's published authority.",
                      len(disagreements))
        return disagreements

    def _classify_local(self, action: str) -> "BridgeGateway.Polarity":
        """The gateway's own static classification (the cached hint)."""
        if action in self._ANY_ACTIONS:
            return self.Polarity.ANY
        if action in self._WRITE_ACTIONS:
            return self.Polarity.UNPAUSED_TICK
        if action in self._UNPAUSED_READS:
            return self.Polarity.UNPAUSED_TICK
        if action.startswith("query_") or action.startswith("get_") or action.startswith("list_"):
            return self.Polarity.PAUSED
        if action not in self._polarity_warned:
            self._polarity_warned.add(action)
            log.warning("BridgeGateway: unclassified action %r → defaulting to PAUSED", action)
        return self.Polarity.PAUSED

    def _polarity(self, action: str) -> "BridgeGateway.Polarity":
        """Classify an action's required pause polarity.

        The bridge's published table wins when we have it (§2.2); otherwise fall
        back to the local hint. Unknown ⇒ PAUSED (the safe read default) + a
        one-time warning so a new action is loud, never mis-routed."""
        published = self._bridge_polarity_table().get(action)
        if published is not None:
            return published
        return self._classify_local(action)

    @asynccontextmanager
    async def _scoped_pause(self, polarity: "BridgeGateway.Polarity"):
        """RECORD {paused,speed} → FLIP to `polarity` → (caller executes) → RESTORE in
        finally. UNPAUSED_TICK holds the single-flight lock for the whole critical
        section (flip→execute→restore). PAUSED reads only toggle pause (idempotent),
        never speed; ANY does not flip at all. Restore is idempotent and runs on every
        exit path (exception, timeout, blocked) so tempo is never leaked."""
        g0 = self.game()
        was_paused = bool(g0.get("paused", True))
        prev_speed = int(g0.get("speed", 3) or 3)
        i_flipped_pause = False
        lock_held = False

        if polarity is self.Polarity.ANY:
            # No flip, no restore — the action operates the tempo directly.
            yield {"recorded": g0}
            return

        if polarity is self.Polarity.UNPAUSED_TICK:
            await self._strict_lock.acquire()
            lock_held = True
        try:
            if polarity is self.Polarity.PAUSED:
                if not was_paused:
                    await _action_bridge(self.state, "pause_game", {})
                    i_flipped_pause = True
            elif polarity is self.Polarity.UNPAUSED_TICK:
                if was_paused:
                    await _action_bridge(self.state, "unpause_game", {})
                    i_flipped_pause = True
            yield {"recorded": g0, "flipped": i_flipped_pause}
        finally:
            try:
                # RESTORE the recorded tempo. PAUSED-read restore only re-toggles the
                # pause IT flipped (never speed); UNPAUSED_TICK holders are authoritative
                # for tempo and re-apply both pause and speed.
                if polarity is self.Polarity.PAUSED:
                    if i_flipped_pause:
                        await _action_bridge(self.state, "unpause_game", {})
                elif polarity is self.Polarity.UNPAUSED_TICK:
                    if was_paused:
                        await _action_bridge(self.state, "pause_game", {})
                    else:
                        if i_flipped_pause:
                            await _action_bridge(self.state, "unpause_game", {})
                        if prev_speed and prev_speed != int(self.game().get("speed", prev_speed) or prev_speed):
                            await _action_bridge(self.state, "set_speed", {"speed": prev_speed})
            finally:
                if lock_held:
                    self._strict_lock.release()

    async def _call(self, action: str, params: dict, *, write: bool = False,
                    force_unpaused: bool = False):
        """The single record→flip→execute→restore entry point. read/read_unpaused/write
        are thin wrappers that pick polarity and delegate here."""
        if self.health == "hung":
            return {"reason": "bridge_disconnected"}
        # ANY-polarity actions (tempo/liveness/identity) are served inline even when a
        # caller routes them through write()/read_unpaused() — pause_game via write()
        # must NOT pause-then-restore-to-running (it IS the tempo). Otherwise force the
        # UNPAUSED_TICK polarity for writes / explicit deferred reads.
        table_pol = self._polarity(action)
        if table_pol is self.Polarity.ANY:
            pol = self.Polarity.ANY
            write = False
        elif write or force_unpaused:
            pol = self.Polarity.UNPAUSED_TICK
        else:
            pol = table_pol

        async def _do_ipc():
            # Writes: dispatch via action_bridge (fabricates sent_no_confirmation on
            # timeout, never raises), preceded by a tick-wait under UNPAUSED_TICK.
            if write:
                await self._await_tick_advance()
                return await _action_bridge(self.state, action, params, timeout=8.0)
            # ANY-polarity tempo/identity COMMANDS go through action_bridge (full
            # response); ANY-polarity readbacks fall through to query_bridge (.data).
            if action in self._ANY_COMMANDS:
                return await _action_bridge(self.state, action, params, timeout=8.0)
            # Deferred UNPAUSED reads: action_bridge path (longer timeout, drains on tick).
            if pol is self.Polarity.UNPAUSED_TICK:
                try:
                    return await _action_bridge(self.state, action, params, timeout=12.0)
                except (TimeoutError, ConnectionError):
                    return {}
            # Plain TIER-2 reads (and ANY-polarity get_*/query_* readbacks).
            try:
                return await _query_bridge(self.state, action, params, timeout=12.0)
            except (TimeoutError, ConnectionError):
                return {}

        needs_unpaused_retry = False
        async with self._scoped_pause(pol):
            res = await _do_ipc()
            # ── Bridge pause-transparency deference (the thin fallback) ──────────
            # The bridge self-classifies a deferred game-thread read it cannot serve
            # while paused with {status:"needs_unpaused"} (emit_needs_unpaused). Honor
            # that as the AUTHORITATIVE polarity signal even if our static _UNPAUSED_READS
            # table mis-classified the action as PAUSED: re-issue UNPAUSED below (outside
            # this scope, so tempo restores once and the UNPAUSED single-flight lock isn't
            # taken under the PAUSED scope). As the bridge takes over pause-transparency
            # for more handlers, the gateway defers to it automatically; OLD bridges never
            # emit this, so nothing changes for them.
            if (isinstance(res, dict) and res.get("status") == "needs_unpaused"
                    and pol is not self.Polarity.UNPAUSED_TICK):
                needs_unpaused_retry = True
            # Flip-miss: the bridge rejected with the opposite-polarity typed reason
            # (a race where our cached pause state lagged reality). Re-flip once + retry.
            elif isinstance(res, dict) and res.get("reason") in ("game_unpaused", "game_paused"):
                if res.get("reason") == "game_unpaused":
                    await _action_bridge(self.state, "pause_game", {})
                else:
                    await _action_bridge(self.state, "unpause_game", {})
                    if pol is self.Polarity.UNPAUSED_TICK:
                        await self._await_tick_advance()
                res = await _do_ipc()
        if needs_unpaused_retry:
            async with self._scoped_pause(self.Polarity.UNPAUSED_TICK):
                await self._await_tick_advance()
                res = await _do_ipc()
        return res

    # --- pause-owning bridge calls (thin wrappers over _call) ---------------
    async def read(self, action: str, params: dict):
        """TIER-2 read (or a deferred UNPAUSED read) — gw owns the polarity + restore.
        The agent never sees requires_paused/requires_unpaused."""
        return await self._call(action, params)

    async def read_unpaused(self, action: str, params: dict):
        """B* deferred read (strict-CB, decision eval, cb-capability) — UNPAUSED +
        single-flight. Retained as an explicit entry point for redesign.py call sites."""
        return await self._call(action, params, force_unpaused=True)

    async def write(self, action: str, params: dict):
        """TIER-3 / B* write — UNPAUSED ≥1 tick (E3), dispatch, then RESTORE the recorded
        tempo. Callers that need the game to keep ticking through a post-dispatch readback
        poll use confirmed_write() instead (one scope spanning dispatch+poll+restore)."""
        return await self._call(action, params, write=True)

    async def read_inline(self, action: str, params: dict):
        """An unscoped, unlocked read for use INSIDE a confirmed_write readback poll
        OR an unpaused_window(): the game is already unpaused and the single-flight lock
        is already held by the enclosing scope, so re-entering _scoped_pause (which would
        re-acquire the lock or pause mid-poll) must be avoided. UNPAUSED game-thread reads
        (strict-CB / cbs / conquest) drain on the live tick, so route them via
        action_bridge (longer timeout) exactly like _do_ipc's UNPAUSED_TICK branch; plain
        TIER-2 reads fall through to query_bridge. No flip, no lock — the window owns both."""
        if action in self._UNPAUSED_READS or action in ("query_cbs_for_targets",):
            try:
                return await _action_bridge(self.state, action, params, timeout=12.0)
            except (TimeoutError, ConnectionError):
                return {}
        try:
            return await _query_bridge(self.state, action, params, timeout=8.0)
        except (TimeoutError, ConnectionError):
            return {}

    async def read_many(self, requests: "list[tuple[str, dict]]") -> list:
        """Issue INDEPENDENT reads CONCURRENTLY, PAUSED ones under ONE scope.

        ck-p3vd. The agent paid one flush period PER READ: observe_v2(depth=full)
        fans out to ~10 reads awaited in turn, ~10 s each, ~100 s for one
        observation — the whole graded episode. A flush serves its entire buffer
        in one span (flush_scheduler.cpp:407-415), so reads issued together cost
        ONE period however many there are.

        MEASURED by ck-zgh4 on the live bridge, four independent reads:
            sequential            20.01 s
            one call_many envelope 10.23 s
            SEPARATE envelopes issued back-to-back then collected  9.83 s
        The third is this design, and it matches the batched envelope — so no
        new client method is needed. `_send_and_wait` already mints a uuid4
        request_id per call and registers its own future, so concurrent issue
        correlates BY CONSTRUCTION, with none of the misrouting risk that made
        ck-iu11 expensive.

        ONE SCOPE, NOT N, AND THAT IS THE WHOLE POINT. `_scoped_pause` records
        {paused,speed}, flips, and restores in a finally. N concurrent per-read
        scopes would each sample `was_paused` independently and race their
        restores — one could unpause while another still required PAUSED. In
        PIPELINE mode that race is currently unreachable because the flips are
        refused outright (ck-tygb), but "correct because a refusal happens to
        save us" is a coincidence with a timer on it, and in LEGACY mode the
        verbs are not refused and the race is real. So: one window, like
        `unpaused_window` already does for the UNPAUSED_TICK side.

        POLARITY IS PRESERVED PER READ, NOT PER BATCH. ck-zgh4 measured that a
        fast read gathered with slow ones pays the slow one's latency —
        query_realm cost 0.22 s alone and 9.83 s gathered. So ANY-polarity reads
        are issued OUTSIDE the paused scope and keep their inline fast path;
        sweeping them in would buy nothing and cost them a flush.

        UNPAUSED_TICK reads are NOT batched here at all: they hold the
        single-flight lock and are serial by necessity (`unpaused_window` is
        their batching story). They fall through to the ordinary per-read path
        rather than being silently reclassified.
        """
        if not requests:
            return []
        results: list = [None] * len(requests)
        any_idx, paused_idx, other_idx = [], [], []
        for i, (action, _) in enumerate(requests):
            pol = self._polarity(action)
            if pol is self.Polarity.ANY:
                any_idx.append(i)
            elif pol is self.Polarity.PAUSED:
                paused_idx.append(i)
            else:
                other_idx.append(i)

        async def _fill(idx_list, coro_for):
            if not idx_list:
                return
            got = await asyncio.gather(*[coro_for(i) for i in idx_list],
                                       return_exceptions=True)
            for i, r in zip(idx_list, got):
                # A read that raised is recorded as an EMPTY result, exactly as
                # read_inline does for its own timeouts — one read failing must
                # not take the batch down, and the caller's existing
                # isinstance(x, dict) guards already treat {} as "absent".
                results[i] = {} if isinstance(r, BaseException) else r

        # ANY first and unscoped: they are inline-served, so they keep their
        # sub-second path instead of waiting on the paused span.
        await _fill(any_idx, lambda i: self._call(*requests[i]))
        if paused_idx:
            async with self._scoped_pause(self.Polarity.PAUSED):
                await _fill(paused_idx,
                            lambda i: self.read_inline(*requests[i]))
        # Not batchable — issued the ordinary way so nothing is reclassified.
        for i in other_idx:
            results[i] = await self._call(*requests[i])
        return results

    @asynccontextmanager
    async def unpaused_window(self):
        """Open ONE UNPAUSED_TICK scope (flip→unpause + single-flight lock acquired ONCE),
        wait for a live tick, and yield. Callers issue every deferred game-thread read
        inside via read_inline — so the whole plan_war enumeration loop pays a single
        flip-in/flip-out and ONE lock acquire/release instead of one per call. The scope's
        finally restores the recorded tempo exactly once. Pause-transparent: when the game
        was already paused (the agent's observe window) the entry unpauses and the exit
        re-pauses, identically to the per-call path — just once. Crash-safety is UNCHANGED:
        reads inside still serialize through the same g_cb_query single-flight slot (it is
        held for the whole window), so they never overlap each other or the engine's AI CB
        evals; read_inline does no name-cache drain and honors the same is_mapped/safe_read
        bridge discipline as the per-call reads it replaces."""
        async with self._scoped_pause(self.Polarity.UNPAUSED_TICK):
            await self._await_tick_advance()
            yield

    async def confirmed_write(self, action: str, params: dict, *, readback,
                              deadline_s: float = 12.0, max_polls: int = 8):
        """Deferred-write confirmation (_HARDENING_PLAN.md FIX 1 §1.4): ONE
        UNPAUSED_TICK scope spanning dispatch → readback poll → restore. The game keeps
        ticking for the whole poll window (no premature restore mid-poll), then the
        scope's finally restores tempo exactly once.

        `readback` is an async predicate taking the raw dispatch dict and returning the
        resolved object (truthy) or None. It uses ANY-polarity reads (get_active_wars /
        query_realm / query_armies) so it does not perturb the flip.

        Returns {"raw": <dispatch dict>, "resolved": <readback obj or None>,
                 "polls": <count>}."""
        if self.health == "hung":
            return {"raw": {"reason": "bridge_disconnected"}, "resolved": None, "polls": 0}
        async with self._scoped_pause(self.Polarity.UNPAUSED_TICK):
            await self._await_tick_advance()
            try:
                raw = await _action_bridge(self.state, action, params, timeout=8.0)
            except (TimeoutError, ConnectionError):
                raw = {"status": "sent_no_confirmation", "action": action}
            raw = raw if isinstance(raw, dict) else {}
            # Immediate readback, then poll riding the live tick up to deadline.
            resolved = await readback(raw)
            polls = 0
            if resolved is None:
                deadline = asyncio.get_event_loop().time() + deadline_s
                while (resolved is None
                       and asyncio.get_event_loop().time() < deadline
                       and polls < max_polls):
                    await self._await_tick_advance(timeout_s=2.5)
                    resolved = await readback(raw)
                    polls += 1
            return {"raw": raw, "resolved": resolved, "polls": polls}

    async def _await_tick_advance(self, timeout_s: float = 6.0) -> bool:
        """Block until the live tick advances at least once after unpause, or timeout.
        Returns True if a tick was observed. Bounded so a frozen thread can't hang the
        write (the layer surfaces drain_timeout/severity:frozen elsewhere)."""
        start_tick = self.state.tick_id
        deadline = asyncio.get_running_loop().time() + timeout_s
        while asyncio.get_running_loop().time() < deadline:
            if self.state.tick_id > start_tick:
                return True
            await asyncio.sleep(0.2)
        return self.state.tick_id > start_tick

    async def delta_since_last(self) -> list:
        """Since-tick event aggregation (the API tracks the tick; kills manual carry)."""
        st = self._last_delta_tick
        self._last_delta_tick = self.state.tick_id
        events: list = []
        for act, key, kind in (("query_war_declarations", "declarations", "war"),
                               ("query_scheme_inits", "scheme_inits", "scheme"),
                               ("query_council_tasks", "assignments", "council_task")):
            try:
                res = await _query_bridge(self.state, act, {"since_tick": st, "limit": 20})
            except Exception:
                continue
            if isinstance(res, dict):
                for rec in (res.get(key) or []):
                    events.append(f"{kind}: {rec.get('summary') or rec.get('name') or rec}")
        return events[:20]

    async def delta_records_since_last(self) -> list:
        """Since-tick events as RECORDS, not repr strings (ck-s8vr).

        `delta_since_last` formats each record with
        `f"{kind}: {rec.get('summary') or rec.get('name') or rec}"`, and the
        bridge's war and scheme records carry neither `summary` nor `name` — so
        the fallback stringifies the dict and the structure is lost inside an
        f-string. Measured over d49+d50: 5,640 delta entries, EVERY ONE a
        Python-repr dict ("war: {'tick': 61, 'attacker_id': 30966, ...}"), and
        75.5 per cent of observe_v2's bytes. A consumer cannot filter what it
        cannot parse, which is why the foreign-war firehose reached every seat
        whole.

        Same reads, same budget, same tick bookkeeping. Only the shape differs.
        `delta_since_last` is left exactly as it was: it has two gateway
        implementations and this changes neither of their contracts.
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


def _get_gateway() -> "BridgeGateway":
    state = _get_state()
    gw = getattr(state, "_gateway", None)
    if gw is None:
        gw = BridgeGateway(state)
        state._gateway = gw
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
    latest_tick_start: dict = field(default_factory=dict)
    latest_observation: dict = field(default_factory=dict)
    handshake_info: dict = field(default_factory=dict)
    last_action_result: dict = field(default_factory=dict)
    #: ck-9rto. Broadcast action_results this session refused because they
    #: correlated to nobody here. Non-zero means the bridge emitted one.
    uncorrelated_results: int = 0
    #: ck-ey8p. Frames this listener's type dispatch did not handle, counted by
    #: `type`. THE DICT IS EMPTY UNTIL ONE ARRIVES, never pre-seeded with zeroes:
    #: an absent key means none of that type was seen, and a zero would claim a
    #: reading nobody took. Before this existed the dispatch had no `result`
    #: branch and no terminal else, so a ledger record matching no pending id
    #: was discarded with NO log line and NO counter — the client could not
    #: witness an unmatched record even in principle, and a zero in a bundle
    #: could not be told from "none was ever emitted".
    unhandled_frames: dict = field(default_factory=dict)
    action_log: list = field(default_factory=list)
    pending_queries: dict = field(default_factory=dict)  # request_id -> asyncio.Future
    #: ck-2r9k, THE SECOND CONSUMER. request_id -> the flush ETA IN SECONDS that
    #: the pipeline's own acceptance ack quoted for that call. The ack is keyed
    #: `for_request_id` and never `request_id` (§8.2), so the correlation block
    #: below cannot see it and this is where it is remembered.
    #:
    #: THE ACK IS NOT THE ANSWER and is deliberately not used to resolve the
    #: future. For a write it carries no outcome — a `board_post` refused at
    #: flush time for being 3,831 bytes acks `accepted:true` exactly like one
    #: that lands — so settling on it would report a refusal as a success. What
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
    _gateway: Optional[object] = None  # BridgeGateway for the 8 redesign tools
    mock_mode: bool = False
    #: lane-multi / lane-prebind: the outcome of binding CK3_AGENT_CHARACTER_ID
    #: ({character_id, ok, reason, bound_at?, also: [...], wait: {...}}), None
    #: until the bind has been attempted. While the game is not yet active the
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
            # tick/game_date are current even while paused (no tick_start is pushed
            # when the game is paused, which is the required state for reads).
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
            # the consumer the board tools run through: board_post ->
            # BridgeGateway.write -> _action_bridge -> _send_and_wait, all in
            # this file. Two clients, one protocol, one of them taught.
            #
            # THE ACK EXTENDS THE DEADLINE; IT DOES NOT SETTLE THE CALL. A
            # write's outcome is not in the ack: an oversized or over-quota
            # board_post is `accepted: true` here and `success: false` in the
            # settled frame, so resolving on the ack would report a REFUSAL AS A
            # SUCCESS. The future still settles on the real result.
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
                state.latest_tick_start = msg
                state.tick_id = msg.get("tick_id", 0)
                state.game_date = msg.get("game_date", "unknown")

            elif msg_type == "observation":
                state.latest_observation = msg
                state.tick_id = msg.get("tick_id", state.tick_id)
                state.game_date = msg.get("game_date", state.game_date)

            elif msg_type == "action_result":
                # ck-9rto: UNCORRELATED BY CONSTRUCTION — do not store it.
                #
                # The block above routes by `request_id` and `continue`s, so a
                # frame arriving here carries no request_id or somebody else's.
                # The bridge BROADCASTS queue-bound replies to every client slot
                # (ck-st70), and THIS IS THE B PATH — the runtime the arms
                # actually run (decision G6). Arms A and B are six sessions on
                # one bridge.
                #
                # It could not reach the model TODAY only because
                # `last_action_result` is WRITE-ONLY: two writes, no readers
                # anywhere in ck3agent/. That is an accident of the field never
                # having been consumed, not a guard — the first seat to surface
                # it would arm the hazard silently. The same branch in
                # `agent.py` (the A path) had the identical shape and I fixed
                # it one file over without checking here, which is the whole
                # reason to run the family rather than the site.
                state.uncorrelated_results += 1
                log.warning(
                    "DROPPED an uncorrelated action_result (request_id=%r): it "
                    "was not matched to any pending request of this session, "
                    "and the bridge broadcasts queue-bound replies — so it is "
                    "most likely ANOTHER session's. Total dropped: %d",
                    msg.get("request_id"), state.uncorrelated_results)

            elif msg_type == "handshake_ack":
                state.handshake_info = msg
                log.info("Late handshake ack received")

            elif msg_type == "notification":
                event = msg.get("event", "")
                if event == "character_died":
                    char_id = msg.get("character_id", 0)
                    log.warning("GAME OVER: Controlled character %d has died!", char_id)
                    state.last_action_result = msg
                else:
                    log.info("Bridge notification: %s", event)

            elif msg_type == "ping":
                pass

            elif msg_type == "error":
                log.error("Bridge error: %s", msg.get("message"))

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
    state.latest_tick_start = {
        "type": "tick_start",
        "tick_id": 42,
        "game_date": "1066.09.15",
        "game_speed": 3,
        "paused": False,
    }
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
        # ck-6kvq.9 / §2.2: adopt the bridge's polarity table as the authority
        # and report any disagreement with the local hint LOUDLY. Best-effort:
        # an older bridge answers unknown_action and we keep the local table.
        await _sync_polarity_authority(state)
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

mcp = FastMCP(
    "CK3 Bridge",
    instructions=(
        # ck-zgh4 deploy-51: "the LLM acts as a 'personality tuner'" told the
        # caller what its ROLE was. It describes what the tools do instead.
        "This server connects to a running Crusader Kings 3 game via an "
        "injected bridge library. Its tools read game state and write changes "
        "to it: personality weights, AI subsystem toggles, and typed actions "
        "such as interactions, council tasks, laws and war declarations. The "
        "game's own AI continues to run everything not written through them.\n\n"
        "Tools with clean phase boundaries: query / discover -> act.\n"
        "All responses use a standard envelope: {ok, data, error, tick, game_date}."
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
        "query / discover -> act for game state and actions."
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
    for key in ("character", "personality", "subsystems", "realm", "succession",
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


def _envelope(state: BridgeState, data: dict | str | None = None,
              resolved_id: int | None = None) -> dict[str, Any]:
    """Wrap a successful result in the standard envelope as a real dict.

    Returns structured JSON (a dict), NOT a json.dumps string — FastMCP serializes
    the dict to both unstructured content and structured_content, so the MCP client
    receives a real blob it never has to re-parse.

    Double-encoding guard: if `data` arrives as a `str` that is actually JSON (some
    caller handed us a pre-serialized blob), parse it back to a real object so the
    returned `data` is never a nested JSON string. If it isn't JSON, keep the string.

    #94: when `resolved_id` is provided (or can be lifted from `data`), echo it as a
    top-level `resolved_character_id` so the agent can assert WHICH character the
    read/steer actually acted on (default character_id resolves to the bridge primary
    controlled char — the silent wrong-target trap).
    """
    if isinstance(data, str):
        try:
            data = json.loads(data)
        except (json.JSONDecodeError, ValueError):
            pass  # genuine plain string — leave as-is
    if resolved_id is None:
        resolved_id = _extract_resolved_id(data)
    env: dict[str, Any] = {
        "ok": True,
        "data": data,
        "tick": state.tick_id,
        "game_date": state.game_date,
    }
    if isinstance(resolved_id, int) and resolved_id > 0:
        env["resolved_character_id"] = resolved_id
    return env


def _error(state: BridgeState, code: str, message: str) -> dict[str, Any]:
    """Wrap an error in the standard envelope as a real dict (not a json.dumps string)."""
    return {
        "ok": False,
        "error": {"code": code, "message": message},
        "tick": state.tick_id if state else None,
        "game_date": state.game_date if state else None,
    }


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


async def _ensure_connected(state: BridgeState) -> dict[str, Any] | None:
    """Ensure bridge is connected. Returns error envelope dict if not, None if OK."""
    if state.mock_mode:
        return None
    if not state.connected:
        await _try_reconnect(state)
    if not state.connected:
        return _error(state, "bridge_disconnected",
                      "Bridge not connected. Is CK3 running with the bridge DLL?")
    return None


async def _sync_polarity_authority(state: BridgeState) -> list:
    """Fetch the bridge's published polarity table and adopt it (ck-6kvq.9 §2.2).

    The bridge is the single polarity authority: it is the thing that actually
    partitions a flush buffer, and it cannot invent a polarity it is handed. The
    gateway's own table is demoted to a cached timeout hint. Any disagreement is
    logged at ERROR by sync_polarity_table() rather than silently corrected.

    Best-effort and never fatal: an older bridge that does not know
    `get_action_polarity` answers unknown_action, and the gateway simply keeps
    using its local classification.
    """
    gw = getattr(state, "_gateway", None)
    if gw is None:
        gw = BridgeGateway(state)
        state._gateway = gw
    if not hasattr(gw, "sync_polarity_table"):
        return []
    try:
        res = await _send_and_wait(state, "get_action_polarity", {}, timeout=8.0)
    except Exception as e:                       # noqa: BLE001 — never fatal
        log.warning("Polarity authority: could not fetch the bridge table (%s: %s); "
                    "using the local classification", type(e).__name__, e)
        return []
    table = res.get("polarity") if isinstance(res, dict) else None
    if not isinstance(table, dict) or not table:
        log.warning("Polarity authority: bridge published no table "
                    "(older bridge?); using the local classification")
        return []
    disagreements = gw.sync_polarity_table(table)
    log.info("Polarity authority: adopted %d action classifications from the bridge "
             "(%d disagreement(s) with the local table)", len(table), len(disagreements))
    return disagreements



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

    It is not every call, and the honest claim is narrower than "board_post
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
    """Send a query to the bridge and return the data portion of the response."""
    result = await _send_and_wait(state, action, params, timeout)
    return result.get("data", result.get("message", result))


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
# Tool 1: observe
# ---------------------------------------------------------------------------

@mcp.tool()
async def observe(
    include: Annotated[
        list[str],
        Field(
            description="Sections to include. Default = character + tick + "
                        "personality + evaluations. Add 'subsystems' to also "
                        "read the 38 subsystem toggles.",
            json_schema_extra={"items": {"type": "string",
                "enum": ["character", "tick", "personality",
                         "evaluations", "subsystems"]}},
        ),
    ] = None,
    character_id: int = 0) -> dict[str, Any]:
    """Combined snapshot of the current game state — the once-per-cycle read.

    Returns the controlled character's profile (stats, traits, gold/prestige/
    piety), the tick/date/speed, the 10 personality axes, and the AI evaluations,
    in a single call. Add 'subsystems' to include when you plan to toggle any.

    Args:
        include: Sections to include (default: character, tick, personality,
            evaluations). Optionally add 'subsystems'.
        character_id: Character to observe (0 = controlled character).
    """
    state = _get_state()
    err = await _ensure_connected(state)
    if err:
        return err

    from tools import resolve_observe_sections

    params = {"character_id": character_id}
    sections = resolve_observe_sections(include, params)
    data = {}
    section_errors = {}
    for name, action, bridge_params in sections:
        try:
            data[name] = await _query_bridge(state, action, bridge_params)
        except (ConnectionError, TimeoutError) as e:
            section_errors[name] = str(e)
        except Exception as e:
            section_errors[name] = str(e)
    if section_errors:
        data["section_errors"] = section_errors
    return _envelope(state, data)


# ---------------------------------------------------------------------------
# Tool 2: query
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
            max_length=5,
        ),
    ] = None,
    character_id: int = 0, target_id: int = 0,
    title_id: int = 0, trait_id: int = 0,
    category: int = 0, include_vassals: bool = False,
    war_id: int = 0, culture_id: int = 0,
    dynasty_id: int = 0, filter: str = "", sort: str = "",
    sort_order: str = "", max_results: int = 0,
    since_tick: int = 0, limit: int = 0,
    require_in_range: bool = True, verbose: bool = False,
    scope: str = "", hops: int = 0, max_nodes: int = 0,
    target_county: int = 0, target_county_key: str = "",
    counties: str = "", detail: str = "") -> dict[str, Any]:
    """Deep-dive query for a specific game domain. Supports batch queries via
    'domains' (max 5) with partial failure.

    Args:
        domain: Which game domain to query (single).
        domains: Batch query: up to 5 domains. Mutually exclusive with domain.
        character_id: Character context (0 = controlled).
        target_id: Secondary character for opinion, valid_cbs, diplomatic_range.
        title_id: Title context for titles, de_jure, election, neighbors, adjacency, development.
            neighbors: title_id is a COUNTY. With title_id=0 the character is used
            (character_id, 0 = the bound ruler): its capital county's neighbours, plus
            bordering_realms[] = every realm whose counties touch that character's realm
            (own + vassal counties), with a border_counties count each. Each neighbour row
            carries title_id, holder_id and realm_ruler_id (the holder's top liege).
        trait_id: Trait ID for trait_compatibility.
        category: Building category for buildings domain (1095=constructible, 1825=special, 0=all).
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
        max_results: For domain="characters": cap on returned rows (lean default 25,
            verbose default 50; lean ceiling 500, verbose ceiling 5000).
        verbose: Opt-in to full detail for characters/wars/council/armies/realm/claims.
            Default False returns lean payloads (drops per-row skills, government, age,
            war participant id arrays, score_breakdown, and per-army secondary status
            fields) — all of which the agent's default formatters do not read. Set True
            only when you specifically need those extra fields.
    """
    state = _get_state()
    err = await _ensure_connected(state)
    if err:
        return err

    params = {
        "character_id": character_id, "target_id": target_id,
        "title_id": title_id, "trait_id": trait_id,
        "category": category, "include_vassals": include_vassals,
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

    # Batch mode
    if domains:
        if len(domains) > 5:
            return _error(state, "invalid_param",
                          "Batch query limited to 5 domains.")
        data = {}
        batch_errors = {}
        for d in domains:
            resolved = resolve_query(d, params)
            if resolved is None:
                batch_errors[d] = f"Unknown domain: {d}"
                continue
            action, bridge_params = resolved
            try:
                result = await _query_bridge(state, action, bridge_params)
                data[d] = result
            except (ConnectionError, TimeoutError) as e:
                batch_errors[d] = str(e)
            except Exception as e:
                batch_errors[d] = str(e)
        if batch_errors:
            data["batch_errors"] = batch_errors
        return _envelope(state, data)

    # Single domain mode
    if not domain:
        return _error(state, "invalid_param",
                      "Either 'domain' or 'domains' is required.")

    # event_log: synthetic "what happened since I last looked" feed — merges the
    # bridge's existing since_tick hook buffers (war declarations, scheme inits,
    # council tasks) into one chronological stream. Pure agent-side aggregation over
    # existing handlers (no bridge change). Call once per cycle with the prior
    # current_tick as since_tick; optional `filter` = comma-list of kinds.
    if domain == "event_log":
        lim = limit if limit > 0 else 100
        st = since_tick
        kinds = {k.strip() for k in filter.split(",")} if filter else None
        feeds = [
            ("query_war_declarations", "declarations", "war"),
            ("query_scheme_inits",     "scheme_inits", "scheme"),
            ("query_council_tasks",    "assignments",  "council_task"),
        ]
        events = []
        source_errors = {}
        cur_tick = 0
        for action, list_key, etype in feeds:
            if kinds and etype not in kinds:
                continue
            try:
                res = await _query_bridge(state, action,
                                          {"since_tick": st, "limit": lim})
            except (ConnectionError, TimeoutError) as e:
                source_errors[etype] = str(e)
                continue
            except Exception as e:
                source_errors[etype] = str(e)
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
        out = {
            "action": "event_log",
            "current_tick": cur_tick,
            "since_tick": st,
            "event_count": len(events),
            "truncated": truncated,
            "events": events,
        }
        if source_errors:
            out["source_errors"] = source_errors
        return _envelope(state, out)

    # relative_power: the playbook's M1 ratio helper — my strength (+allies) vs a
    # target (+their allies), and vs my largest faction. Agent-side aggregation over
    # alliances + faction_threat (no bridge change). target_id optional.
    if domain == "relative_power":
        try:
            me = await _query_bridge(state, "query_alliances", {"character_id": character_id})
        except (ConnectionError, TimeoutError) as e:
            return _error(state, "bridge_disconnected", str(e))
        me = me if isinstance(me, dict) else {}
        my_str = float(me.get("own_military_strength") or 0)
        my_callable = float(me.get("total_callable_strength") or my_str)
        out = {"action": "relative_power", "character_id": character_id,
               "my_strength": my_str, "my_callable_strength": my_callable}
        if target_id:
            try:
                t = await _query_bridge(state, "query_alliances", {"character_id": target_id})
            except (ConnectionError, TimeoutError):
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
        except (ConnectionError, TimeoutError):
            ft = {}
        ft = ft if isinstance(ft, dict) else {}
        largest = max([float(f.get("power_pct_of_liege") or 0) for f in (ft.get("factions") or [])], default=0.0)
        internal = (100.0 / largest) if largest > 0 else float("inf")
        out.update({"largest_faction_power_pct": largest,
                    "internal_ratio": (round(internal, 2) if internal != float("inf") else None),
                    "internal_safe": internal >= 1.25,
                    "hint": "offensive_ratio>=1.5 ⇒ winnable; internal_ratio<1.25 ⇒ suspend external wars (playbook M1)"})
        return _envelope(state, out)

    resolved = resolve_query(domain, params)
    if resolved is None:
        return _error(state, "unknown_domain",
                      f"Unknown domain: {domain}. Valid: {', '.join(QUERY_DOMAINS)}")

    action, bridge_params = resolved
    try:
        result = await _query_bridge(state, action, bridge_params)
        return _envelope(state, result)
    except TimeoutError:
        return _error(state, "timeout",
                      f"Query timed out for domain '{domain}'.")
    except ConnectionError as e:
        return _error(state, "bridge_disconnected", str(e))


# ---------------------------------------------------------------------------
# Tool 3: discover
# ---------------------------------------------------------------------------

async def _discover_targets(state, action: str, actor_id: int,
                            title_id: int) -> dict[str, Any]:
    """Execute a discover(what='targets', action=...) plan: enumerate the valid
    targets for an action, annotating per-target validity where available."""
    from tools import resolve_targets_plan
    plan = resolve_targets_plan(action, {"actor_id": actor_id, "title_id": title_id})

    if plan.get("mode") == "error":
        return _error(state, "invalid_param", plan.get("reason", "unsupported action"))

    try:
        if plan["mode"] == "single":
            result = await _query_bridge(state, plan["bridge_action"], plan["params"])
            return _envelope(state, {"action": action, **(result if isinstance(result, dict) else {"result": result})})

        # pool_annotate: enumerate the candidate pool, then annotate each entry.
        pool = plan["pool"]
        pool_resp = await _query_bridge(state, pool["bridge_action"], pool["params"])
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
        return _envelope(state, {
            "action": action,
            "label": plan.get("label"),
            "target_count": len(targets),
            "truncated": len(items) > cap,
            "targets": targets,
        })
    except TimeoutError:
        return _error(state, "timeout", f"targets(action='{action}') timed out.")
    except ConnectionError as e:
        return _error(state, "bridge_disconnected", str(e))


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
        return _error(state, "invalid_param",
                      f"decision_why_blocked supports {SHIELD_DECISIONS}; got '{decision}'")

    cid = actor_id or 0
    try:
        realm = await _query_bridge(state, "query_realm", {"character_id": cid})
        character = await _query_bridge(state, "query_character", {"character_id": cid})
    except (ConnectionError, TimeoutError) as e:
        return _error(state, "bridge_disconnected", str(e))
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
        if not isinstance(titles, dict) or titles.get("error") or (
                "titles" not in titles and "held_titles" not in titles):
            # D1-B: never fabricate a confident empty verdict from a failed
            # read — report the gate as unmeasured, loudly.
            reason = (titles.get("error") if isinstance(titles, dict)
                      else f"unexpected_reply:{type(titles).__name__}")
            log.warning("duchy-control read failed (%s) — reporting unmeasured",
                        reason)
            # duchy_control=None routes to the shield's existing UNMEASURED
            # clause (a hint, not a false fail) instead of a confident empty.
            verdict = shield_decision_why_blocked(
                decision, realm=realm, character=character, duchy_control=None)
            return _envelope(state, verdict)
        held = titles.get("titles") or titles.get("held_titles") or []
        complete, candidates = [], []
        for t in (held if isinstance(held, list) else [])[:40]:
            if not isinstance(t, dict):
                continue
            tier = t.get("tier")
            key = str(t.get("key") or t.get("title_key") or "")
            if tier != 3 and not key.startswith("d_"):
                continue
            # de-jure-above filter: prefer an explicit field; else accept any duchy
            # (the checker's count is conservative — the agent verifies the parent).
            dejure_above = str(t.get("de_jure_kingdom") or t.get("de_jure_liege_key") or "")
            if dejure_above and _DANELAW_KINGDOM_KEY not in dejure_above:
                continue
            tkey = key or None
            row = {"title_id": t.get("title_id"), "key": tkey}
            try:
                prog = await _query_bridge(state, "query_dejure_progress",
                                           {"character_id": cid, "title_key": tkey}) if tkey else None
            except (ConnectionError, TimeoutError):
                prog = None
            if isinstance(prog, dict):
                held_c = prog.get("held"); total_c = prog.get("total")
                row["held"] = held_c; row["total"] = total_c
                if isinstance(held_c, int) and isinstance(total_c, int) and total_c > 0 and held_c >= total_c:
                    complete.append(row)
                else:
                    candidates.append(row)
            else:
                candidates.append(row)
        duchy_control = {"complete_duchies": complete, "candidate_duchies": candidates}

    verdict = shield_decision_why_blocked(
        decision, realm=realm, character=character, duchy_control=duchy_control)
    return _envelope(state, verdict)


@mcp.tool()
async def discover(what: str, interaction_type: str = "",
                   target_id: int = 0, actor_id: int = 0,
                   interaction: str = "", cb: str = "",
                   title: str = "", action: str = "", title_id: int = 0,
                   building: str = "", location_id: int = 0, slot: int = 0,
                   task: str = "", max_results: int = 0,
                   character_id: int = 0, decision_type: str = "",
                   decisions: list = None) -> dict[str, Any]:
    """Pre-action validation. List interaction/decision types, check acceptance, or
    validate casus belli. Use before act() to verify feasibility.

    Args:
        what: What to discover: interactions, decisions, valid_decisions,
              acceptance, valid_cbs,
              valid_cbs_strict, trigger_result, trigger_slots, war_validity,
              evaluate_trigger, interaction_validity,
              decisions (ALL decisions, each annotated with
              shown/valid/cost_ok/available + a failures[] of unmet-requirement
              loc-keys + the decision cost{} and the character's
              current{gold,piety,prestige}); valid_decisions (same eval, ONLY
              available==true decisions). Both take optional character_id.
              war_targets (enumerate every target+title+CB the actor can attack —
              the "who can I declare war on" surface; CANDIDATES ONLY: `declarable`
              means no relation gate (kin, alliance, truce, imprisonment, already at
              war) blocks the row, not that the game accepts the war. The strict engine
              check, cbs(target_id) or what=valid_cbs_strict, is authoritative and wins
              when the two disagree),
              imprisonment (can actor imprison/banish/execute target_id, and on
              what grounds — gate before imprison_interaction),
              targets (THE "who are valid targets for action X" surface — pass
              action=war|imprison|execute|banish|revoke_title|claim; returns the
              candidate list with per-target validity. claim needs title_id),
              fabricate_targets (which counties are ELIGIBLE fabricate-claim
              targets for actor_id right now — runs the council task's real
              eligibility triggers per county; feed a returned title_id into
              act/set_council_task(task='task_fabricate_claim', target_title_id)).
        task: For what=fabricate_targets: council task id (default
              'task_fabricate_claim').
        max_results: For what=fabricate_targets: cap on eligible counties
              returned (default 40).
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
    """
    state = _get_state()
    err = await _ensure_connected(state)
    if err:
        return err

    # Legibility capstone: what="targets" — "who are the valid targets for
    # action X right now". Some actions resolve in one call; others enumerate a
    # candidate pool then annotate each entry with per-target validity.
    if what == "targets":
        return await _discover_targets(state, action, actor_id, title_id)

    # Legibility (re/findings/re_shield_robustness.md (D)): per-clause pass/fail for
    # the SHIELD decisions (negotiate_the_danelaw / found_kingdom) from CHEAP
    # STRUCTURAL READS — the SAFE alternative to the hanging engine eval
    # (decisions_annotated). Pure reads → works PAUSED; additive; crash-safe.
    if what == "decision_why_blocked":
        return await _discover_decision_why_blocked(
            state, decision_type or interaction_type or "negotiate_the_danelaw_decision",
            actor_id or character_id, title_id)

    from tools import resolve_discover, shape_discover_response
    resolver_params = {
        "what": what, "interaction_type": interaction_type,
        "target_id": target_id, "actor_id": actor_id,
        "interaction": interaction, "cb": cb, "title": title,
        "building": building, "location_id": location_id, "slot": slot,
        "task": task, "max_results": max_results,
        "character_id": character_id,
        # #64: for what=decisions_annotated / valid_decisions — annotate ONLY these
        # decisions (key names and/or name-hashes from what="decisions").
        "decisions": decisions or [],
    }
    resolved = resolve_discover(resolver_params)
    if resolved is None:
        return _error(state, "invalid_param",
                      f"Unknown discover target: {what}")

    action, bridge_params = resolved

    # Deferred queries need a game tick to drain. valid_cbs_strict,
    # trigger_result and interaction_validity all queue work onto the game
    # thread, so route them through the longer-timeout action path.
    is_deferred = what in (
        "acceptance", "valid_cbs", "valid_cbs_strict",
        "trigger_result", "evaluate_trigger", "interaction_validity",
    )
    try:
        if is_deferred:
            result = await _action_bridge(state, action, bridge_params, timeout=10.0)
        else:
            result = await _query_bridge(state, action, bridge_params)
        shaped = shape_discover_response(resolver_params, result)
        return _envelope(state, shaped)
    except TimeoutError:
        return _error(state, "timeout", f"Discover '{what}' timed out.")
    except ConnectionError as e:
        return _error(state, "bridge_disconnected", str(e))


# ---------------------------------------------------------------------------
# Tool 4: act
# ---------------------------------------------------------------------------
# RETIRED (Phase-2 migration, _PHASE2_PARITY.md): FULL v2 parity — every `act` type
# maps to a `do` intent or `declare_war` (INTENT_TO_ACT + declare_war). Deregistered
# from the MCP surface + get_tool_definitions + prompts.py. Function kept for the
# in-process dispatch fallback (agent.py) but NO LONGER advertised. To re-enable for
# debugging, restore the @mcp.tool() decorator below.
# @mcp.tool()
async def act(type: str, interaction_type: str = "", decision_type: str = "",
              target_id: int = 0, actor_id: int = 0, title_id: int = 0,
              secondary_actor_id: int = 0, secondary_recipient_id: int = 0,
              cb_index: int = -1, cb_title_id: int = 0, claimant_id: int = 0,
              law_name: str = "", option: int = 0, event_id: str = "",
              instance_id: int = 0,
              force_player_path: bool = False, casus_belli: str = "",
              method: str = "", skip_pre_validate: bool = False,
              focus: str = "", task: str = "",
              building: str = "", location_id: int = 0,
              position_id: int = 0, character_id: int = 0, replace: bool = False,
              maa_type: str = "", action: str = "", regiment_id: int = 0,
              vassal_id: int = 0, obligation: str = "", level: int = 0,
              faction_action: str = "", faction_type: str = "",
              scheme: str = "", scheme_id: int = 0,
              company_id: int = 0, contract_len: int = 0,
              doctrine: str = "", law_scope: str = "",
              faith: str = "", faith_id: int = 0,
              culture: str = "", culture_id: int = 0,
              title_key: str = "", title: str = "") -> dict[str, Any]:
    """Execute a game action: interaction, war, decision, event response, law change, lifestyle focus, council task, or building.

    Args:
        type: Action category: interaction, war, decision, event, law, lifestyle, council_task, building.
        focus: For type=lifestyle: focus key to set (e.g. "stewardship_domain_focus").
        task: For type=council_task: task key to assign (e.g. "task_increase_control").
            Some tasks need a de jure title target via title_id. Applies on a game
            tick — poll query(domain="council") to confirm the transition.
        interaction_type: For type=interaction: exact interaction name.
        decision_type: For type=decision: exact decision name.
        target_id: Target character for interactions and war.
        actor_id: Acting character (0 = controlled).
        title_id: Title for grant/revoke interactions. For type=create_title, the
            numeric de-jure title id to create.
        title_key: For type=create_title: the title KEY to create (e.g. "k_danelaw",
            "d_northumbria"). Alternative to numeric title_id — the bridge resolves
            the key. (title= is an accepted alias.)
        secondary_actor_id: For type=interaction: secondary actor.
        secondary_recipient_id: For type=interaction: secondary recipient.
        cb_index: For type=war: CB index (-1 = auto-select).
        cb_title_id: For type=war: title to claim.
        claimant_id: For type=war: claimant character.
        law_name: For type=law: target law name.
        option: For type=event: option index (0-based).
        event_id: For type=event: event ID string.
        instance_id: For type=event: event instance ID.
        method: For type=war: "ai" (default) routes through the AI war evaluator
            (target/CB are hints; the AI may decline). "steered" deterministically
            declares EXACTLY the chosen (target, casus_belli, cb_title_id,
            claimant_id) via WAR_CREATE_CORE direct — named war, no crash. Use
            "steered" to force a specific war the AI keeps declining. Requires
            casus_belli. NOTE: claim_cb needs a PRESSED claim or the engine culls
            the war.
        force_player_path: For type=war: bypass the AI war pipeline and call
            WAR_CREATE_CORE directly. Prefer method="steered" for the supported
            deterministic path. Requires casus_belli when true.
        casus_belli: For type=war + (method="steered" or force_player_path=true):
            CB script name (e.g. "claim_cb"). From discover(what='valid_cbs_strict').
        skip_pre_validate: For type=war: skip inline pre-validation checks.
        scheme: For type=scheme: scheme-type key to start (e.g. "murder",
            "fabricate_hook", "claim_throne"). actor_id = the schemer (must be AI),
            target_id = the target character. The engine's own scheme-start gates
            run inside the call (an invalid/dead/cooldown target is refused, not
            forced). Confirm + watch progress via query(domain="schemes",
            character_id=<schemer>). Run hostile schemes at PEACE (success drops 50%
            if at war with the target).
        scheme_id: For type=scheme + action="cancel": the scheme id to abandon (from
            query(domain="schemes", character_id=<owner>).schemes[].scheme_id). Cancels
            the OWNER's own scheme via the engine's RemoveActiveScheme. action defaults
            to "start" (the scheme=<type>, target_id=<victim> path); set action="cancel"
            + scheme_id to abandon. Returns {success, action:"cancel", scheme_id, removed}.
    """
    state = _get_state()
    err = await _ensure_connected(state)
    if err:
        return err

    from tools import resolve_act
    from action_selector import validate_act_params
    act_params = {
        "type": type, "interaction_type": interaction_type,
        "decision_type": decision_type, "target_id": target_id,
        "actor_id": actor_id, "title_id": title_id,
        "secondary_actor_id": secondary_actor_id,
        "secondary_recipient_id": secondary_recipient_id,
        "cb_index": cb_index, "cb_title_id": cb_title_id,
        "claimant_id": claimant_id, "law_name": law_name,
        "option": option, "event_id": event_id,
        "instance_id": instance_id,
        "force_player_path": force_player_path,
        "casus_belli": casus_belli,
        "method": method,
        "skip_pre_validate": skip_pre_validate,
        "focus": focus,
        "task": task,
        "target_title_id": title_id,
        "building": building,
        "location_id": location_id,
        # write-tool params (appoint / maa / contract / faction / hire / faith / title-law)
        "position_id": position_id, "character_id": character_id, "replace": replace,
        "maa_type": maa_type, "action": action, "regiment_id": regiment_id,
        "vassal_id": vassal_id, "obligation": obligation, "level": level,
        "faction_action": faction_action, "faction_type": faction_type,
        "scheme": scheme, "scheme_id": scheme_id,
        "company_id": company_id, "contract_len": contract_len,
        "doctrine": doctrine, "law_scope": law_scope,
        "faith": faith, "faith_id": faith_id,
        "culture": culture, "culture_id": culture_id,
        # type=create_title: title by KEY (e.g. "k_danelaw") OR numeric title_id.
        # tools.resolve_act reads title_key/title for the create_title path; the
        # bridge IPC handler resolves the key → title_id.
        "title_key": title_key, "title": title,
    }
    verdict = validate_act_params(act_params)
    if not verdict.get("valid"):
        return {
            "ok": False,
            "error": {
                "code": "rejected_pre_dispatch",
                "message": verdict.get("hint", ""),
                "reason": verdict.get("reason"),
                "rejected_fields": verdict.get("rejected_fields", []),
                "retry_with": verdict.get("retry_with", {}),
            },
            "tick": state.tick_id,
            "game_date": state.game_date,
        }
    try:
        resolved = resolve_act(act_params)
    except ValueError as e:
        return _error(state, "invalid_param", str(e))
    if resolved is None:
        return _error(state, "unknown_action_type",
                      f"Unknown action type: {type}")

    action, bridge_params = resolved
    try:
        result = await _action_bridge(state, action, bridge_params)
        return _envelope(state, result)
    except ConnectionError as e:
        return _error(state, "bridge_disconnected", str(e))


# ---------------------------------------------------------------------------
# Tool 5: steer
# ---------------------------------------------------------------------------

@mcp.tool()
async def steer(character_id: int = 0, personality: dict = None,
                subsystems: dict = None) -> dict[str, Any]:
    """Read or adjust AI steering controls: personality weights and subsystem toggles.
    No args = read current values. Partial updates supported.

    Args:
        character_id: Character to steer (0 = controlled).
        personality: Personality axes to set. Keys: honor, greed, rationality, energy, boldness, zeal, vengefulness, compassion, sociability, dominance. Values: -100 to +100.
        subsystems: Subsystem toggles. Keys: Decisions, DeclareWar, Marriages, etc. Values: true/false.
    """
    state = _get_state()
    err = await _ensure_connected(state)
    if err:
        return err

    data = {}

    # Read mode: no personality or subsystems specified
    if not personality and not subsystems:
        try:
            p = await _query_bridge(state, "get_ai_personality",
                                    {"character_id": character_id})
            data["personality"] = p
        except Exception as e:
            data["personality"] = {"error": str(e)}

        try:
            s = await _query_bridge(state, "get_character_subsystems",
                                    {"character_id": character_id})
            data["subsystems"] = s
        except Exception as e:
            data["subsystems"] = {"error": str(e)}

        return _envelope(state, data)

    # Write mode
    if personality:
        params = {"character_id": character_id}
        params.update(personality)
        try:
            result = await _action_bridge(state, "set_ai_personality", params)
            data["personality"] = result
        except Exception as e:
            data["personality"] = {"error": str(e)}

    if subsystems:
        params = {"character_id": character_id}
        params.update(subsystems)
        try:
            result = await _action_bridge(state, "set_character_subsystems", params)
            data["subsystems"] = result
        except Exception as e:
            data["subsystems"] = {"error": str(e)}

    return _envelope(state, data)


# ---------------------------------------------------------------------------
# Tool 6: intent
# ---------------------------------------------------------------------------

@mcp.tool()
async def intent(action: str, interaction_type: str = "",
                 actor_id: int = 0, target_id: int = 0,
                 max_age_ticks: int = 500, request_id: str = "",
                 max_count: int = 16) -> dict[str, Any]:
    """Manage the intent queue: queue interactions, list pending, read RESULTS,
    cancel, or clear all.

    A queued intent is a DEFERRED write — it executes on a later AI tick, not at
    queue time, so `action="queue"` returns only a request_id, NOT a success/fail
    verdict. To CONFIRM whether a queued intent actually took effect, poll
    `action="results"` (the deferred-write confirmation surface): it returns the
    completed-intent ring with a per-intent verdict you can match by request_id —
    `status` ∈ success | rejected | crashed (with `rejection_reason` when not
    success), plus `intent_id`, `action_name`, `tick_id`, `actor_id`, `target_id`.
    `action="list"` shows only what is still PENDING (not yet drained); a queued
    intent that has executed leaves `list` and appears under `results`.

    Args:
        action: Operation: queue, list, results, cancel, clear.
        interaction_type: For action=queue: interaction to schedule.
        actor_id: For action=queue: acting character (0 = controlled).
        target_id: For action=queue: target character (0 = AI chooses).
        max_age_ticks: For action=queue: expire after N ticks.
        request_id: For action=cancel: the request_id to cancel.
        max_count: For action=results: max completed intents to return (default 16).
    """
    state = _get_state()
    err = await _ensure_connected(state)
    if err:
        return err

    from tools import resolve_intent
    resolved = resolve_intent({
        "action": action, "interaction_type": interaction_type,
        "actor_id": actor_id, "target_id": target_id,
        "max_age_ticks": max_age_ticks, "request_id": request_id,
        "max_count": max_count,
    })
    if resolved is None:
        return _error(state, "invalid_param",
                      f"Unknown intent action: {action}. "
                      "Valid: queue, list, results, cancel, clear.")

    bridge_action, bridge_params = resolved
    try:
        # list / results are READS (object-graph queries that mutate nothing) — route
        # them through the query path, not _action_bridge (which logs to the action
        # log + fabricates sent_no_confirmation on timeout, both wrong for a read).
        if action in ("list", "results"):
            result = await _query_bridge(state, bridge_action, bridge_params)
        else:
            result = await _action_bridge(state, bridge_action, bridge_params)
        return _envelope(state, result)
    except (ConnectionError, TimeoutError) as e:
        return _error(state, "bridge_disconnected", str(e))


# ---------------------------------------------------------------------------
# Tool 7: game
# ---------------------------------------------------------------------------
# RETIRED (Phase-2 migration): FULL v2 parity — session(action=pause|unpause|speed|
# auto_events) covers all four. Deregistered from the MCP surface. session ALSO
# tightens the speed guard (4-5 refused while at war). Function kept for fallback.
# @mcp.tool()
async def game(action: str, speed: int = 3, enabled: bool = True) -> dict[str, Any]:
    """Control game flow: pause, unpause, set speed, or toggle auto-event responses.

    Args:
        action: Operation: pause, unpause, speed, auto_events.
        speed: For action=speed: game speed 1-5.
        enabled: For action=auto_events: true = while no session is open the bridge clears the event pause and picks no option.
    """
    state = _get_state()
    err = await _ensure_connected(state)
    if err:
        return err

    from tools import resolve_game
    resolved = resolve_game({
        "action": action, "speed": speed, "enabled": enabled,
    })
    if resolved is None:
        return _error(state, "invalid_param",
                      f"Unknown game action: {action}")

    bridge_action, bridge_params = resolved
    try:
        result = await _action_bridge(state, bridge_action, bridge_params)
        return _envelope(state, result)
    except (ConnectionError, TimeoutError) as e:
        return _error(state, "bridge_disconnected", str(e))


# ---------------------------------------------------------------------------
# Tool 8: control
# ---------------------------------------------------------------------------
# RETIRED (Phase-2 migration): FULL v2 parity — session(action=control|release|whoami)
# maps to set_controlled_character/release_character and adds whoami. Deregistered.
# @mcp.tool()
async def control(action: str, character_id: int = 0,
                  disable_native_ai: bool = False) -> dict[str, Any]:
    """Select which AI character the LLM steers, or release one back to native AI.

    Args:
        action: select or release.
        character_id: Character to select/release. For select: 0 reverts to human player.
        disable_native_ai: For action=select: fully disable native AI.
    """
    state = _get_state()
    err = await _ensure_connected(state)
    if err:
        return err

    from tools import resolve_control
    resolved = resolve_control({
        "action": action, "character_id": character_id,
        "disable_native_ai": disable_native_ai,
    })
    if resolved is None:
        return _error(state, "invalid_param",
                      f"Unknown control action: {action}")

    bridge_action, bridge_params = resolved
    try:
        result = await _action_bridge(state, bridge_action, bridge_params)
        return _envelope(state, result)
    except (ConnectionError, TimeoutError) as e:
        return _error(state, "bridge_disconnected", str(e))


# ---------------------------------------------------------------------------
# Tool 9: bridge
# ---------------------------------------------------------------------------
# RETIRED (Phase-2 migration): FULL v2 parity — session(action=health) composes
# get_bridge_state + get_offset_validation + who_holds_the_lock + agent_mode, and
# session(action=whoami) gives the roster. Deregistered from the MCP surface.
# @mcp.tool()
async def bridge() -> dict[str, Any]:
    """Check bridge connection health: session info, controlled characters,
    personality locks, intent queue depth, tick count."""
    state = _get_state()
    err = await _ensure_connected(state)
    if err:
        return err

    try:
        result = await _query_bridge(state, "get_bridge_state", {})
        return _envelope(state, result)
    except (ConnectionError, TimeoutError) as e:
        return _error(state, "bridge_disconnected", str(e))


# ---------------------------------------------------------------------------
# Tool 10: execute_console_command
# ---------------------------------------------------------------------------
# DISABLED 2026-06-14: the debug console is a CHEAT a human player cannot use
# (add_claim/add_prestige/give_title/etc.). For the "operate like a player"
# campaign we de-register it from the MCP surface so the agent CANNOT cheat —
# it must earn claims/titles/resources legitimately. To re-enable for debugging,
# restore the @mcp.tool() decorator below.
# @mcp.tool()
async def execute_console_command(command: str) -> dict[str, Any]:
    """Execute a CK3 debug console command. This bypasses interaction validation
    and is the ultimate escape hatch for game actions.

    Examples:
        AI.declare_war 30966
        add_trait brave 30966
        add_claim c_tuscany 30966
        add_piety 500
        add_realm_law partition_succession_law

    Args:
        command: Full console command string as typed in the game console.
    """
    state = _get_state()
    err = await _ensure_connected(state)
    if err:
        return err

    try:
        result = await _action_bridge(state, "execute_console_command",
                                      {"command": command}, timeout=10.0)
        return _envelope(state, result)
    except (ConnectionError, TimeoutError) as e:
        return _error(state, "bridge_disconnected", str(e))


# ---------------------------------------------------------------------------
# Tool 11: pass_turn
# ---------------------------------------------------------------------------

@mcp.tool()
def pass_turn(reason: str = "") -> dict[str, Any]:
    """Explicitly choose not to act this cycle. Provide a reason for the decision log.

    Args:
        reason: Brief explanation for inaction.
    """
    state = _get_state()
    state.action_log.append({
        "tick": state.tick_id,
        "action": {"type": "pass_turn", "reason": reason},
    })
    return _envelope(state, {
        "action": "pass_turn",
        "reason": reason or "maintaining stability",
    })


# ===========================================================================
# The redesigned 8-tool goal-shaped surface (SPEC docs/notes/mcp-redesign/SPEC.md).
#
# ADDITIVE (SPEC §6.2 Phase 1): registered ALONGSIDE the 11 legacy tools above —
# nothing breaks. Each is a thin wrapper over redesign.py composition + the
# BridgeGateway (pause-ownership §4, actor-explicitness §5). Every bridge action
# composed is an existing §3 router entry — see redesign.TOOL_BRIDGE_ACTIONS.
# ===========================================================================


@mcp.tool()
async def observe_v2(character_id: int = 0, depth: str = "brief") -> dict[str, Any]:
    """Your situation report. Returns de-jure county counts for the titles nearest and
    widest around your holdings (`de_jure`), your current `standing` (gold, debt, stress,
    strongest faction percentage, succession ETA, whether you are at war), and what
    changed since the last call (`delta`). `realm_size` is ABSENT because it counts
    titles rather than counties; `de_jure` counts counties and `self.counties_held`
    counts yours.

    Also folds in a concise `succession` block, the per-title partition map summarised:
    `scatters` (does the next succession split titles across more than one heir?),
    `eligible_heir_count`, and which heir takes which titles. Drill in with
    inspect(subject='succession_preview') or succession_plan.

    Args:
        character_id: ruler to reason about (0 = controlled primary; REQUIRED in multi-actor).
        depth: 'brief' (de_jure+standing+succession+posture+delta) or 'full' (+self vitals, war/council brief).
    """
    err = await _ensure_connected(_get_state())
    if err:
        return err
    import redesign
    return await redesign.observe_impl(_get_gateway(), character_id=character_id, depth=depth)


@mcp.tool()
async def plan_war(character_id: int = 0, goal_scoped: bool = True,
                   ally_free_only: bool = True, exclude_straddling: bool = True,
                   max_results: int = 8) -> dict[str, Any]:
    """"What is my single best next war?" Returns a RANKED list of targets that are
    ALREADY declarable, ally-free, and goal-advancing — each row carries a `declare_args`
    object you pass to declare_war VERBATIM. You never name a casus_belli string or hunt
    a claimant. Rows gate on the ENGINE-authoritative strict-CB inner declarability, not
    the over-optimistic outer filter (which falsely read as a structural wall for 17
    generations). Empty list ⇒ `empty_reason` names the single cause (e.g. all truced),
    which is NOT "structurally walled in".

    Args:
        character_id: aggressor (0 = controlled primary; REQUIRED in multi-actor).
        goal_scoped: True = only de-jure-goal-advancing targets. False = all declarable.
        ally_free_only: exclude targets that pull allies you'd fight.
        exclude_straddling: exclude wars expected to cross a death ETA.
        max_results: top-N by score.
    """
    err = await _ensure_connected(_get_state())
    if err:
        return err
    import redesign
    return await redesign.plan_war_impl(_get_gateway(), character_id=character_id,
                                        goal_scoped=goal_scoped, ally_free_only=ally_free_only,
                                        exclude_straddling=exclude_straddling, max_results=max_results)


@mcp.tool()
async def declare_war(character_id: int = 0, declare_args: dict = None,
                      target_id: int = 0, cb: str = "", cb_title_id: int = 0,
                      claimant_id: int = 0, confirm_straddle: bool = False) -> dict[str, Any]:
    """Declare ONE war. Pass a plan_war row's `declare_args` verbatim (or the fields
    explicitly). The only irreversible high-stakes act with its own verb so it cannot
    fire by accident. BLOCKS until the war is confirmed PERSISTED on the next tick (owns
    unpause-and-wait) — you never get sent_no_confirmation ambiguity nor a paused-too-fast
    cull. A `valid` CB does NOT guarantee a landed war; this re-checks pressed-claim +
    target-state up front. Evaporated war ⇒ verdict:rejected with the exact gate
    (e.g. target_truced). A straddles_succession war REQUIRES confirm_straddle=true.

    Args:
        character_id: aggressor (0 = controlled primary).
        declare_args: a plan_war row's declare_args dict (preferred path).
        target_id / cb / cb_title_id / claimant_id: escape-hatch fields if no declare_args.
        confirm_straddle: HARD GUARD — refuses a near-death-straddling war unless true.
    """
    err = await _ensure_connected(_get_state())
    if err:
        return err
    import redesign
    return await redesign.declare_war_impl(_get_gateway(), character_id=character_id,
                                           declare_args=declare_args, target_id=target_id,
                                           cb=cb, cb_title_id=cb_title_id, claimant_id=claimant_id,
                                           confirm_straddle=confirm_straddle)


@mcp.tool()
async def succession_plan(character_id: int = 0) -> dict[str, Any]:
    """"Will my realm pass through my heir's succession INTACT?" Returns one verdict:
    passes_intact:true, or splits:true with the cadet who breaks it and the single lever
    that actually WORKS (drive eligible-heir count toward 1). Do NOT read
    partition_preview.is_partition yourself — it reflects the CURRENT single holder, not
    the post-death split, and gave false reassurance that collapsed past runs. The
    wont_work list names the "fixes" the AI refuses, so you don't burn cycles on them.

    Args:
        character_id: the ruler whose death you plan around (0 = controlled primary).
    """
    err = await _ensure_connected(_get_state())
    if err:
        return err
    import redesign
    return await redesign.succession_plan_impl(_get_gateway(), character_id=character_id)


@mcp.tool()
async def set_phase(phase: str = None, character_id: int = 0,
                    overrides: dict = None) -> dict[str, Any]:
    """Assert your strategic posture by NAME instead of hand-tuning ten integers.
    phase=None reads the current genome. Nothing re-applies it after a succession:
    call it again for the heir.

    Each call writes, for the ruler: the 10 personality axes; DeclareWar/Decisions
    subsystem toggles, which are bookkeeping the engine does NOT enforce; and
    set_character_ai, re-enabling the native AI tick. phase='expansion' also arms
    the off-goal war veto (AI-picked wars vetoed, declare_war wars not). The veto is
    ONE process-wide flag: every controlled character of every session,
    character_id ignored, never disarmed by another phase, on until
    session(action='set_experimental', flag='off_goal_veto', enabled=false) or the
    game exits. result.offgoal_veto_armed is true only when the bridge confirms
    the write; see also offgoal_veto_scope.

    Args:
        phase: expansion|rebuild|succession_trough|stalled_siege|fragile_minority|
               consolidate, or None to read current.
        character_id: who to pin (0 = controlled primary; REQUIRED in multi-actor to assert).
        overrides: escape hatch — per-axis int overrides (discouraged; warns on CallAllies).
    """
    err = await _ensure_connected(_get_state())
    if err:
        return err
    import redesign
    return await redesign.set_phase_impl(_get_gateway(), phase=phase,
                                         character_id=character_id, overrides=overrides)


@mcp.tool()
async def do(intent: str = None, action_class: str = None, candidates: list = None,
             character_id: int = 0, args: dict = None) -> dict[str, Any]:
    """Perform one concrete game action that is NOT a war (wars use declare_war), OR
    enumerate what you can do. Two modes:
     • Enumerate — do(intent=None, action_class="council_tasks") returns available actions,
       each with the EXACT do_call to run it (copy the row; never guess a type string or
       English key) plus a blocked-why for each unavailable one. For decisions you MUST
       pass a bounded `candidates` list (unbounded engine eval FREEZES the game, #64).
     • Execute — do(intent="council_task", args={...}) runs ONE action. Every write returns
       verdict:applied (PERSISTED — re-read confirms) or a typed rejected. council_task /
       fabricate warn if the AI will REVERT them. appoint is an intent here, not a
       discoverable interaction.

    Args:
        intent: one of mint_title, take_decision, interact, convert_faith, convert_culture,
                set_doctrine, change_law, council_task, appoint, lifestyle, build, muster,
                men_at_arms, hire_merc, move_capital, set_obligation, respond_event,
                scheme, faction, scripted_effect, break_alliance, force_offensive_stance.
                None + action_class ⇒ enumerate mode. break_alliance dissolves a real alliance
                (args={"target_id": <ally>}) to stop FUTURE off-goal ally-drags — bridge
                default-OFF + observer-save gated (arm via session set_experimental
                break_alliance). force_offensive_stance is the per-war DRIVE
                (args={"war_id": <id>, "attacker_id": <atk>}; attacker_id defaults to
                character_id) that makes a FORCE-DECLARED war land an ATTACKER-OFFENSIVE stance
                so it actually sieges — bridge default-OFF + NEEDS-LIVE-VALIDATION (arm via
                session set_experimental force_offensive_stance).
        action_class: interactions|decisions|council_tasks|laws|fabricate_targets|
                      mercenaries|factions_to_join|buildings|imprison_targets|formable_titles|
                      men_at_arms (enumerate only). formable_titles lists the titles you could
                      create_title now, each with its de-jure county count and a ready
                      mint_title do_call (pass `candidates=[...]` to test a custom key set).
                      mercenaries: free companies, nearest first, each labelled with home
                      county and men, plus regiments; cost is null (the game prices a hire
                      per hirer; the hire refuses what you cannot afford). men_at_arms: MaA
                      types with recruit cost and fielded_regiments (types the realm already
                      fields are known to be available; the rest are decided at recruit);
                      30 rows, fielded first then cheapest, or pass candidates=[types].
                      laws: the other laws in each law group you have; whether a change is
                      allowed now is decided when change_law runs.
                      council_task takes args.seat optionally; a task belongs to ONE seat
                      (task_fabricate_claim is the court chaplain's), a seat that cannot take
                      it is refused, and every reply names the seat and councillor used.
        candidates: BOUNDED list for action_class="decisions" (#64 safety); for
                    "men_at_arms", the MaA type keys to look up.
        character_id: actor (0 = controlled primary; REQUIRED in multi-actor).
        args: intent-specific keys (see the SPEC §2 intent table).
    """
    err = await _ensure_connected(_get_state())
    if err:
        return err
    import redesign
    return await redesign.do_impl(_get_gateway(), intent=intent, action_class=action_class,
                                  candidates=candidates, character_id=character_id, args=args)


@mcp.tool()
async def inspect(subject: str, character_id: int = 0, target_id: int = 0,
                  title_id: int = 0, war_id: int = 0, title_key: str = "",
                  dynasty_id: int = 0, include: list = None,
                  max_results: int = 20) -> dict[str, Any]:
    """Deep, typed read of ONE subject (character, realm, council, succession,
    succession_preview, claims, wars, armies, factions, vassals, diplomacy, title, dynasty,
    laws, schemes, marriage, economy, research, county, opinion, opinion_breakdown,
    relations, hooks, title_hierarchy, title_succession, claimants, war_score, call_to_arms,
    development, incoming_schemes, lifestyle, heir_plan, guardian_candidates).
    `succession_preview` is the per-title
    PARTITION MAP — which heir gets which title under which succession law (partition vs
    single-heir) and whether the next succession SCATTERS titles across more than one
    heir. `heir_plan` lists each child and eligible heir with martial, traits, and the
    levers that exist for them (education focus / guardian / marriage) with honest
    status; education_focus/guardian/projected_adult_martial read NULL because no bridge
    read exposes them. `guardian_candidates` lists in-court guardian prospects for a
    child with their martial. Field names mean what they say.
    dynasty_id is null for landed rulers by design. borders/de_jure are KNOWN_BROKEN and
    return a warning, not data — reconstruct adjacency from plan_war targets.

    Args:
        subject: the subject to read (see list above).
        character_id / target_id / title_id / war_id / dynasty_id: per-subject ids.
        title_key: for the 'title' (de-jure) subject: a title key such as "k_england".
        include: subject-specific sub-sections.
        max_results: cap.
    """
    err = await _ensure_connected(_get_state())
    if err:
        return err
    import redesign
    return await redesign.inspect_impl(_get_gateway(), subject=subject, character_id=character_id,
                                       target_id=target_id, title_id=title_id, war_id=war_id,
                                       title_key=title_key, dynasty_id=dynasty_id,
                                       include=include, max_results=max_results)


@mcp.tool()
async def cbs(target_id: int, character_id: int = 0) -> dict[str, Any]:
    """"Show me EVERY casus belli I can use against THIS target, with tradeoffs." Returns
    the full curated CB list for the (actor, target) pair — each option's `scope`
    (county | duchy | kingdom | realm | faith_counties), what it `grants` on victory,
    why it is unmet (`requirements_unmet`), `tradeoffs`, and the dispatch-marshalling hint
    (`needs_cb_title_id` / `needs_claimant` / `dispatch_supported`). Use this to WEIGH
    options before declaring — e.g. tribal_subjugation (whole realm, once/lifetime) vs
    county_conquest (one county) vs duchy_conquest (a whole duchy). `valid_options` is the
    declarable subset; `best` is the widest-scope declarable one. Re-point a plan_war row's
    declare_args.cb at any option here. UNPAUSED engine read.

    Args:
        target_id: the defender character id (REQUIRED).
        character_id: aggressor (0 = controlled primary; REQUIRED in multi-actor).
    """
    err = await _ensure_connected(_get_state())
    if err:
        return err
    import redesign
    return await redesign.cbs_impl(_get_gateway(), target_id=target_id, character_id=character_id)


@mcp.tool()
async def session(action: str, character_id: int = 0, speed: int = 3,
                  enabled: bool = True, disable_native_ai: bool = False,
                  flag: str = "", seconds: int = 60) -> dict[str, Any]:
    """Game-flow, identity, and liveness control. You rarely call this — observe/plan_war/
    do own pause-state for you. Use it to: switch which ruler you control after a
    succession (control); confirm the controlled ruler is alive+landed (whoami — control
    can silently drift); check bridge liveness if calls time out (health — 'hung' = STOP
    looping); set speed/pause; or let time pass (wait). Speed is capped at 3; 4-5 is
    REFUSED while at war, otherwise clamped to 3. The human player can be a target but never an actor.

    wait: the way to let time pass between cycles. Unpauses if paused, blocks for
    `seconds` (1-110; longer waits are several calls), pauses again, and returns
    start_date/end_date. Do not use Bash `sleep` for this: Claude Code refuses sleeps
    of 30 s or more.

    With no ruler bound, whoami returns ok=false, blocked.gate='no_ruler_bound',
    controlled=null, and blocked.measured.unbound_reason says why ('heir_is_player':
    the human inherited; '': no reason recorded, as after a reconnect). So do
    observe_v2, plan_war, declare_war, succession_plan, set_phase, do, inspect and
    cbs when character_id is omitted. observe, query, discover, steer and intent are
    not checked: they return ok=true with the bridge's empty or declined reply.
    'actor_unresolved' means the read failed, not that nothing is bound. A seat whose
    ruler became the human's character gets 'cannot_act_as_player'. control needs
    character_id: 0 or omitted is refused (missing_character_id), never an unbind.

    Several rulers in one session: add claims one more ruler and keeps the ones held
    (the first stays the primary, the one a call without character_id acts as);
    control SWITCHES the primary to another ruler. whoami's my_claims lists every
    ruler held. With several, pass character_id on EVERY call. release frees one;
    the last one is freed when the session ends. CK3_AGENT_CHARACTER_ID pre-binds
    one id or a comma-separated list ("13902,12145"); the first is the primary.

    experimental_flags lists the EXPERIMENTAL-unvalidated engine flags, all OFF at
    boot; set_experimental(flag=<name>, enabled=bool) arms or disarms ONE
    (off_goal_veto / break_alliance / force_offensive_stance). set_phase('expansion')
    also arms off_goal_veto, which is process-wide (every controlled character of
    every session) until disarmed or the game exits. The bridge has no readback:
    each flag's `live` is null and `last_written_here` is this server's last
    confirmed write (another seat's write is not seen; null does not mean off).

    Args:
        action: control|add|release|whoami|speed|pause|unpause|wait|health|auto_events|
            experimental_flags|set_experimental.
        character_id: for control and add (required, > 0) and release.
        speed: 1-3 (4-5 refused while at war).
        enabled: for auto_events AND set_experimental (arm/disarm the flag). The
            bridge auto-handles events only while NO client socket is open, so never
            while you are connected; it then clears the event pause, unpauses a
            paused game, and picks no option.
        disable_native_ai: for control and add.
        flag: for set_experimental — off_goal_veto|break_alliance|force_offensive_stance.
        seconds: for wait — how long to let the clock run, 1-110 (default 60).
    """
    err = await _ensure_connected(_get_state())
    if err:
        return err
    import redesign
    return await redesign.session_impl(_get_gateway(), action=action, character_id=character_id,
                                       speed=speed, enabled=enabled, disable_native_ai=disable_native_ai,
                                       flag=flag, seconds=seconds)


# ===========================================================================
# ck-6kvq.12 (R4) — THE SHARED BOARD, ARM-GATED (decision G5, option A2)
# ===========================================================================
# These two tools are REGISTERED ONLY IN ARM B. In arms A, C and E0 — and in
# free play — the decorator is never applied, `list_tools` does not mention a
# board, and the system prompt has nothing to say about one. That is what "no
# channel" means in ordinary language, and it is the manipulation itself: the
# tool-surface delta between A and B is exactly these two names, named as such
# in the pre-registration rather than hidden inside the held-constant list.
#
# The bridge enforces the same gate independently (board.cpp's
# `board_init_from_env` + the arm gate in dispatch_action /
# is_registered_action), so even a hand-rolled client calling `board_post` in
# arm A gets `unknown_action`. This half exists so the agent never SEES a tool
# it would then be refused — surface and wire agree because both read the same
# episode config.


async def board_post(text: str) -> dict[str, Any]:
    """Post one message to the shared board every English lord can read.

    APPEND-ONLY: there is no edit, no delete and no reply threading. Your post
    is stamped by the bridge with who you are, which character you speak for,
    the in-game date and the flush it executed in — you cannot set any of those
    and you do not need to. A post longer than 2,048 bytes (about 300 words) is
    refused, not shortened, and there is a per-flush limit, so say the thing that
    changes someone else's decision rather than narrating.

    Args:
        text: what you want the other lords to read.
    """
    err = await _ensure_connected(_get_state())
    if err:
        return err
    # ck-2r46. `BridgeGateway` HAS NO `call`. Its public surface is
    # read / read_unpaused / write / read_inline / read_many /
    # confirmed_write, with `_call` private — and this line said `.call`, so
    # every board post in the deploy-40 arm-B smoke failed with
    # "'BridgeGateway' object has no attribute 'call'". The board tools had
    # never been registered in a live seat before ck-2r46's arm fix, so this
    # path had never executed. Same shape as the seal-time `self.client`
    # fault: a method named on an object that does not have it, on a line no
    # row drove.
    #
    # WRITE, NOT READ, and the bridge is the authority for why. snapshot.cpp
    # lists board_post as bridge-internal, so it is served INLINE and never
    # handed to the game-thread drain — but its own comment records that
    # `board_post` still classifies as TICK_WRITE in classify_polarity(),
    # because a post "is buffered and executed in a flush like any other
    # write (§3.2), and carries the same flush_id as the actions around it".
    # A post is a mutation that goes through the flush, so it takes the write
    # path and the gateway owns the polarity, exactly as this class's contract
    # says the tool impls must let it.
    return await _get_gateway().write("board_post", {"text": text})


async def board_read(since_seq: int = 0, limit: int = 0) -> dict[str, Any]:
    """Read the shared board. Returns every post with seq >= since_seq.

    Pass back the `next_seq` you were last given to see only what is new; pass
    0 to read the board from the beginning. Each post carries its author's
    session, the character they speak for, the in-game date, and the flush it
    landed in, so "who said what, when" is not a matter of trust.

    Args:
        since_seq: resume cursor; 0 reads from the start.
        limit: max posts to return (0 = the server's page size).
    """
    err = await _ensure_connected(_get_state())
    if err:
        return err
    # ck-2r46, as above: `.call` does not exist. A read takes `read()`, which
    # delegates to the same `_call` with read polarity and lets the gateway
    # own the pause matrix.
    return await _get_gateway().read(
        "board_read", {"since_seq": int(since_seq), "limit": int(limit)})


if _board_enabled():
    mcp.add_tool(board_post)
    mcp.add_tool(board_read)


def _write_tool_surface_record() -> None:
    """ck-jpej. THE SERVER IS THE ONLY AUTHORITY ON WHAT IT REGISTERED.

    ck-3ron graded the arm's tool surface from the CLI's `init` event, and in
    registered-d45 slot 2 that event listed 15 tools where the seat had 17 —
    the two absent being `board_post` and `board_read`, which are registered
    LAST, immediately above. All six seats were refused `arm_tools_absent`, and
    all six then used both tools: 60 board posts from 6 sessions. A false
    refusal of a functioning treatment costs a slot and leaves the seats running.

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
            "board_enabled": bool(_board_enabled()),
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
                bs = bs.get("data") if isinstance(bs.get("data"), dict) else bs
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
