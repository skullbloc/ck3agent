"""
CK3 Agent MCP API — the redesigned 8-tool goal-shaped surface.

Implements docs/notes/mcp-redesign/SPEC.md. This module is a CONSUMER layer: it
composes only bridge action names the router already dispatches (inv_bridge-map.md
§3). It invents NO bridge capability. Where the bridge does not expose a datum, the
tool degrades to a typed honest code (the §7 O-list), never a guess.

The 8 tools (SPEC §2):
  observe        — goal scalar + kill_zone + delta (the once-per-cycle situation report)
  plan_war       — ranked, declarable, ally-free, goal-advancing war targets
  declare_war    — declare ONE war (the only irreversible high-stakes verb)
  succession_plan— passes_intact verdict + the lever that actually works + the per-title
                   partition map (which heir gets which title under which law)
  set_phase      — assert a named strategic posture (10-axis genome + toggles)
  do             — one non-war write OR enumerate-what-can-I-do (with handed-back do_call)
  inspect        — deep typed read of ONE subject
  session        — game flow + identity + liveness

Design notes:
  - This module holds NO bridge I/O. mcp_server.py owns the socket; it passes a
    `BridgeGateway` object that exposes `read(action, params)` /
    `write(action, params)` coroutines plus read_many and confirmed_write. No
    gateway member changes the pause state or the speed. Each tool here builds the
    bridge calls and shapes the result. This keeps the contract (which bridge actions
    are reachable) inspectable in one file.
  - query, discover, steer and pass_turn live in mcp_server.py and return this
    module's envelope too: every tool replies in one shape.
"""

from __future__ import annotations

import asyncio
import re
from typing import Any, Optional

# Reuse the existing resolvers — the redesign tools delegate dispatch to them so the
# agent<->bridge contract (inv_bridge-map.md) stays single-sourced.
import tools as _tools
import factions as _factions
#: ck-ba35. THE pre-dispatch validator, not a second copy of it: the
#: legacy `act` tool in mcp_server.py calls this same function, so the two
#: surfaces cannot disagree about which field a verb requires.
from action_selector import validate_act_params as _validate_act_params
from personality_axes import AXES, AXIS_MIN, AXIS_MAX, personality_refusal  # noqa: F401  (steer and set_phase validate with these)


# ===========================================================================
# §1.4 the gate enum + §1.2 why_blocked model
# ===========================================================================

# Closed gate set (SPEC §1.4). Each maps a documented bridge refusal reason to a
# stable machine enum carrying class/severity/retryable. A bridge refusal this table
# does not map becomes gate:"unmapped_refusal" with the bridge's own code in
# `raw_reason` (the unmapped-refusal rule): visibly unforeseen, never bucketed.
GATE_TABLE: dict[str, dict[str, Any]] = {
    "de_jure_control":           {"class": "territory",      "severity": "blocked", "retryable": True},
    "insufficient_gold":         {"class": "resource",       "severity": "blocked", "retryable": True},
    "insufficient_prestige":     {"class": "resource",       "severity": "blocked", "retryable": True},
    "insufficient_piety":        {"class": "resource",       "severity": "blocked", "retryable": True},
    "missing_innovation":        {"class": "resource",       "severity": "invalid", "retryable": False},
    "missing_prerequisite_law":  {"class": "resource",       "severity": "blocked", "retryable": True},
    "vassal_cannot_change_realm_law": {"class": "resource",  "severity": "invalid", "retryable": False},
    "cooldown_active":           {"class": "timing",         "severity": "transient", "retryable": True},
    "claim_not_pressed":         {"class": "target_state",   "severity": "blocked", "retryable": True},
    "target_allied":             {"class": "target_state",   "severity": "blocked", "retryable": True},
    "target_truced":             {"class": "target_state",   "severity": "blocked", "retryable": True},
    "target_imprisoned":         {"class": "target_state",   "severity": "transient", "retryable": True},
    "target_not_independent":    {"class": "target_state",   "severity": "blocked", "retryable": True},
    "target_not_assignable":     {"class": "target_state",   "severity": "transient", "retryable": True},
    "not_at_peace":              {"class": "timing",         "severity": "blocked", "retryable": True},
    # create_title engine gate (#113): the bridge already de-jure-pre-checks, so an
    # engine-gate refusal here is almost always the gold/peace gate (or not-extant). Map
    # to a loud distinct gate carrying the raw reason rather than silently bucketing.
    "create_title_engine_gate":  {"class": "resource",       "severity": "blocked", "retryable": True},
    "succession_straddle":       {"class": "timing",         "severity": "blocked", "retryable": True},
    # Lane l4: the disinherit kin guard (KIN_GUARDED_INTERACTIONS): the target is not
    # the actor's own child, grandchild or great-grandchild.
    "target_not_own_descendant": {"class": "target_state",   "severity": "invalid", "retryable": False},
    # Lane l4: the engine's own validity check said no (set_title_law: the title-law
    # window's IsValid); `summary` carries the engine's text. A re-plan, not a retry.
    "engine_refused":            {"class": "target_state",   "severity": "invalid", "retryable": False},
    "custom_scope_bridge_cannot_fire": {"class": "steer_vs_control", "severity": "invalid", "retryable": False, "is_known_limitation": True},
    "regency_blocks_action":     {"class": "steer_vs_control", "severity": "blocked", "retryable": True},
    "gate_introspection_unavailable": {"class": "bridge",    "severity": "blocked", "retryable": True, "is_known_limitation": True},
    # ck-r3fs. The bridge's own eligibility culls (action_handlers.cpp:
    # target_rejected, capital_validate_failed, councillor_dead) share one
    # mechanic: the GAME rejected the target, not the arguments. Declared here
    # so the resolve in why_blocked yields a complete envelope — a gate NAME
    # resolved while class/severity/retryable fall back to the `unknown` row is
    # a half-resolved refusal, which is worse than an honest `unknown`.
    "target_ineligible":         {"class": "target_state",   "severity": "invalid", "retryable": False},
    "experimental_write_disabled": {"class": "bridge",       "severity": "invalid", "retryable": False, "is_known_limitation": True},
    # lane-scheme 2026-10-18: the scheme_action (do(intent="scheme")) refusals.
    # Before these, every one of them reached the seat as gate "unknown", and the
    # timeout ones as `drain_timeout` through the substring pass ("timeout").
    #   scheme_start_refused    the ENGINE's own start checks said no: CanStartScheme
    #                           (the type's allow/valid triggers, an owned scheme on the
    #                           target, the cooldown; its reasons ride in
    #                           measured.engine_reasons) or CreateActiveScheme's null
    #                           sentinel. A re-plan, not a retry.
    #   scheme_type_unsupported every engine start of this type seen this session
    #                           carried a task contract or artifact the bridge cannot
    #                           supply.
    #   scheme_bridge_fault     the bridge's own call faulted or could not be prepared
    #                           (crash-net fired, null objects unreadable). Contained,
    #                           and not something a retry fixes.
    "scheme_start_refused":      {"class": "target_state",   "severity": "invalid", "retryable": False},
    "scheme_type_unsupported":   {"class": "bridge",         "severity": "invalid", "retryable": False, "is_known_limitation": True},
    "scheme_bridge_fault":       {"class": "bridge",         "severity": "invalid", "retryable": False, "is_known_limitation": True},
    "cannot_act_as_player":      {"class": "bridge",         "severity": "invalid", "retryable": False},
    # ck-switch / Ruling Three: the control refusals. Without these eight,
    # why_blocked() falls to GATE_TABLE["unknown"] — severity "blocked",
    # retryable TRUE — so a seat refused because another seat HOLDS the
    # character would be told to try again forever. The raw spelling is carried
    # through as `raw_reason` in every case, so nothing downstream has to infer
    # the code from the prose.
    "no_session":                {"class": "bridge",         "severity": "transient", "retryable": True},
    "invalid_character":         {"class": "target_state",   "severity": "invalid", "retryable": False},
    "observe_mode":              {"class": "bridge",         "severity": "invalid", "retryable": False, "is_known_limitation": True},
    "owned_by_other":            {"class": "bridge",         "severity": "invalid", "retryable": False},
    # A slot frees when some seat releases, so this one IS retryable — it is the
    # only member of the nine that a later attempt can legitimately win.
    "watchlist_full":            {"class": "resource",       "severity": "blocked", "retryable": True},
    "character_not_found":       {"class": "target_state",   "severity": "invalid", "retryable": False},
    "character_dead":            {"class": "target_state",   "severity": "invalid", "retryable": False},
    # The registry contradicting itself (§7.3g). Not the agent's to retry.
    "ownership_divergence":      {"class": "bridge",         "severity": "invalid", "retryable": False},
    # A spelling the code returned and this list does not name. NEVER bucketed
    # into one of the nine: an unforeseen refusal reads as unforeseen.
    "refusal_reason_unknown":    {"class": "bridge",         "severity": "invalid", "retryable": False},
    # ck-switch: release_character's OWN vocabulary — four reasons, and only
    # `observe_mode` is shared with control's nine. A release is not a control
    # call with the sign flipped and it does not refuse for the same causes.
    "missing_character_id":      {"class": "bridge",         "severity": "invalid", "retryable": False},
    "not_in_roster":             {"class": "bridge",         "severity": "invalid", "retryable": False},
    "not_owned_by_session":      {"class": "bridge",         "severity": "invalid", "retryable": False},
    # ck-cwal, and the row above it is why it is here: I added this reason to
    # RELEASE_REFUSAL_REASONS with a summary and wrote at that site that "the
    # gate already degrades correctly for an unknown code". It does not. An
    # unmapped code falls to GATE_TABLE["unknown"] — severity "blocked",
    # retryable TRUE — and this refusal is PERMANENT, so the agent would have
    # been told to retry, forever, the one release the bridge will never grant.
    # `is_known_limitation` matches `observe_mode`, the sibling that is also a
    # standing rule about what a seat may do rather than a state that passes.
    "last_claim_of_control_session": {"class": "bridge",     "severity": "invalid", "retryable": False, "is_known_limitation": True},
    # lane-multi: session(action="add") against a bridge whose reply lacks
    # `added`: it predates the add path and treated the call as a SWITCH.
    "add_unsupported_by_bridge": {"class": "bridge",         "severity": "invalid", "retryable": False, "is_known_limitation": True},
    "actor_required":            {"class": "bridge",         "severity": "invalid", "retryable": False},
    # rc2 B6: character_id=0 and the bridge resolved NO character, or this
    # seat's own row holds no primary. Not retryable as sent: bind first.
    "no_ruler_bound":            {"class": "bridge",         "severity": "invalid", "retryable": False},
    # rc2 B6: character_id=0 and no usable reply (timeout, fault, error). We
    # looked and could not see, which is not "nothing is bound".
    "actor_unresolved":          {"class": "bridge",         "severity": "transient", "retryable": True},
    "unbounded_eval_refused":    {"class": "bridge",         "severity": "invalid", "retryable": False, "is_known_limitation": True},
    "bridge_hung":               {"class": "bridge",         "severity": "frozen",  "retryable": True},
    "drain_timeout":             {"class": "bridge",         "severity": "frozen",  "retryable": True},
    # CB-validity single-flight slot collision (engine AI eval vs our query). TRANSIENT,
    # retryable — NOT a verdict on the target. Distinct from drain_timeout (frozen) so
    # the agent re-plans instead of HOLDING (the #95 hold-on-collision pathology).
    # ck-zgh4 deploy-51 item (6): the clause the retired skill carried, kept
    # because the surface never said it. VERIFIED against the loop as it is:
    # `cb_query_acquire_slot` (ai_eval_hook.cpp) polls kAttempts=3 x
    # kBusyWaitMs=500 (~1.5s, inside the >=2000ms strict-query timeout),
    # RECLAIMS a state-1 slot whose submit is stale and a state-4 slot whose
    # service start is older than the staleness window, and returns busy "only
    # when the slot stayed genuinely busy across the whole budget". The wrong
    # reading is expensive and named in that file: an agent treating it as a
    # degraded subsystem HOLDS and misses expansion (the #95 class).
    "cb_query_busy":             {"class": "timing",         "severity": "transient", "retryable": True,
                                  "note": "the bridge already retried this internally "
                                          "(3 attempts over ~1.5s, reclaiming a wedged slot); "
                                          "retrying is correct, waiting is not"},
    # ck-zgh4 THE SCOPE LATCH. NOT transient and NOT retryable, unlike every
    # other bridge-class gate here. The bridge saw a RandomScopeManager slot it
    # could not reason about, stopped touching it for the rest of the PROCESS,
    # and nothing in the episode clears that — so "retry" would cost the seat a
    # cycle every time it believed it. The note says how long, because a seat
    # that reads "unavailable" without "for the rest of this episode" will
    # re-plan around a wait that never ends (the #95 hold-and-miss class).
    "rng_scope_latched":         {"class": "bridge",         "severity": "invalid",  "retryable": False,
                                  "is_known_limitation": True,
                                  "note": "unavailable for the REST OF THIS EPISODE: the bridge "
                                          "stopped using the engine's RNG scope after a safety "
                                          "check failed (a restore was refused). Do NOT retry and "
                                          "do NOT wait for it — plan with the verbs you still "
                                          "have. Steered war still works; its validity pre-check "
                                          "is skipped and the reply says so"},
    # The engine drained the slot and returned 0 usable CBs for this target — a REAL
    # "no casus belli here" verdict (truced/allied/ineligible), not a tooling failure.
    # The agent should pick a DIFFERENT target, not retry this one.
    "no_valid_targets":          {"class": "target_state",   "severity": "blocked",  "retryable": False},
    # A steered-war intent that dispatched and is unconfirmed as persisted. The
    # intent drains on the session thread's next pass, after the reply — this is
    # NOT a hard block. severity:"pending", retryable:False (VERIFY, do not re-declare).
    "confirmation_pending":      {"class": "timing",         "severity": "pending", "retryable": False, "is_known_limitation": True},
    # ── ck-3mz0: THE 37 CAUSES THE CENSUS FOUND FALLING THROUGH ──────────
    #
    # Four copied bundles (d39 slot01/slot02, arm-B smokes d41/d42) carried 390
    # blocked results with gate "unknown" and 37 distinct causes between them —
    # every one telling the agent RETRY, because the fallback said so. The
    # comments on `owned_by_other` and `last_claim_of_control_session` above are
    # the same defect found twice before, one refusal at a time. These are the
    # rest of it, typed BY KIND, and the kind is what decides `retryable`:
    #
    #   a rule of the GAME     the refusal stands; can a later attempt win?
    #   a missing ARGUMENT     never won by trying again — the call is malformed
    #   a name that NOTHING    never won by trying again — pick another name
    #     matches
    #   a state of the BRIDGE  depends, and each says which below
    #
    # TYPING IS NOT ALLOWING. `ai_potential` and `cannot afford` are the engine
    # refusing on its own rules, and they still refuse; what changes is that the
    # agent is told which kind of wall it is, so "wait until richer" and "this
    # will never work" stop reading identically.
    "decision_unavailable":      {"class": "target_state",   "severity": "invalid", "retryable": False},
    "ai_potential_not_met":      {"class": "target_state",   "severity": "invalid", "retryable": False},
    # Gold accrues. This is the one kind in the group a later attempt CAN win,
    # so it is the one that keeps retryable True.
    "cannot_afford":             {"class": "resource",       "severity": "blocked", "retryable": True},
    "already_in_that_state":     {"class": "target_state",   "severity": "invalid", "retryable": False},
    "no_such_contract":          {"class": "target_state",   "severity": "invalid", "retryable": False},
    # The call named something the game has no such thing as. A different name
    # may work; THIS one never will.
    "name_not_found":            {"class": "target_state",   "severity": "invalid", "retryable": False},
    # ck-ba35's family. Typed here too, because a call can still reach the
    # bridge malformed even after the client validates up front — and a refusal
    # for a missing field is never won by sending the same call again.
    "missing_required_argument": {"class": "bridge",         "severity": "invalid", "retryable": False},
    # L18 ruling (a): the call named an argument the tool's schema does not declare
    # (mcp_server._StrictArgumentsFastMCP); refused before the tool ran, never dropped.
    "unknown_argument":          {"class": "bridge",         "severity": "invalid", "retryable": False},
    # A safety refusal, not a verdict on the move: the character's AI controller
    # pointer chain is null and AI_DECISION_EVALUATION would read
    # controller+0x18 unchecked (action_handlers.cpp). The chain can bind later,
    # but not because this call was repeated — so NOT retryable, and flagged as
    # the standing limitation it is.
    "ai_controller_unbound":     {"class": "bridge",         "severity": "invalid", "retryable": False, "is_known_limitation": True},
    # The seat does not hold the actor it is acting as (session.cpp). Claim it
    # first; repeating the call cannot.
    "actor_not_claimed":         {"class": "bridge",         "severity": "invalid", "retryable": False},
    # ck-ycim: the agent named an intent or an action_class the surface does not
    # have. A different name may work; this one is not in the vocabulary.
    "unknown_verb":              {"class": "bridge",         "severity": "invalid", "retryable": False},
    # The bridge answered that it had not run the evaluation at all.
    # ck-zgh4 deploy-51 item (6): a seat reading this as "no" abandons an action
    # the engine never judged.
    "not_evaluated":             {"class": "bridge",         "severity": "blocked", "retryable": True,
                                  "note": "no verdict was reached; this is not a refusal"},
    # ck-ie6o: the flush loop owns the clock in pipeline mode and excludes every
    # session from it — "by G1's own design" (flush_scheduler.h). A seat's
    # set_speed/set_pause is refused for the whole episode, so this is a
    # standing rule and not a state that passes.
    "clock_owned_by_pipeline":   {"class": "pause",          "severity": "invalid", "retryable": False, "is_known_limitation": True},
    # ── THE FALLBACK, AND ck-3mz0 CHANGED ITS RETRYABILITY ────────────────
    # It was `retryable: True`. An unforeseen refusal is exactly the one nobody
    # has established can be won by trying again, and telling the agent to retry
    # it is how ck-switch, ck-cwal and then 390 results in four bundles all went
    # wrong in the same direction. An unmapped cause is now NOT retryable: the
    # agent re-plans instead of looping, and the cause still reads as UNKNOWN so
    # it is visible as unforeseen rather than bucketed into a neighbour.
    "unmapped_refusal":          {"class": "bridge",         "severity": "blocked", "retryable": False},
    # ── cleanup 2026.11 (F2): every refusal this server builds itself has a name.
    # Before these, each reached the seat as gate "unknown" with the cause in prose.
    "invalid_argument":          {"class": "bridge",         "severity": "invalid", "retryable": False},
    "unknown_subject":           {"class": "bridge",         "severity": "invalid", "retryable": False},
    "unknown_domain":            {"class": "bridge",         "severity": "invalid", "retryable": False},
    # ── Study 0 (channel lane), pre-registration v3 section 5.2: the channel's
    # three ledger verdicts as the gate a refused msg_send carries, and the one
    # tool-call refusal that precedes any ledger (no episode id to namespace it).
    "topology_denied":           {"class": "channel",        "severity": "invalid", "retryable": False},
    "quota_exceeded":            {"class": "channel",        "severity": "transient", "retryable": True},
    "text_too_long":             {"class": "channel",        "severity": "invalid", "retryable": True},
    "channel_unavailable":       {"class": "channel",        "severity": "blocked", "retryable": True},
    "bridge_disconnected":       {"class": "bridge",         "severity": "frozen",  "retryable": True},
    # L14 int5: the bridge's OWN fault and timeout replies, which carry no
    # `success`: {"error":"memory_access_fault"} (MEMORY_ACCESS_FAULT_REPLY,
    # bridge/include/bridge.h: every answer_memory_access_fault, and the SIGSEGV
    # recovery in ipc_server.cpp serve_on_game_thread) and
    # {"error":"game_thread_timeout",...} (the same function when the drain did
    # not finish the job in time). Before this row the fault was unmapped_refusal
    # (refusal_reason_unknown on the session guard) and the timeout fell through
    # the substring pass to drain_timeout, which says retry. Neither is a retry:
    # the SIGSEGV recovery can fire after the handler touched the game, and a
    # job already RUNNING at the deadline still finishes (run_on_game_thread
    # abandons only a QUEUED one), so the effect may have landed. Read back first.
    "bridge_fault":              {"class": "bridge",         "severity": "invalid", "retryable": False},
    "read_timeout":              {"class": "bridge",         "severity": "transient", "retryable": True},
    # A write was sent and the bridge did not confirm it. Not retryable as sent:
    # read the state back first, or the write may be applied twice.
    "write_unconfirmed":         {"class": "timing",         "severity": "pending", "retryable": False},
    "speed_refused_at_war":      {"class": "timing",         "severity": "blocked", "retryable": True},
    "war_cap_reached":           {"class": "bridge",         "severity": "blocked", "retryable": True},
    # L18-B item 9: the same call prunes the same rows (a deterministic sort), so a
    # retry is not what surfaces them; fix.max_results is.
    "low_priority_pruned":       {"class": "timing",         "severity": "transient", "retryable": False},
    "faction_power_tripwire":    {"class": "target_state",   "severity": "blocked", "retryable": True},
    "stress_break_risk":         {"class": "target_state",   "severity": "blocked", "retryable": True},
    "stress_climbing":           {"class": "target_state",   "severity": "transient", "retryable": True},
    "game_did_not_unpause":      {"class": "pause",          "severity": "transient", "retryable": True},
    # v3 §7.2 (lane s0-seats): session(action="wait", game_days=N). An ask over the
    # registered cadence is refused as sent, never clamped; a cadence env that
    # does not parse is an instrument fault; a loop that stopped short says how far.
    "cycle_game_days_over_cadence": {"class": "timing",      "severity": "invalid", "retryable": False},
    "cycle_cadence_unreadable":  {"class": "bridge",         "severity": "invalid", "retryable": False},
    "cycle_not_reached":         {"class": "pause",          "severity": "transient", "retryable": True},
    # v3 §7.2 (Study 0 round 2): a wait asked at or past the registered horizon
    # (CK3_EPISODE_HORIZON_END) is refused; the harness ends the seat.
    "horizon_reached":           {"class": "timing",         "severity": "blocked", "retryable": False},
    "episode_horizon_unreadable": {"class": "bridge",        "severity": "invalid", "retryable": False},
    # lane l5 (F8): the war intents (make_peace, call_ally, join_war) and the
    # game's two refusals of a directly sent interaction (ai_eval_hook.cpp
    # execute_tls_pipeline_dispatch): CTX_VALIDATE said no, or the recipient's
    # own acceptance check said no.
    "interaction_invalid":       {"class": "target_state",   "severity": "invalid", "retryable": False},
    "recipient_refuses":         {"class": "target_state",   "severity": "blocked", "retryable": True},
    "war_not_found":             {"class": "target_state",   "severity": "invalid", "retryable": False},
    "not_war_leader":            {"class": "target_state",   "severity": "invalid", "retryable": False},
    # F16 (cleanup 2026.11): faction management. faction_refused is the GAME's own
    # faction rules saying no (can_create / can_join / can_leave / can press the
    # demand); `summary` carries the game's sentence. Retryable: discontent, power,
    # opinion and cooldowns move, so a later attempt can pass.
    "faction_refused":           {"class": "target_state",   "severity": "blocked", "retryable": True},
    # The bridge's own pre-check, made before the game is asked (factions.cpp:
    # not the faction's leader, not a member, no liege); the summary says which in
    # plain words. Not the game's rule, so never faction_refused.
    "faction_precondition":      {"class": "target_state",   "severity": "invalid", "retryable": False},
    "faction_bridge_fault":      {"class": "bridge",         "severity": "invalid", "retryable": False, "is_known_limitation": True},
    "faction_write_not_applied": {"class": "bridge",         "severity": "invalid", "retryable": False},
    "faction_query_busy":        {"class": "bridge",         "severity": "transient", "retryable": True},
    # L14 S3 (surface-eval finding 14): a mint whose price and engine verdict could
    # not both be read (query_create_title_cost failed, or the price functions are
    # unmapped and the engine's IsValid unread). Refused rather than sent: an
    # unpriced mint is how k_ireland was created at 17.85 gold.
    "mint_cost_unverified":      {"class": "bridge",         "severity": "blocked", "retryable": False, "is_known_limitation": True},
}

# Maps documented raw bridge refusal strings → a gate enum. Maintained next to the
# dispatch resolver (the unmapped-refusal rule: anything not here ⇒ gate:"unmapped_refusal").
RAW_REASON_TO_GATE: dict[str, str] = {
    "cannot_act_as_player": "cannot_act_as_player",
    "experimental_write_disabled": "experimental_write_disabled",
    # A dead actor is a dead actor: the same gate as a dead target (retry
    # never). The bridge's actor refusals (action_dispatch_internal.h
    # actor_refusal) are actor_not_found (character_not_found, below) and
    # actor_not_alive (character_dead, below); the one word no_living_actor that
    # said both is gone from the bridge, and so is actor_dead, which nothing
    # emits (test_dead_actor_gate.py's census).
    "claim_not_pressed": "claim_not_pressed",
    "target_not_independent": "target_not_independent",
    # query_conquest_targets' block_reason (query_handlers.cpp) and the strict CB
    # read's cull_reason: the county's holder is someone's vassal (L14 finding 2:
    # plan_war listed it as unmapped_refusal).
    "holder_not_independent": "target_not_independent",
    "holder_allied": "target_allied",
    "holder_truced": "target_truced",
    "target_imprisoned": "target_imprisoned",
    "target_in_alliance": "target_allied",
    "target_in_truce": "target_truced",
    "already_at_war_opposing": "target_truced",
    "at_war": "not_at_peace",
    "cooldown_active": "cooldown_active",
    "insufficient_prestige": "insufficient_prestige",
    "insufficient_piety": "insufficient_piety",
    "insufficient_gold": "insufficient_gold",
    "missing_prerequisite": "missing_prerequisite_law",
    "not_eligible_yet": "cooldown_active",
    "cb_query_timeout": "drain_timeout",
    # cb_query_busy is a TRANSIENT slot collision after the bridge's own bounded retry,
    # not a frozen bridge — map to the dedicated transient gate so the agent re-plans.
    "cb_query_busy": "cb_query_busy",
    "busy_deferred": "cb_query_busy",
    "no_valid_targets": "no_valid_targets",
    # ck-zgh4: the bridge emits this reason verbatim from scope_latched_reason().
    "rng_scope_latched": "rng_scope_latched",
    "timeout": "drain_timeout",
    "bridge_disconnected": "bridge_disconnected",
    # L14 int5: the bridge's own fault and timeout replies (GATE_TABLE bridge_fault).
    # Exact keys, so game_thread_timeout no longer reaches the "timeout" substring.
    "memory_access_fault": "bridge_fault",
    "game_thread_timeout": "bridge_fault",
    # L14 FF3 N3-3: a write in flight when the connection dropped may have landed
    # (reply_refusal, DISCONNECTED_IN_FLIGHT): read back first, never a retry.
    "bridge_disconnected_in_flight": "write_unconfirmed",
    # construct_building (cleanup 2026.11, lane l6): the game's construction
    # validity refused (its reasons ride in engine_reasons and the summary).
    "construction_refused": "target_ineligible",
    "construction_not_started": "write_unconfirmed",
    "invalid_location": "target_not_assignable",
    # set_obligation_level (lane l6): the contract has no such obligation, or a
    # raise (the game asks the vassal's consent and it costs opinion; this write
    # does neither, so it is refused). The vassal that is not the liege's (no
    # contract, action_handlers.cpp do_set_obligation_level) is no_such_contract,
    # with the ck-3mz0 codes below: retrying the same vassal cannot succeed.
    "obligation_not_in_contract": "target_ineligible",
    "raise_requires_negotiation": "target_ineligible",
    # A dispatch whose handler answered before the drain ran it — the steered war
    # lands on the next pass. Map to confirmation_pending (pending, NOT blocked) so no
    # path mislabels an async-landing war as a hard rejection.
    "intent_not_completed_within_5s": "confirmation_pending",
    "sent_no_confirmation": "confirmation_pending",
    # L14 S4: call_war_create_core / create_title on the game thread wait for
    # nothing (the drain runs the intent after the handler returns) and say so,
    # with drains_when "next_pass": the verdict has not arrived yet.
    "no_poll_on_game_thread": "confirmation_pending",
    # create_title (#113) typed rejections — the bridge self-polls 12s then reports
    # status:"rejected" + one of these reasons (intent_dispatch.cpp). The de-jure-control
    # refusal carries the measured X_of_Y counts (parsed in _confirm_create_title).
    "create_title_refused_insufficient_de_jure_control": "de_jure_control",
    "create_title_refused_by_engine_gate": "create_title_engine_gate",
    # L14 S3: the bridge's own price check in execute_create_title_intent.
    "create_title_cannot_afford": "cannot_afford",
    "title_key_not_found": "target_not_assignable",
    # Deferred writes that self-poll 8s (< the 10s client recv) then report
    # dispatched_pending; the agent polls query_intents / query_dispatch_result for the
    # typed verdict — pending, NOT blocked.
    "not_confirmed_within_8s": "confirmation_pending",
    # ── ck-3mz0: the snake_case codes the census found unmapped ───────────
    # Exact keys, because these are CODES the bridge emits and not prose. The
    # prose family is a separate, ordered table below — mixing the two here
    # would let a short English word shadow a code through the substring pass.
    "ai_controller_unbound": "ai_controller_unbound",
    "actor_not_claimed": "actor_not_claimed",
    "not_allowed_or_unaffordable": "cannot_afford",
    "rejected_or_unaffordable": "cannot_afford",
    "no_contract_with_that_vassal": "no_such_contract",
    "no_vassal_contracts": "no_such_contract",
    "no_change_already_set": "already_in_that_state",
    "focus_not_found": "name_not_found",
    "building_not_found": "name_not_found",
    "decision_not_found": "name_not_found",
    "interaction_not_found": "name_not_found",
    "maa_type_not_found": "name_not_found",
    "not_an_education_focus": "name_not_found",
    "unrun": "not_evaluated",
    # ck-ie6o: the clock verbs in pipeline mode.
    "clock_owned_by_pipeline": "clock_owned_by_pipeline",
    # ck-ba35's family — every field the census saw named. Mapped here as well
    # as validated up front, because a malformed call can still reach the
    # bridge and must not come back "retry".
    "target_required": "missing_required_argument",
    "maa_type_required": "missing_required_argument",
    "company_id_required": "missing_required_argument",
    "vassal_id_required": "missing_required_argument",
    "actor_id_required": "missing_required_argument",
    "position_id_required": "missing_required_argument",
    "character_id_required": "missing_required_argument",
    "title_id_required": "missing_required_argument",
    "obligation_required": "missing_required_argument",
    "regiment_id_and_location_id_required": "missing_required_argument",
    "missing_decision_type": "missing_required_argument",
    "missing_law_name": "missing_required_argument",
    "missing_focus": "missing_required_argument",
    "missing_location_id": "missing_required_argument",
    "missing_building": "missing_required_argument",
    # ck-r3fs: five codes the BRIDGE emits (action_handlers.cpp) that nothing
    # mapped, so they reached the agent as gate "unknown" with the real code
    # buried in `summary`. Measured over d45-d50: target_rejected 90,
    # seat_holder_unchanged 27, no_matching_position 24, capital_validate_failed
    # 15, councillor_dead 15.
    "target_rejected": "target_ineligible",
    "seat_holder_unchanged": "already_in_that_state",
    "no_matching_position": "name_not_found",
    "capital_validate_failed": "target_ineligible",
    "councillor_dead": "target_ineligible",
    # lane-scheme 2026-10-18: scheme_action's own codes (action_handlers.cpp
    # do_dispatch_scheme_intent, intent_dispatch.cpp execute_scheme_intent /
    # execute_scheme_cancel_intent). Several arrive as `code:detail`; the prefix
    # resolution in map_raw_reason keys those on the code.
    "engine_refused_scheme_null_sentinel": "scheme_start_refused",
    # l3-schemes: CanStartScheme said no; the reply's result.engine_reasons names
    # the failing triggers (scheme_engine.cpp).
    "engine_refused_can_start_scheme": "scheme_start_refused",
    "scheme_check_failed": "scheme_bridge_fault",
    "can_start_scheme_unmapped": "scheme_bridge_fault",
    "can_start_scheme_crashed": "scheme_bridge_fault",
    "can_start_scheme_timeout": "scheme_bridge_fault",
    "unknown_scheme_action": "unknown_verb",
    "candidates_required": "missing_required_argument",
    "scheme_type_targets_a_title": "target_ineligible",
    "scheme_type_needs_contract_or_artifact": "scheme_type_unsupported",
    "scheme_type_not_found": "name_not_found",
    "scheme_type_required": "missing_required_argument",
    "target_id_required": "missing_required_argument",
    "scheme_id_required": "missing_required_argument",
    "not_scheme_owner": "target_ineligible",
    "scheme_not_found": "name_not_found",
    "owner_not_found": "character_not_found",
    "owner_not_alive": "character_dead",
    "target_not_found": "character_not_found",
    "target_handle_zero": "character_not_found",
    "target_not_alive": "character_dead",
    "scheme_call_crashed": "scheme_bridge_fault",
    "scheme_null_args_unavailable": "scheme_bridge_fault",
    "scheme_null_args_shape_mismatch": "scheme_bridge_fault",
    "scheme_cancel_crashed": "scheme_bridge_fault",
    "scheme_manager_null": "scheme_bridge_fault",
    "game_state_root_null": "scheme_bridge_fault",
    # the engine's remove ran and the readback still finds the scheme
    "scheme_still_present_after_remove": "scheme_bridge_fault",
    # The bridge waited 5 s, no tick ran it, and it RETIRED the intent: it will
    # never run. Distinct from the one below, where it may still land.
    "intent_timeout_cancelled_not_executed": "drain_timeout",
    "intent_timeout_no_result": "confirmation_pending",
    "scheme_not_owned_after_call": "confirmation_pending",
    # lane-query item 1: an explicit `seat` that is not the task's own seat.
    "seat_cannot_take_task": "target_ineligible",
    # lane l5: the direct interaction path's two game refusals.
    "interaction_not_valid_for_target": "interaction_invalid",
    "target_would_reject": "recipient_refuses",
    # F16: the send pipeline's other two refusals (query_interaction_check's
    # refused_by and a direct send): the game's can_send, and the cost.
    "send_conditions_not_met": "interaction_invalid",
    "send_cost_unaffordable": "cannot_afford",
    # F16: faction_action (factions.cpp execute_faction_intent / do_dispatch_faction_intent),
    # set_faction_demand_response and query_joinable_factions.
    "engine_refused_create": "faction_refused",
    "engine_refused_join": "faction_refused",
    "engine_refused_leave": "faction_refused",
    "engine_refused_demand": "faction_refused",
    "not_the_faction_leader": "faction_precondition",
    "not_a_member": "faction_precondition",
    "actor_has_no_liege": "faction_precondition",
    "already_a_member": "already_in_that_state",
    "faction_not_found": "name_not_found",
    "faction_type_not_found": "name_not_found",
    "faction_type_required": "missing_required_argument",
    "faction_id_required": "missing_required_argument",
    "unknown_faction_action": "invalid_argument",
    "response_must_be_accept_refuse_or_ai": "invalid_argument",
    "actor_not_found": "character_not_found",
    "actor_not_alive": "character_dead",
    "not_a_controlled_character": "actor_not_claimed",
    "faction_call_crashed": "faction_bridge_fault",
    "engine_call_faulted": "faction_bridge_fault",
    "faction_query_exception": "faction_bridge_fault",
    "faction_returned_null_sentinel": "faction_bridge_fault",
    "demand_hook_not_installed": "faction_bridge_fault",
    "actor_not_member_after_call": "faction_write_not_applied",
    "still_a_member_after_leave": "faction_write_not_applied",
    "demand_still_pressable_after_press": "faction_write_not_applied",
    "readback_disagrees": "faction_write_not_applied",
    "faction_query_busy": "faction_query_busy",
    # appoint_councillor (action_handlers.cpp): the bridge swaps occupied seats only.
    "vacant_seat_fill_unsupported": "target_ineligible",
    # cleanup L9: codes the steer and discover reads come back with.
    "no_controlled_character": "no_ruler_bound",
    "axis_out_of_range": "invalid_argument",
    "missing interaction_type": "missing_required_argument",
}

#: ENGINE PROSE, ORDERED, AND CHECKED AFTER THE EXACT MAP (ck-3mz0).
#:
#: Some refusals arrive as the GAME's own sentence rather than a code —
#: action_handlers.cpp:773 says so at the site: "On failure: 'Does not fulfill
#: ai_potential' / 'cannot afford' / 'cannot take' / etc." There is no code to
#: key on, so these match by SUBSTRING, and that is why they live apart:
#:
#:   * ORDER MATTERS and a dict's order is its insertion order, which is an
#:     accident of editing. A tuple says the order is deliberate, and the
#:     entries run LONGEST-FIRST so "cannot afford decision" cannot be eaten by
#:     a shorter neighbour.
#:   * PROSE IS THE ENGINE'S and it can change under a patch or a locale. A
#:     miss here must fall through to `unknown` — visibly unforeseen — rather
#:     than land on the nearest gate. So nothing here is a generic word: no
#:     bare "cannot", no bare "afford".
ENGINE_PROSE_TO_GATE: tuple = (
    ("does not fulfill ai_potential", "ai_potential_not_met"),
    ("cannot afford decision", "cannot_afford"),
    ("cannot take decision", "decision_unavailable"),
    ("unknown action_class", "unknown_verb"),
    ("unknown intent", "unknown_verb"),
)


# ═══ ck-r3fs: A REFUSAL SAYS WHAT WOULD WORK ═════════════════════════════════
#
# THE EVIDENCE. 655 refused acts across the arm-A registered sets; 454 retries
# of the same verb within 15 minutes, of which only 29% succeeded and 80 were
# BYTE-IDENTICAL. Per intent: take_decision 17/251 (7%), change_law 0/24,
# declare_war 0/19. A seat that cannot tell "you cannot afford this" from "this
# can never work" retries the second as if it were the first.
#
# WHAT IS ADDED, and it is PURELY ADDITIVE per the hub: `why`, `need`/`have`,
# `next`, `retry`, `scope`. Every existing field stays — gate, class, severity,
# retryable, is_known_limitation, summary, raw_reason, measured, fix — because a
# census compares refusal shapes across deploys and renaming or dropping a field
# silently re-buckets every historical row. `blocked` is NOT renamed to
# `unavailable_because` for the same reason, though the design proposed it.
#
# WHAT IS DELIBERATELY NOT ADDED. A `next` is emitted only where it is
# DERIVABLE from the gate's own mechanics. Where it is not, the key is ABSENT
# rather than filled with a guess: a refusal that invents a plausible next step
# is worse than one that says nothing, because the seat will spend a call on it.
# That is the same rule as the typed absence in ck-qzoo and it is why there is
# no catch-all sentence here.

#: One plain sentence per gate, stating the MECHANIC and nothing about strategy.
#: A gate absent from this table falls back to its `summary`; it never gets an
#: invented sentence.
GATE_WHY: dict[str, str] = {
    "cannot_afford": "the action has a price and the character cannot pay it now",
    "insufficient_gold": "the action has a gold price and the character cannot pay it now",
    "insufficient_piety": "the action has a piety price and the character cannot pay it now",
    "insufficient_prestige": "the action has a prestige price and the character cannot pay it now",
    "missing_required_argument": "a required argument for this intent was absent",
    "unknown_argument": "the call named an argument this tool does not take; it was not run",
    "name_not_found": "the name given does not exist in the game's database",
    "decision_unavailable": "the decision exists but is not available to this character now",
    "ai_potential_not_met": "the game's own ai_potential gate on this action is not satisfied",
    "no_change_already_set": "the write was a no-op: the state already matched",
    "already_in_that_state": "the write was a no-op: the state already matched",
    "experimental_write_disabled": "this write is off by default in the bridge and an agent cannot enable it",
    "unknown_verb": "no tool wraps that action, so it is not callable however it is named elsewhere",
    "unknown_subject": "the tool has no subject or target by that name",
    "unknown_domain": "query has no domain by that name",
    "invalid_argument": "an argument was present but its value is not one this call accepts",
    "bridge_disconnected": "the server has no connection to the bridge in the game",
    "read_timeout": "the bridge did not answer the read within its deadline",
    "bridge_fault": "the bridge's own read or call into the game faulted (memory_access_fault) or the game thread did not finish it in time (game_thread_timeout); it was contained, and the effect may or may not have landed",
    "write_unconfirmed": "the write was sent and the bridge did not confirm it; it may or may not have applied",
    "speed_refused_at_war": "speeds above the cap are refused while the ruler is at war",
    "cycle_game_days_over_cadence": "game_days exceeds the registered cycle cadence (CK3_CYCLE_GAME_DAYS); ask for at most the cadence",
    "cycle_cadence_unreadable": "CK3_CYCLE_GAME_DAYS is set to something that is not a positive integer; nothing was waited",
    "cycle_not_reached": "the clock did not cover the asked game-days within the bounded waits; ask again for the remaining days",
    "horizon_reached": "the game date is at or past the episode's registered horizon; no further wait is made and the harness ends the seat",
    "episode_horizon_unreadable": "CK3_EPISODE_HORIZON_END is set to something that is not a game date; nothing was waited",
    "unmapped_refusal": "the bridge refused with a code this server does not map; raw_reason carries it",
    "owned_by_other": "another control session already holds that character",
    "last_claim_of_control_session": "this is the seat's last character and releasing it is refused",
    "clock_owned_by_pipeline": "the clock belongs to the harness; a session cannot pause, unpause or set speed",
    "actor_not_claimed": "the acting character is not claimed by this session",
    "no_ruler_bound": "no ruler is bound to this session, so there is no actor to read or act for",
    "actor_unresolved": "character_id=0 got no usable reply; the actor is unknown, which does not prove nothing is bound",
    "target_ineligible": "the target was rejected by the game's own eligibility check",
    "confirmation_pending": "the call was accepted and its verdict has not arrived yet",
    "cb_query_busy": "the shared casus-belli slot stayed busy for the whole retry budget",
    "de_jure_control": "the title requires de-jure counties the character does not hold",
    "not_at_peace": "the action requires peace and the character is at war",
    "interaction_invalid": "the game's own validity check for this interaction, actor and recipient said no",
    "recipient_refuses": "the recipient's own acceptance check says they would refuse, so nothing was sent",
    "war_not_found": "the character is not in a war with that id",
    "target_not_independent": "the target is someone's vassal, not an independent ruler, so this war cannot be declared on him",
    "claim_not_pressed": "a claim casus belli needs a PRESSED claim on the target's title, and the actor holds none (an unpressed claim does not count)",
    "succession_straddle": "the ruler is old enough that a war opened now is expected to run across his succession",
    "not_war_leader": "only a war's leader (its primary attacker or defender) can do this",
    "scheme_start_refused": "the game's own scheme-start checks refused it; measured.engine_reasons has one row per failing check (the scheme type's allow or valid trigger, an existing scheme or cooldown on that target, or an invalid target), each with the game's words, or for an allow/valid trigger that the bridge does not read the game's words for it",
    "scheme_type_unsupported": "the game only starts this scheme type with a task contract or artifact, which the bridge cannot supply",
    "scheme_bridge_fault": "the bridge's call into the game's scheme code faulted or could not be prepared; it was contained, so check query(domain='schemes') before assuming nothing changed",
    "faction_refused": "the game's own faction rules refused it; the summary is the game's reason",
    "faction_precondition": "the bridge's own check before asking the game refused it (you are not the faction's leader, not a member, or have no liege); the summary says which",
    "faction_bridge_fault": "the bridge's call into the game's faction code faulted or could not be made; it was contained, so read do(action_class='factions_to_join') before assuming nothing changed",
    "faction_write_not_applied": "the game ran the call and reading the faction back shows it did not take effect",
    "faction_query_busy": "another faction read was being computed on the game's thread",
    "mint_cost_unverified": "the game's price for this title could not be read, so the mint was not sent",
}

#: Gates that fire REGARDLESS of the arguments. `scope: "generic"` tells a seat
#: that re-sending with different arguments cannot help, which is precisely the
#: distinction the 80 byte-identical retries were missing.
GENERIC_GATES: frozenset = frozenset({
    "experimental_write_disabled", "unknown_verb", "clock_owned_by_pipeline",
    "bridge_disconnected",
    "ai_controller_unbound", "last_claim_of_control_session",
    "gate_introspection_unavailable", "actor_not_claimed",
})

#: Where a next step is genuinely derivable from the gate. Anything not here
#: gets NO `next` key.
GATE_NEXT: dict[str, str] = {
    "missing_required_argument": "call do(action_class=<this intent's class>) to enumerate valid arguments",
    "unknown_argument": "call again with only the names in fix.accepted_fields",
    "name_not_found": "call do(action_class=<this intent's class>) to enumerate valid names",
    "decision_unavailable": "call do(action_class='decisions') to see which decisions are available now",
    "unknown_verb": "the callable intents are the `intent` enum on this tool",
    "actor_not_claimed": "bind the character first: session(action='control', character_id=<id>), or session(action='add', character_id=<id>) to play it beside your others",
    "no_ruler_bound": "bind one: session(action='control', character_id=<id>), then session(action='whoami')",
    "actor_unresolved": "check session(action='health'), then session(action='whoami')",
    "owned_by_other": "that character is not free; choose one this seat holds",
    "write_unconfirmed": "read the state back before sending the write again",
    "read_timeout": "check session(action='health'), then retry once",
    "bridge_fault": "read the state back where the effect would show before sending it again; session(action='health') says whether the bridge is still answering",
    "bridge_disconnected": "check that CK3 is running with the bridge loaded, then session(action='health')",
    "faction_refused": "call do(action_class='factions_to_join') for what the game allows now",
    "faction_precondition": "call do(action_class='factions_to_join') for the factions you are in and who leads them",
    "faction_query_busy": "retry the read once",
}

#: Said in full rather than implied, for the gates where nothing will ever work.
NOT_POSSIBLE = "not possible for this character in this episode"

#: lane-query item 2: THE PLACEHOLDER WAS SHIPPED. GATE_NEXT says "call
#: do(action_class=<this intent's class>)" and that text reached the agent
#: verbatim, including for men_at_arms, which had no class at all. do() knows
#: the intent, so it names the class; an intent with no enumerate says so.
INTENT_ENUMERATE_CLASS: dict[str, str] = {
    "council_task": "council_tasks",
    "take_decision": "decisions",
    "interact": "interactions",
    "change_law": "laws",
    "hire_merc": "mercenaries",
    "men_at_arms": "men_at_arms",
    "build": "buildings",
    "faction": "factions_to_join",
    "mint_title": "formable_titles",
}
_NEXT_PLACEHOLDER = "<this intent's class>"

#: do(action_class="men_at_arms") row cap (lane-query live: 109 rows = 47 KB).
MAA_ENUMERATE_CAP = 30
#: do(action_class="buildings") row cap: the type list is the whole building database.
BUILDING_ENUMERATE_CAP = 40


def concrete_next(blocked, intent: str):
    """Replace the GATE_NEXT placeholder with the intent's real enumerate class."""
    if not isinstance(blocked, dict):
        return blocked
    nxt = blocked.get("next")
    if isinstance(nxt, str) and _NEXT_PLACEHOLDER in nxt:
        cls = INTENT_ENUMERATE_CLASS.get(intent or "")
        if cls:
            blocked["next"] = nxt.replace(_NEXT_PLACEHOLDER, repr(cls))
        else:
            blocked["next"] = (f"intent '{intent}' has no enumerate mode; its arguments are "
                               "listed under ARGUMENTS BY INTENT in the do tool description")
    return blocked


def _actionable(gate: str, out: dict, *, fix: Optional[dict] = None,
                measured: Optional[dict] = None) -> dict:
    """`why` / `need` / `have` / `next` / `retry` / `scope` for one refusal.

    Derived only from what the envelope already knows. Nothing here guesses:
    an unlisted gate gets its `summary` as `why` and no `next` at all.
    """
    add: dict[str, Any] = {}

    add["why"] = GATE_WHY.get(gate) or str(out.get("summary") or gate)

    # need/have: lifted out of `measured` ONLY when it actually carries them, so
    # a reader can tell "the gate reported a shortfall" from "nobody measured".
    if isinstance(measured, dict):
        for k in ("need", "have"):
            if k in measured and measured[k] is not None:
                add[k] = measured[k]
        # the common bridge spelling for a de-jure/count gate
        if "need" not in add and measured.get("required") is not None:
            add["need"] = measured["required"]
        if "have" not in add and measured.get("held") is not None:
            add["have"] = measured["held"]

    # retry: from the gate's own retryability and severity, never invented.
    sev = out.get("severity")
    if out.get("retryable"):
        add["retry"] = "now" if sev == "transient" else "after the blocking condition clears"
    else:
        add["retry"] = "never"

    # scope: does the gate look at the arguments at all?
    add["scope"] = "generic" if gate in GENERIC_GATES else "arguments"

    # next: a copyable fix wins; then a derivable enumerate; then, only for a
    # permanently impossible call, the literal sentence. Otherwise ABSENT.
    if isinstance(fix, dict) and fix:
        add["next"] = fix
    elif gate in GATE_NEXT:
        add["next"] = GATE_NEXT[gate]
    elif add["retry"] == "never" and (out.get("is_known_limitation")
                                      or gate in GENERIC_GATES):
        add["next"] = NOT_POSSIBLE
    return add


def why_blocked(gate: str, summary: str = "", *, fix: Optional[dict] = None,
                measured: Optional[dict] = None, raw_reason: str = "",
                severity: Optional[str] = None) -> dict[str, Any]:
    """Build the uniform why_blocked model (SPEC §1.2).

    A gate this table does not name is reported as `unmapped_refusal` with the
    name carried in `raw_reason`: there is no unnamed gate.

    ck-r3fs: A LATE `unmapped_refusal` IS RESOLVED HERE, at the choke point, because not
    every refusal path routes through `map_raw_reason`. Measured over d45-d50:
    936 refusals reached the agent with `gate: "unknown"` while `summary` and
    `raw_reason` carried the real code — target_required (99), target_rejected
    (90), not_allowed_or_unaffordable (33), seat_holder_unchanged (27),
    no_matching_position (24), maa_type_required (18), capital_validate_failed
    (15), councillor_dead (15). Four of those were ALREADY in
    RAW_REASON_TO_GATE at deploy-49 and deploy-50, so the map was never the
    defect: a caller that builds the envelope itself skips the mapper, and
    adding entries could not have reached it. Resolving at the single place
    every envelope is built does.

    Only an exact code match resolves. Prose stays `unmapped_refusal`, loudly: a
    `summary` of "faction power 80% (>=80 tripwire)" is not a code and must not
    be bucketed by a substring. When a late resolve happens it is RECORDED
    (`gate_resolved_from`), so a census can tell a gate the caller knew from one
    this function recovered.
    """
    resolved_from = ""
    if gate not in GATE_TABLE:
        raw_reason = raw_reason or gate
        gate = "unmapped_refusal"
    if gate == "unmapped_refusal":
        for candidate in (raw_reason, summary):
            key = str(candidate or "").strip()
            if key and key in RAW_REASON_TO_GATE:
                gate = RAW_REASON_TO_GATE[key]
                resolved_from = key
                break
    meta = GATE_TABLE[gate]
    out: dict[str, Any] = {
        "gate": gate,
        "class": meta["class"],
        "severity": severity or meta["severity"],
        "retryable": meta["retryable"],
        "is_known_limitation": bool(meta.get("is_known_limitation", False)),
        "summary": summary or (raw_reason if gate == "unmapped_refusal" else gate),
    }
    # ck-zgh4 deploy-51 item (6): a gate may carry one seat-facing sentence
    # explaining what the code MEANS, for the few where the code alone has
    # misled. Absent for every other gate, so this costs nothing on the ~500
    # refusals a transcript carries.
    if meta.get("note"):
        out["note"] = meta["note"]
    if measured is not None:
        out["measured"] = measured
    if fix is not None:
        out["fix"] = fix
    if raw_reason:
        out["raw_reason"] = raw_reason
    if resolved_from:
        out["gate_resolved_from"] = resolved_from
    out.update(_actionable(gate, out, fix=fix, measured=measured))
    return out


#: The bridge's default-off write list (dispatch_router.cpp experimental_write_enabled,
#: kExperimental), as the seat sees it. MIRRORED, not imported: the bridge is C++.
#: test_experimental_gate_contract.py reads kExperimental out of the source and the
#: operator docs (docs/research.md) and fails if any of the three disagree.
EXPERIMENTAL_WRITES: tuple = ()
# scheme_action left the list 2026-10-18 after live validation (lane-scheme);
# faction_action left it 2026-10-21 after live validation from both seats (F16,
# live867 runs l10-p6 and l10-p7).


def experimental_gate_blocked(raw: Any, bridge_action: str) -> dict[str, Any]:
    """The `experimental_write_disabled` refusal, carrying what an OPERATOR needs.

    lane-scheme 2026-10-18. The persona test got a summary pointing at
    "enabled_writes_file in this reply" from a reply that did not carry it: the
    bridge sends the path, and this envelope dropped it. The path and the exact
    line now ride in `fix`, marked operator-only, because the seat cannot and
    must not arm its own gate (GATE_WHY says so) -- but the human reading the
    transcript can, for a controlled validation run.
    """
    raw = raw if isinstance(raw, dict) else {}
    path = raw.get("enabled_writes_file") or ""
    fix = {"who": "operator", "enabled_writes_file": path, "add_line": bridge_action,
           "docs": "docs/research.md#default-off-writes"}
    summary = (f"'{bridge_action}' is off by default in the bridge (unvalidated "
               f"crash-free). An operator can arm it for a controlled run by adding the "
               f"line '{bridge_action}' to {path or 'the ck3-enabled-writes file in the bridge state dir'}; "
               f"the seat cannot.")
    return why_blocked("experimental_write_disabled", summary=summary, fix=fix,
                       raw_reason="experimental_write_disabled")


#: What a refusal says when the game gave no reason text (or only a compound
#: header with nothing under it): one sentence, every path (L12).
NO_GAME_DETAIL = "the game gives no detail"
#: A check whose words the bridge never asks the game for (L14 FF1 M-11): a
#: scheme's failing allow/valid trigger. The game builds that tooltip at render
#: time (re_schemes.md) and the bridge's CanStartScheme call does not request it
#: (scheme_engine.cpp wants_text 0), so "the game gives no detail" would claim a
#: reading nobody made.
NO_BRIDGE_WORDS = "the bridge does not read the game's words for this check"
#: The scheme-start checks the bridge reads no words for (NO_BRIDGE_WORDS): a
#: failing trigger block of the scheme type, which emit_reasons
#: (scheme_engine.cpp) names allow, valid, or trigger (a block that is neither)
#: and always sends with text "" (L14 FF2 N-7: trigger was missing, and read as
#: NO_GAME_DETAIL).
SCHEME_UNWORDED_CHECKS = frozenset({"allow", "valid", "trigger"})
#: The other wordless rows scheme_engine.cpp sends, each with what it means. The
#: game was not asked for words on any of them, so none says "the game gives no
#: detail".
SCHEME_CHECK_WORDS = {
    # emit_reasons: the engine called a reasons-sink method past slot 0x18,
    # which the bridge's sink does not model, so whatever it wrote was not read.
    "sink_method_not_modelled": ("the game wrote a reason through a method the bridge "
                                 "does not model, so its words were not read"),
    # scheme_can_start: nothing reached the sink; the owner's validity check,
    # which CanStartScheme runs before it writes any reason, said no.
    "owner_invalid": "the game's validity check on the scheme's owner said no before it gave a reason",
    # ... or ValidateSchemeTarget said no for this target.
    "target_invalid": "the game's validity check on the target said no before it gave a reason",
}
#: A wordless row whose check this server does not know.
SCHEME_CHECK_UNKNOWN = "the bridge sent no words for this check"


def _engine_chunk(s: str) -> str:
    """One tooltip line's markup reduced to words."""
    # An icon-only concept (`..._i`) stands for its name; any other tooltip or click
    # target is followed by its own display text, so the target itself goes.
    s = re.sub(r"\x15TOOLTIP:[A-Z_]+,([a-z_]+?)_i(?=[\s\x15\x16]|$)", r"\1", s)
    s = re.sub(r"\x15(?:TOOLTIP|ONCLICK):[A-Z_]+,\S*\s?", "", s)
    # A trigger line's mark is the only icon that carries meaning on its own.
    s = s.replace("\x16trigger_fail!", " not met: ").replace("\x16trigger_success!", " met: ")
    s = re.sub(r"\x16[A-Za-z0-9_]+!", "", s)
    s = re.sub(r"\x15!", "", s)
    s = re.sub(r"\x15[A-Za-z_]+;?\s?", "", s)    # colour and format codes (X, L, E;, high)
    s = re.sub(r"[\x00-\x1f]", " ", s)
    return re.sub(r"\s+", " ", s).strip(" ;")


def engine_text(raw: str) -> str:
    """The engine's tooltip text as a player reads it: its icon and concept markup
    (\x16<name>_icon!, \x15TOOLTIP:GAME_CONCEPT,<concept>_i, \x15X / \x15! colour codes)
    reduced to words, one "; " per line. Measured on set_title_law's refusal on the
    867 save: '\x16warning_icon! \x15X Missing \x15TOOLTIP:GAME_CONCEPT,prestige_i
    \x16prestige_icon!\x15! 948\x15!' reads 'Missing prestige 948'.

    The line rules are bridge/include/engine_text.h's (clean_engine_text, which
    cleans the faction and send-check reasons in the bridge; this cleans the
    change_law, construction and intent-verdict texts the bridge passes raw):
    a compound header (a line ending in ':') is kept only over a kept deeper
    line, and a repeated line is said once. test_engine_text.py runs both on one
    table of cases. "" means the game gave no reason (NO_GAME_DETAIL)."""
    s = str(raw or "")
    lines = []                       # (level, words)
    level = 0
    for part in re.split(r"(\x15indent_newline:\d+\s?|\n)", s):
        m = re.match(r"\x15indent_newline:(\d+)", part)
        if m or part == "\n":
            level = int(m.group(1)) if m else 0
            continue
        words = _engine_chunk(part)
        if words:
            lines.append((level, words))
    keep = [False] * len(lines)
    for k in range(len(lines) - 1, -1, -1):
        lv, words = lines[k]
        if not words.endswith(":"):
            keep[k] = True
            continue
        n = k + 1
        while n < len(lines) and lines[n][0] > lv:
            if keep[n]:
                keep[k] = True
                break
            n += 1
    out = []
    for k, (_, words) in enumerate(lines):
        if keep[k] and words not in out:
            out.append(words)
    return "; ".join(out)


def map_raw_reason(raw_reason: str, summary: str = "") -> dict[str, Any]:
    """Map a documented bridge refusal string to a why_blocked. Unmapped ⇒
    gate:unmapped_refusal carrying the raw reason (loud, never silently bucketed)."""
    if not raw_reason:
        return why_blocked("unmapped_refusal", summary=summary or "bridge refused with no reason")
    key = str(raw_reason).strip()
    # A raw reason that already IS a gate name is that gate.
    gate = RAW_REASON_TO_GATE.get(key) or (key if key in GATE_TABLE else None)
    if gate is None:
        # ck-3mz0: the ENGINE's own sentences, in a deliberate order, BEFORE the
        # generic substring pass below — so a prose needle is matched as prose
        # and cannot be pre-empted by a code that happens to be a substring of
        # it, nor pre-empt one.
        low = key.lower()
        for needle, g in ENGINE_PROSE_TO_GATE:
            if needle in low:
                gate = g
                break
    if gate is None and ":" in key:
        # lane-scheme: `code:detail` (scheme_type_not_found:murder,
        # scheme_call_crashed:SIGSEGV). Key on the code BEFORE the substring
        # pass, which would otherwise let any short code that happens to occur
        # inside the detail win.
        gate = RAW_REASON_TO_GATE.get(key.split(":", 1)[0].strip())
    if gate is None:
        # try a prefix/substring match for the #109 reason family.
        for raw, g in RAW_REASON_TO_GATE.items():
            if raw in key:
                gate = g
                break
    if gate is None:
        return why_blocked("unmapped_refusal", summary=summary or key, raw_reason=key)
    return why_blocked(gate, summary=summary or key, raw_reason=key)


# ===========================================================================
# §1.1 the uniform envelope
# ===========================================================================

#: The write outcomes a reply can carry. `pending` is the one spelling of "the
#: bridge accepted it and has not reported the outcome" (it was also spelled
#: `dispatched_pending`).
VERDICTS = ("applied", "rejected", "pending")


def _code_detail(item: Any) -> dict[str, Any]:
    """One warning or error row as {code, detail, ...}."""
    if isinstance(item, dict):
        row = dict(item)
        row["code"] = str(row.get("code") or "note")
        row["detail"] = str(row.get("detail") if row.get("detail") is not None
                            else row.get("message") or row.get("summary") or row["code"])
        return row
    return {"code": "note", "detail": str(item)}


def raw_reply_envelope(tool: str, raw: Any, *, game: Optional[dict] = None,
                       bridge_health: str = "ok") -> dict[str, Any]:
    """A bridge reply passed through whole, in the one envelope: a reply that says it
    failed (success false, or an error/reason with no success) is a named refusal."""
    raw = raw if isinstance(raw, dict) else ({} if raw is None else {"value": raw})
    failed = raw.get("success") is False or (
        "success" not in raw and bool(raw.get("error")))
    if raw.get("status") == "sent_no_confirmation":
        return envelope(tool, ok=False, game=game, bridge_health=bridge_health,
                        blocked=why_blocked("write_unconfirmed",
                                            summary=f"{tool} was sent and the bridge did not confirm it",
                                            raw_reason=str(raw.get("reason_code") or "")))
    if failed:
        reason = str(raw.get("reason") or raw.get("error") or "")
        return envelope(tool, ok=False, game=game, bridge_health=bridge_health,
                        result=raw, blocked=map_raw_reason(reason, summary=reason))
    return envelope(tool, result=raw, game=game, bridge_health=bridge_health)


def envelope(tool: str, *, ok: bool = True, result: Any = None,
             verdict: Optional[str] = None, blocked: Optional[dict] = None,
             actor: Optional[dict] = None, game: Optional[dict] = None,
             delta: Optional[list] = None, pending: Optional[dict] = None,
             warnings: Optional[list] = None, errors: Optional[list] = None,
             bridge_health: str = "ok") -> dict[str, Any]:
    """Build the uniform top-level envelope (SPEC §1.1). `data` is always a parsed
    object; a partial read never masquerades as success (errors[] non-empty ⇒ ok:false).

    Every warning and every error is {code, detail}: a bare string is carried as
    {"code": "note", "detail": <string>}. `verdict` is one of VERDICTS."""
    if verdict is not None and verdict not in VERDICTS:
        raise ValueError(f"verdict {verdict!r} is not one of {VERDICTS}")
    env: dict[str, Any] = {
        "ok": ok and not errors,
        "tool": tool,
        "result": result if result is not None else {},
        "blocked": blocked,
        "warnings": [_code_detail(w) for w in (warnings or [])],
        "errors": [_code_detail(e) for e in (errors or [])],
        "bridge_health": bridge_health,
    }
    if verdict is not None:
        env["verdict"] = verdict
        env["applied"] = (verdict == "applied")
    if actor is not None:
        env["actor"] = actor
    if game is not None:
        env["game"] = game
    if delta is not None:
        env["delta"] = delta
    if pending is not None:
        env["pending"] = pending
    if blocked is not None:
        env["ok"] = False
    return env


# ===========================================================================
# §5 actor-explicitness helpers
# ===========================================================================

def _as_int(v) -> int:
    try:
        return int(v)
    except (TypeError, ValueError):
        return 0


def actor_echo(char: dict | None, requested_id: int, is_player: bool = False,
               is_controlled: bool = True) -> dict[str, Any]:
    """Build the echoed actor block (axiom 7). Best-effort — name/alive lifted from a
    query_character section when available."""
    cid = requested_id
    name = None
    alive = True
    pending, pending_key = False, None
    if isinstance(char, dict):
        sect = char
        cid = _as_int(sect.get("id") or requested_id) or requested_id
        # query_character emits the name at the top level via emit_char_names ("name").
        name = sect.get("name")
        if "alive" in sect:
            alive = bool(sect.get("alive"))
        # lane-namekey: a name the bridge has not localized yet is PENDING, not
        # absent. The bridge omits `name` and sends `name_key` (the game's
        # localization key, `E_thelred` for Æthelred — not a name) plus
        # `name_pending: true` until the game thread resolves it (the bridge's
        # drain, paused or running). Carry that state through so the reader
        # of this echo knows a null name means "later", never "unnamed".
        if not name and sect.get("name_pending"):
            pending = True
            pending_key = sect.get("name_key")
    return {
        "id": cid, "name": name, "alive": alive,
        "is_controlled": is_controlled, "is_player": is_player,
        **({"name_pending": True, "name_key": pending_key} if pending else {}),
    }


def unbound_actor_block(reply) -> Optional[dict]:
    """rc2 B6: classify a `query_character(character_id=0)` reply that names no
    character. None when it names one.

    With nothing bound the bridge's handler fails with no reason, and
    dispatch_action_with_response fills `{"success": false, "action": ...}`:
    that is `no_ruler_bound`. Any other empty reply (timeout `{}`, `None`, an
    error code) is `actor_unresolved`: we looked and could not see. Before
    this, both fell through to actor_echo's alive/is_controlled defaults and
    observe reported zeros as the ruler's state with ok=true.
    """
    if isinstance(reply, dict) and _as_int(reply.get("id")) > 0:
        return None
    if isinstance(reply, dict):
        code = reply.get("error") or reply.get("reason")
        if reply.get("success") is False and not code:
            return why_blocked(
                "no_ruler_bound",
                summary=("no ruler is bound to this session: character_id=0 resolves to "
                         "nothing. Bind one with session(action='control', character_id=<id>)."),
                measured={"reply": "declined"})
        if isinstance(code, str) and code:
            return why_blocked("actor_unresolved",
                               summary=f"character_id=0 could not be resolved: {code}",
                               measured={"reply": "error"}, raw_reason=code)
    return why_blocked("actor_unresolved",
                       summary=("character_id=0 got no reply naming a character; whether a "
                                "ruler is bound is unknown. See session(action='whoami')."),
                       measured={"reply": "absent"})


def unbound_measured(row: dict) -> dict:
    """rc2 B6: what this seat's own session row says about WHY it holds nothing.

    The bridge publishes `unbound_reason` ("" unless the bridge itself unbound
    the seat; today only `heir_is_player`) and `unbound_heir_id`
    (session.cpp sessions_json). A row without the key is a bridge that does not
    report it: None, never "".
    """
    reason = row.get("unbound_reason") if "unbound_reason" in row else None
    return {"reply": "seat_row_has_no_primary",
            "unbound_reason": reason if isinstance(reason, str) or reason is None else str(reason),
            "unbound_heir_id": _as_int(row.get("unbound_heir_id")) if "unbound_heir_id" in row else None}


def unbound_summary(row: dict) -> str:
    m = unbound_measured(row)
    bind = "Bind one with session(action='control', character_id=<id>)."
    if m["unbound_reason"] == "heir_is_player":
        return (f"the bridge unbound this session: its ruler died and the heir "
                f"({m['unbound_heir_id']}) is the human's character. Ask which AI ruler to "
                f"take. " + bind)
    if m["unbound_reason"]:
        return f"the bridge unbound this session ({m['unbound_reason']}). " + bind
    if m["unbound_reason"] == "":
        return ("this session holds no ruler and the bridge reports no reason, as after a "
                "new or reconnected session, a release, or a handoff before a restart. If you "
                "had a ruler, check it is alive with query(domain='character') before "
                "rebinding; if it died, ask which AI ruler to take. " + bind)
    return "this session holds no ruler; this bridge does not report why. " + bind


# ---------------------------------------------------------------------------
# Scale-as-king legibility (AL-12 / re_scale_as_king_lever.md). The whole-duchy
# expansion engine is the NATIVE AI self-declaring duchy_conquest_cb wars — it
# only runs when TWO preconditions hold:
#   (1) the controlled king's per-char AI tick is ENABLED. Taking control with
#       disable_native_ai=true zeroes ai_ctrl+0x28 and CAIManager_MainTick skips
#       the whole per-char tick (re_control_suppresses_war.md, HIGH) → the king
#       neither self-declares nor sieges → occupation_score stays 0 FOREVER.
#       agent_mode==true (from get_bridge_state) is the proxy: it is set whenever
#       a controlled char took control with native AI disabled.
#   (2) the ruler is a genuine CONQUEROR with a credible army (AL-12): a content/
#       shy/low-martial heir self-declares NOTHING even on a pinned expansion
#       genome. A high genome pin is necessary but NOT sufficient.
# This is a PURE-PYTHON advisory synthesized from already-fetched reads — no
# bridge change, no live probe, best-effort (never raises). It turns the silent
# occ-0 bleed into an observable "engine is/ isn't live" signal.
_CONQUEROR_TRAITS = {
    "conqueror", "viking", "adventurer", "ambitious", "brave",
}
_PASSIVE_TRAITS = {
    "content", "shy", "humble", "craven", "lazy", "calm",
}


# ── ck-zgh4 deploy-51: `expansion_engine_health` IS GONE. ─────────────────────
# It returned an `advisory` list that prescribed the previous project's strategy
# ("PRESERVE the realm + shield and survive to a conqueror heir", "engineer a
# conqueror heir", "let the king self-declare whole-petty-kingdom
# duchy_conquest wars") and named `do(intent='set_character_ai')` as its FIX —
# an intent that does not exist on the seat surface, which is why seats called
# it 26 times and got `unknown_verb`.
#
# Item (1) of this deploy removed its two result sites, so by the time item (5)
# reached it the function was already unreachable: dead code carrying false
# advice, which is worse than either alone. The BRIDGE finding it was built on
# (ck-4b41: `disable_native_ai=false` has no else-branch and undoes nothing) is
# TRUE and is kept, at the two places a seat can still read it, naming only
# remedies the seat can actually call.


# ===========================================================================
# §2 set_phase — the phase→genome table (MCP-side config, moved out of agent head)
# ===========================================================================

# The personality axes and their range are personality_axes's table (the one
# Python definition, checked against the bridge's by test_steer_tool.py). The
# bridge ignores a key that is not an axis, so steer refuses one rather than
# report an axis written that was not.
#
# Tables encode the memory doctrine (expansion = bold/energetic; trough =
# defensive/dominant; etc.) over AXES.
PHASE_GENOME: dict[str, dict[str, int]] = {
    "expansion": {
        "boldness": 85, "energy": 90, "greed": -20,
        "rationality": 50, "vengefulness": 30, "compassion": -10,
        "honor": 20, "zeal": 40, "sociability": 30,
    },
    "rebuild": {
        "boldness": 45, "energy": 55, "greed": 0,
        "rationality": 70, "vengefulness": 10, "compassion": 30,
        "honor": 40, "zeal": 30, "sociability": 50,
    },
    "succession_trough": {
        "boldness": 40, "energy": 55, "greed": 0,
        "rationality": 80, "vengefulness": 10, "compassion": 20,
        "honor": 40, "zeal": 30, "sociability": 40,
    },
    "stalled_siege": {
        # Unstick a stalled siege: max energy/boldness, patience (no white-peace).
        "boldness": 95, "energy": 100, "greed": -20,
        "rationality": 40, "vengefulness": 40, "compassion": -20,
        "honor": 10, "zeal": 50, "sociability": 20,
    },
    "fragile_minority": {
        # Turtle a fragile minority — never raid, crush threats.
        "boldness": 30, "energy": 50, "greed": 0,
        "rationality": 80, "vengefulness": 10, "compassion": 40,
        "honor": 50, "zeal": 30, "sociability": 50,
    },
    "consolidate": {
        "boldness": 35, "energy": 60, "greed": -10,
        "rationality": 70, "vengefulness": 20, "compassion": 30,
        "honor": 40, "zeal": 40, "sociability": 50,
    },
}

PHASES = list(PHASE_GENOME.keys())

#: observe(depth=...) values.
OBSERVE_DEPTHS = ("brief", "full")

#: session(action=...) values: the branches of _session_impl_branches, in the order
#: the tool description lists them (test_surface_contract checks the two agree).
SESSION_ACTIONS = ("control", "add", "release", "whoami", "speed", "pause", "unpause",
                   "wait", "health", "experimental_flags", "set_experimental")

#: The game speed range. THE DEFINITION IS THE BRIDGE'S: game_set_speed
#: (bridge/src/event_handler.cpp) clamps every speed write to 1..3, and
#: test_surface_contract reads that clamp out of the C++ and fails if these differ.
SPEED_MIN = 1
SPEED_MAX = 3


# ===========================================================================
# §2 do — the closed intent set → bridge action (each maps to ONE §3 action)
# ===========================================================================

# Each intent → the act_params dict fed to tools.resolve_act (so dispatch stays
# single-sourced). Args are the per-intent keys the schema documents (SPEC §2 table).
INTENT_TO_ACT: dict[str, dict[str, Any]] = {
    "mint_title":     {"type": "create_title"},
    "take_decision":  {"type": "decision"},
    "interact":       {"type": "interaction"},
    "convert_faith":  {"type": "faith"},
    "convert_culture":{"type": "culture"},
    "set_doctrine":   {"type": "faith"},   # doctrine mode of faith
    "change_law":     {"type": "law"},
    "council_task":   {"type": "council_task"},
    "appoint":        {"type": "appoint"},
    "lifestyle":      {"type": "lifestyle"},
    "set_education_focus": {"type": "set_education_focus"},  # §2 child education focus
    "build":          {"type": "building"},
    # REMOVED: order_siege intent def deleted per design (agent does not drive army micro).
    "men_at_arms":    {"type": "maa"},
    "hire_merc":      {"type": "hire_mercenary"},
    "move_capital":   {"type": "move_capital"},
    "set_obligation": {"type": "contract"},
    "scheme":         {"type": "scheme"},
    # The war intents (lane l5, F8): each sends one character interaction and has
    # a dedicated branch in do_impl that finds the war and the recipient, sends,
    # and reads the war back (_do_war_intent).
    "make_peace":     {"type": "interaction"},
    "call_ally":      {"type": "interaction"},
    "join_war":       {"type": "interaction"},
}

# do(intent="cancel_pending", args={"request_id": <pending.intent_id>}) withdraws one
# write the bridge still holds in its intent queue (the bridge's cancel_intent). It
# replaced the retired `intent` tool's cancel; inspect(subject="pending") replaced its
# list and results.
_CANCEL_PENDING_INTENT = "cancel_pending"

# do(intent="faction", args={action: create|join|leave|press_demand|respond_demand})
# is served by factions.do_faction (cleanup 2026.11, F16): it composes the bridge
# write with a second read of the game's member list, which resolve_act cannot.
_FACTION_INTENT = "faction"

INTENTS = list(INTENT_TO_ACT.keys()) + [_CANCEL_PENDING_INTENT, _FACTION_INTENT]

# enumerate-mode action_class → the bridge enumeration call(s).
#: ck-ycim: FROM tools.py, which is where the agent-facing schema is built, so
#: the enum an agent reads and the list this module validates against cannot
#: disagree. They did: the schema had nine of these and this had ten.
ACTION_CLASSES = list(_tools.ACTION_CLASSES)


#: Lane l4: the KIN GUARD. These interactions are sent only at the actor's own
#: descendants (child, grandchild, great-grandchild: the line the engine's own
#: disinherit_interaction treats as family, 00_dynast_interactions.txt). The engine
#: would also let a dynast disinherit any dynasty member in his realm; the guard keeps
#: the lever on the succession line it exists for. Enforced in do() before anything
#: reaches the bridge (_kin_guard), by reads of the actor's children.
KIN_GUARDED_INTERACTIONS = frozenset({"disinherit_interaction"})

#: How deep the kin guard walks the actor's children (1 = children, 3 = great-grandchildren).
KIN_GUARD_DEPTH = 3


def _send_options_list(raw) -> list:
    """The send options a do(intent='interact') call names, as a list of flag names.
    Accepts a list or a comma-separated string; drops empties and duplicates, keeps order."""
    if raw is None:
        return []
    items = raw.split(",") if isinstance(raw, str) else list(raw)
    out = []
    for x in items:
        name = str(x).strip()
        if name and name not in out:
            out.append(name)
    return out


async def _own_descendants(gw, actor_id: int, depth: int = KIN_GUARD_DEPTH) -> set:
    """The actor's children, grandchildren, ... to `depth` generations, from
    query_character's children lists (bare ids)."""
    found: set = set()
    frontier = [actor_id]
    for _ in range(max(1, depth)):
        nxt = []
        for cid in frontier:
            sect = _char_section(await gw.read("query_character", {"character_id": cid}))
            for ch in sect.get("children") or []:      # bare ids (build_char_profile)
                kid = _as_int(ch)
                if kid and kid not in found:
                    found.add(kid)
                    nxt.append(kid)
        frontier = nxt
        if not frontier:
            break
    return found


async def _kin_guard(gw, itype: str, actor_id: int, target_id: int) -> dict | None:
    """None when the interaction may be sent; otherwise the why_blocked refusal."""
    if itype not in KIN_GUARDED_INTERACTIONS:
        return None
    if not target_id:
        return why_blocked("missing_required_argument",
                           summary=f"{itype} needs target_id: the child, grandchild or "
                                   "great-grandchild to disinherit")
    line = await _own_descendants(gw, actor_id)
    if target_id in line:
        return None
    return why_blocked("target_not_own_descendant",
                       summary=(f"{target_id} is not a child, grandchild or great-grandchild "
                                f"of {actor_id}; {itype} is sent only down your own line"),
                       measured={"actor_id": actor_id, "target_id": target_id,
                                 "own_descendants": sorted(line)})


#: Intents on the generic dispatch path whose bridge handler requires an explicit
#: actor_id > 0 (no controlled-primary fallback): do_dispatch_scheme_intent.
#: (do_dispatch_faction_intent refuses actor 0 too; factions.do_faction always sends
#: the resolved actor.)
_EXPLICIT_ACTOR_INTENTS: frozenset = frozenset({"scheme"})


def _war_id_arg(v) -> Optional[int]:
    """A war id argument, or None when none was given. War 0 is a real war (the
    engine numbers its wars from 0), so 0 is a value, never "absent"."""
    if v is None or v == "":
        return None
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def _intent_to_act_params(intent: str, args: dict, actor_id: int) -> dict[str, Any]:
    """Translate a do(intent, args) call into the act_params dict for resolve_act."""
    base = dict(INTENT_TO_ACT[intent])
    a = dict(args or {})
    a["actor_id"] = actor_id
    # §5 actor-explicitness: resolve_act reads params["actor_id"] off the RETURNED
    # dict (base). Carry the resolved actor onto base so every write dispatches to the
    # intended actor — never silently to the controlled primary (actor_id=0). This is
    # the D1/D3 silent-wrong-target failure the SPEC kills; the actor echo would
    # otherwise falsely confirm an actor the dispatch never used.
    base["actor_id"] = actor_id

    if intent == "mint_title":
        base["title_key"] = a.get("title_key") or a.get("title")
        base["title_id"] = _as_int(a.get("title_id"))
    elif intent == "take_decision":
        base["decision_type"] = a.get("decision")
    elif intent == "interact":
        base["interaction_type"] = a.get("interaction") or a.get("interaction_type")
        base["target_id"] = _as_int(a.get("target_id"))
        base["title_id"] = _as_int(a.get("title_id"))
        base["secondary_actor_id"] = _as_int(a.get("secondary_actor_id"))
        base["secondary_recipient_id"] = _as_int(a.get("secondary_recipient_id"))
        # the war an interaction is about (call_ally_interaction, join_war_interaction);
        # None when not given (war 0 is a real war, so 0 cannot mean "none")
        base["war_id"] = _war_id_arg(a.get("war_id"))
        base["send_options"] = _send_options_list(a.get("send_options"))
    elif intent == "convert_faith":
        base["faith"] = a.get("faith")
        base["faith_id"] = _as_int(a.get("faith_id"))
    elif intent == "set_doctrine":
        base["doctrine"] = a.get("doctrine")
    elif intent == "convert_culture":
        base["culture"] = a.get("culture")
        base["culture_id"] = _as_int(a.get("culture_id"))
    elif intent == "change_law":
        base["law_name"] = a.get("law")
        if a.get("scope") == "title":
            base["law_scope"] = "title"
            base["title_id"] = _as_int(a.get("title_id"))
    elif intent == "council_task":
        base["task"] = a.get("task")
        # THE KEY THE CONSUMER READS (ck-z3kh). `do_set_council_task` gates on
        # `json_get_int(params_json, "target_title_id") <= 0` -> "target_required"
        # and NEVER reads `title_id`, so sending `title_id` dropped the target
        # before it left this process and no agent-side spelling could work:
        # five seats, twenty-five attempts, zero successes in the deploy-43
        # smoke. The legacy tools.py path always sent the right key; only this
        # surface was wrong, and this surface is the only write path the seats
        # have. `target_character_id` is read by the same handler, so it is
        # forwarded rather than dropped for the character-scoped tasks.
        #
        # ONE NAME (L14, finding 8): the enumerator's do_call rows, the docs and
        # the bridge all say target_title_id. The `target_id` alias the rows used
        # to print is gone (test_council_task_target_key compares the three).
        base["target_title_id"] = _as_int(a.get("target_title_id"))
        base["target_character_id"] = _as_int(a.get("target_character_id"))
        # lane-query item 1: `seat` was accepted, documented in the do_call rows
        # this module prints, and then dropped. It now reaches the bridge, which
        # refuses a seat that cannot take the task (seat_cannot_take_task).
        if a.get("seat"):
            base["seat"] = str(a.get("seat"))
    elif intent == "appoint":
        base["position_id"] = _as_int(a.get("seat") or a.get("position_id"))
        base["character_id"] = _as_int(a.get("councillor_id") or a.get("character_id"))
        base["replace"] = True
    elif intent == "lifestyle":
        base["focus"] = a.get("focus")
    elif intent == "set_education_focus":
        # §2: actor is the CHILD whose childhood focus is being set.
        base["focus"] = a.get("focus")
    elif intent == "build":
        base["location_id"] = _as_int(a.get("location_id"))
        base["building"] = a.get("building")
    # REMOVED: order_siege intent handling deleted per design (agent does not drive army micro).
    elif intent == "men_at_arms":
        base["action"] = a.get("action") or "recruit"
        base["maa_type"] = a.get("maa_type")
        base["location_id"] = _as_int(a.get("location_id"))
        base["regiment_id"] = _as_int(a.get("regiment_id"))
    elif intent == "hire_merc":
        base["company_id"] = _as_int(a.get("company_id"))
    elif intent == "move_capital":
        base["title_id"] = _as_int(a.get("title_id"))
    elif intent == "set_obligation":
        base["vassal_id"] = _as_int(a.get("vassal_id"))
        base["level"] = _as_int(a.get("level"))
        base["obligation"] = a.get("obligation", "")
    elif intent == "scheme":
        base["action"] = a.get("action") or "start"
        base["target_id"] = _as_int(a.get("target_id"))
        base["scheme"] = a.get("scheme") or ""
        base["scheme_id"] = _as_int(a.get("scheme_id"))
    elif intent == "make_peace":
        # war_id: the war; terms: white_peace | surrender | enforce_demands.
        base["war_id"] = _war_id_arg(a.get("war_id"))
        base["terms"] = a.get("terms")
    elif intent == "call_ally":
        base["war_id"] = _war_id_arg(a.get("war_id"))
        base["target_id"] = _as_int(a.get("ally_id"))
    elif intent == "join_war":
        # leader_id: the war leader whose side to join (default: the one you are
        # allied to).
        base["war_id"] = _war_id_arg(a.get("war_id"))
        base["target_id"] = _as_int(a.get("leader_id"))
    return base


# ===========================================================================
# Async tool implementations.
#
# Each takes a `gw` (gateway) object supplied by mcp_server.py exposing:
#   await gw.read(action, params)            → a read, paused or running (deferred-drain
#                                               reads single-flighted by the gateway)
#   await gw.write(action, params)           → a write, paused or running (no clock change)
#   gw.game()                                 → {tick, date, paused, speed}: the last
#                                               fresh clock reading (paused None before one)
#   await gw.clock()                          → a FRESH clock reading (read_clock)
#   gw.health                                 → "ok" | "degraded" | "hung"
#   await gw.delta_records_since_last()      → list[dict] of since-tick events (gw tracks tick)
#   await gw.actor_gate()                    → None, or the why_blocked an id-less call
#                                               answers (actor_gate_of)
#   await gw.read_many([(action, params)])   → replies in order, issued together
#   await gw.confirmed_write(action, params, readback=…) → dispatch + readback in two
#                                               waits (confirm_write): the drain's verdict on
#                                               the next pass in either clock state, then
#                                               game days only while the clock runs
# No gateway member changes the pause state or the speed; only session() does.
# Both gateways (mcp_server.BridgeGateway, agent._AgentGateway) carry every member;
# test_surface_contract checks it, so nothing here falls back for a missing one.
# All bridge I/O lives in the gateway; this module only composes + shapes.
# ===========================================================================


def _f(v, default=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


async def agency_block(gw, character_id: int) -> dict[str, Any]:
    """What this seat controls, stated once and TRUTHFULLY. ck-zgh4 deploy-51
    item (4), on the designer's proposal 6.

    WHY IT EXISTS. d49's king wrote "I never had hands on the army" in its own
    retrospective, and d50 read `native_ai_disabled: true` 210 times. That field
    is `bool(disable_native_ai)` — THE PARAMETER THE CALLER PASSED, echoed back
    as though it were state. Seats then spent cycles on it: 188 `control` and
    183 `whoami` calls in arm A, 4-9% of all calls, asking a question the
    instrument kept answering with their own input.

    EVERY FIELD HERE IS MEASURED OR STATIC FACT, and `ai_tick` is the one that
    matters. It comes from `query_war_stance`, which resolves
    `g_get_ai_ctrl(character)` — the SAME per-character controller
    `set_character_ai` writes through (action_handlers.cpp:3477) — and reads
    `AI_ENABLED` and `IS_SUSPENDED` off it.

    I DELIBERATELY DID NOT USE `get_ai_evaluations`' `ai_controller.ai_enabled`,
    which looks like the same fact and is not: that one reads
    `core_data + AI_CONTROLLER`, a SINGLE global controller, so reporting it
    per-character would repeat the exact defect this block exists to fix with a
    different field name.

    Unreadable is `None` and says so. A block that guessed would be the third
    version of the same lie.
    """
    out: dict[str, Any] = {
        # Static facts about the surface, not advice about what to do with it.
        "armies_are_moved_by": ("the game's own AI for this character; it is "
                                "influenced by the personality weights in "
                                "posture, not commanded directly"),
        "you_act_directly_through": ["do", "declare_war", "intent", "set_phase",
                                     "steer", "session"],
        # The bridge moves the seat to the heir (session.cpp handoff); it refuses
        # only when the heir is the human player, and says so. This string used
        # to claim the opposite and Claude flagged the contradiction to the
        # human every session (persona test 2026-10-18).
        "at_your_death": ("the bridge moves this seat to the heir when the ruler dies; "
                          "whoami then shows the new ruler. If the heir is the human "
                          "player the seat is unbound (unbound_reason heir_is_player): "
                          "claim another ruler with session(action='control')"),
        "ai_tick": {"enabled": None, "suspended": None,
                    "measured": False,
                    "source": "query_war_stance (per-character ai_ctrl)",
                    "why_not": "not read"},
    }
    try:
        ws = await gw.read("query_war_stance", {"character_id": character_id})
        ws = ws if isinstance(ws, dict) else {}
        en, su = ws.get("ai_enabled"), ws.get("ai_suspended")
        if isinstance(en, bool):
            out["ai_tick"] = {"enabled": en,
                              "suspended": su if isinstance(su, bool) else None,
                              "measured": True,
                              "source": "query_war_stance (per-character ai_ctrl)",
                              "why_not": None}
        else:
            out["ai_tick"]["why_not"] = (
                "query_war_stance did not report ai_enabled for this character; "
                "an older bridge does not serve it")
    except Exception as exc:                                    # noqa: BLE001
        out["ai_tick"]["why_not"] = f"{type(exc).__name__}: {exc}"
    return out


def dejure_keys_for(realm: dict | None, hierarchy: dict | None = None) -> list[str]:
    """Title keys to report de-jure county progress for: the ones this seat HOLDS.

    ck-zgh4 deploy-51. `observe_impl` used to read exactly two meters,
    `k_ireland` and `e_britannia`, chosen by the harness for every seat in every
    arm — so an earl of Kent was shown its progress toward Ireland. Found by
    ck-9rto reading 9762aef4: that commit renamed the OUTPUT (`goal` ->
    `de_jure`) and left the objective in the INPUT, which is the half my own
    lane could not see because it asserted on key names.

    Keyed on what the seat holds, which is what a player's screen shows. NO
    FALLBACK LITERAL: an undetermined set returns empty and the caller reports
    the block unavailable, because substituting a guess is how the objective got
    in. Primary title first (query_realm primary_title_key), then the rest of the
    held titles from query_title_hierarchy(character_id)'s held_titles (each row's
    `key` and `tier`, emit_title_info), highest tier first, the bridge's order
    within a tier, de-duplicated.

    L15 (witness r4 N4-1): the rest came from query_realm's `titles`, which the
    bridge never writes, so every seat was told "this seat holds only one
    resolvable title" (l14i geval-p eval_e_names, Ludwig with 61 counties).
    """
    out: list[str] = []
    r = realm if isinstance(realm, dict) else {}
    pk = r.get("primary_title_key")
    if isinstance(pk, str) and pk:
        out.append(pk)
    h = hierarchy if isinstance(hierarchy, dict) else {}
    held = [t for t in (h.get("held_titles") or []) if isinstance(t, dict)]
    ranked = sorted(enumerate(held), key=lambda it: (-_as_int(it[1].get("tier")), it[0]))
    for _, t in ranked:
        k = t.get("key")
        if isinstance(k, str) and k and k not in out:
            out.append(k)
    return out


def hierarchy_was_read(hierarchy, realm) -> bool:
    """True when a query_title_hierarchy reply carries its held-titles list, the
    key do_query_title_hierarchy writes on success (query_handlers.cpp
    `r.key("held_titles")`); its failures write `error` and no list.

    L16 (ruling R9): the emitter no longer clamps. A title-array count outside
    [0, 500] answers `error: "title_count_clamped"` with `raw_count` (and a held
    title whose lookup fails answers `error: "title_lookup_failed"`), never an
    empty list (query_handlers.cpp title_count_clamped / do_query_title_hierarchy).
    L15 witness n6 stays as the second line of defence: an EMPTY list is not
    read when `realm` (the same seat's query_realm reply, which writes
    TITLE_ARRAY_COUNT as `held_title_count`) counts held titles: unread, never
    "holds none". Every reader of the list calls this, with the realm."""
    if not (isinstance(hierarchy, dict) and not hierarchy.get("error")
            and isinstance(hierarchy.get("held_titles"), list)):
        return False
    return bool(hierarchy["held_titles"]) or not (
        isinstance(realm, dict) and _as_int(realm.get("held_title_count")) > 0)


async def _dejure_pair(gw, cid: int, keys: tuple[str, ...]) -> list[dict[str, Any]]:
    """Both goal meters, ISSUED TOGETHER in one read_many.

    ck-p9vt. `observe`'s p50 was 50-52 s and this is where the time was: the
    two meters were read in sequence, each with a poll loop of up to seven
    reads. The two reads are at one dependency level (each takes only
    `character_id` and its own `title_key`), so they go out in one batch.

    L13: the client-side re-poll on a 0/0 tree is gone. It existed for one race
    (re_observe_meter_0_0.md): an IPC-thread read sub-milliseconds after a pause
    flip, while the tick in flight was still clearing-and-refilling the de-jure
    children vector. Every read now runs on the game thread between ticks, so a
    tick's recompute cannot be in flight under it.
    """
    batch = await gw.read_many([("query_dejure_progress",
                                 {"character_id": cid, "title_key": k}) for k in keys])
    return [g if isinstance(g, dict) else {} for g in batch]


# --------------------------------------------------------------------------- succession verdict
# One definition (cleanup 2026.11 L14, finding 8): succession_verdict.verdict, called
# by succession_plan, observe's succession block and the live-state hook.
import succession_verdict  # noqa: E402


async def _dead_actor_blocked(gw, actor_id: int, war_id: Optional[int] = None) -> dict[str, Any]:
    """The refusal for a call made from a dead character's seat: character_dead,
    the gate a dead target gets (retry never). The way out is named only from
    what a read carries: for a call about a war (war_id not None: war 0 is a
    real war), the war's leaders as query_wars reads them now, and the leader
    of the side the dead actor's id is listed on (its successor in the war). A
    dead character's own reply carries no heir (its titles have passed), so
    none is named. A war read that failed (no reply, a timeout's {}, an error,
    no war list) is war_read "failed", and a war missing from a degraded list
    is war_read "degraded": nobody looked, or the look could not see, is not
    "looked and it is not there" (war_listed False)."""
    summary = (f"character {actor_id} is dead: a call from his seat cannot act; "
               "act from a living seat (claim the heir, not the corpse)")
    measured = None
    if war_id is not None:
        raw = await gw.read("query_wars", {"all": True, "verbose": True})
        listed = raw.get("wars") if isinstance(raw, dict) and not raw.get("error") else None
        w = _war_pick([_war_view(x) for x in _war_rows(raw)], war_id) if isinstance(listed, list) else None
        if not isinstance(listed, list):
            measured = {"war_id": war_id, "war_read": "failed"}
        elif w is None and raw.get("degraded"):
            measured = {"war_id": war_id, "war_read": "degraded"}
        elif w is None:
            measured = {"war_id": war_id, "war_listed": False}
        else:
            side = ("attacker" if actor_id in w["attackers"] else
                    "defender" if actor_id in w["defenders"] else None)
            measured = {"war_id": war_id, "attacker_id": w["attacker_id"],
                        "defender_id": w["defender_id"], "dead_actor_side": side}
            lead = w["attacker_id"] if side == "attacker" else (
                w["defender_id"] if side == "defender" else None)
            if lead:
                summary = (f"character {actor_id} is dead; war {war_id}'s {side} leader now is "
                           f"{lead}: session(action='control', character_id={lead}) and ask from that seat")
            else:
                summary = (f"character {actor_id} is dead and is on neither side of war {war_id} as "
                           f"it reads now; its leaders are attacker {w['attacker_id']} and defender "
                           f"{w['defender_id']}: ask from the seat of the one on your side")
    return why_blocked("character_dead", summary=summary, measured=measured)


def _war_id_or_none(v) -> Optional[int]:
    """A war id as given, or None when none was (or it is not a number): never 0,
    which is war 0, a real war."""
    if v is None or isinstance(v, bool):
        return None
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


async def _resolve_actor(gw, character_id: int, tool: str, war_id: Optional[int] = None):
    """§5: resolve + validate the actor. Returns (actor_echo_dict, error_envelope|None).

    A call without character_id first answers the gateway's actor_gate()
    (actor_gate_of: actor_required, or no_ruler_bound for a seat holding
    nothing while other seats hold rulers). Otherwise it defaults to this seat's
    primary but validates alive+landed (character_dead; a call
    about a war passes its war_id so the refusal can name the war's leader)."""
    if not character_id:
        gate = await gw.actor_gate()
        if gate is not None:
            return None, envelope(tool, ok=False, blocked=gate,
                                  game=gw.game(), bridge_health=gw.health)
    raw_char = await gw.read("query_character", {"character_id": character_id})
    if not character_id:
        unbound = unbound_actor_block(raw_char)
        if unbound is not None:
            return None, envelope(tool, ok=False, blocked=unbound,
                                  game=gw.game(), bridge_health=gw.health)
    char = raw_char if isinstance(raw_char, dict) else {}
    # No player check here: query_character carries no is_player (build_char_profile),
    # so a read of it was always False (L15). The bridge refuses a write as the human
    # player itself (cannot_act_as_player), and that refusal reaches the seat.
    actor = actor_echo(char, character_id)
    if character_id and not actor.get("alive", True):
        return actor, envelope(
            tool, ok=False, actor=actor,
            blocked=await _dead_actor_blocked(gw, _as_int(character_id), _war_id_or_none(war_id)),
            game=gw.game(), bridge_health=gw.health)
    return actor, None


from actor_doors import ACTOR_PARAM_KEYS, about_the_bound_ruler  # noqa: E402,F401


async def unbound_refusal(gw, tool: str, action: str, bridge_params: dict) -> Optional[dict]:
    """THE unbound answer for a read about the bound ruler (L14 finding 6; FF1 m-20).

    Whether `action` with `bridge_params` asks about the bound ruler is the one
    declared table's answer (actor_doors.about_the_bound_ruler: the bridge handler
    falls back to the controlled character when no actor and no other subject is
    named), not the presence of a character_id key. Such a read, with none bound,
    answers _resolve_actor's own refusal (no_ruler_bound with `next`,
    actor_unresolved, actor_required), the one every actor-bearing tool answers.
    Any other door (a catalog, a feed, a read that names its ids) is answered.
    None: bound, or not about the bound ruler."""
    if not about_the_bound_ruler(action, bridge_params):
        return None
    _actor, err = await _resolve_actor(gw, 0, tool)
    return err


# --------------------------------------------------------------------------- observe


#: The get_active_wars keys an observe war row passes through, each only when
#: the reply carries it (is_attacker becomes the row's my_side).
#: test_observe_war_truth checks this set against the keys do_get_active_wars
#: emits per war.
_ACTIVE_WAR_PASSTHROUGH = ("war_id", "name", "attacker_id", "defender_id", "cb_type",
                           "target_titles", "war_score", "favored_side", "war_score_stale")

#: What an observe war row says about the fields it does not have.
_WAR_BREAKDOWN_NOT_READ = (
    "not read on this path (score source, battle and ticking scores, sieges); "
    'query(domain="wars") reads them')


async def observe_impl(gw, character_id: int = 0, depth: str = "brief") -> dict[str, Any]:
    if depth not in OBSERVE_DEPTHS:
        return envelope("observe", ok=False, game=gw.game(), bridge_health=gw.health,
                        blocked=why_blocked(
                            "invalid_argument",
                            summary=f"depth is one of {', '.join(OBSERVE_DEPTHS)}; got {depth!r}"))
    actor, err = await _resolve_actor(gw, character_id, "observe")
    if err:
        return err

    warnings: list = []
    errors: list = []

    # DE-JURE COUNTY COUNTS, for the titles THIS SEAT HOLDS. query_dejure_progress
    # is the meter; both reads go out together through _dejure_pair.
    #
    # ck-zgh4 deploy-51: the two keys were the LITERALS "k_ireland" and
    # "e_britannia" — the previous project's objective, shown to every seat in
    # every arm whatever it held. They now come from `dejure_keys_for`, and when
    # it yields nothing the block says so instead of falling back to a guess.
    _batch = await gw.read_many([
        ("query_realm",                {"character_id": character_id}),
        ("query_faction_threat",       {"character_id": character_id}),
        ("get_active_wars",            {"character_id": character_id}),
        ("query_character",            {"character_id": character_id}),
        ("query_title_succession_map", {"character_id": character_id}),
        # L14 (finding 8): the verdict's other two inputs, so observe states the
        # same passes_intact succession_plan does (succession_verdict.verdict).
        ("query_succession_simulation", {"character_id": character_id}),
        ("query_succession",           {"character_id": character_id}),
        # ck-p9vt: `get_ai_personality` was a span of its own on EVERY call,
        # 150 lines below. 93% of observe calls are depth="brief" (539 of
        # 582 over d45-d50), so it was on the hot path every time. Its params
        # are built from this function's argument and nothing between here and
        # its old site consumed `pers` — read, not assumed.
        ("get_ai_personality",         {"character_id": character_id}),
        # L15: the held titles for the second de-jure meter (dejure_keys_for).
        ("query_title_hierarchy",      {"character_id": character_id}),
    ])
    realm, ft, wars, chr, sp_raw, sim_raw, succ_raw, pers, hier = _batch
    realm = realm if isinstance(realm, dict) else {}
    ft = ft if isinstance(ft, dict) else {}
    wars = wars if isinstance(wars, dict) else {}
    chr_sect = _char_section(chr)

    # ── ck-p9vt: THE KEYS COME OUT OF THE BATCH ABOVE, NOT A READ OF THEIR OWN
    #
    # ck-zgh4 left this read for me to subsume and said so: its fix needed
    # `query_realm` to derive the held-title keys, and `query_realm` was ALREADY
    # in the batch — so the shipped shape paid a span for a reply it already
    # had. The batch is simply lifted above the meter block; nothing between the
    # two positions consumed a meter, which is why it moves safely. Verified by
    # reading the span, the way this file's other notes do it.
    _dj_keys = dejure_keys_for(realm, hier)
    _UNAVAIL_NONE = {"title": None, "have": None, "need": None, "total": None,
                     "fraction": None, "met": False,
                     "unavailable": "no held title key could be resolved for this seat"}
    _UNAVAIL_ONE = {"title": None, "have": None, "need": None, "total": None,
                    "fraction": None, "met": False,
                    "unavailable": "this seat holds only one resolvable title"}
    # L15 witness m3/n6: a FAILED or CLAMPED held-titles read is not a count. Only
    # a reply that carries `held_titles` (do_query_title_hierarchy), non-empty
    # whenever query_realm counts titles, says what the seat holds; otherwise
    # nobody looked, and the block says that instead of "only one".
    if not hierarchy_was_read(hier, realm):
        _err = hier.get("error") if isinstance(hier, dict) else None
        _UNAVAIL_ONE = dict(_UNAVAIL_ONE, unavailable=(
            (f"unread: the held-titles read (query_title_hierarchy) answered {_err}"
             + (f" (raw count {hier['raw_count']})" if "raw_count" in hier else "")
             + ", so the seat's other titles are unknown")
            if isinstance(_err, str) and _err else
            "unread: the held-titles read (query_title_hierarchy) failed or came back "
            "empty while query_realm counts held titles (clamped), so the seat's other "
            "titles are unknown"))
    # ONE span for both meters.
    if len(_dj_keys) >= 2:
        sub_goal, empire = await _dejure_pair(
            gw, character_id, (_dj_keys[0], _dj_keys[1]))
    elif len(_dj_keys) == 1:
        sub_goal = (await _dejure_pair(gw, character_id, (_dj_keys[0],)))[0]
        empire = dict(_UNAVAIL_ONE)
    else:
        # NO FALLBACK LITERAL, which is ck-zgh4's rule and the reason the
        # objective got in: an undetermined set reports unavailable.
        sub_goal, empire = dict(_UNAVAIL_NONE), dict(_UNAVAIL_NONE)
    sub_goal = sub_goal if isinstance(sub_goal, dict) else {}
    empire = empire if isinstance(empire, dict) else {}

    def _goal_block(g, need_frac=0.51):
        if g.get("unavailable"):
            # No title to measure: say why, and carry no counts (a 0 would read
            # as "you hold none of it").
            return dict(g)
        total = _as_int(g.get("total"))
        held = _as_int(g.get("held"))
        need = -(-int(total * need_frac) // 1) if total else 0  # ceil
        need = int(total * need_frac + 0.999) if total else 0
        return {"title": g.get("title_key"), "have": held, "need": need,
                "total": total, "fraction": round(_f(g.get("fraction")), 3),
                "met": held >= need and total > 0}

    sub_b = _goal_block(sub_goal)
    emp_b = _goal_block(empire)

    # kill_zone — ONE survival verdict, pre-correlated (axiom 2).
    #
    # ck-p3vd: THESE FOUR ARE ONE DEPENDENCY LEVEL. Each takes only
    # `character_id`, which is known before any of them, and none reads
    # another's reply — so awaiting them in turn bought nothing and cost one
    # flush period EACH. Measured sequential 20.01 s, issued together 9.83 s
    # (ck-zgh4, live). `read_many` issues them together.
    #
    # Same discipline as ck-abaa on the meter lane: phase by what is actually
    # dependent, not by the order someone wrote the lines in.
    # ck-zgh4 timed the shipped version at ~3 spans and found why: the
    # succession-map read sat OUTSIDE this batch, on its own. VERIFIED before
    # moving it rather than assumed — its params are
    # {"character_id": character_id}, built from this function's argument and
    # not from any reply in the batch, and nothing between the two consumes a
    # batch result to construct them. Same dependency level; it was paying a
    # span for nothing.
    #
    # NOTE FOR ANYONE EDITING THIS: `succession_plan_impl` issues the SAME read
    # with the same literal params. It has no batch, and an unanchored
    # search-and-replace across this file WILL hit it.

    # Gold, the income and its breakdown are query_character's (build_char_profile,
    # query_handlers.cpp); query_realm carries none of them (L15: observe read them off
    # query_realm and reported monthly_income null and "no income_breakdown" for every
    # seat, l14i geval-p eval_e_names).
    gold = _f(chr_sect.get("gold"))
    debt = -gold if gold < 0 else 0

    # ── ck-m3xd: THE MONEY, AND WHAT IT CANNOT SAY ──────────────────────────
    #
    # MEASURED over d45-d50, 1,983 observations carrying `income_breakdown`:
    #   1,952 (98.4%)  EVERY component is zero -- gross_domain, net_domain,
    #                  total_expenses and net_gold -- while `total_monthly`
    #                  carries a real number. The seat is shown a breakdown
    #                  that breaks nothing down, and four zeros read as "no
    #                  expenses" rather than as "not populated".
    #      761 (38%)   `income_breakdown.total_monthly` DISAGREED with
    #                  `monthly_income`. L15 A3 found why: query_character's
    #                  `monthly_income` was core_data+0x170, the MERIT stock (loc
    #                  key TOTAL_MERIT; re/findings/re_core_data_merit_not_income.md),
    #                  not income at all. The bridge now emits it as `merit`, and
    #                  money.monthly_income IS income_breakdown.total_monthly
    #                  (core_data+0x2B0, MEDIUM): one field, nothing to disagree.
    #
    # THE LIMIT, STATED RATHER THAN PAPERED OVER. The design asked for
    # "income/expense lines that sum". THERE IS NO EXPENSE SIDE IN THIS DATA:
    # `total_expenses` is 0 in every one of the 1,983 observations. So a
    # reconciliation cannot be computed, and inventing one -- deriving expenses
    # as income minus a delta, say -- would be a fabricated line on a screen
    # the seat is meant to trust. What this block does instead is report the
    # figures that are real and SAYS what is missing. A field that cannot be sourced is absent with a reason, never a
    # zero: four zeros are indistinguishable from a realm with no costs.
    _bd = chr_sect.get("income_breakdown")
    _bd = _bd if isinstance(_bd, dict) else {}
    _tm = _bd.get("total_monthly")
    _components = ("gross_domain", "net_domain", "total_expenses", "net_gold")
    _populated = [k for k in _components
                  if isinstance(_bd.get(k), (int, float)) and _bd.get(k) != 0]
    money: dict[str, Any] = {
        "gold": round(gold, 2),
        "in_debt": gold < 0,
        "monthly_income": round(_f(_tm), 2) if isinstance(_tm, (int, float)) else None,
    }
    if not _bd:
        money["breakdown"] = None
        money["breakdown_absent"] = "the bridge published no income_breakdown"
    elif not _populated:
        # The 98.4% case. Do NOT pass the zeros through.
        money["breakdown"] = None
        money["breakdown_absent"] = (
            "income_breakdown was present but every component "
            f"({', '.join(_components)}) was zero — unpopulated, not a realm "
            "with no costs. Measured on 98.4% of 1,983 observations across "
            "d45-d50.")
    else:
        money["breakdown"] = {k: round(_f(_bd.get(k)), 2) for k in _populated}
        money["breakdown_partial"] = sorted(set(_components) - set(_populated))
    money["expenses"] = None
    # The hub asked for this sentence to live IN THE REPLY, not only in a
    # commit message, so a seat cannot read the absence as zero spending.
    money["expenses_absent"] = (
        "NO EXPENSE FIGURE EXISTS IN THIS DATA — this is not a statement that "
        "spending is zero. total_expenses was 0 in every one of 1,983 "
        "observations measured across d45-d50, i.e. unpopulated rather than "
        "measured, so income minus expenses cannot be computed and is not "
        "guessed at here. Treat your balance as unknown on the expense side.")
    stress = _f(chr_sect.get("stress"))
    faction_max = max([_f(x.get("power_pct_of_liege")) for x in (ft.get("factions") or [])], default=0.0)
    at_war = bool(wars.get("wars"))
    age = _f(chr_sect.get("age"))
    # O5 fallback: no engine death-ETA → age heuristic, with a warning.
    eta = succession_eta_years(age)
    g = gw.game()
    speed = _as_int(g.get("speed"))
    speed_safe = not (speed > SPEED_MAX and (at_war or (eta is not None and eta <= 3)))

    reasons: list = []
    if debt > 0:
        reasons.append(why_blocked("insufficient_gold", summary=f"in debt {int(debt)}"))
    if faction_max >= 80:
        reasons.append(why_blocked("faction_power_tripwire", summary=f"faction power {int(faction_max)}% (>=80 tripwire)"))
    if stress >= STRESS_BREAK_RISK:
        reasons.append(why_blocked("stress_break_risk", summary=f"stress {int(stress)} (mental break risk)"))
    in_trough = bool(reasons) or (at_war and eta is not None and eta <= 3)

    # ck-zgh4 deploy-51: `kill_zone` was a single survival VERDICT named after
    # the previous project's doctrine ("the trough"). The facts it was computed
    # from are what a ruler can see in the game; the verdict is a conclusion
    # drawn for the seat. Facts stay, under a name that states what they are.
    standing = {
        "succession_eta_years": eta,
        **({"succession_eta_basis": SUCCESSION_ETA_BASIS} if eta is not None else {}),
        "faction_max_pct": int(faction_max),
        "debt": int(debt), "gold": int(gold),
        "at_war": at_war,
        "stress": int(stress),
        "speed_safe": speed_safe,
    }

    # can_mint_now — sub_met && at_peace && gold (the explicit mint signal).
    can_mint = sub_b["met"] and not at_war and gold >= 0
    mint_requirements = None
    if not can_mint:
        if sub_b.get("unavailable"):
            mint_requirements = why_blocked(
                "de_jure_control",
                summary=f"no de-jure title to measure: {sub_b['unavailable']}")
        elif not sub_b["met"]:
            mint_requirements = why_blocked(
                "de_jure_control",
                summary=f"hold {sub_b['have']}/{sub_b['need']} de-jure counties of {sub_b['title']}",
                measured={"held": sub_b["have"], "need": sub_b["need"],
                          "total": sub_b["total"], "fraction": sub_b["fraction"]},
                fix={"tool": "plan_war",
                     "hint": "minting requires de-jure counties of this title; "
                             "plan_war enumerates which are held and by whom"})
        elif at_war:
            mint_requirements = why_blocked("not_at_peace", summary="must be at peace to mint a title")
        elif gold < 0:
            mint_requirements = why_blocked("insufficient_gold", summary="in debt — cannot afford the mint cost")

    # ck-zgh4 deploy-51: NO OBJECTIVE. This block used to carry
    # `"active": "e_britannia"` — the previous project's goal — and every one of
    # d50's six transcripts contained it, handed over by this field on every
    # cycle. The registered BOOT.md contains the string ZERO times, so §3.1's
    # "No strategy is prescribed to you" was honoured by the prompt and
    # contradicted by the instrument. The FACTS stay; the aim does not.
    de_jure = {
        # ck-zgh4 deploy-51: RENAMED, because the old names became false when the
        # meters stopped being the objective's two titles. `nearest_title` and
        # `widest_title` described a sub-goal and an empire — a nearness and a
        # width that existed only relative to e_britannia. They are now simply
        # the seat's first and second resolvable HELD titles, in
        # `dejure_keys_for`'s order, so they are named that. Flagged by ck-9rto
        # after it took the block: the names "read a little oddly" for what they
        # now hold, which is the polite version of "they are inaccurate".
        "primary_title": sub_b,
        "second_title": emp_b,
        "can_mint_now": can_mint,
        "mint_requirements": mint_requirements,
    }

    # ── ck-w4tq: THE WAR, ON THE PATH THE SEAT ACTUALLY TAKES ───────────────
    #
    # THE HARM, in the agents' own words. Three kings and an earl reported a
    # stalemate for YEARS on the strength of `war_score: 0` while the Normans
    # took England:
    #   d46 king-england 01:43:18Z "For 18 months I reported a 'dead-even
    #     stalemate' on the strength of war_score: 0 ... [it] never credited
    #     William's occupation."
    #   d45 king-england 15:03:27Z "trusting a war_score and occupation_score
    #     frozen at zero — while levy_own_domain_max fell 3,073 -> 2,718 ->
    #     2,576."
    #   d49 earl-kent 16:36:33Z "war_id 1 read 0-0, 'even' at every check."
    #
    # WHY THEY COULD NOT SEE IT. `war_brief` and `_war_stance_view` are both
    # inside `depth == "full"`, and 93% of observe calls are `brief` (539 of
    # 582, measured over d45-d50). So the war reached the seat as ONE BOOLEAN,
    # `standing.at_war`, on the path it actually took. The information existed;
    # it was off the hot path.
    #
    # COSTS NO SPAN. Every field below is read from the `get_active_wars` reply
    # fetched by the batch above; nothing new is read. That reply
    # (query_handlers.cpp do_get_active_wars) carries war_id, name (cleaned by
    # engine_text.h), attacker_id, defender_id, is_attacker, and war_score with
    # favored_side only when the breakdown cache is fresh (war_score_stale when
    # it holds an old entry, nothing when it holds none). my_side is the
    # reader's side from is_attacker (the war-ref array the reply walks lists
    # only wars the character fights, so not-attacker is defender).
    #
    # A ROW CARRIES WHAT THE REPLY CARRIES, AND NOTHING IT DOES NOT. This path
    # does not read the war's breakdown (score source, battle and ticking
    # scores, sieges): query_wars does, through emit_war_entry, whose siege
    # placement walks the whole landed-title table (32768 slots in the 867
    # dump) and SIEGE_DB (1024) on the first read of a tick. get_active_wars is
    # bridge-internal (served on the IPC thread) on every observe, so it stays
    # the cheap read.
    # Until cleanup 2026.11 the row filled the unread breakdown in as 0 / None /
    # no sieges, and because the source was always absent, every war at a
    # score of 0 was flagged "do not read 0 as parity" on evidence nobody had
    # read. Now an unread field is absent from the row, and the row says once
    # where the breakdown is read.
    wars_list = wars.get("wars") if isinstance(wars.get("wars"), list) else []
    war_rows = []
    for w in wars_list:
        if not isinstance(w, dict):
            continue
        row = {key: w[key] for key in _ACTIVE_WAR_PASSTHROUGH if key in w}
        if isinstance(w.get("is_attacker"), bool):
            # the reader's side; a row that does not say gets no side (never a guess)
            row["my_side"] = "attacker" if w["is_attacker"] else "defender"
        row["breakdown"] = _WAR_BREAKDOWN_NOT_READ
        war_rows.append(row)
    war_block = {"count": len(war_rows), "active": war_rows}
    if wars_list and not war_rows:
        # The list was present and nothing in it was readable. Say so rather
        # than returning count 0, which reads as "no wars".
        war_block = {"count": None, "active": [],
                     "unreadable": "get_active_wars returned entries this "
                                   "reader could not parse"}

    # SUCCESSION watch (ck-succprev): a CONCISE per-title-partition summary folded in so the
    # agent SEES at a glance whether the NEXT succession SCATTERS titles or passes to one
    # heir — the death-spiral leading indicator (drive eligible-heir count toward 1 BEFORE
    # the trough). Pure crash-hardened READ; degrades to available=False on an older bridge.
    # (ck-p3vd: the read itself moved UP into the batch above; only the
    # interpretation stays here.)
    msum = succession_verdict.map_summary(sp_raw if isinstance(sp_raw, dict) else {})
    sv = succession_verdict.verdict(sim_raw, succ_raw, msum)
    succession_block = {
        "outcome": sv["outcome"],
        **({"undetermined_reason": sv["undetermined_reason"]}
           if sv["outcome"] == "undetermined" else {}),
        "passes_intact": sv["passes_intact"],
        "breaker": sv["breaker"],
        "eligible_heir_count": sv["eligible_heir_count"],
        "primary_heir": {"id": sv["primary"].get("id"), "name": sv["primary"].get("name")}
                        if sv["primary"] else None,
        "heirs_source": sv["heirs_source"],
        "succession_eta_years": eta,
        "near_trough": bool(eta is not None and eta <= 3),
    }
    if sv["outcome"] == "undetermined":
        warnings.append(succession_verdict.warning(sv))
    elif sv["outcome"] == "splits":
        # The cycle loop surfaces this: act to reduce the heirs who land titles.
        warnings.append({"code": "succession_not_intact", "is_known_limitation": False,
                         "detail": "the next succession does not pass intact: "
                                   f"{sv['eligible_heir_count']} heirs land titles — drive that "
                                   "toward 1 BEFORE the trough (single_heir law / land spares as "
                                   "in-realm vassals / consolidate). succession_plan has the levers."})

    # posture
    # `pers` came from the batch above (ck-p9vt); no span of its own.
    # L18 ruling (b): posture.genome.lock_state, the bridge's word (lock_state_of).
    genome_read = dict(pers) if isinstance(pers, dict) else {}
    genome_read["lock_state"] = lock_state_of(genome_read)
    posture = {"genome": genome_read}

    result: dict[str, Any] = {"de_jure": de_jure, "standing": standing,
                              "wars": war_block, "money": money,
                              "succession": succession_block, "posture": posture}

    if depth == "full":
        result["self"] = {
            "counties_held": (_as_int(sub_b["have"])
                              + max(0, _as_int(emp_b["have"]) - _as_int(sub_b["have"]))),
            # query_realm's held_title_count (do_query_realm): every title held, all tiers.
            "titles_held": _as_int(realm.get("held_title_count")),
            "gold": int(gold), "prestige": int(_f(chr_sect.get("prestige"))),
            "piety": int(_f(chr_sect.get("piety"))), "stress": int(stress),
            "faith": chr_sect.get("faith"),
            "culture": chr_sect.get("culture"),
            # L14 FF2 N-14: the scripted keys beside the display names, as
            # query_character sends them (emit_char_names, dispatch_router.cpp);
            # doctrine teaches the keys (britannia-campaign.md). Absent read = null.
            "faith_key": chr_sect.get("faith_key"), "culture_key": chr_sect.get("culture_key"),
        }
        result["war_brief"] = wars.get("wars", [])
        # war_stance brief (item 3): surface stuck offensive wars (occupation 0 / losing)
        # so the agent abandons them via white-peace instead of bleeding its army across a
        # succession. Only the stalled-war shortlist + the flag — the full per-war view is
        # inspect(subject="war_stance"). Synthesized read (crash-safe).
        if wars.get("wars"):
            stance = await _war_stance_view(gw, character_id)
            result["war_stance_brief"] = {
                "any_stalled": stance.get("any_stalled", False),
                "stalled": stance.get("stalled", []),
                "wars": stance.get("wars", []),
            }
        council = await gw.read("query_council", {"character_id": character_id})
        result["council_brief"] = (council.get("council") if isinstance(council, dict) else council) or []
        # The AI's evaluations (the section the retired legacy observe carried;
        # D14 merged it into this one).
        evals = await gw.read("get_ai_evaluations", {"character_id": character_id})
        result["evaluations"] = evals if isinstance(evals, dict) else {}

    # ── ck-s8vr: `since` IS ABOUT THE SEAT, AND SAYS WHAT IT WITHHELD ───────
    #
    # MEASURED over d49+d50: `delta` is 75.5% of observe's BYTES. Of its
    # 5,640 entries, 14 (0.25%) mention the actor and 100% have empty names —
    # they are Python-repr dicts of foreign wars and schemes
    # ("war: {'tick': 61, 'attacker_id': 30966, ...}") because
    # the old string delta fell back to str(rec) when a record had no
    # `summary` or `name`, which the bridge's war and scheme records never do.
    #
    # TWO FIXES, AND THE SECOND IS THE ONE THAT MATTERS.
    # First, read RECORDS (`delta_records_since_last`) so the thing can be
    # filtered at all; a consumer cannot filter what it cannot parse.
    # Second, and this is ck-zgh4's point and it is right: A FILTER THAT DOES
    # NOT SAY WHAT IT DROPPED IS A CLAIM, NOT A FACT. Returning the 14 entries
    # that touch the seat, silently, asserts that 14 is all there was. So
    # `withheld` carries the count and the kinds, and the seat can ask for more
    # by other means. An absence that is counted is evidence; an absence that
    # is invisible is a lie with good manners.
    #
    # WHAT COUNTS AS MINE, stated rather than inferred: the seat's own
    # character id appearing in any id-shaped field of the record. Nothing
    # fuzzy, no name matching — a numeric field equal to character_id. A
    # record whose fields this reader does not recognise is counted as foreign
    # AND included in `unclassified`, because "I could not tell" is not "not
    # about you".
    _recs = await gw.delta_records_since_last()
    # ── ck-s8vr: "MINE" IS THE REALM, NOT JUST THE CHARACTER ────────────────
    #
    # The design's standard is that foreign events appear only when they
    # involve your realm, your liege or your neighbours. Keying on the seat's
    # own character id alone withholds a war against the seat's LIEGE or one of
    # its VASSALS as "foreign", which is precisely the kind of thing a player
    # sees on their own screen.
    #
    # Ids are taken from replies already in the batch: the seat, its vassals
    # (realm.vassals[].id), and its liege and top liege where the character
    # payload carries them. `matched_on` publishes the REACH, so a seat can
    # tell "nothing about my realm happened" from "this filter could not see my
    # liege" — the same reason `withheld` publishes a count. A reach that is
    # not stated is a filter whose silence cannot be read.
    # The id sets come from replies ALREADY in the batch (query_character emits
    # liege_id/top_liege_id via build_char_profile, query_handlers.cpp:791-796),
    # so widening the reach costs no extra read.
    #
    # The two liege keys do NOT share a zero meaning, and the bridge's own
    # comments say so, so they are not read with one condition:
    #   liege_id == 0      -> independent/unlanded (observation.cpp:834). A real
    #                         ANSWER: there is no liege. Not a gap.
    #   top_liege_id == 0  -> "the walk could not be made" (ck-6kvq.27,
    #                         query_handlers.cpp:793). UNREADABLE. A gap.
    #   top_liege_id == me -> an independent character walks to their OWN id.
    #                         Also an answer, and already in the set.
    # Collapsing those into one "unreachable" bucket would report a blind filter
    # to every independent ruler in the game, which is most of the interesting
    # ones. `reach` names the state per key instead of defaulting.
    _seat_ids = {character_id} if isinstance(character_id, int) else set()
    _matched_on = ["character_id"] if _seat_ids else []
    _vassals = realm.get("vassals") or []
    _vass_ids = {v["id"] for v in _vassals
                 if isinstance(v, dict) and isinstance(v.get("id"), int)}
    _seat_ids |= _vass_ids
    if _vass_ids:
        _matched_on.append(f"vassals({len(_vass_ids)})")

    _reach: dict = {}
    for _lk in ("liege_id", "top_liege_id"):
        _lv = chr_sect.get(_lk)
        if not isinstance(_lv, int):
            _reach[_lk] = "absent_from_payload"      # nobody emitted it
        elif _lv == 0:
            # Same spelling, two questions -- hence the per-key branch.
            _reach[_lk] = ("walk_failed" if _lk == "top_liege_id"
                           else "independent_no_liege")
        elif _lv == character_id:
            _reach[_lk] = "self_independent"
        else:
            _seat_ids.add(_lv)
            _matched_on.append(_lk)
            _reach[_lk] = "matched"

    _mine, _foreign, _unclassified = [], [], 0
    _ID_FIELDS = ("attacker_id", "defender_id", "character_id", "actor_id",
                  "target_id", "owner_id", "schemer_id", "victim_id")
    for rec in (_recs or []):
        if not isinstance(rec, dict):
            _unclassified += 1
            continue
        ids = [rec.get(k) for k in _ID_FIELDS]
        known = [v for v in ids if isinstance(v, int)]
        if not known:
            _unclassified += 1
            _foreign.append(rec)
            continue
        (_mine if _seat_ids.intersection(known) else _foreign).append(rec)
    _kinds: dict = {}
    for rec in _foreign:
        k = rec.get("kind") if isinstance(rec, dict) else "?"
        _kinds[k] = _kinds.get(k, 0) + 1
    since = {
        "mine": _mine,
        # THE REACH, published. A filter whose scope is unstated has a silence
        # nobody can read.
        # THE REACH, published per key. A filter whose scope is unstated has a
        # silence nobody can read: "nothing involving my realm happened" and
        # "this reader could not see my liege" are different facts.
        "matched_on": _matched_on,
        "reach": _reach,
        "withheld": {
            "count": len(_foreign),
            "by_kind": _kinds,
            "unclassified": _unclassified,
            "note": ("entries naming none of the ids under `matched_on` in "
                     "any id field. Counted so a reader can tell a quiet period "
                     "from a filtered one; `unclassified` are entries this "
                     "reader could not classify and are counted as withheld "
                     "rather than as yours"),
        },
    }
    # The old field keeps its name and its shape for any reader that still
    # expects a flat list, and it is now THE SEAT'S entries rather than
    # everyone's. The foreign traffic is behind `since.withheld`, not leading.
    delta = _mine
    # ck-s8vr: attached to RESULT, not the envelope. §1.1's envelope has a
    # fixed keyword signature and is registered; `result` is the tool's own
    # payload. Attached here rather than listed in the literal above because
    # the block needs a gateway read and is therefore computed after it.
    result["since"] = since
    return envelope("observe", result=result, actor=actor, game=gw.game(),
                    delta=delta, warnings=warnings, errors=errors, bridge_health=gw.health)


# --------------------------------------------------------------------------- CB options
# inv_gamecb.md dispatch-marshalling: which CBs need a cb_title_id vs a claimant.
# target_mode 0 = no title (subjugation/vassalization); 6 = claim (claimant + title);
# everything else is title-targeted (REQUIRES cb_title_id).
_CB_NO_TITLE_MODES = {0}
_CB_CLAIM_MODES = {6}

# CB families that are documented NOT to dispatch cleanly via the steered WCC
# (inv_bridge.md §3 / inv_gamecb.md): great_holy_war is religious-head-lifecycle-only.
_CB_UNSUPPORTED_SUBSTR = ("great_holy_war",)

# FIXPLAN A3 (CB-audit G5): de-jure CB family detector. The real engine names are the
# take-all `de_jure_cb` and the per-title `individual_county_de_jure_cb` /
# `individual_duchy_de_jure_cb` (cross-checked against bridge cb_semantics_for). The
# `de_jure` substr captures all three (and any future de-jure variant) in one test.
#: Mode-0 CBs named outside the subjugation/vassalization substrings.
#: make_tributary_cb is target_mode 0 (re/findings/re_conquest_subjugation_cb_dispatch.md
#: §2, idx 49) and its live war rows carry target_titles [] (l15p2 gl5-p/gl5-r
#: l5_war_outcomes.json; L15 witness n5).
_CB_MODE0_NAMES = frozenset({"make_tributary_cb"})


def _is_mode0_cb(cb_name) -> bool:
    """True iff a CB takes NO title and NO claimant (target_mode 0): the one
    name test declare_war marshals and matches by."""
    low = str(cb_name or "").lower()
    return "subjugation" in low or "vassalization" in low or low in _CB_MODE0_NAMES


def _is_de_jure_cb(cb_name) -> bool:
    """True iff a CB key belongs to the de-jure family (de_jure_cb / individual_*_de_jure_cb)."""
    return "de_jure" in str(cb_name or "").lower()

#: Stress lines, one definition for observe and plan_war. The game adds a stress
#: level every 100 stress (common/defines STRESS_PER_LEVEL, MAX_STRESS_LEVEL 3) and a
#: mental break comes with a new level: CLIMBING is one level, BREAK_RISK is half way
#: to the second.
STRESS_CLIMBING = 100
STRESS_BREAK_RISK = 150

#: L18-B: the succession ETA is 65 minus the ruler's age; no engine estimate is read.
#: observe and succession_plan used to say so as a `war_duration_unavailable` warning on
#: EVERY reply (391 of 392 pilot observes, at war or not, under a name about wars), a
#: constant that told a seat nothing new after the first read. The basis is now stated
#: once, on the field it qualifies.
SUCCESSION_ETA_BASIS = "age_heuristic: 65 minus the ruler's age (the game gives no estimate)"


def succession_eta_years(age) -> Optional[int]:
    """The one succession-ETA rule (observe, succession_plan)."""
    return max(0, int(65 - _f(age))) if age else None

#: A ruler this old is near enough to death that a war he opens now is expected to
#: run across his succession: plan_war marks every row straddles_succession and
#: declare_war asks for confirm_straddle. One number for both.
STRADDLE_AGE = 62


def _affordability(opt: Optional[dict]) -> dict[str, Any]:
    """The cost columns of one cbs option (the bridge's base declaration cost for the
    CB family, an estimate: the game scales the real price by target)."""
    opt = opt or {}
    affordable = opt.get("affordable")
    cp, cpi = opt.get("cost_prestige"), opt.get("cost_piety")
    hint = None
    if affordable is False:
        hint = (f"a {opt.get('cb')} war costs about {cp or 0} prestige and {cpi or 0} piety "
                "(base cost; the game scales it by target) and you hold less")
    return {"affordable_now": affordable, "cost_prestige": cp, "cost_piety": cpi,
            "estimate": True, "hint": hint}


def is_claim_cb_name(cb: Any) -> bool:
    """A claim CB by its key (claim_cb, *_claim_cb): the CB takes a claimant. One
    definition for plan_war's row gate and declare_war's marshalling."""
    cb_lower = str(cb or "").lower()
    return cb_lower == "claim_cb" or cb_lower.endswith("_claim_cb")


def row_needs_claimant(chosen: Optional[dict], cb_name: Any) -> bool:
    """Does a plan_war row's CB need a claimant? The cbs option's own
    needs_claimant (target_mode 6) when one was read, else the CB's key."""
    if chosen is not None:
        return bool(chosen.get("needs_claimant"))
    return is_claim_cb_name(cb_name)


def _cb_dispatch_shape(cb_row: dict) -> dict[str, Any]:
    """Translate one query_cbs_for_target CB row → the dispatch requirements an agent
    needs to marshal a declare_war declare_args (inv_gamecb.md per-CB call table)."""
    mode = _as_int(cb_row.get("target_mode"))
    key = str(cb_row.get("cb_key", ""))
    needs_title = mode not in _CB_NO_TITLE_MODES
    needs_claimant = mode in _CB_CLAIM_MODES
    unsupported = any(s in key for s in _CB_UNSUPPORTED_SUBSTR)
    return {"needs_cb_title_id": needs_title, "needs_claimant": needs_claimant,
            "dispatch_supported": not unsupported}


def _shape_cb_options(cbs_raw: dict, *, target_id: int) -> tuple[list, list]:
    """Shape a query_cbs_for_target response into (valid_options, all_options) lists,
    each row carrying scope/grants/tradeoffs + the dispatch-marshalling hint so a
    declare_args can pick the best CB. Returns ([], []) on a degraded/empty read."""
    valid_opts: list = []
    all_opts: list = []
    for cb in (cbs_raw.get("cbs") or []):
        shape = _cb_dispatch_shape(cb)
        opt = {
            "cb": cb.get("cb_key"),
            "valid": bool(cb.get("valid")),
            "scope": cb.get("scope"),           # county|duchy|kingdom|realm|faith_counties
            "grants": cb.get("grants"),         # human-readable territory grant
            "requirements_unmet": cb.get("requirements_unmet") or [],
            "tradeoffs": cb.get("notes") or [],
            "target_mode": _as_int(cb.get("target_mode")),
            "needs_cb_title_id": shape["needs_cb_title_id"],
            "needs_claimant": shape["needs_claimant"],
            "dispatch_supported": shape["dispatch_supported"],
            # FIXPLAN A3: carry the engine-authoritative per-CB title id through shaping so
            # a de_jure (or any title-targeted) row can marshal the CORRECT cb_title_id
            # without a second cbs/strict call. Previously dropped → a de_jure declare_args
            # had no title to point at. Additive field; existing consumers ignore it.
            "cb_title_id": _as_int(cb.get("cb_title_id") or cb.get("title_id")),
            # L18-B: a claim CB's claimant, as the cbs emitter names it beside the
            # pressed claim's title (query_handlers.cpp emit_cbs_target_body:
            # r.key("claimant_id") when cb_title_id is the pressed claim's title).
            "claimant_id": _as_int(cb.get("claimant_id")) or None,
            # the CB's family (invasion, conquest, holy_war, claim, de_jure,
            # subjugation, vassalization, great_holy_war, other): bridge cb_family()
            "family": cb.get("family"),
            # the bridge's base declaration cost for the CB family and whether the
            # actor's prestige/piety covers it. The game scales the real price by
            # target (counties taken, tier), so this is an estimate.
            "cost_prestige": cb.get("cost_prestige"),
            "cost_piety": cb.get("cost_piety"),
            "affordable": cb.get("affordable"),
        }
        all_opts.append(opt)
        if cb_option_declarable(opt):
            valid_opts.append(opt)
    return valid_opts, all_opts


def cb_option_declarable(opt: dict) -> bool:
    """A shaped cbs option a player can declare: the game's check accepts it and
    declare_war can send it."""
    return bool(opt.get("valid")) and bool(opt.get("dispatch_supported"))


def cb_options_by_family(all_opts: list) -> dict:
    """cbs' by_family: every family among the options, each with the CBs a player
    can declare now (cb_option_declarable), sorted."""
    return {fam: sorted(o["cb"] for o in all_opts
                        if cb_option_declarable(o) and o.get("family") == fam)
            for fam in sorted({o.get("family") or "other" for o in all_opts})}


# --------------------------------------------------------------------------- CB busy-retry
# A CB-validity / cbs-for-target read can come back TRANSIENTLY busy: the bridge's
# single-flight g_cb_query slot is shared with the engine's OWN AI CB evaluations,
# and the engine's concurrent eval can hold the slot → the bridge returns
# {status:"busy", error:"cb_query_busy"} (or a bare cb_query_busy/cb_query_timeout
# error). The bridge already rides out most of these with bounded retry + stale-slot
# reclaim, but a hot run can still surface a transient collision. Treating that as a
# hard "degraded" makes the agent HOLD and miss expansion (the #95 class) — so we
# RETRY a few times with a short backoff before surfacing it, and we DISTINGUISH a
# genuinely-empty drained result (no_valid_targets) from a tooling-busy failure.

# Bridge status/error tokens that mean "couldn't get a clean slot — retry", NOT
# "no valid CBs". Kept narrow so a real verdict is never mistaken for busy.
_CB_BUSY_TOKENS = ("cb_query_busy", "cb_query_timeout", "busy_deferred", "busy")


def _cb_read_is_busy(res: dict) -> bool:
    """True iff a CB read came back transiently busy (slot collision / drain timeout),
    as opposed to a real verdict (ok with 0 rows = genuinely no valid CBs)."""
    if not isinstance(res, dict) or not res:
        # An empty {} is the gateway's timeout/connection-drop fallback — treat as
        # transient (retryable), not as an authoritative "no targets".
        return True
    status = str(res.get("status", "")).lower()
    err = str(res.get("error", "")).lower()
    if status in ("ok", "bilateral_filter_reject"):
        return False  # a real verdict
    if status in ("busy", "timeout", "busy_deferred"):
        return True
    return any(tok in err for tok in _CB_BUSY_TOKENS)


async def _cb_read_with_retry(gw, action: str, params: dict,
                              *, attempts: int = 3, backoff_s: float = 0.4) -> dict:
    """gw.read(action, params) with bounded retry-on-busy. Rides out a
    transient g_cb_query slot collision (engine AI vs our query) before giving up,
    so a single hot-tick contention does not make the agent fail-closed and HOLD.
    Returns the last response dict (its status/error tells the caller busy vs real)."""
    res: dict = {}
    for i in range(max(1, attempts)):
        raw = await gw.read(action, params)
        res = raw if isinstance(raw, dict) else {}
        if not _cb_read_is_busy(res):
            return res
        if i + 1 < attempts:
            await asyncio.sleep(backoff_s)
    return res  # exhausted retries — last (still-busy) response surfaced to caller


async def _cbs_for_targets_batched(gw, character_id: int, target_ids: list[int]) -> Optional[dict]:
    """OPTIONAL batched CB-options fetch: ONE query_cbs_for_targets round-trip for ALL
    targets, when the bridge serves it. Returns {target_id: cbs_raw_dict} on success, or
    None when the handler is unserved (graceful fallback to per-target reads). The
    bridge handler must be crash-hardened identically to query_cbs_for_target (safe_read/
    is_mapped, same g_cb_query single-flight discipline, no name-cache drain); until it
    lands this probes once and returns None, so behavior is UNCHANGED on shipping builds."""
    if not target_ids:
        return {}
    raw = await gw.read("query_cbs_for_targets",
                        {"attacker_id": character_id,
                         "target_ids": list(target_ids), "timeout_ms": 9000})
    raw = raw if isinstance(raw, dict) else {}
    err = str(raw.get("error") or raw.get("reason") or "").lower()
    if any(tok in err for tok in _UNKNOWN_CMD_TOKENS):
        return None
    # do_query_cbs_for_targets writes its rows under `results` (a list, each with
    # target_id); `targets`/`by_target` were never written (L15 witness n4).
    rows = raw.get("results")
    if rows is None:
        # Not a recognized batched shape (and not a clean busy/empty for THIS schema) →
        # treat as unserved so the caller falls back per-target. Never invents data.
        if not (raw.get("status") or raw.get("cbs")):
            return None
    out: dict = {}
    if isinstance(rows, list):
        for entry in rows:
            if isinstance(entry, dict):
                out[_as_int(entry.get("target_id"))] = entry
    return out


async def _cb_options_for(gw, character_id: int, target_id: int) -> tuple[list, list, dict]:
    """Fetch + shape the CB tradeoff list for ONE (actor,target). query_cbs_for_target
    is serviced on the game thread (g_cb_query slot), like query_cb_validity_strict;
    gw.read single-flights it. Returns (valid_options, all_options, target_state)."""
    cbs_raw = await _cb_read_with_retry(gw, "query_cbs_for_target",
                                        {"attacker_id": character_id, "target_id": target_id,
                                         "timeout_ms": 6000})
    cbs_raw = cbs_raw if isinstance(cbs_raw, dict) else {}
    valid_opts, all_opts = _shape_cb_options(cbs_raw, target_id=target_id)
    return valid_opts, all_opts, (cbs_raw.get("target_state") or {})


# --------------------------------------------------------------------------- war stance / lifecycle
# FIXPLAN A4 doc-reconcile (was a STALE-comment dead integration): the bridge DOES ship a
# query_war_stance handler (re_query_war_stance.md, ck-rw9m — the CAIWarStance-DB record
# walk: ai_mgr_root+0x48 / count +0x54, per-record stance ptr rec+0x70 reverse-mapped to
# the 7 stance-DB slots, layered with the cached occupation_score). The earlier "not
# landed / read-side-unexposed" note was WRONG. The handler ALWAYS serves a per-war
# occupation view (success:true + a `wars` array, built from the war-score cache) — the
# record walk that adds engine STANCE LABELS is the only part DEFAULT-OFF behind the bridge
# flag `set_war_stance_record_walk` (the rec↔war-id key rec+0x18 is static-confirmed but
# has no live probe on an at-war record — FIXPLAN B2). This Python layer does NOT arm that
# flag and has no "prefer engine stance over synthesis" switch, so the flag-OFF served response
# carries occupation but no stance label, and we layer it onto the SYNTHESIZED agent-facing
# "war stance" view from the two reads the bridge serves UNGATED: get_active_wars (side +
# war_score) + query_war_score_breakdown (occupation_score, the siege signal).
# query_war_stance is classified `is_game_state_query` (snapshot.cpp), served paused or
# running. (It was once unclassified → queued to the game-thread queue → never drained
# while paused → MCP timeout → cached permanently into _WAR_STANCE_UNSERVED, which
# silently degraded this gateway to synthesis-only for its whole life. Fixed in the
# bridge.) _native_war_stance below already prefers
# the engine stance object when served, so the moment B2 arms the record-walk flag the
# stance-label path lights up with no further Python change — synthesis is the working
# fallback for builds that lack the handler entirely.
#
# The strategically-important signal is the documented occupation impasse: a controlled
# ruler's OFFENSIVE war that never sieges sits at occupation_score=0 forever (defensive
# stance / native-AI-disabled — re_control_suppresses_war.md). Surfacing per-war
# occupation_score + a STALL verdict lets the agent ABANDON doomed occ-0 wars (white-peace)
# instead of bleeding its army across a succession. This is a PURE read (no engine hook,
# no write) — crash-safe by construction.

# A war that an ATTACKER has held with occupation_score==0 past this many game-DAYS is
# "stalled" (the siege never started). Bounded so a freshly-declared war isn't flagged.
_WAR_STALL_OCC0_DAYS = 365         # ~1 game year of zero occupation = stuck siege
_WAR_STALL_LOSING_SCORE = -25      # losing badly as attacker → also exit-worthy


# Bridge error/status tokens that mean "this build does not SERVE query_war_stance at all"
# — i.e. an OLD bridge that predates the handler (re_query_war_stance.md), which answers with
# an unknown-command error. (This is NOT the record-walk-OFF case: a current bridge always
# serves a `wars` array with occupation, just without the stance label until B2 arms the
# flag.) On such a build we fall back to the synthesized occupation view.
# Cache the one-time capability verdict per gateway: once query_war_stance comes back
# unserved (None) on a build, skip the wasted round-trip on every later plan_war (R5).
# Keyed on id(gw) so a fresh gateway re-probes; cleared implicitly when the gw is GC'd.
# NOTE: query_war_stance is is_game_state_query, served paused or running, so the
# timeout branch below no longer false-caches a capable build into synthesis-only.
_WAR_STANCE_UNSERVED: set = set()


async def _native_war_stance(gw, character_id: int) -> Optional[dict]:
    """Try the bridge's native query_war_stance (engine stance-DB read) if it exists.
    Returns the raw dict on success, None when the bridge does not serve it (so the
    caller falls back to the synthesized occupation view). Honest — never invents data.
    One-time capability probe: after the first None on a gateway the call is skipped
    (cached in _WAR_STANCE_UNSERVED) so it is not re-paid every plan_war (R5)."""
    if id(gw) in _WAR_STANCE_UNSERVED:
        return None
    try:
        raw = await gw.read("query_war_stance", {"character_id": character_id})
    except Exception:
        _WAR_STANCE_UNSERVED.add(id(gw))
        return None
    if not isinstance(raw, dict) or not raw:
        _WAR_STANCE_UNSERVED.add(id(gw))
        return None
    err = str(raw.get("error") or raw.get("reason") or "").lower()
    if any(tok in err for tok in _UNKNOWN_CMD_TOKENS):
        _WAR_STANCE_UNSERVED.add(id(gw))
        return None
    # A real served response carries a wars/stances array; anything else = not served.
    if not (raw.get("wars") or raw.get("success")):
        _WAR_STANCE_UNSERVED.add(id(gw))
        return None
    return raw


async def _war_stance_view(gw, character_id: int) -> dict[str, Any]:
    """Per-active-war stance view for `character_id`: side (attacker/defender),
    war_score, occupation_score, and a STALL verdict + recommended exit lever for stuck
    offensive wars. Prefers the bridge's native query_war_stance (engine stance-DB read,
    re_query_war_stance.md) when served; otherwise SYNTHESIZES from get_active_wars +
    query_war_score_breakdown (pure reads, crash-safe). Returns
    {wars:[...], stalled:[...], any_stalled:bool}."""
    wars_raw = await gw.read("get_active_wars", {"character_id": character_id})
    wars_raw = wars_raw if isinstance(wars_raw, dict) else {}
    out_wars: list = []
    stalled: list = []
    g = gw.game()
    now_tick = _as_int(g.get("tick") or g.get("current_tick"))
    # Native engine-stance read (authoritative when served) — indexed by war_id.
    native = await _native_war_stance(gw, character_id)
    native_by_war: dict = {}
    if native:
        for ns in (native.get("wars") or native.get("stances") or []):
            native_by_war[_as_int(ns.get("war_id"))] = ns
    for w in (wars_raw.get("wars") or []):
        wid = _as_int(w.get("war_id"))
        is_attacker = bool(w.get("is_attacker")) or (_as_int(w.get("attacker_id")) == character_id)
        war_score = _as_int(w.get("war_score"))
        # Per-war breakdown for the occupation signal (the siege-progress tell).
        occ = None
        wsb = await gw.read("query_war_score_breakdown", {"character_id": character_id, "war_id": wid})
        wsb = wsb if isinstance(wsb, dict) else {}
        # the handler nests the live numbers under a flat row or a `breakdowns` list.
        occ = wsb.get("occupation_score")
        if occ is None:
            occ = wsb.get("occupation")
        occ = _as_int(occ) if occ is not None else None
        # my-side occupation (attacker drives occupation_atk).
        my_occ = _as_int(wsb.get("occupation_atk")) if is_attacker else _as_int(wsb.get("occupation_def"))
        if not my_occ and occ is not None:
            my_occ = occ if (is_attacker == (occ >= 0)) else 0

        # Stance label: prefer the engine stance object (query_war_stance) when the
        # bridge serves it; otherwise INFER behaviourally — an attacker with rising
        # occupation is effectively offensive; an attacker stuck at occupation 0 is the
        # defensive-stance / impasse case (re_control_suppresses_war.md / re_query_war_stance.md).
        ns = native_by_war.get(wid) or {}
        native_stance = ns.get("stance")
        # will_siege (native): the authoritative "this stance actually sieges" bit.
        will_siege = ns.get("will_siege")
        if native_stance:
            stance = str(native_stance)
            stance_source = "engine_stance_db"
        elif is_attacker:
            stance = "offensive_sieging" if (my_occ and my_occ > 0) else "attacker_not_sieging"
            stance_source = "inferred_from_occupation"
        else:
            stance = "defending"
            stance_source = "inferred_from_occupation"

        # Stall verdict: an ATTACKER war with occupation 0 (no siege) OR losing badly.
        # When the native will_siege bit is present, prefer it (an attacker stance that
        # will NOT siege at occupation 0 is the definitive stuck case). A losing attacker
        # is exit-worthy too.
        occ0 = is_attacker and (my_occ == 0) and (will_siege is not True)
        losing = is_attacker and (war_score <= _WAR_STALL_LOSING_SCORE)
        stall_suspect = occ0 or losing
        exit_lever = None
        if stall_suspect:
            # The two exits a war leader has: white peace (the enemy leader must
            # accept it) and surrender (the enemy accepts it; it is a DEFEAT and
            # costs the CB's penalty, which `surrender_cost` says: L18-C).
            exit_lever = {
                "white_peace": {"intent": "make_peace", "args": {"war_id": wid, "terms": "white_peace"}},
                "surrender": {"intent": "make_peace", "args": {"war_id": wid, "terms": "surrender"}},
                "surrender_cost": ("your defeat: the casus belli's penalty in prestige and often "
                                   "gold (2026.11 pilots: 15-350 prestige, up to 590 gold); not free"),
                "why": ("attacker occupation_score=0 (siege never started — the controlled-ruler "
                        "impasse / defensive stance)" if occ0 else f"losing as attacker (war_score {war_score})"),
            }

        # L18-A: the engine's offensive stance is a strength gate (the attacker goes
        # offensive only above 1.333x the enemy's strength; constants, personality does
        # not enter: re/findings/re_offensive_strength_gate.md). No steering lever moves
        # an inferred non-sieging attacker; the row says so, typed, instead of implying one.
        limitations = []
        if stance == "attacker_not_sieging" and stance_source == "inferred_from_occupation":
            limitations.append({
                "code": "war_stalled_no_lever", "is_known_limitation": True,
                "detail": ("the AI sieges only when it is above 1.33x the enemy's strength "
                           "(an engine constant; no personality axis or phase moves it); "
                           "call allies into the war (do(intent=\"call_ally\")), or leave it open")})
        row = {
            "war_id": wid,
            "name": w.get("name"),
            "side": "attacker" if is_attacker else "defender",
            "stance": stance,
            "stance_source": stance_source,   # engine_stance_db (authoritative) or inferred_from_occupation
            "will_siege": will_siege,         # native bit when served, else None
            "war_score": war_score,
            "occupation_score": my_occ,
            "defender_id": _as_int(w.get("defender_id")),
            "attacker_id": _as_int(w.get("attacker_id")),
            "stall_suspect": stall_suspect,
            "exit_lever": exit_lever,
            "sieges": w.get("sieges", []),  # per-siege detail from emit_war_entry (county/progress/status)
            "limitations": limitations,
        }
        out_wars.append(row)
        if stall_suspect:
            stalled.append(row)
    return {
        "wars": out_wars,
        "stalled": stalled,
        "any_stalled": bool(stalled),
        "tick": now_tick,
        "stance_source": ("engine_stance_db" if native_by_war else "inferred_from_occupation"),
        "note": ("Per-war stance: from the engine CAIWarStance DB when the bridge serves "
                 "query_war_stance (stance_source=engine_stance_db), else INFERRED from "
                 "occupation_score (stance_source=inferred_from_occupation). An "
                 "attacker_not_sieging war (occupation 0, will_siege!=true) is the documented "
                 "impasse — abandon it via the exit_lever (white-peace) rather than bleeding your "
                 "army across a succession. A controlled ruler whose native AI is disabled "
                 "never sieges: keep disable_native_ai off (the default) or RELEASE control "
                 "for the war's duration (re_control_suppresses_war.md)."),
    }


# --------------------------------------------------------------------------- cbs
async def cbs_impl(gw, target_id: int = 0, character_id: int = 0) -> dict[str, Any]:
    """'Show me EVERY casus belli I can use against this target, with tradeoffs.'
    Returns the full curated CB list (county/duchy/realm/faith_counties scope, what each
    GRANTS on victory, why-unmet, dispatch-marshalling hint) so the agent can weigh
    e.g. tribal_subjugation (whole realm, once/lifetime) vs county_conquest (one county).
    An engine read on the game thread; works paused or running."""
    actor, err = await _resolve_actor(gw, character_id, "cbs")
    if err:
        return err
    if not target_id:
        return envelope("cbs", ok=False, actor=actor, game=gw.game(),
                        blocked=why_blocked("missing_required_argument", summary="target_id is required"),
                        bridge_health=gw.health)
    # Use the busy-aware read directly so we can distinguish a transient slot
    # collision (cb_query_busy — retry) from a genuinely-empty drained result
    # (no_valid_targets — the target is truced-out / no usable CB, not a tooling
    # failure). Mislabeling the empty case as "degraded" wasted agent cycles.
    cbs_raw = await _cb_read_with_retry(gw, "query_cbs_for_target",
                                        {"attacker_id": character_id, "target_id": target_id,
                                         "timeout_ms": 6000})
    cbs_raw = cbs_raw if isinstance(cbs_raw, dict) else {}
    valid_opts, all_opts = _shape_cb_options(cbs_raw, target_id=target_id)
    tstate = cbs_raw.get("target_state") or {}
    warnings: list = []
    if not all_opts:
        if _cb_read_is_busy(cbs_raw):
            warnings.append({"code": "cb_query_busy", "is_transient": True, "retryable": True,
                             "detail": "CB-validity slot busy (engine AI eval collision / drain timeout) "
                                       "after bounded retry — transient, retry; NOT a no-targets verdict"})
        else:
            warnings.append({"code": "no_valid_targets", "is_transient": False, "retryable": False,
                             "detail": "engine drained 0 CB rows for this target — no usable casus belli "
                                       "(truced/allied/ineligible). Not a tooling failure; pick another target"})
    result = {
        "target_id": target_id,
        "target_state": tstate,
        "options": all_opts,
        "valid_options": valid_opts,
        "best": (valid_opts[0] if valid_opts else None),
        # every family the engine was asked about, with the CBs it accepts now
        "by_family": cb_options_by_family(all_opts),
        # lane-query item 6: this is the game's own CB check, and it wins over
        # discover(what="war_targets"), whose `declarable` is a relation-gate
        # heuristic (that reply now says so too).
        "authority": "engine",
        "authority_note": ("valid_options come from the game's strict casus-belli check; "
                           "discover(what='war_targets') lists candidates only and is "
                           "overruled by this answer when they disagree"),
    }
    return envelope("cbs", result=result, actor=actor, game=gw.game(),
                    warnings=warnings, bridge_health=gw.health)


# --------------------------------------------------------------------------- plan_war
#: L18-B item 9: the last empty plan_war reply per ruler, {ruler_id: {"key",
#: "since", "times"}}, in this server process (one seat). Pilots re-asked plan_war
#: cycle after cycle and got the same empty reason with no sign it had not moved.
_PLAN_WAR_EMPTY_MEMO: dict[int, dict] = {}


def remember_empty_reason(ruler_id: int, empty_reason: Optional[dict], date: Any,
                          call: tuple) -> None:
    """Stamp `unchanged_since` (the game date this ruler was first given this same
    empty reason, for the same call) and `times` (consecutive plan_war calls that
    gave it) onto `empty_reason`; a plan_war with rows, or a different reason,
    starts over. A date the clock did not read is None, never a guess."""
    if not ruler_id:
        return
    if not empty_reason:
        _PLAN_WAR_EMPTY_MEMO.pop(ruler_id, None)
        return
    measured = empty_reason.get("measured") or {}
    key = (empty_reason.get("gate"), repr(sorted((measured.get("by_gate") or {}).items())),
           measured.get("own_realm_counties"), call)
    prev = _PLAN_WAR_EMPTY_MEMO.get(ruler_id)
    if prev and prev["key"] == key:
        prev["times"] += 1
    else:
        prev = {"key": key, "since": (date if isinstance(date, str) and date and date != "unknown"
                                      else None), "times": 1}
        _PLAN_WAR_EMPTY_MEMO[ruler_id] = prev
    empty_reason["unchanged_since"] = prev["since"]
    empty_reason["times"] = prev["times"]


def plan_war_empty_reason(excluded: list, *, ally_free_only: bool,
                          exclude_straddling: bool, own_realm_counties: int = 0,
                          goal_scoped: bool = False) -> dict[str, Any]:
    """Why plan_war has no row: what ACTUALLY excluded the candidates (L14 finding 2).

    The gate is the commonest `excluded` gate, with its count; a tie goes to the
    retryable gate, then to the gate's name (said in `measured.tied`). Every gate
    and its count is in `measured.by_gate`. Where a plan_war parameter would admit
    the excluded rows, `fix` names it: exclude_straddling=False (and
    confirm_straddle=True on the declare) for succession_straddle rows,
    ally_free_only=False for target_allied rows it dropped. Before this the reply
    said "target_allied ... allied/truced" whatever the rows said (12037:
    claim_not_pressed + holder_not_independent; 9460: succession_straddle)."""
    if not excluded:
        if own_realm_counties:
            # L18-B: the duchies scanned hold no county outside the actor's own realm.
            return why_blocked(
                "no_valid_targets",
                summary=(f"no candidate targets: the {own_realm_counties} county(ies) left in the "
                         "duchies scanned are held inside your own realm (your vassals), and a war "
                         "is never declared on your own realm"),
                measured={"excluded": 0, "own_realm_counties": own_realm_counties},
                fix=({"retry_with": {"goal_scoped": False}} if goal_scoped else None))
        return why_blocked("no_valid_targets", summary="no candidate targets enumerated")
    by_gate: dict[str, int] = {}
    names: dict[str, list] = {}
    for row in excluded:
        blk = row.get("blocked") if isinstance(row, dict) else None
        gate = str((blk or {}).get("gate") or "unmapped_refusal")
        by_gate[gate] = by_gate.get(gate, 0) + 1
        if row.get("target_name"):
            names.setdefault(gate, []).append(row["target_name"])
    top_n = max(by_gate.values())
    tied = sorted((g for g, n in by_gate.items() if n == top_n),
                  key=lambda g: (not GATE_TABLE.get(g, {}).get("retryable", False), g))
    gate = tied[0]
    total = len(excluded)
    parts = [f"{g} x{n}" + (f" ({', '.join(str(x) for x in names[g][:3])})" if names.get(g) else "")
             for g, n in sorted(by_gate.items(), key=lambda kv: (-kv[1], kv[0]))]
    fix: dict[str, Any] = {}
    if exclude_straddling and by_gate.get("succession_straddle"):
        fix["succession_straddle"] = {
            "admits": by_gate["succession_straddle"],
            "retry_with": {"exclude_straddling": False},
            "then": "declare_war(..., confirm_straddle=True)"}
    if ally_free_only and by_gate.get("target_allied"):
        fix["target_allied"] = {"admits": by_gate["target_allied"],
                                "retry_with": {"ally_free_only": False}}
    measured: dict[str, Any] = {"excluded": total, "by_gate": by_gate}
    if len(tied) > 1:
        measured["tied"] = tied
    summary = (f"{by_gate[gate]} of {total} candidate target(s) excluded by {gate}: "
               f"{GATE_WHY.get(gate) or gate}. All exclusions: {'; '.join(parts)}")
    return why_blocked(gate, summary=summary, measured=measured, fix=fix or None)


async def plan_war_impl(gw, character_id: int = 0, goal_scoped: bool = True,
                        ally_free_only: bool = True, exclude_straddling: bool = True,
                        max_results: int = 8, toward: str = "") -> dict[str, Any]:
    actor, err = await _resolve_actor(gw, character_id, "plan_war")
    if err:
        return err
    warnings: list = []

    _chr = await gw.read("query_character", {"character_id": character_id})
    _chr_sect = _char_section(_chr)

    # ── War lifecycle gates (item 2): succession-trough + stress-aware pacing ───
    # Reuse the same character read for stress/age (no extra read). The planner does
    # NOT fire interactions — it produces an HONEST lifecycle advisory + a declare-gate
    # the agent honors: never declare across a succession trough; back off when stress
    # is climbing; abandon stalled occ-0 wars first.
    actor_stress = _f(_chr_sect.get("stress"))
    actor_age = _f(_chr_sect.get("age"))
    # Succession: a ruler at STRADDLE_AGE or older is near enough to death that a
    # war opened now is expected to run across his succession (declare_war's
    # confirm_straddle guard reads the same constant).
    near_succession = actor_age >= STRADDLE_AGE
    # Stress: the same two lines observe reports (STRESS_CLIMBING, STRESS_BREAK_RISK).
    stress_climbing = actor_stress >= STRESS_CLIMBING
    stress_break_risk = actor_stress >= STRESS_BREAK_RISK
    # Stalled-war check: if an existing attacker war is stuck at occupation 0, the
    # doctrine is EXIT it before opening another front (don't bleed the army two ways).
    stance_view = await _war_stance_view(gw, character_id)
    has_stalled_war = bool(stance_view.get("any_stalled"))

    # A new declaration is DISCOURAGED (not hard-blocked — the agent may override with
    # confirm_straddle on declare_war) when any lifecycle gate trips.
    declare_discouraged = near_succession or stress_break_risk or has_stalled_war

    # Enumerate candidate (target,title,CB) rows. query_valid_war_targets is the base
    # surface; O1: it omits conquest/subjugation CB classes → degrade with a warning
    # and lean on the strict-CB validator (the authoritative inner declarability gate).
    vt = await gw.read("query_valid_war_targets", {"actor_id": character_id})
    vt = vt if isinstance(vt, dict) else {}
    # L18-B: do_query_valid_war_targets writes the war's target as
    # r.key("target_char_id") (query_handlers.cpp), never target_id, so every
    # enumerator row used to reach the loop below with target 0 and be dropped:
    # the actor's own claims never seeded a row. Read the emitter's key; a pressed
    # row sorts ahead so the per-target dedup keeps it over an unpressed one.
    rows = [dict(t, target_id=_as_int(t.get("target_id") or t.get("target_char_id")))
            for t in (vt.get("targets") or []) if isinstance(t, dict)]
    rows.sort(key=lambda t: not t.get("pressed"))
    own_realm_counties = 0

    targets: list = []
    excluded: list = []
    seen = set()

    # The deferred game-thread reads below (the per-target strict-CB gate and the
    # per-target cbs tradeoff list) each go through gw.read, which single-flights
    # them through the bridge's g_cb_query slot; none of them touches the clock.
    # The conquest candidates: goal_scoped keeps the duchies you partly hold (and
    # the `toward` title's); goal_scoped=False adds every duchy bordering the
    # realm. `toward` names any de-jure title, anywhere. The handler runs the
    # engine checks (county_conquest_cb per holder) on the game thread.
    ct_params = {"character_id": character_id, "include_expansion": not goal_scoped}
    if toward:
        ct_params["toward"] = str(toward)
    ct: dict = {}
    for _ in range(3):
        ct = await gw.read("query_conquest_targets", ct_params)
        ct = ct if isinstance(ct, dict) else {}
        if not _cb_read_is_busy(ct) or ct.get("duchies") is not None or ct.get("error"):
            break
    if ct.get("error") == "toward_title_not_found":
        warnings.append({"code": "toward_title_not_found", "is_known_limitation": False,
                         "detail": f"no landed title is called {toward!r}"})

    # ★ cb_title_id baking: county_conquest_cb is COUNTY-targeted — its cb_title_id
    # is the COUNTY id (rc.county_id), NOT the duchy. The bridge already hands back
    # the authoritative declare block (declare.cb_title_id = county_id) on a
    # conquerable_now county; carry that VERBATIM so declare_war(declare_args=row)
    # fires the exact county the engine validated. target_in_truce/alliance carried
    # from the holder relation flags so the row's tags are correct without a 2nd read.
    for d in (ct.get("duchies") or []):
        for rc in (d.get("remaining_counties") or []):
            decl = rc.get("declare") or {}
            # The county is the CB title for county_conquest_cb (engine-authoritative
            # via the declare block; fall back to county_id when not conquerable_now).
            county_cb_title = _as_int(decl.get("cb_title_id") or rc.get("county_id"))
            # The war is against the holder's TOP liege. L15 A3: when the bridge could
            # not finish that walk it writes holder_top_liege_unresolved and no
            # holder_top_liege_id (do_query_conquest_targets; liege_walk.h answers 0,
            # never the holder's own id). The target is then UNMEASURED: it used to
            # fall back to holder_id, so a vassal county's war went to the vassal.
            war_target = _as_int(decl.get("target_id") or rc.get("holder_top_liege_id"))
            if war_target and war_target == _as_int(character_id or (actor or {}).get("id")):
                # L18-B: a county whose holder's top liege is the actor lies inside
                # his own realm (Cui 13191: every remaining county of his partly-held
                # duchies is a vassal's). It is no war target; probing it as one made
                # the whole goal-scoped plan read "target_not_independent".
                own_realm_counties += 1
                continue
            if not war_target:
                excluded.append({
                    "target_name": rc.get("holder_name"), "county_id": rc.get("county_id"),
                    "holder_id": rc.get("holder_id"),
                    "blocked": why_blocked(
                        "gate_introspection_unavailable",
                        summary=("the county holder's top liege could not be resolved, so "
                                 "the war's target is unmeasured (it is not the holder)"),
                        raw_reason="holder_top_liege_unresolved")})
                continue
            rows.append({
                "target_id": war_target,
                "target_name": rc.get("holder_name"),
                # county-scoped CB → cb_title_id is the COUNTY (NOT the duchy).
                "title_id": county_cb_title,
                "title": rc.get("county_key") or rc.get("county_name")
                         or d.get("duchy_key") or d.get("title"),
                "cb_type": str(decl.get("casus_belli") or "county_conquest_cb"),
                "pressed": False, "_from_conquest_targets": True,
                # relation tags so affordability/truce surface without a 2nd read.
                "target_in_truce": bool(rc.get("is_truced")),
                "target_in_alliance": bool(rc.get("is_allied")),
                "_conquerable_now": (decl != {}) or bool(rc.get("conquerable_now")),
                # ★ adjacency steer (directive 1): county_conquest_cb is an
                # engine REALM-LEVEL (incl. vassals), SEA-INCLUSIVE adjacency
                # rule — a far de-jure target comes back NOT conquerable_now with
                # block_reason `not_adjacent_unverified` (or `engine_cb_invalid`
                # with adjacent=false). Carry the bridge's per-county `adjacent`
                # flag + `block_reason` so the per-target loop can STEER a
                # non-adjacent county_conquest target to its de_jure_cb / claim_cb
                # row instead of bouncing off county_conquest. de_jure_cb (de-jure
                # liege) and claim_cb (pressed claim) are NOT adjacency-gated.
                "_adjacent": bool(rc.get("adjacent")),
                "_cc_block_reason": rc.get("block_reason"),
                "dejure_gain": 1,
            })

    # ── PRUNE the target set BEFORE the expensive per-target strict/cbs loop (R3
    # of the redesign profile §3 "prune to the plausible declarable few"). Dedup by
    # target_id, then keep only the PLAUSIBLE/declarable candidates so the O(N) CB
    # loop runs on a handful, not on every raw enumerated row. A goal-scoped run
    # appends a county row per remaining county across every conquest duchy → easily
    # 10–30 raw rows, most of which are NOT conquerable-now / are allied / truced.
    # The prune is a PURE filter on data we already have (no extra reads): conquest
    # rows that are not conquerable-now AND are allied/truced cannot become declarable
    # this cycle, so they are dropped before paying a strict-CB round-trip. Base
    # enumerator rows (claims) are always kept — they are already the few. The strict
    # gate remains AUTHORITATIVE for everything we keep; pruning only removes rows the
    # strict gate would have excluded anyway, so no declarable target is ever lost.
    pruned_rows: list = []
    for r in rows:
        tid = _as_int(r.get("target_id"))
        if not tid or tid in seen:
            continue
        seen.add(tid)
        if r.get("_from_conquest_targets"):
            conquerable = bool(r.get("_conquerable_now"))
            # Drop a conquest county that is neither conquerable-now NOR worth a strict
            # probe: allied (always culled) — keep truced (the agent plans around it).
            if not conquerable and r.get("target_in_alliance"):
                excluded.append({"target_name": r.get("target_name"),
                                 "blocked": why_blocked("target_allied",
                                                        summary="conquest county holder is allied "
                                                                "(culled) and not conquerable now")})
                continue
        pruned_rows.append(r)

    # Cap the strict/cbs probe count to a sane bound (a handful), preferring the
    # plausible rows: conquerable-now conquest rows and pressed-claim rows first, then
    # the rest. This keeps the per-target loop O(handful) even when the enumerator and
    # conquest ladder together produce a long tail. The cap is generous (≥ max_results
    # so ranking still has headroom) and never drops a declarable row silently — capped
    # rows are surfaced as excluded with a 'pruned_low_priority' reason.
    _PROBE_CAP = max(12, max_results * 2)

    def _plausibility(r: dict) -> int:
        score = 0
        if r.get("_from_conquest_targets") and r.get("_conquerable_now"):
            score += 3
        if r.get("pressed"):
            score += 2
        if not r.get("target_in_truce"):
            score += 1
        return score

    pruned_rows.sort(key=_plausibility, reverse=True)
    if len(pruned_rows) > _PROBE_CAP:
        # L18-B item 9: this said "re-plan to surface", but the sort above is
        # deterministic, so the same call prunes the same rows. The cap is
        # max(12, 2*max_results): a larger max_results is what surfaces them.
        _need = (len(pruned_rows) + 1) // 2
        for r in pruned_rows[_PROBE_CAP:]:
            excluded.append({"target_name": r.get("target_name"),
                             "blocked": why_blocked("low_priority_pruned",
                                                    summary=(f"pruned_low_priority: beyond the strict-CB "
                                                             f"probe cap ({_PROBE_CAP} = max(12, 2 x "
                                                             f"max_results)); the same call prunes the "
                                                             f"same rows: raise max_results (to {_need} "
                                                             f"to probe all {len(pruned_rows)})"),
                                                    fix={"max_results": _need})})
        pruned_rows = pruned_rows[:_PROBE_CAP]

    probe_tids = [_as_int(r.get("target_id")) for r in pruned_rows]

    # ── Batched cbs prefetch (R3), graceful fallback to per-target ──────────────
    # One query_cbs_for_targets round-trip for all kept targets when served; else
    # per-target below. Prefetch for ALL pruned targets (cheap when batched);
    # the per-target shaping consumes whichever is available.
    cbs_by_tid = await _cbs_for_targets_batched(gw, character_id, probe_tids)

    for r in pruned_rows:
        tid = _as_int(r.get("target_id"))
        # Strict-CB authoritative declarability: one read per target.
        strict = await _cb_read_with_retry(
            gw, "query_cb_validity_strict",
            {"actor_id": character_id, "target_id": tid,
             "include_ineligible": True, "timeout_ms": 6000})
        strict = strict if isinstance(strict, dict) else {}
        # A transiently-busy slot (engine AI eval collision) is NOT a verdict on this
        # target — after bounded retry it's still busy, so surface it as transient and
        # do NOT exclude the target permanently (the #95 hold-on-collision pathology).
        if _cb_read_is_busy(strict):
            excluded.append({"target_name": r.get("target_name"),
                             "blocked": map_raw_reason("cb_query_busy",
                                                       summary="CB slot busy after retry — transient, re-plan")})
            continue
        results = strict.get("results") or []
        # pick the best declarable CB row. A row that carries `declarable` is decided
        # by it: the engine's inner verdict passes claim_cb with no pressed claim,
        # and the war is then refused claim_not_pressed (live867 l5-r8/r10: plan_war
        # offered 13495, who holds no pressed claim, two claim_cb wars). Only a row
        # without the key falls back to the engine's outer+inner verdict.
        best = None
        for cr in results:
            ok = (cr.get("declarable") is True if "declarable" in cr else
                  cr.get("outer_status") == "pass" and cr.get("inner_status") == "pass")
            if ok:
                best = cr
                break

        # ── Full CB tradeoff list for THIS target (fetched UP-FRONT so the non-adjacency
        # steer below can consult it). query_cbs_for_target — a deferred game-thread read;
        # each option carries scope/grants/tradeoffs + the dispatch-marshalling hint.
        # Batched prefetch when served; else one read per target.
        if cbs_by_tid is not None and tid in cbs_by_tid:
            _craw = cbs_by_tid.get(tid) or {}
            cb_options, _all_cb = _shape_cb_options(_craw, target_id=tid)
            tstate = _craw.get("target_state") or {}
        else:
            cb_options, _all_cb, tstate = await _cb_options_for(gw, character_id, tid)

        if best is None:
            # ★ Adjacency steer (directive 1): a county_conquest_cb row that came back
            # NOT declarable because the target is NON-ADJACENT (county_conquest is an
            # engine REALM-level, sea-inclusive adjacency rule) is NOT a dead end — a
            # de_jure_cb (actor is de-jure liege) or claim_cb (pressed claim) for the
            # SAME target is NOT adjacency-gated. Before excluding, consult the CB
            # tradeoff list: if a de-jure or claim CB is VALID + dispatch-supported,
            # SYNTHESIZE `best` from it and fall through to the declare-ready row build
            # (do NOT bounce off county_conquest). This honestly SURFACES the non-
            # adjacency cull_reason on the steered row's notes; it does not bypass the
            # engine adjacency rule (county_conquest stays culled — we pick a different,
            # un-gated CB the engine itself reports valid).
            cc_non_adjacent = (
                r.get("_from_conquest_targets")
                and not r.get("_adjacent")
                and str(r.get("_cc_block_reason") or "").startswith("not_adjacent")
            )
            alt = None
            if cc_non_adjacent:
                # prefer de_jure (folds many counties); else a claim CB.
                alt = next((o for o in cb_options
                            if _is_de_jure_cb(o.get("cb"))
                            and o.get("valid") and o.get("dispatch_supported")), None)
                if alt is None:
                    alt = next((o for o in cb_options
                                if str(o.get("cb") or "").endswith("claim_cb")
                                and o.get("valid") and o.get("dispatch_supported")), None)
            if alt is not None:
                # Build a strict-`best`-shaped row from the alternative CB so the rest
                # of the loop marshals cb_title_id / claimant correctly.
                best = {
                    "cb_name": alt.get("cb"),
                    "cb_title_id": _as_int(alt.get("cb_title_id") or r.get("title_id")),
                    "outer_status": "pass", "inner_status": "pass",
                    "declarable": True,
                    "_steered_from_non_adjacent_county_conquest": True,
                }
                warnings.append({
                    "code": "steered_non_adjacent_to_de_jure_or_claim",
                    "is_known_limitation": False,
                    "detail": (f"{r.get('target_name')}: county_conquest_cb culled "
                               f"({r.get('_cc_block_reason')}) — county_conquest adjacency is a "
                               "REALM-level, sea-inclusive ENGINE rule; steered to the non-"
                               f"adjacency-gated {alt.get('cb')} for the same target."),
                })
            else:
                # not declarable — surface the per-target cull reason. A genuinely-empty
                # drained result (no rows) is a real "no usable CB" verdict, distinct from busy.
                cull = next((cr.get("cull_reason") for cr in results if cr.get("cull_reason")), "")
                if not cull:
                    cull = str(r.get("_cc_block_reason") or "") if r.get("_from_conquest_targets") else ""
                if not cull and not results:
                    cull = "no_valid_targets"
                excluded.append({"target_name": r.get("target_name"),
                                 "blocked": map_raw_reason(cull or "target_ineligible",
                                                           summary=cull or "no declarable CB")})
                continue
        ally_free = not r.get("target_in_alliance") and not best.get("target_in_alliance")
        if ally_free_only and not ally_free:
            excluded.append({"target_name": r.get("target_name"),
                             "blocked": why_blocked("target_allied", summary="target pulls allies")})
            continue
        dejure_gain = _as_int(r.get("dejure_gain")) or (1 if r.get("_from_conquest_targets") else 0)
        # Every war this ruler opens now is expected to run across his succession
        # once he is STRADDLE_AGE (the same rule declare_war's guard applies).
        straddles = near_succession
        if exclude_straddling and straddles:
            excluded.append({"target_name": r.get("target_name"),
                             "blocked": why_blocked("succession_straddle",
                                                    summary=f"ruler is {int(actor_age)} (>= {STRADDLE_AGE}): "
                                                            "the war is expected to cross his succession")})
            continue

        # (cb_options / tstate were fetched up-front, above the best-is-None adjacency
        # steer, so a non-adjacent county_conquest target could be re-pointed at its
        # de_jure_cb / claim_cb before exclusion. They are reused verbatim here.)

        # Pick the default declare_args CB: prefer the strict-CB `best` if it is in the
        # valid CB-options (so cb_title_id / claimant marshalling is correct), else the
        # widest-scope valid option. Scope rank: realm > kingdom > duchy > county.
        _scope_rank = {"realm": 4, "kingdom": 3, "duchy": 2, "faith_counties": 1, "county": 0}
        best_cb_name = best.get("cb_name") or r.get("cb_type")
        chosen = next((o for o in cb_options if o["cb"] == best_cb_name), None)
        if chosen is None and cb_options:
            chosen = max(cb_options, key=lambda o: _scope_rank.get(o.get("scope"), -1))
            best_cb_name = chosen["cb"]

        # ★ cb_title_id baking (item 1): the row's declare_args.cb_title_id must be the
        # EXACT title the chosen CB targets, so declare_war(declare_args=row) fires
        # first-try without a second cbs/strict call.
        #   • title-targeted CBs (county/duchy/de_jure/holy_war) REQUIRE a cb_title_id.
        #     The authoritative value is, in priority order: the row's pre-baked title_id
        #     (conquest_targets sets this to the COUNTY id via the engine declare block),
        #     then the strict-CB `best.cb_title_id`. NO second cbs call.
        #   • mode-0 CBs (subjugation/vassalization) take NO title (target_mode 0).
        #   • claim CBs take claimant + (the claim's) title.
        cb_title_id = _as_int(r.get("title_id") or best.get("cb_title_id"))
        claimant = _as_int(r.get("claimant_id")) or None
        claim_pressed = bool(r.get("pressed") or best.get("claim_pressed"))
        row_title = r.get("title")
        if chosen is not None and chosen.get("needs_claimant") and best_cb_name != r.get("cb_type"):
            # L18-B (budget0 e1/e2 Charles 0867.01.01): the row came from a conquest
            # county (c_venaissin 7448) and the CB the game accepts against its holder
            # is a CLAIM CB, valid on the title the actor holds a PRESSED claim on
            # (k_italy 2189: the cbs row's cb_title_id, resolve_pressed_claim_title).
            # The row used to keep the county as cb_title_id and declare_war refused
            # it claim_not_pressed. A claim row's title and claimant are the cbs
            # row's; with none, the row is not declarable.
            cb_title_id = _as_int(chosen.get("cb_title_id"))
            if not cb_title_id:
                excluded.append({"target_name": r.get("target_name"),
                                 "blocked": map_raw_reason(
                                     "claim_not_pressed",
                                     summary=f"{best_cb_name} passes the game's check but no pressed "
                                             "claim title was read against this target")})
                continue
            claimant = _as_int(chosen.get("claimant_id")) or _as_int(character_id) or None
            claim_pressed = True     # the cbs row is valid only with a pressed claim (claim_cull)
            row_title = cb_title_id
        if chosen is not None:
            # Honor the chosen CB's dispatch shape: drop a title_id the CB doesn't take
            # (target_mode 0 subjugation), keep claimant only for claim CBs.
            if not chosen.get("needs_cb_title_id"):
                cb_title_id = 0
            if not chosen.get("needs_claimant"):
                claimant = None
        # L18 round 2 (witness 1b): the property, on every path. A row whose CB needs
        # a claimant is offered only with a pressed claim; declare_war's gate refuses
        # anything else claim_not_pressed. With no cbs options read (`chosen` None)
        # the claim rewrite above never ran, and the row went out with
        # claim_pressed false.
        if row_needs_claimant(chosen, best_cb_name) and not claim_pressed:
            excluded.append({"target_name": r.get("target_name"),
                             "blocked": map_raw_reason(
                                 "claim_not_pressed",
                                 summary=f"{best_cb_name} needs a pressed claim and none was read "
                                         "against this target")})
            continue

        # ready_to_declare per (target, cb): a CB is ready when it is in the valid set.
        ready_by_cb = {o["cb"]: bool(o["valid"]) for o in cb_options}

        # ── truce tag (item 1) ─────────────────────────────────────────────────
        # Surface whether this target is truced so the agent plans AROUND it instead of
        # "no targets" stalling. (ally_free_only already excludes alliances; a truce is
        # carried here both from the enumerator row flag and the strict-CB target_state.)
        in_truce = bool(r.get("target_in_truce") or best.get("target_in_truce")
                        or (tstate.get("in_truce") if isinstance(tstate, dict) else False))

        # ── cost (the cbs row's base cost for the chosen CB; an estimate) ───────
        affordability = _affordability(chosen)

        # A row is DECLARE-READY when the engine accepts the CB and no truce stands.
        declare_ready = not in_truce
        notes = "Independent, declarable per strict-CB inner gate. See cb_options to weigh scope."
        if in_truce:
            notes += " ⚠ TRUCE active — wait for expiry or pick another target."
        if affordability.get("affordable_now") is False:
            notes += f" ⚠ {affordability['hint']}."

        targets.append({
            "declare_args": {
                "target_id": tid,
                "cb": best_cb_name,
                "cb_title_id": cb_title_id,
                "claimant_id": claimant,
            },
            "target_name": r.get("target_name"),
            "title": row_title,
            "dejure_gain": dejure_gain,
            "ally_free": ally_free,
            "truce": in_truce,
            "affordability": affordability,
            "claim_pressed": claim_pressed,
            "straddles_succession": straddles,
            # The full CB tradeoff list — weigh scope/grants and re-point declare_args.cb
            # at any option here (the marshalling fields tell you what each CB needs).
            "cb_options": cb_options,
            # ready_to_declare: engine-declarable AND not truced.
            # (ready_to_declare_by_cb stays the raw per-CB valid set.)
            "ready_to_declare": declare_ready,
            "ready_to_declare_by_cb": ready_by_cb,
            "notes": notes,
        })

        # ── FIXPLAN A3 (CB-audit G5): surface de-jure CB rows in the planner ─────
        # "USURP k_england by de-jure plurality" is the documented win-line, but the
        # base enumerator (claims) + conquest_targets (county_conquest) never seed a
        # de_jure declare row — de-jure options were reachable ONLY via per-target
        # valid_cbs_strict and the agent had to be hand-fed them. The strict/cbs data
        # is ALREADY fetched per target above, so this is PURE additive surfacing: a
        # VALID de_jure CB in cb_options means the actor IS the de-jure liege of the
        # target title (the engine gates individual_*_de_jure_cb on exactly that), so
        # emit a de_jure declare row WHENEVER one is valid and it isn't already the
        # chosen best CB (no duplicate). No new bridge action, no declare-path change
        # (the CB already resolves and is gated by name); same declare-ready row shape.
        for dj in cb_options:
            if not _is_de_jure_cb(dj.get("cb")):
                continue
            if dj.get("cb") == best_cb_name:
                continue  # already the primary row's chosen CB — don't duplicate
            if not (dj.get("valid") and dj.get("dispatch_supported")):
                continue
            dj_aff = _affordability(dj)
            # de-jure CBs are title-targeted; carry the option's own title when the
            # shaped row provides one, else fall back to the target's row title_id.
            dj_title_id = _as_int(dj.get("cb_title_id") or r.get("title_id") or best.get("cb_title_id"))
            dj_ready = not in_truce
            dj_notes = (f"DE-JURE war ({dj.get('scope')}): actor is the de-jure liege of "
                        f"{r.get('target_name')}'s title — usurp by de-jure right. "
                        "See cb_options for scope.")
            if in_truce:
                dj_notes += " ⚠ TRUCE active — wait for expiry."
            if dj_aff.get("affordable_now") is False:
                dj_notes += f" ⚠ {dj_aff['hint']}."
            targets.append({
                "declare_args": {
                    "target_id": tid,
                    "cb": dj.get("cb"),
                    "cb_title_id": dj_title_id if dj.get("needs_cb_title_id") else 0,
                    "claimant_id": None,
                },
                "target_name": r.get("target_name"),
                "title": dj.get("grants") or r.get("title"),
                # replaced after the window by the counties the target holds under
                # the CB's title (query_dejure_progress), when that reads
                "dejure_gain": max(dejure_gain, 2),
                "ally_free": ally_free,
                "truce": in_truce,
                "affordability": dj_aff,
                "claim_pressed": False,
                "straddles_succession": straddles,
                "cb_options": cb_options,
                "ready_to_declare": dj_ready,
                "ready_to_declare_by_cb": ready_by_cb,
                "is_de_jure": True,
                "notes": dj_notes,
            })

    # Strength, from reads: each side's current military strength and the strength
    # its allies add (query_alliances: own_military_strength, total_callable_strength),
    # read after the CB reads. score = de-jure counties the war adds x the
    # ratio of your callable strength to the target's (capped at 3).
    mine = await gw.read("query_alliances", {"character_id": character_id})
    mine = mine if isinstance(mine, dict) else {}
    strength_cache: dict = {}
    for t in targets:
        tid = _as_int((t.get("declare_args") or {}).get("target_id"))
        if tid not in strength_cache:
            th = await gw.read("query_alliances", {"character_id": tid})
            strength_cache[tid] = th if isinstance(th, dict) else {}
        th = strength_cache[tid]
        my_call = _as_int(mine.get("total_callable_strength") or mine.get("own_military_strength"))
        th_call = _as_int(th.get("total_callable_strength") or th.get("own_military_strength"))
        ratio = round(my_call / max(1, th_call), 2) if (my_call or th_call) else None
        t["strength"] = {"mine": _as_int(mine.get("own_military_strength")),
                         "mine_with_allies": my_call,
                         "theirs": _as_int(th.get("own_military_strength")),
                         "theirs_with_allies": th_call,
                         "ratio": ratio}
        if t.get("is_de_jure"):
            # the counties a de jure war takes: the target realm's counties under the
            # CB's title (query_dejure_progress measured from the target's seat)
            ttl = _as_int((t.get("declare_args") or {}).get("cb_title_id"))
            if ttl:
                prog = await gw.read("query_dejure_progress", {"character_id": tid, "title_id": ttl})
                held = _as_int((prog or {}).get("held")) if isinstance(prog, dict) else 0
                if held:
                    t["dejure_gain"] = held
        t["score"] = round((t.get("dejure_gain") or 1) * min(ratio if ratio is not None else 1.0, 3.0), 3)

    # Rank declare-ready rows first (truced rows sink but are NOT dropped — the agent
    # still sees them to plan around the truce), then by score.
    targets.sort(key=lambda t: (1 if t.get("ready_to_declare") else 0, t["score"]), reverse=True)
    for i, t in enumerate(targets[:max_results]):
        t["rank"] = i + 1
    targets = targets[:max_results]

    empty_reason = None
    if not targets:
        # ck-3mz0: the else branch said gate "unknown" and the census counted
        # it 55 times — the single commonest unknown cause, and it was minted
        # HERE rather than by the bridge. The sibling branch already carried a
        # real gate; this one simply never got one. `no_valid_targets` is the
        # existing gate for exactly this verdict (target_state, not retryable:
        # pick a different target, do not re-ask about this one).
        empty_reason = plan_war_empty_reason(excluded, ally_free_only=ally_free_only,
                                             exclude_straddling=exclude_straddling,
                                             own_realm_counties=own_realm_counties,
                                             goal_scoped=goal_scoped)
    remember_empty_reason(_as_int((actor or {}).get("id")) or _as_int(character_id),
                          empty_reason, gw.game().get("date"),
                          (goal_scoped, ally_free_only, exclude_straddling, toward))

    # ── Lifecycle advisory (item 2): war-exit + succession + stress pacing ──────
    # ck-zgh4 deploy-51: this was "an HONEST recommendation layer" telling the
    # agent what to do before declaring — abandon stalled wars first, not across
    # a succession, back off when stress climbs. Honest it was; prescriptive it
    # also was. The facts are published and the recommendation is not: a stalled
    # war's available exit lever is a FACT about the game, "ABANDON first" is a
    # strategy. `exit_levers` names what exists; nothing ranks or urges it.
    lifecycle_reasons: list = []
    exit_levers: list = []
    if has_stalled_war:
        for sw in stance_view.get("stalled", []):
            exit_levers.append({
                "war_id": sw.get("war_id"), "name": sw.get("name"),
                "why": (sw.get("exit_lever") or {}).get("why"),
                "exit_lever": sw.get("exit_lever"),
            })
        lifecycle_reasons.append(
            why_blocked("not_at_peace",
                        summary=f"{len(stance_view.get('stalled', []))} stalled attacker war(s) at "
                                "occupation 0 / losing; white-peace levers for them are "
                                "listed in lifecycle.exit_levers"))
    if near_succession:
        lifecycle_reasons.append(
            why_blocked("succession_straddle",
                        summary=f"ruler is {int(actor_age)} (>= {STRADDLE_AGE}) — a new war "
                                "would resolve ACROSS the succession trough (the at-peace-single-heir "
                                "doctrine); do NOT open one now (declare_war still needs confirm_straddle)"))
    if stress_break_risk:
        lifecycle_reasons.append(
            why_blocked("stress_break_risk",
                        summary=f"stress {int(actor_stress)} >= {STRESS_BREAK_RISK} (mental-break risk) — opening "
                                "more wars compounds it; relieve stress first"))
    elif stress_climbing:
        lifecycle_reasons.append(
            why_blocked("stress_climbing",
                        summary=f"stress {int(actor_stress)} climbing (>= {STRESS_CLIMBING}) — pace declarations; "
                                "one war at a time until it falls"))

    lifecycle = {
        # ck-zgh4 deploy-51: the recommend/discourage pair told the seat what to
        # do. The three facts it was derived from are published instead, so the
        # seat draws its own conclusion from the same inputs.
        "near_succession": near_succession,
        "stress_break_risk": stress_break_risk,
        "has_stalled_war": has_stalled_war,
        "reasons": lifecycle_reasons,
        "succession": {
            "near_trough": near_succession,
            "age": int(actor_age),
            "doctrine": "AT-PEACE single-heir successions — never be at war across a succession.",
        },
        "stress": {
            "value": int(actor_stress),
            "climbing": stress_climbing,
            "break_risk": stress_break_risk,
            "soft_cap": STRESS_CLIMBING, "hard_cap": STRESS_BREAK_RISK,
        },
        "active_wars": stance_view.get("wars", []),
        "stalled_wars": stance_view.get("stalled", []),
        "exit_levers": exit_levers,
    }

    result = {"targets": targets, "excluded": excluded, "empty_reason": empty_reason,
              "lifecycle": lifecycle}
    if declare_discouraged:
        warnings.append({
            # the CODE stays a stable identifier for readers; the TEXT no longer
            # tells the seat what to do, it names the three conditions found.
            "code": "lifecycle_conditions_present", "is_known_limitation": False,
            "detail": "lifecycle gate: " + "; ".join(r.get("summary", "") for r in lifecycle_reasons),
        })
    return envelope("plan_war", result=result, actor=actor, game=gw.game(),
                    warnings=warnings, bridge_health=gw.health)


# ===========================================================================
# FIX 1 — deferred-write confirmation (#113): the reusable confirm-by-readback.
#
# A write the engine actually executed NEVER returns a bare success:false, and a
# deferred write NEVER terminates as a surfaced dispatched_pending — it auto-polls to
# `applied` (readback proves the object exists) or `rejected` (typed gate). One code
# path, two callers (steered war, mint_title).
# ===========================================================================

async def _read_dict(gw, action: str, params: dict) -> dict:
    """gw.read, coerced to a dict: the readback predicates index straight into it."""
    res = await gw.read(action, params)
    return res if isinstance(res, dict) else {}


# ===========================================================================
# The clock, and the two waits of a write (L13 contract D5 as amended by
# amendment 3). ONE definition of each, read by both gateways
# (mcp_server.BridgeGateway, agent._AgentGateway) and by every write below.
# ===========================================================================

def _unwrap_reply(reply) -> dict:
    """A bridge reply as a dict ({} for anything else). No bridge reply carries a
    `data` sub-dict; L15 A2 deleted the merge that looked for one."""
    return reply if isinstance(reply, dict) else {}


def clock_of(tick_info) -> dict:
    """THE clock as the bridge sees it, from one get_tick_info reply:
    {"paused", "speed", "date", "tick"}.

    `paused` is True or False only when the reply said so; None means no reading
    (the read failed or carried no `paused`), never a guess. The bridge pushes no
    tick_start or observation frame (witness W1), so a cache fed by those frames
    reads "paused" forever: the clock is read here, from get_tick_info, or it is
    not known."""
    ti = _unwrap_reply(tick_info)
    paused, speed = ti.get("paused"), ti.get("game_speed")
    tick = ti.get("tick_id", ti.get("tick"))
    date = ti.get("game_date")
    return {"paused": paused if isinstance(paused, bool) else None,
            "speed": speed if isinstance(speed, int) and not isinstance(speed, bool) else None,
            "date": date if isinstance(date, str) and date and date != "unknown" else None,
            "tick": tick if isinstance(tick, int) and not isinstance(tick, bool) else None}


async def read_clock(read) -> dict:
    """One fresh clock reading through a gateway's `read` (get_tick_info is
    bridge-internal: served on the IPC thread, paused or running). A read that
    raises gives the no-reading clock (paused None)."""
    try:
        ti = await read("get_tick_info", {})
    except Exception:  # noqa: BLE001
        ti = {}
    return clock_of(ti)


#: Stage 1, the DRAIN. The bridge executes a queued write on the next pass of the
#: session thread, paused or running (5-50 ms a pass). The server reads the
#: verdict back on this schedule (seconds to wait before each re-read), about
#: 2.75 s in all; the schedule is the bound, so a fake clock cannot stretch it.
DRAIN_SCHEDULE_S = (0.05, 0.1, 0.2, 0.4, 0.5, 0.5, 0.5, 0.5)

#: query_dispatch_result's terminal statuses (action_handlers.cpp
#: do_query_dispatch_result). Only "success" is an executed write; "rejected"
#: and "crashed" are the drain's refusal. "not_found" is NOT terminal (witness
#: R3): the drain marks an intent consumed while it runs it and stores it in the
#: completed ring later in the same pass, so a read in between finds neither.
#: It is re-read on the schedule like "pending"; still not found when the
#: schedule ends is a verdict not read (next_pass), never a refusal.
DRAIN_DONE = "success"
DRAIN_REFUSED = frozenset({"rejected", "crashed"})

#: Stage 2, the WORLD: seconds between re-reads while the clock runs (an
#: outcome that needs game days; at speed 1 a day is a second or two). The one
#: definition both gateways' confirmed_write default to (poll_interval_s=None).
WORLD_POLL_INTERVAL_S = 2.5

#: The two waits a pending write names (pending.drains_when).
#:   next_pass       stage 1: the drain's verdict was not read within the drain
#:                   budget; the write runs on the session thread's next pass
#:                   in either clock state.
#:   clock_advances  stage 2: the write ran; its outcome needs game DAYS, which
#:                   pass only while the clock runs.
DRAINS_WHEN = ("next_pass", "clock_advances", "readback")
#:   readback        the write RAN (result.sent) and the server could not make the
#:                   reading that would show its effect within the drain budget (a
#:                   paired read straddled a daily tick, the reply naming what to read
#:                   never came, the executor returned no verdict): read it where
#:                   verify_with says before sending again. ok:true, never rejected
#:                   (FF5 ruling: a write the drain ran is never `rejected` for a
#:                   reading the server could not make).


class _Unread:
    """A readback's answer when it could not make its reading (not "absent")."""

    def __bool__(self):
        return False

    def __repr__(self):
        return "UNREAD"


UNREAD = _Unread()

#: The drain's rejection reasons that are not refusals: the executor ran and
#: recorded no verdict (intent_queue.cpp finish), so the write may have landed.
DRAIN_RAN_NO_VERDICT = frozenset({"executor_returned_no_verdict"})

#: A write reply's statuses that mean "later": the write was queued (or its
#: reply never came), not executed. ck-zgh4 deploy-51 item (3): "queued" and
#: "pending" read as applied before they were named here.
PENDING_STATUSES = ("dispatched_pending", "sent_no_confirmation", "queued", "pending")


def queued_expected_change(intent: str, bridge_params: dict) -> str:
    """What the player can check for a queued do() write the server cannot read
    back itself (settle_write with no readback)."""
    p = bridge_params if isinstance(bridge_params, dict) else {}
    if intent == "interact":
        what = p.get("interaction_type") or p.get("interaction") or "the interaction"
        tgt = p.get("target_id")
        return (f"{what} to {tgt} takes effect, or its recipient answers (an AI answers "
                f"within days; inspect the target and the pending list)" if tgt else
                f"{what} takes effect")
    return f"{intent} takes effect"


def intent_id_of(raw) -> str:
    """The queued intent a write reply names (intent_request_id, or its alias
    dispatch_id); "" when the handler answered with its own verdict."""
    raw = raw if isinstance(raw, dict) else {}
    return str(raw.get("intent_request_id") or raw.get("dispatch_id") or "")


async def _dispatch_status(read, rid: str) -> tuple:
    try:
        got = _unwrap_reply(await read("query_dispatch_result", {"dispatch_id": rid}))
    except Exception:  # noqa: BLE001
        got = {}
    status = got.get("status")
    return (status if isinstance(status, str) and status else "unread"), got


async def await_drain(read, raw) -> dict:
    """STAGE 1 for a reply that queued an intent: read its verdict back
    (query_dispatch_result) on DRAIN_SCHEDULE_S, in both clock states, until it
    has one. Returns {"intent_id", "status", "row", "polls"}; status is

      "success" | "rejected" | "crashed"   the drain's verdict
      "pending"     the intent was still queued when the budget ran out
      "not_found"   neither queued nor completed at the last read (the
                    consume-to-store gap, or an id the bridge never held)
      "unread"      no read of the verdict succeeded
      "no_intent"   the reply named no intent (nothing to wait for)."""
    rid = intent_id_of(raw)
    if not rid:
        return {"intent_id": "", "status": "no_intent", "row": None, "polls": 0}
    status, row = await _dispatch_status(read, rid)
    polls = 0
    for wait_s in DRAIN_SCHEDULE_S:
        if status == DRAIN_DONE or status in DRAIN_REFUSED:
            break
        await _SLEEP(wait_s)
        status, row = await _dispatch_status(read, rid)
        polls += 1
    out = {"intent_id": rid, "status": status, "row": row or None, "polls": polls}
    if status in DRAIN_REFUSED:
        out["detail"] = await _drain_detail(read, rid, row)
    return out


#: How many of the newest drained verdicts _drain_detail looks through for the
#: refused intent (the bridge keeps intent_queue.h MAX_COMPLETED = 128).
DRAIN_DETAIL_WINDOW = 32


async def _drain_detail(read, rid: str, row) -> Optional[str]:
    """The game's own words for a refused drain (L14 finding 1), cleaned.

    query_dispatch_result carries no detail (action_handlers.cpp
    do_query_dispatch_result); the completed ring does (query_intent_results'
    `detail`, the executor's console line: the interaction's localized refusal).
    Returns the words; NO_GAME_DETAIL when the ring row was found and says
    nothing; None when it could not be looked at (no reply, or the row is no
    longer in the window) -- "nobody could look" is not "the game said nothing"."""
    own = (row or {}).get("detail") if isinstance(row, dict) else None
    if isinstance(own, str) and own:
        return engine_text(own) or NO_GAME_DETAIL
    try:
        got = _unwrap_reply(await read("query_intent_results",
                                       {"max_count": DRAIN_DETAIL_WINDOW}))
    except Exception:  # noqa: BLE001
        return None
    rows = got.get("results", got.get("completed")) if isinstance(got, dict) else None
    if not isinstance(rows, list):
        return None
    for r in reversed(rows):            # newest last: the most recent match wins
        if isinstance(r, dict) and str(r.get("intent_id") or r.get("request_id") or "") == rid:
            d = r.get("detail")
            return (engine_text(d) if isinstance(d, str) else "") or NO_GAME_DETAIL
    return None


def drain_refusal(drain: dict) -> str:
    """The refusal text of a drain verdict in DRAIN_REFUSED."""
    row = drain.get("row") or {}
    return str(row.get("rejection_reason") or f"dispatch_{drain.get('status')}")


def refusal_summary(conf: dict) -> str:
    """The summary for a settle_write refusal. A CRASHED drain stays a refusal
    (the gate names the crash), but the write may have landed before the fault:
    say so, and to read before sending again (FF5 ruling)."""
    why = str(conf.get("refused") or "")
    if (conf.get("drain") or {}).get("status") == "crashed":
        return (f"{why}: the drain crashed while running it, so its effect MAY have landed; "
                "read it before sending again")
    return why


#: A refusal's words read off a write's own reply, by the intent id it named, so
#: inspect(pending) can say them for a ring row the bridge stored without them
#: (set_title_law: the reply says "Missing prestige 960", the ring row nothing).
#: Process-local and bounded like the bridge ring.
_REPLY_REFUSAL_WORDS: dict[str, str] = {}
_REPLY_REFUSAL_WORDS_MAX = 128

#: An engine refusal line that is only a price (engine_text form): "Missing prestige 960".
_COST_LINE = re.compile(r"^Missing (prestige|gold|piety) (\d[\d.,]*)$")

#: The engine-refusal gates a cost-only refusal is re-typed from (minor 11): the
#: game's no, read as "never", when its only line is a price that can be earned.
_COST_REMAPPABLE = frozenset({"engine_refused", "interaction_invalid", "target_ineligible",
                              "unmapped_refusal"})


def reply_refusal_words(raw) -> Optional[str]:
    """The game's words in a write's own (synchronous) reply: its `engine_reason`
    cleaned, NO_GAME_DETAIL when the key is there and says nothing, None when the
    reply carries no engine text at all."""
    if not isinstance(raw, dict) or "engine_reason" not in raw:
        return None
    return engine_text(raw.get("engine_reason")) or NO_GAME_DETAIL


def _cost_gate(words: str) -> Optional[tuple]:
    """(gate, measured) when every line of the game's refusal is a price."""
    lines = [w.strip() for w in str(words or "").split(";") if w.strip()]
    if not lines:
        return None
    missing: dict[str, float] = {}
    for line in lines:
        m = _COST_LINE.match(line)
        if not m:
            return None
        missing[m.group(1)] = float(m.group(2).replace(",", ""))
    gate = f"insufficient_{next(iter(missing))}" if len(missing) == 1 else "cannot_afford"
    return gate, {"missing": missing}


def write_blocked(reason: str = "", *, summary: str = "", conf: Optional[dict] = None,
                  drain: Optional[dict] = None, raw: Any = None,
                  intent: str = "", gate: str = "", raw_reason: str = "",
                  measured: Optional[dict] = None, fix: Optional[dict] = None) -> dict[str, Any]:
    """THE `blocked` block of a refused write (L14 finding 1): every write path in
    this module builds its refusal here (test_write_refusal_words enumerates them).

    `blocked.gate`/`summary` stay the typed refusal (map_raw_reason). `detail`
    carries the game's own sentence when the refusal came from the game: the
    drain's (a settle_write `conf`, or an await_drain `drain`, which read it off
    the completed ring) or the reply's own `engine_reason` (`raw`). It is
    NO_GAME_DETAIL when the game was asked and said nothing, and absent when the
    refusal is not the game's (an argument code) or the ring could not be read.
    A refusal whose only words are a price is the resource gate, retryable
    (minor 11). `intent` names do()'s enumerate class in `next`.

    `gate` is a refusal the caller already typed (a readback that disagrees, a
    bridge switch's own refusal, a faction pre-check): the block is that gate
    with `raw_reason` (else `reason`), `measured` and `fix`, built here so the
    game's words still ride in `detail` (L14 FF1 m-8: one builder for every
    refusal a write path returns after it wrote)."""
    if conf is not None and drain is None:
        drain = conf.get("drain")
    if not summary and conf is not None and conf.get("refused"):
        summary = refusal_summary(conf)
    elif not summary and isinstance(drain, dict) and drain.get("status") == "crashed":
        summary = refusal_summary({"refused": reason, "drain": drain})
    words = drain.get("detail") if isinstance(drain, dict) else None
    if words is None:
        words = reply_refusal_words(raw)
    if gate:
        rr = raw_reason or reason
        return _with_words(why_blocked(gate, summary=summary or rr, raw_reason=rr,
                                       measured=measured, fix=fix), words, raw, intent)
    blk = map_raw_reason(reason, summary=summary or reason)
    if measured is not None:
        blk["measured"] = measured
    if fix is not None:
        blk["fix"] = fix
    if words and words != NO_GAME_DETAIL and blk["gate"] in _COST_REMAPPABLE:
        cost = _cost_gate(words)
        if cost is not None:
            blk = why_blocked(cost[0], summary=summary or reason, measured=cost[1],
                              raw_reason=blk.get("raw_reason") or reason)
    return _with_words(blk, words, raw, intent)


def _with_words(blk: dict, words, raw, intent: str) -> dict:
    """write_blocked's tail: the game's words in `detail`, remembered by intent id."""
    if words:
        blk["detail"] = words
        rid = intent_id_of(raw)
        if rid and reply_refusal_words(raw):
            _REPLY_REFUSAL_WORDS[rid] = words
            while len(_REPLY_REFUSAL_WORDS) > _REPLY_REFUSAL_WORDS_MAX:
                _REPLY_REFUSAL_WORDS.pop(next(iter(_REPLY_REFUSAL_WORDS)))
    return concrete_next(blk, intent) if intent else blk


async def confirm_write(clock, read, send, readback, *, deadline_s: float = 12.0,
                        max_polls: int = 8, poll_interval_s: Optional[float] = None) -> dict:
    """THE write-then-readback rule for both gateways' confirmed_write: take the
    clock at submit, dispatch with `send()`, then settle_write. `clock()` is the
    gateway's fresh clock reading (read_clock); `read` its read; `readback(raw)`
    an async predicate returning the resolved object (truthy) or None."""
    game_date_at_submit = (await clock()).get("date")
    raw = await send()
    return await settle_write(clock, read, raw, readback, game_date_at_submit=game_date_at_submit,
                              deadline_s=deadline_s, max_polls=max_polls,
                              poll_interval_s=poll_interval_s)


#: The listener's answer to a call that was IN FLIGHT when the connection
#: dropped (mcp_server.py _listen, `except ConnectionError`: every pending future
#: gets {"error":"bridge_disconnected"}). The call had been sent, so the bridge
#: may have received it and run it. For a WRITE that is "may have landed": its
#: own raw code, mapped to write_unconfirmed (not retryable, read back first),
#: never the gateway's retryable bridge_disconnected, which is the shape of a
#: call that was never sent ({"reason":"bridge_disconnected"},
#: BridgeGateway._dispatch while hung). A READ keeps bridge_disconnected.
DISCONNECTED_IN_FLIGHT = "bridge_disconnected_in_flight"

#: The `error` values a READ reply carries that are never data (L14 FF3 N3-1):
#: the bridge's own fault and timeout codes (RAW_REASON_TO_GATE -> bridge_fault)
#: and the listener's in-flight disconnect. Any other string `error` on a read
#: can sit beside the reading (query_handlers.cpp: the title-fraction reply
#: returns true with title_id -1 and error "character_unresolved"; the
#: interaction-acceptance reply carries valid/would_accept beside its error), so
#: a read's `error` is not, by itself, a refusal; a write's is (reply_refusal).
READ_FAULT_ERRORS = frozenset(
    {k for k, g in RAW_REASON_TO_GATE.items() if g == "bridge_fault"} | {"bridge_disconnected"})

#: Where a READ's refusal resolves differently from a write's: a game-thread
#: read that did not run in time ran nothing (pipeline.cpp serve, ipc_server.cpp
#: serve_on_game_thread: "Nothing was read.") or is merely late, so it is
#: read_timeout, retryable, never the write's "may have landed" bridge_fault.
READ_REASON_TO_GATE = {"game_thread_timeout": "read_timeout"}


def _refusal_shape(raw, error_counts) -> Optional[str]:
    """THE shape rules a reply refuses by, shared by reply_refusal (writes) and
    read_refusal (reads): None when the reply does not refuse, else its raw
    reason ("" when it named none).

    A reply refuses when it does not say "later" (PENDING_STATUSES), does not say
    `success: true`, and says no in one of the shapes the bridge really sends:
      - `success: false`, or status "rejected" (a handler's own refusal);
      - a string `error` with no `success` that `error_counts` accepts (L14 FF2
        N-MAJ-1): the bridge's fault and timeout replies carry no `success` at
        all — `{"error":"memory_access_fault"}` (MEMORY_ACCESS_FAULT_REPLY,
        bridge.h, every answer_memory_access_fault and the SIGSEGV recovery in
        ipc_server.cpp serve_on_game_thread) and
        `{"error":"game_thread_timeout",...}` (the same function, job not run in
        time) — and the listener's in-flight `{"error":"bridge_disconnected"}`;
      - the gateway's own `{"reason":"bridge_disconnected"}` (mcp_server.py
        BridgeGateway._dispatch when the bridge is hung)."""
    if not isinstance(raw, dict) or raw.get("status") in PENDING_STATUSES:
        return None
    if raw.get("success") is True:
        return None
    err = raw.get("error")
    faulted = (isinstance(err, str) and bool(err) and error_counts(err)) \
        or raw.get("reason") == "bridge_disconnected"
    if raw.get("success") is not False and raw.get("status") != "rejected" and not faulted:
        return None
    return str(raw.get("reason") or raw.get("error") or raw.get("status") or "")


def reply_refusal(raw) -> str:
    """The refusal a WRITE's reply states for itself, or "". ONE rule for
    settle_write's synchronous verdict, the mint (_confirm_intent_write), the
    session guard (unwitnessed_applied) and every write branch that reads a reply;
    the shapes are _refusal_shape's, with any string `error` counting.

    The listener's in-flight disconnect (no `reason`, `error`
    "bridge_disconnected") is DISCONNECTED_IN_FLIGHT: the write may have landed
    (L14 FF3 N3-3). A reason that means "not confirmed yet" (RAW_REASON_TO_GATE
    -> confirmation_pending) is a verdict not read, never a refusal."""
    reason = _refusal_shape(raw, lambda e: True)
    if reason is None:
        return ""
    if raw.get("error") == "bridge_disconnected" and not raw.get("reason"):
        return DISCONNECTED_IN_FLIGHT
    reason = reason or "refused_without_reason"
    return "" if map_raw_reason(reason).get("gate") == "confirmation_pending" else reason


def read_refusal(raw, name: str = "the read") -> Optional[dict[str, Any]]:
    """The why_blocked for a READ's reply that refuses, or None when it answered
    (the read twin of reply_refusal, L14 FF3 N3-1: one module, one set of shape
    rules, _refusal_shape). A refusal is never a reading: shown as ok=true it
    reads like data. Its `error` counts only when it is one of READ_FAULT_ERRORS;
    a game-thread timeout is read_timeout (retryable), a fault bridge_fault, the
    listener's disconnect bridge_disconnected (retryable: a read changed
    nothing)."""
    reason = _refusal_shape(raw, lambda e: e in READ_FAULT_ERRORS)
    if reason is None:
        return None
    if not reason:
        return why_blocked("unmapped_refusal", raw_reason="declined",
                           summary=f"the bridge declined {name} and named no reason")
    summary = f"the bridge declined {name}: {reason}"
    gate = READ_REASON_TO_GATE.get(reason)
    if gate:
        return why_blocked(gate, summary=summary, raw_reason=reason)
    return map_raw_reason(reason, summary=summary)

async def settle_write(clock, read, raw, readback, *, game_date_at_submit,
                       deadline_s: float = 12.0, max_polls: int = 8,
                       poll_interval_s: Optional[float] = None) -> dict:
    """THE rule for what a write's reply may claim (L13 contract D5, amendment 3,
    and the FF2 ruling): ONE definition, for confirm_write and the do() tail.

    `applied` needs an EFFECT the server read back. A write has two waits:

      STAGE 1, the DRAIN, in BOTH clock states. The bridge executes the queued
      write on the session thread's next pass. If the reply names an intent its
      verdict is read back (await_drain); then `readback(raw)` is re-run on
      DRAIN_SCHEDULE_S until it resolves (an outcome the drain itself produced:
      a war listed, a title held, an ended war in the ledger). A drain refusal
      with no readback is `refused`. A verdict never read is
      drains_when "next_pass".

      STAGE 2, the WORLD, only once the write ran and its outcome is still not
      visible: it needs game days. `readback` is re-run while a FRESH clock
      reading says the game is running, at most `max_polls` times,
      `poll_interval_s` apart, within `deadline_s`. Paused (or unread) it stops
      at once: drains_when "clock_advances". Nothing here moves the clock.

    `readback=None` is a lever with NO readback (most interactions: the drain's
    "success" means the interaction was SENT, and the recipient answers in its
    own time). Such a write is never resolved: once it ran it is
    drains_when "clock_advances" with `sent: True`, and the caller names what the
    player can check (expected_change). `clock` is not called on that path.

    Returns {"raw", "resolved", "polls", "drain", "refused", "drains_when",
    "sent", "intent_id", "game_date_at_submit"}: exactly one of resolved /
    refused / drains_when is set; `sent` is True once the write ran. Callers
    build the pending block with conf_pending(conf)."""
    raw = raw if isinstance(raw, dict) else {}
    drain = await await_drain(read, raw)
    out = {"raw": raw, "resolved": None, "polls": drain["polls"], "drain": drain,
           "refused": None, "drains_when": None, "sent": False,
           "intent_id": drain["intent_id"], "game_date_at_submit": game_date_at_submit}
    # Did the write run? A queued intent ran when the drain says "success". A reply
    # that queued nothing carries the handler's own verdict: success True ran,
    # success False is its refusal, and no `success` at all (the client's
    # sent_no_confirmation, an empty reply) is a verdict never read. A reply that
    # says "later" (PENDING_STATUSES) is no verdict whatever its `success` says.
    sync_verdict = (raw.get("success") if drain["status"] == "no_intent"
                    and raw.get("status") not in PENDING_STATUSES else None)
    # An executor that ran and recorded no verdict ran: never a refusal.
    no_verdict = (drain["status"] in DRAIN_REFUSED
                  and str((drain.get("row") or {}).get("rejection_reason") or "")
                  in DRAIN_RAN_NO_VERDICT)
    ran = drain["status"] == DRAIN_DONE or sync_verdict is True or no_verdict
    out["sent"] = ran
    out["unread"] = no_verdict
    # The drain's refusal comes FIRST (witness r4 m1): a refused write's readback
    # can still "resolve" on something else (an expense on the sender's purse, a
    # law already listed), and that must never outrank the game's own no.
    if drain["status"] in DRAIN_REFUSED and not no_verdict:
        out["refused"] = drain_refusal(drain)
        return out
    # A reply that queued nothing and refuses (reply_refusal: success false, or
    # the bridge's fault/timeout shapes that carry no `success`) is refused here,
    # before any readback can "resolve" on a world the write never touched.
    if drain["status"] == "no_intent" and reply_refusal(raw):
        out["refused"] = reply_refusal(raw)
        return out
    async def _read_back():
        got = await readback(raw)
        if got is UNREAD:              # the reading could not be made: keep trying
            out["unread"] = True
            return None
        out["unread"] = no_verdict
        return got

    resolved = await _read_back() if readback is not None else None
    if resolved is None and ran and readback is not None:
        for wait_s in DRAIN_SCHEDULE_S:
            await _SLEEP(wait_s)
            out["polls"] += 1
            resolved = await _read_back()
            if resolved is not None:
                break
    if resolved is not None:
        out["resolved"] = resolved
        return out
    if not ran:
        out["drains_when"] = "next_pass"
        return out
    if out["unread"]:
        # it ran, and the reading that would show it was never made (FF5 ruling)
        out["drains_when"] = "readback"
        return out
    if readback is None:
        out["drains_when"] = "clock_advances"
        return out
    loop = asyncio.get_running_loop()
    deadline = loop.time() + deadline_s
    world_polls = 0
    while world_polls < max_polls and loop.time() < deadline:
        if (await clock()).get("paused") is not False:
            break
        await _SLEEP(WORLD_POLL_INTERVAL_S if poll_interval_s is None else poll_interval_s)
        world_polls += 1
        out["polls"] += 1
        resolved = await readback(raw)
        if resolved is not None and resolved is not UNREAD:
            out["resolved"] = resolved
            return out
    out["drains_when"] = "clock_advances"
    return out


async def _confirm_write(gw, action: str, params: dict, *, readback,
                         deadline_s: float = 12.0) -> dict:
    """Dispatch a write and resolve it by readback, through gw.confirmed_write
    (confirm_write)."""
    out = await gw.confirmed_write(action, params, readback=readback, deadline_s=deadline_s)
    if not isinstance(out, dict):
        raise TypeError(f"confirmed_write returned {type(out).__name__}, not the confirm_write dict")
    return out


def write_pending(drains_when: str, *, game_date_at_submit, intent_id: str = "",
                  **fields) -> dict:
    """THE pending block for a write (L13 contract D5, amendment 3). `drains_when`
    names the wait: "next_pass" (stage 1, the drain's verdict was not read in its
    budget) or "clock_advances" (stage 2, the outcome needs game days), beside the
    game date the write was submitted on and the intent id when it queued one.
    Nothing advances the clock for either; a running game or the player's
    session(wait/unpause) does for the second. Any other value is refused: a
    pending must say which wait it is."""
    if drains_when not in DRAINS_WHEN:
        raise ValueError(f"drains_when {drains_when!r} is not one of {DRAINS_WHEN}")
    out = {**fields, "drains_when": drains_when, "game_date_at_submit": game_date_at_submit}
    if intent_id:
        out["intent_id"] = intent_id
    # L14 minor 13: the token IS intent_id; a poll_token that repeats it (or is
    # empty) is noise, kept only when it names something else.
    if not out.get("poll_token") or out.get("poll_token") == intent_id:
        out.pop("poll_token", None)
    return out


def conf_pending(conf: dict, **fields) -> dict:
    """write_pending from a confirm_write result."""
    return write_pending(conf.get("drains_when"),
                         game_date_at_submit=conf.get("game_date_at_submit"),
                         intent_id=conf.get("intent_id") or "", **fields)


#: THE #131 de-jure bar (L14 FF1 m-2: one statement): a KINGDOM/EMPIRE mint
#: (tier >= MINT_DEJURE_MIN_TIER) needs the realm to hold >= MINT_DEJURE_MIN_FRAC of
#: the title's de-jure counties; a duchy or county is the game's own can_create
#: rule alone. Mirrors bridge/src/intent_dispatch.cpp CREATE_TITLE_DEJURE_MIN_TIER
#: and CREATE_TITLE_DEJURE_MIN_FRAC (test_mint_price checks the mirror). The
#: warning text, the formable_titles rows and the mint pre-check all read these.
MINT_DEJURE_MIN_TIER = 4
MINT_DEJURE_MIN_FRAC = 0.51
MINT_DEJURE_RULE = (f"KINGDOM/EMPIRE tier (>={MINT_DEJURE_MIN_TIER}) also needs "
                    f">={round(MINT_DEJURE_MIN_FRAC * 100)}% REALM-inclusive de-jure control "
                    "(ruler + transitive vassals); a duchy or county needs only the game's own "
                    "can_create rules")
#: A landed title's tier from its key's prefix: CK3's own naming convention, the
#: fallback the bridge uses too (query_handlers.cpp do_query_title_hierarchy).
_TIER_BY_KEY_PREFIX = {"b": 1, "c": 2, "d": 3, "k": 4, "e": 5}


def title_tier_of_key(key) -> Optional[int]:
    """The tier a title key names (b_/c_/d_/k_/e_), or None when it names none."""
    k = str(key or "")
    return _TIER_BY_KEY_PREFIX.get(k[0]) if len(k) > 2 and k[1] == "_" else None


def dejure_need(total: int) -> int:
    """Counties the bar needs of `total` (the bridge refuses held/total < the FRAC)."""
    return int(total * MINT_DEJURE_MIN_FRAC + 0.999) if total else 0


def mint_dejure_shortfall(tkey, held: int, total: int) -> Optional[dict]:
    """{held, need, total} when the #131 bar applies to tkey and is not met; else
    None. A title whose tier the key does not name, or a titular title (total 0),
    is left to the bridge's gate, which reads the title's own tier."""
    tier = title_tier_of_key(tkey)
    if tier is None or tier < MINT_DEJURE_MIN_TIER or total <= 0:
        return None
    need = dejure_need(total)
    return {"held": held, "need": need, "total": total} if held < need else None


def _parse_dejure_counts(reason: str) -> Optional[dict]:
    """Parse 'create_title_refused_insufficient_de_jure_control_<held>_of_<total>_counties'
    → {'held': h, 'need': ceil(0.51*total), 'total': t}."""
    import re
    m = re.search(r"de_jure_control_(\d+)_of_(\d+)", str(reason or ""))
    if not m:
        return None
    held, total = int(m.group(1)), int(m.group(2))
    return {"held": held, "need": dejure_need(total), "total": total}


# --------------------------------------------------------------------------- declare_war
def war_pre_read_blocked(reply) -> Optional[dict[str, Any]]:
    """declare_war's get_active_wars pre-read: None when it carries the war list
    (do_get_active_wars writes `wars`), else the typed refusal. A reply the
    bridge refused is read_refusal's gate; no reply at all (both gateways answer
    {} on a timeout or a dropped connection) is read_timeout; a reply without
    the list is unmapped. Never "no wars" (L15 witness m7)."""
    blocked = read_refusal(reply, "get_active_wars (declare_war's pre-read)")
    if blocked is not None:
        return blocked
    if isinstance(reply, dict) and isinstance(reply.get("wars"), list):
        return None
    why = ("the war cap and the already-at-war check stand on this read, so the war "
           "is not declared; read it again (observe) and retry")
    if not reply:
        return why_blocked("read_timeout", raw_reason="absent",
                           summary=f"get_active_wars (declare_war's pre-read) did not come back: {why}")
    return why_blocked("unmapped_refusal", raw_reason="no_wars_list",
                       summary=f"get_active_wars (declare_war's pre-read) carried no war list: {why}")


def war_targets_title(war_row, title_id: int) -> bool:
    """True when a get_active_wars row's target_titles (the CB's title array,
    query_handlers.cpp do_get_active_wars) holds `title_id`. A row without the
    list (unread) holds nothing."""
    tt = war_row.get("target_titles") if isinstance(war_row, dict) else None
    return isinstance(tt, list) and _as_int(title_id) in {_as_int(t) for t in tt}


async def declare_war_impl(gw, character_id: int = 0, declare_args: dict = None,
                           target_id: int = 0, cb: str = "", cb_title_id: int = 0,
                           claimant_id: int = 0, confirm_straddle: bool = False) -> dict[str, Any]:
    actor, err = await _resolve_actor(gw, character_id, "declare_war")
    if err:
        return err
    # F1: character_id=0 means "the bound ruler". Every match below compares a war's
    # attacker_id against the actor (never 0), and the dispatch carries it as
    # actor_id, so the actor is resolved to its id FIRST. Before this, a 0 actor
    # matched no war (the idempotency check could not fire) and reached the bridge
    # as actor_id=0, which refused it: live on the 867 save (l1f1m-848f9b20) eight
    # plan_war rows across four rulers all came back missing_required_argument and
    # no war landed; with the fix the same first row (l1f1d-848f9b20) landed.
    character_id = character_id or _as_int((actor or {}).get("id"))
    if not character_id:
        return envelope("declare_war", ok=False, actor=actor, game=gw.game(),
                        blocked=why_blocked("actor_unresolved",
                                            summary="character_id=0 and the bound ruler's id could not be read"),
                        bridge_health=gw.health)

    da = dict(declare_args or {})
    tid = _as_int(da.get("target_id") or target_id)
    cb_name = da.get("cb") or cb
    cbt = _as_int(da.get("cb_title_id") or cb_title_id)
    clm = _as_int(da.get("claimant_id") or claimant_id)
    if not tid or not cb_name:
        return envelope("declare_war", ok=False, actor=actor, game=gw.game(),
                        blocked=why_blocked("missing_required_argument", summary="target_id and cb (or declare_args) are required"),
                        bridge_health=gw.health)

    # Crash-vector guardrail (inv_bridge-map §5): cap simultaneous wars.
    wars0 = await gw.read("get_active_wars", {"character_id": character_id})
    # L15 witness m7: the war cap and the already-at-war check both stand on this
    # read. When it fails, neither can be made, and a war declared then counts
    # every war as new in the readback: REFUSE, typed, never skip (it used to
    # read the failure as "no wars").
    pre_blocked = war_pre_read_blocked(wars0)
    if pre_blocked is not None:
        return envelope("declare_war", ok=False, actor=actor, game=gw.game(),
                        blocked=pre_blocked, bridge_health=gw.health)
    pre_wars = [w for w in wars0["wars"] if isinstance(w, dict)]
    active = len(pre_wars)
    if active >= 20:
        return envelope("declare_war", ok=False, actor=actor, game=gw.game(),
                        blocked=why_blocked("war_cap_reached", summary=f"refusing: {active} simultaneous wars (heap-corruption crash vector)"),
                        bridge_health=gw.health)

    cb_lower = str(cb_name).lower()
    # A mode-0 CB (subjugation, vassalization, make_tributary_cb) takes no title and
    # no claimant: the marshalling below zeroes both, and the idempotency matches on
    # the CB alone (its war rows carry target_titles []).
    is_mode0 = _is_mode0_cb(cb_name)
    title_match = 0 if is_mode0 else cbt

    def _is_my_attacker_war_vs(w: dict, against: int = 0, cb_match: str = "",
                               title: int = 0) -> bool:
        """A war with THIS actor as primary attacker (optionally vs `against` /
        `cb_match` / the CB title `title`)."""
        if _as_int(w.get("attacker_id")) != character_id:
            return False
        if against and _as_int(w.get("defender_id")) != against:
            return False
        # get_active_wars emits cb_type (query_handlers.cpp do_get_active_wars,
        # war_cb_type_name); a row without one is a war of UNREAD CB, which
        # does not match a named CB (it used to, and any attacker war vs the
        # target answered `applied`: witness r4 NIT-3).
        if cb_match and str(w.get("cb_type") or "") != cb_match:
            return False
        # ... and target_titles (read_war_target_titles, CB_TITLE_ARRAY): a war
        # for another title is another war (L15 witness m7); a row without the
        # list is a war of UNREAD title and does not match a named one.
        if title and not war_targets_title(w, title):
            return False
        return True

    def _war_result(w: dict) -> dict:
        return {"war_id": w.get("war_id"), "verified_active": True,
                "target_id": _as_int(w.get("defender_id")) or tid,
                "target": da.get("target_name") or w.get("defender_id") or tid,
                "cb": w.get("cb_type") or cb_name, "name": w.get("name")}

    pre_war_ids = {_as_int(w.get("war_id")) for w in pre_wars}

    # (1) Idempotency: if a war with this actor as attacker vs the same target and CB
    # is ALREADY active, return it WITHOUT re-dispatching. This
    # stops the duplicate-declaration churn the live agent fell into (re-declaring a war
    # that already landed because the prior call false-rejected on the 5s window).
    for w in pre_wars:
        if _is_my_attacker_war_vs(w, against=tid, cb_match=cb_name, title=title_match):
            return envelope("declare_war", verdict="applied", result=_war_result(w),
                            actor=actor, game=gw.game(),
                            warnings=[{"code": "already_at_war",
                                       "detail": "actor is already at war as attacker vs this target — not re-declaring"}],
                            bridge_health=gw.health)

    # Pre-check pressed-claim + target-state via strict-CB (kills A3) BEFORE dispatch.
    # Bounded retry-on-busy so a transient g_cb_query slot collision does not skip the
    # pre-check (a busy read leaves `match` None → we fall through and dispatch anyway,
    # but the retry gives the pre-check a real chance first).
    strict = await _cb_read_with_retry(gw, "query_cb_validity_strict",
                                       {"actor_id": character_id, "target_id": tid,
                                        "include_ineligible": True, "timeout_ms": 6000})
    strict = strict if isinstance(strict, dict) else {}
    match = next((r for r in (strict.get("results") or []) if r.get("cb_name") == cb_name), None)
    if match is not None and not (match.get("declarable") or
                                  (match.get("outer_status") == "pass" and match.get("inner_status") == "pass")):
        cull = match.get("cull_reason") or match.get("inner_status") or "target_ineligible"
        return envelope("declare_war", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=write_blocked(cull, summary=cull), bridge_health=gw.health)

    # confirm_straddle hard guard.
    if not confirm_straddle:
        sp = await gw.read("query_succession", {"character_id": character_id})
        chr_ = await gw.read("query_character", {"character_id": character_id})
        age = _f(_char_section(chr_).get("age"))
        if age >= STRADDLE_AGE:
            return envelope("declare_war", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=why_blocked("succession_straddle",
                                                summary="ruler near death; war may resolve across the succession — pass confirm_straddle=true to proceed"),
                            bridge_health=gw.health)

    # great_holy_war is religious-head-lifecycle-only — the WCC resolves the name but a
    # hand-declared GHW is not the engine's real path (inv_gamecb.md §7). Reject up front
    # rather than dispatch a war that won't stick.
    if "great_holy_war" in cb_lower:
        return envelope("declare_war", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("custom_scope_bridge_cannot_fire",
                                            summary="great_holy_war uses the religious-head CGreatHolyWar lifecycle, not a declarable CB — unsupported via steered WCC"),
                        bridge_health=gw.health)

    # Per-CB marshalling (inv_gamecb.md call table). Mode-0 CBs (is_mode0, above)
    # take NO cb_title_id and NO claimant. claim_cb takes claimant_id
    # (default = self-press to the actor). Title-targeted conquest/de_jure/holy_war CBs
    # take cb_title_id but NO claimant. We detect mode-0/claim by CB-name family so the
    # marshalling is right whether declare_args came from plan_war or the escape hatch.
    is_claim = is_claim_cb_name(cb_lower)
    if is_mode0:
        cbt = 0          # target_mode 0: no title
        clm = 0          # no claimant
    elif is_claim:
        clm = clm or character_id   # self-press defaults claimant to the actor
    else:
        clm = 0          # title-targeted conquest/de_jure/holy_war: no claimant

    # Dispatch via the steered path (resolve_act → call_war_create_core), confirmed by
    # readback (confirm_write). call_war_create_core runs on the game thread and
    # queues the war intent, which the drain runs on the session thread's NEXT pass,
    # paused or running: its reply says "no_poll_on_game_thread" (no wait happened;
    # drains_when "next_pass") and names the intent. Stage 1 reads that intent's verdict back and then the war list:
    # the war is listed once the intent ran. The readback scans get_active_wars for
    # an attacker-war vs target_id under cb, else a NEW one vs target_id, else any
    # new attacker-war since the pre-dispatch snapshot.
    act_params = {"type": "war", "method": "steered", "actor_id": character_id,
                  "target_id": tid, "casus_belli": cb_name, "cb_title_id": cbt,
                  "claimant_id": clm}
    bridge_action, bridge_params = _tools.resolve_act(act_params)

    async def _scan_for_landed_war(_raw) -> Optional[dict]:
        wars_now = await _read_dict(gw, "get_active_wars", {"character_id": character_id})
        wn = (wars_now or {}).get("wars", []) if isinstance(wars_now, dict) else []
        for w in wn:                                          # exact target+CB(+title) match
            if _is_my_attacker_war_vs(w, against=tid, cb_match=cb_name, title=title_match):
                return w
        for w in wn:                                          # a NEW attacker-war vs target
            # (new only: a pre-existing war vs this target under another CB is
            # not the one this call declared)
            if _is_my_attacker_war_vs(w, against=tid) and _as_int(w.get("war_id")) not in pre_war_ids:
                return w
        for w in wn:                                          # any NEW attacker-war
            if _is_my_attacker_war_vs(w) and _as_int(w.get("war_id")) not in pre_war_ids:
                return w
        return None

    conf = await _confirm_write(gw, bridge_action, bridge_params,
                                readback=_scan_for_landed_war, deadline_s=14.0)
    raw = conf.get("raw") if isinstance(conf, dict) else {}
    raw = raw if isinstance(raw, dict) else {}
    landed = conf.get("resolved") if isinstance(conf, dict) else None
    if landed is not None:
        return envelope("declare_war", verdict="applied", result=_war_result(landed),
                        actor=actor, game=gw.game(), bridge_health=gw.health)

    # (3) No new war in the readback. A refusal (the drain's verdict, or the
    # handler's own success:false when it queued nothing) is a typed gate. Otherwise the pending names its wait: the
    # intent's verdict was not read within the drain budget ("next_pass"), or the
    # intent ran and the war is not listed yet ("clock_advances": the engine
    # settles it on a later day). VERIFY before re-declaring, never blindly retry.
    if conf.get("refused"):
        reason = conf["refused"]
        return envelope("declare_war", ok=False, verdict="rejected", actor=actor,
                        game=gw.game(), blocked=write_blocked(reason, conf=conf, raw=raw),
                        result={"target_id": tid, "cb": cb_name, "raw": raw,
                                "drain": (conf.get("drain") or {}).get("row")},
                        bridge_health=gw.health)
    # One pending spelling with do() (L14 FF1 m-4): ok:true, verdict pending, the
    # pending block names its wait; no `blocked` beside it. raw_reason only when
    # the reply said one (never a default asserting that no poll ran).
    raw_reason = str(raw.get("reason") or raw.get("error") or raw.get("status") or "")
    waiting = ("its verdict was not read within the drain budget"
               if conf.get("drains_when") == "next_pass" else
               "the intent ran and the war is not listed yet; it settles as game days pass")
    return envelope("declare_war", verdict="pending", actor=actor, game=gw.game(),
                    pending=conf_pending(
                        conf, expected_change="new attacker-war vs target",
                        why_pending=f"the war was dispatched and is unconfirmed: {waiting}",
                        verify_with=("observe / get_active_wars before declaring again; do NOT "
                                     "blindly retry"),
                        **({"raw_reason": raw_reason} if raw_reason else {})),
                    result={"target_id": tid, "cb": cb_name, "sent": conf.get("sent")},
                    bridge_health=gw.health)


# --------------------------------------------------------------------------- succession_plan
#: Realm laws whose `flag = can_designate_heirs` lets a ruler use designate_heir_interaction
#: (game/common/laws/00_realm_laws.txt and 00_succession_laws.txt, read 2026-10-20;
#: the engine's can_designate_heir_trigger, 00_legal_triggers.txt, checks that flag for
#: every government but administrative, where a noble-family title does it).
DESIGNATE_HEIR_LAWS = frozenset({
    "crown_authority_3",
    "japanese_bureaucracy_0", "japanese_bureaucracy_1",
    "japanese_bureaucracy_2", "japanese_bureaucracy_3",
    "single_heir_succession_kurultai_law", "acclamation_succession_law",
    "landless_adventurer_succession_law", "japanese_regency_succession_law",
    "mandala_succession_law", "meritocratic_regency_succession_law",
})

#: Realm laws that carry `flag = title_revocation_allowed`, which revoke_title_interaction's
#: is_valid_showing_failures_only asks of every non-administrative actor
#: (game/common/laws/00_realm_laws.txt, read 2026-10-20). The groups are cumulative, so
#: every level above the first that carries the flag carries it too.
#: The bureaucracy law groups only administrative realms have (imperial, celestial,
#: meritocratic; 00_realm_laws.txt). An administrative actor needs no law to revoke
#: (the interaction's `trigger_else_if = { limit = { government_allows = administrative } }`),
#: and an AI county-tier governor of an administrative realm accepts a revocation at once
#: (its auto_accept `has_admin_gov`).
ADMINISTRATIVE_REALM_LAWS = frozenset(
    group + "_" + str(level)
    for group in ("imperial_bureaucracy", "celestial_bureaucracy", "meritocratic_bureaucracy")
    for level in range(4))

REVOKE_TITLE_LAWS = frozenset({
    "crown_authority_1", "crown_authority_2", "crown_authority_3",
    "tribal_authority_2", "tribal_authority_3",
    "nomadic_authority_1", "nomadic_authority_2", "nomadic_authority_3",
    "nomadic_authority_4", "nomadic_authority_5",
    "mandala_decree_none", "mandala_decree_prosperity", "mandala_decree_expansion",
    "mandala_decree_reverence",
    "japanese_bureaucracy_1", "japanese_bureaucracy_2", "japanese_bureaucracy_3",
}) | ADMINISTRATIVE_REALM_LAWS

#: The prestige designate_heir_interaction costs (script value designate_heir_cost,
#: game/common/script_values/00_interaction_values.txt; an administrative realm holding a
#: noble-family title pays designate_heir_admin_prestige_cost 500 plus 150 influence
#: instead). The interaction's is_valid_showing_failures_only checks it before sending.
DESIGNATE_HEIR_PRESTIGE_COST = 1000
DESIGNATE_HEIR_ADMIN_PRESTIGE_COST = 500     # designate_heir_admin_prestige_cost
DESIGNATE_HEIR_ADMIN_INFLUENCE_COST = 150    # designate_heir_admin_influence_cost

#: Realm laws only administrative realms carry, for designate_heir's cost: the
#: bureaucracy groups above plus Ritsuryo's japanese_bureaucracy (an administrative
#: government that revokes under its own law flag). Live867 l4fix-live-3: 10357
#: (japanese_bureaucracy_1) was refused "does not have influence 150".
DESIGNATE_HEIR_ADMIN_LAWS = ADMINISTRATIVE_REALM_LAWS | frozenset(
    "japanese_bureaucracy_" + str(level) for level in range(4))

#: Traits that make a child ineligible as designated heir (designate_heir_interaction's
#: is_valid_showing_failures_only, 00_heir.txt; the eunuch trigger reads `eunuch`).
DESIGNATE_HEIR_BARRING_TRAITS = frozenset({"disinherited", "devoted", "order_member",
                                           "bastard", "eunuch", "eunuch_1"})

#: The realm-law groups that decide how titles pass (00_succession_laws.txt).
SUCCESSION_LAW_GROUPS = ("succession_order_laws", "succession_gender_laws",
                         "succession_faith", "succession_rel_head")


async def _designate_heir_lever(gw, ruler: dict, children: set, designators: list,
                                admin: bool = False) -> dict:
    """designate_heir as the engine checks it before sending, from reads: a realm law
    that allows it, the prestige it costs, and the children it can name (own house, none
    of the barring traits, not already designated). `available` is true only when all
    three hold; each one that fails is named in `why_not`. The engine checks more at
    send time (an elective primary title whose heir is of your dynasty, a co-rulership);
    a refusal comes back with the engine's own text in inspect(pending).completed[].detail."""
    why_not = []
    if not designators:
        why_not.append("no realm law with can_designate_heirs (crown_authority_3, or a "
                       "succession law that carries it); raise crown authority first")
    cost = DESIGNATE_HEIR_ADMIN_PRESTIGE_COST if admin else DESIGNATE_HEIR_PRESTIGE_COST
    prestige = ruler.get("prestige")
    prestige_ok = None
    if isinstance(prestige, (int, float)) and not isinstance(prestige, bool):
        prestige_ok = float(prestige) >= cost
        if not prestige_ok:
            why_not.append(f"designating costs {cost} prestige; "
                           f"you have {float(prestige):.0f}")
    current = _as_int(ruler.get("designated_heir_id"))
    house = ruler.get("house_id")
    candidates, barred = [], []
    for cid in sorted(c for c in children if c):
        sect = _char_section(await gw.read("query_character", {"character_id": cid}))
        if not sect or sect.get("alive") is False:
            continue
        traits = set()
        for t in sect.get("traits") or []:
            name = (t or {}).get("name") if isinstance(t, dict) else None
            if name:
                traits.add(str(name))
        bars = sorted(traits & DESIGNATE_HEIR_BARRING_TRAITS)
        if house is not None and sect.get("house_id") != house:
            bars.append("another house")
        if cid == current:
            bars.append("already your designated heir")
        if bars:
            barred.append({"id": cid, "why": bars})
        else:
            candidates.append(cid)
    if not candidates:
        why_not.append("no child can be named: each is of another house, already "
                       "designated, or disinherited/bastard/devoted/order member/eunuch")
    available = bool(designators) and bool(candidates) and prestige_ok is not False
    unread = None
    if available and admin:
        # No read carries influence: the lever does not claim what it cannot see.
        available, unread = None, (
            f"an administrative realm holding a noble-family title also pays "
            f"{DESIGNATE_HEIR_ADMIN_INFLUENCE_COST} influence, which no read shows; "
            f"the engine's refusal names it")
    return {
        "available": available,
        "unread": unread,
        "granted_by": designators,
        "prestige_cost": cost,
        "influence_cost": DESIGNATE_HEIR_ADMIN_INFLUENCE_COST if admin else 0,
        "candidates": candidates,
        "barred": barred,
        "why_not": "; ".join(why_not) or None,
        "call": ("do(intent='interact', args={'interaction': 'designate_heir_interaction', "
                 "'target_id': <a candidate>}); the engine checks the rest when it is "
                 "sent and a refusal carries its own text in inspect(pending)"),
        "readback": "inspect(subject='character', character_id=<you>).designated_heir_id",
    }


async def _succession_levers(gw, ruler_id: int, heirs: list, ruler_raw) -> dict[str, Any]:
    """The succession levers a CK3 player has for this ruler, from stable reads only:
    each heir's age (query_character), the realm's succession laws and whether one of
    them allows designating an heir (query_realm_laws), and the call that pulls each
    lever. A lever this ruler does not have says why, in the engine's terms."""
    ruler = _char_section(ruler_raw)
    children = {_as_int(c) for c in (ruler.get("children") or [])}   # bare ids
    heir_rows = []
    for h in (heirs or [])[:8]:
        hid = _as_int((h or {}).get("id") or (h or {}).get("character_id"))
        if not hid:
            continue
        sect = _char_section(await gw.read("query_character", {"character_id": hid}))
        age = _as_int(sect.get("age"))
        heir_rows.append({"id": hid, "name": sect.get("name") or (h or {}).get("name"),
                          "age": age, "adult": age >= 16,
                          "own_child": hid in children,
                          "is_female": sect.get("is_female")})
    laws_raw = await gw.read("query_realm_laws", {"character_id": ruler_id})
    active = [l for l in ((laws_raw or {}).get("active_laws") or []) if isinstance(l, dict)]
    names = [str(l.get("name") or "") for l in active]
    succession_laws = [str(l.get("name")) for l in active
                       if l.get("group_name") in SUCCESSION_LAW_GROUPS]
    designators = sorted(set(names) & DESIGNATE_HEIR_LAWS)
    revokers = sorted(set(names) & REVOKE_TITLE_LAWS)
    own_heirs = [r["id"] for r in heir_rows if r["own_child"]]
    designate = await _designate_heir_lever(gw, ruler, children, designators,
                                            admin=bool(set(names) & DESIGNATE_HEIR_ADMIN_LAWS))
    # The engine's disinherit needs the actor to be his dynasty's head. The dynasty is
    # read by its id (query_character.dynasty_id, from the house), never through
    # query_dynasty's character fallback (family+0x10 is the betrothed, re_interactions.md).
    dynasty_id = _as_int(ruler.get("dynasty_id"))
    head = None
    if dynasty_id:
        dyn = await gw.read("query_dynasty", {"dynasty_id": dynasty_id, "character_id": ruler_id})
        head = _as_int((dyn or {}).get("head_character_id")) if isinstance(dyn, dict) else None
    primary_title = _as_int(ruler.get("primary_title_id"))
    return {
        "heirs": heir_rows,
        "succession_laws": succession_laws,
        "partition": {"eligible_heirs": len(heirs or []),
                      "adult_heirs": sum(1 for r in heir_rows if r["adult"])},
        "designate_heir": designate,
        "disinherit": {
            "available": (head == ruler_id) if head else None,
            "dynasty_head_id": head or None,
            "why_not": (None if head == ruler_id else
                        "only your dynasty's head can disinherit" if head else
                        "the dynasty head could not be read"),
            "candidates": own_heirs,
            "call": ("do(intent='interact', args={'interaction': 'disinherit_interaction', "
                     "'target_id': <child>}) — sent only down your own line (the kin "
                     "guard); the engine also requires you to be your dynasty's head and, "
                     "unless the child is a bastard, medium dynasty prestige"),
            "readback": "inspect(subject='character', character_id=<child>).traits "
                        "has 'disinherited'; succession_plan heirs drop the child",
        },
        "grant_title": {
            "call": ("do(intent='interact', args={'interaction': 'grant_titles_interaction', "
                     "'target_id': <courtier or child>, 'title_id': <a title you hold>}) — "
                     "lands a spare son as your vassal so he stays in the realm"),
            "revoke": ("do(intent='interact', args={'interaction': 'revoke_title_interaction', "
                       "'target_id': <vassal>, 'title_id': <title>}) — tyranny and vassal "
                       "opinion unless you hold a reason to revoke; the vassal answers in "
                       "4-9 days and one who refuses rebels"),
            "revoke_allowed_by": revokers,
            "revoke_why_not": (None if revokers else
                               "no realm law allows title revocation (crown_authority_1 or "
                               "higher, tribal_authority_2 or higher, or a bureaucracy level "
                               "1); an administrative realm needs none"),
            "revoke_limits": ("not a title granted in the last year, not an ally's without a "
                              "reason to revoke, not from a vassal at war or in truce with "
                              "you or holding a strong hook on you"),
            "revoke_accepted_at_once_by": ("a baron, your prisoner, a vassal you spend a "
                                           "strong hook on (send_options ['hook']), or an "
                                           "AI county governor of an administrative realm; "
                                           "any other vassal may refuse and rebel"),
            "readback": "inspect(subject='title_succession', title_id=<title>).current_holder_id",
        },
        "title_law": {
            "primary_title_id": primary_title or None,
            "call": ("do(intent='change_law', args={'law': <title succession law, e.g. "
                     "feudal_elective_succession_law>, 'scope': 'title', 'title_id': "
                     "<title>}) — the engine says yes or gives its reason"),
            "readback": "inspect(subject='title_succession', title_id=<title>).title_laws",
        },
        "marriage": {
            "call": ("do(intent='interact', args={'interaction': 'arrange_marriage_interaction', "
                     "'target_id': <the other family's ruler, or yourself for two of your "
                     "own court>, 'secondary_actor_id': <your relative>, "
                     "'secondary_recipient_id': <their relative>, 'send_options': "
                     "['matrilineal']}) — a child is betrothed instead of married"),
            "readback": "inspect(subject='character', character_id=<your relative>)"
                        ".spouses[].matrilineal",
        },
    }


async def succession_plan_impl(gw, character_id: int = 0) -> dict[str, Any]:
    actor, err = await _resolve_actor(gw, character_id, "succession_plan")
    if err:
        return err

    sim = await gw.read("query_succession_simulation", {"character_id": character_id})
    sim = sim if isinstance(sim, dict) else {}
    succ = await gw.read("query_succession", {"character_id": character_id})
    succ = succ if isinstance(succ, dict) else {}
    wars = await gw.read("get_active_wars", {"character_id": character_id})
    at_war = bool((wars or {}).get("wars")) if isinstance(wars, dict) else False

    # The per-title map (ck-succprev): which heir gets which title under which law,
    # and the heirs when the simulation names none. The verdict is
    # succession_verdict's, the one definition observe and the live-state hook share.
    sp_raw = await gw.read("query_title_succession_map", {"character_id": character_id})
    msum = succession_verdict.map_summary(sp_raw if isinstance(sp_raw, dict) else {})
    sv = succession_verdict.verdict(sim, succ, msum)
    heirs, primary = sv["heirs"], sv["primary"]

    age = None
    chr_ = await gw.read("query_character", {"character_id": character_id})
    age = _f(_char_section(chr_).get("age"))
    eta = succession_eta_years(age)

    result = {
        "outcome": sv["outcome"],
        **({"undetermined_reason": sv["undetermined_reason"]}
           if sv["outcome"] == "undetermined" else {}),
        "passes_intact": sv["passes_intact"],
        "eligible_heir_count": sv["eligible_heir_count"],
        # Where the heirs above come from: the succession simulation, or (when it names
        # none) the per-title map's projected heirs.
        "heirs_source": sv["heirs_source"],
        "primary_heir": {"id": primary.get("id"), "name": primary.get("name"),
                         "keeps_titles": primary.get("keeps_titles") or primary.get("titles") or []},
        "breaker": sv["breaker"],
        "succession_eta_years": eta,
        **({"succession_eta_basis": SUCCESSION_ETA_BASIS} if eta is not None else {}),
        "at_war": at_war,
        # The per-title map (ck-succprev): each title with its projected heir, and which
        # titles ride PARTITION law vs single-heir. Detail only: the verdict is above.
        "per_title_map": {
            "available": msum.get("available", False),
            "titles": msum.get("titles"),
            "partition_titles": msum.get("partition_titles"),
            "single_heir_titles": msum.get("single_heir_titles"),
            "top_title": msum.get("top_title"),
            **({"note": "per-title map unavailable (" + str(msum.get("reason", "unknown")) +
                        ")"} if not msum.get("available") else {}),
        },
        "levers": await _succession_levers(gw, character_id or _as_int(actor.get("id")),
                                           heirs, chr_),
    }
    warnings = []
    if sv["outcome"] == "undetermined":
        warnings.append(succession_verdict.warning(sv))
    return envelope("succession_plan", result=result, actor=actor, game=gw.game(),
                    warnings=warnings, bridge_health=gw.health)


# ===========================================================================
# HEIR-PLANNING TOOLCHAIN (AL-13: "engineer a conqueror heir")
# Findings: re_heir_education_mechanic.md, re_heir_guardian_mechanic.md,
#           re_heir_legibility_gap.md.
#
# THE GOAL the agent is steering toward: a martial≥18 conqueror heir, achieved
# via (1) the deterministic education_martial_4 lane (+8 base martial → needs a
# martial childhood FOCUS + a HIGH-MARTIAL guardian + the rowdy/bossy affinity
# trait), and (2) a martial/viking marriage for congenital base martial. The
# conqueror gate is the SAME predicate redesign already uses in
# (the removed expansion_engine_health used a martial>=18 OR conqueror/viking
# trait predicate; ck-zgh4 deploy-51 deleted it — see the note at the top.)
#
# WHAT IS BUILDABLE NOW (Phase 1 of re_heir_legibility_gap.md §Part3): per child /
# eligible heir, id/name/age/martial/traits/is_primary_heir + the conqueror-gate
# verdict — all PURE existing-read composition (query_character per child;
# query_succession_simulation for the ranked heir line). NO bridge change.
#
# education_focus is a real read (query_heir_education). No bridge read locates a
# ward's guardian or projects adult martial, so guardian_id, guardian_martial and
# projected_adult_martial_estimate are null and flagged unresolved, never invented.
# ===========================================================================

# The childhood AFFINITY traits that bias a child toward education_martial (each
# +20/event in the education score, re_heir_education_mechanic.md §1a). Read-only
# hint surfaced when present on a child — we never assume it; we report what
# query_character's trait walk actually shows.
_MARTIAL_AFFINITY_CHILD_TRAITS = {"rowdy", "bossy"}


def _char_section(raw: dict | None) -> dict:
    """query_character's reply as a dict ({} for anything else). The bridge writes
    the profile flat at the reply's top level (build_char_profile,
    query_handlers.cpp); no reply nests it under "character" (L15: the readers that
    accepted that shape were fed it only by unit doubles)."""
    return raw if isinstance(raw, dict) else {}


def _skill(sect: dict, name: str) -> int:
    """A character's skill as the bridge writes it: nested under `skills`
    (build_char_profile and search_characters both, query_handlers.cpp), never at
    the top level. 0 when the reply carries no skills object."""
    sk = sect.get("skills") if isinstance(sect, dict) else None
    return _as_int(sk.get(name)) if isinstance(sk, dict) else 0


def _trait_names(sect: dict) -> set[str]:
    """Lower-cased trait-name set from a query_character section: its `traits`
    objects' `name` (build_char_profile). search_characters writes `traits` as an
    array of names, so a string entry is read as one."""
    out: set[str] = set()
    traits_raw = sect.get("traits") if isinstance(sect.get("traits"), list) else []
    for t in traits_raw:
        if isinstance(t, str):
            out.add(t.lower())
        elif isinstance(t, dict) and t.get("name"):
            out.add(str(t.get("name")).lower())
    return out


def _conqueror_gate(sect: dict) -> dict[str, Any]:
    """The AL-13 conqueror-gate verdict for one character section. Mirrors
    the same predicate the removed expansion_engine_health used (martial>=18 OR a
    conqueror/viking trait) so the
    heir read and the engine-health read agree on what 'a conqueror' is. Martial is
    skills.martial (L15: the top-level `martial` it read is never written, so every
    heir scored 0 and no heir met the gate by martial)."""
    martial = _skill(sect, "martial")
    traits = _trait_names(sect)
    has_conqueror = bool(traits & {"conqueror", "adventurer", "ambitious"})
    has_viking = "viking" in traits
    martial_ge_18 = martial >= 18
    is_conqueror = martial_ge_18 or bool(traits & _CONQUEROR_TRAITS)
    is_passive = bool(traits & _PASSIVE_TRAITS) and martial < 12
    if is_conqueror:
        verdict = "conqueror"
    elif is_passive:
        verdict = "passive"
    else:
        verdict = "developing"  # neither gate met yet — the grooming target
    return {
        "martial_ge_18": martial_ge_18,
        "has_conqueror_trait": has_conqueror,
        "has_viking_trait": has_viking,
        "verdict": verdict,
    }


def _heir_row(sect: dict, *, is_primary_heir: bool, is_child: bool,
              in_heir_line: bool, edu: dict | None = None) -> dict[str, Any]:
    """One heir/child row carrying the conqueror-gate inputs. Phase-1 fields are
    real reads. education_focus is now a REAL read (query_heir_education §1a);
    guardian_* are null: no read locates a ward's guardian (re_heir_guardian_RE.md §1b)."""
    martial = _skill(sect, "martial")
    traits = _trait_names(sect)
    gate = _conqueror_gate(sect)
    affinity = sorted(traits & _MARTIAL_AFFINITY_CHILD_TRAITS)
    # recommended_action: the single AL-13 lever this child most needs next.
    if gate["verdict"] == "conqueror":
        rec = "ready — conqueror gate MET; keep this heir as the primary line"
    elif gate["verdict"] == "passive" and not is_child:
        rec = ("passive ADULT — education window closed; rely on a martial MARRIAGE for "
               "congenital base in the NEXT generation, do not bank on this ruler expanding")
    elif is_child:
        rec = ("groom NOW: do(intent='set_education_focus', character_id=<this child>, "
               "args={'focus': 'education_martial'}) + a HIGH-MARTIAL guardian "
               "(inspect(subject='guardian_candidates'), then levers.assign_guardian) "
               + ("(child already has the rowdy/bossy affinity → +20/event, focus will take strongly)"
                  if affinity else
                  "(no martial-affinity child trait visible — the focus+guardian is the only lever)"))
    else:
        rec = ("adult below the gate: a martial lifestyle focus (do(intent='lifestyle', "
               "character_id=<this heir>, args={'focus': 'martial_strategy_focus'})) + a "
               "martial marriage for the next generation")
    return {
        "id": _as_int(sect.get("id")),
        "name": sect.get("name"),
        "age": _f(sect.get("age")),
        "is_primary_heir": bool(is_primary_heir),
        "is_child": bool(is_child),
        "in_heir_line": bool(in_heir_line),
        # BUILDABLE NOW (offsets exist) ----------------------------------------
        "child_martial": martial,
        "martial_affinity_child_traits": affinity,
        "traits": sorted(traits),
        "conqueror_gate_martial_ge_18": gate["martial_ge_18"],
        "conqueror_gate": gate,
        "recommended_action": rec,
        # §1a BUILT — real read via query_heir_education (childhood == adult focus slot).
        "education_focus": (edu or {}).get("education_focus"),
        "education_focus_skill": (edu or {}).get("education_focus_skill"),
        # query_heir_education sends these null: no read locates a ward's guardian
        # (the relation-index offset behind FUN_06777940(ward, 8) is unknown,
        # re_heir_guardian_RE.md §1b).
        "guardian_id": (edu or {}).get("guardian_id"),
        "guardian_martial": (edu or {}).get("guardian_martial"),
        "projected_adult_martial_estimate": None,
        # education_focus is resolved; guardian remains the unresolved half.
        "education_unresolved": (edu or {}).get("education_focus") is None,
        "guardian_unresolved": True,
    }


async def heir_plan_impl(gw, character_id: int = 0, max_results: int = 12) -> dict[str, Any]:
    """Per-child / per-eligible-heir roster for heir planning, composed from reads
    (query_succession_simulation, then query_character and query_heir_education per
    child or heir). education_focus is read; guardian and projected adult martial are
    null and flagged unresolved (no bridge read exposes them)."""
    actor, err = await _resolve_actor(gw, character_id, "heir_plan")
    if err:
        return err

    sim = await gw.read("query_succession_simulation", {"character_id": character_id})
    sim = sim if isinstance(sim, dict) else {}
    chr_ = await gw.read("query_character", {"character_id": character_id})
    ruler_sect = _char_section(chr_)

    # Ordered eligible-heir line (from the partition simulation). Primary heir = the
    # first in the simulation order (the one who keeps the kingdom under single-heir).
    sim_heirs = succession_verdict.sim_heirs(sim)
    heir_ids: list[int] = []
    heir_meta: dict[int, dict] = {}
    for idx, h in enumerate(sim_heirs):
        hid = _as_int(h.get("id"))
        if hid:
            heir_ids.append(hid)
            heir_meta[hid] = {"is_primary_heir": idx == 0, "in_heir_line": True}

    # Children (the grooming pool — bare uint IDs in query_character.children).
    child_ids: list[int] = []
    raw_children = ruler_sect.get("children")
    if isinstance(raw_children, list):
        for c in raw_children:
            cid = _as_int(c)
            if cid:
                child_ids.append(cid)

    # Union, preserving heir-line order first then remaining children. Bounded.
    ordered: list[int] = []
    seen: set[int] = set()
    for cid in heir_ids + child_ids:
        if cid and cid not in seen:
            seen.add(cid)
            ordered.append(cid)
    ordered = ordered[: max(1, max_results)]

    child_set = set(child_ids)
    rows: list[dict] = []
    warnings: list = []
    for cid in ordered:
        raw = await gw.read("query_character", {"character_id": cid})
        sect = _char_section(raw)
        if not sect or _as_int(sect.get("id")) <= 0:
            warnings.append({"code": "heir_read_failed", "is_known_limitation": False,
                             "detail": f"could not read child/heir {cid} — skipped"})
            continue
        meta = heir_meta.get(cid, {"is_primary_heir": False, "in_heir_line": False})
        age = _f(sect.get("age"))
        is_child = (cid in child_set) and (age < 16 if age else False)
        # §1a: real education-focus read per child/heir (guardian_* still null — §1b).
        edu_raw = await gw.read("query_heir_education", {"character_id": cid})
        edu = edu_raw if isinstance(edu_raw, dict) else {}
        rows.append(_heir_row(sect, is_primary_heir=meta["is_primary_heir"],
                              is_child=is_child, in_heir_line=meta["in_heir_line"],
                              edu=edu))

    has_conqueror_heir = any(r["conqueror_gate"]["verdict"] == "conqueror" for r in rows)
    groomable = [r for r in rows if r["is_child"] and r["conqueror_gate"]["verdict"] != "conqueror"]

    result = {
        "ruler_id": actor.get("id"),
        "ruler_name": actor.get("name"),
        "primary_title_id": _as_int(sim.get("primary_title_id") or ruler_sect.get("primary_title_id")),
        "heir_count": len(heir_ids),
        "child_count": len(child_ids),
        "has_conqueror_heir": has_conqueror_heir,
        "heirs": rows,
        "groom_targets": [{"id": r["id"], "name": r["name"], "age": r["age"],
                           "child_martial": r["child_martial"],
                           "affinity": r["martial_affinity_child_traits"]}
                          for r in groomable],
        # Honest scope of what this read can and cannot see.
        "legibility": {
            "phase1_built": ["id", "name", "age", "child_martial", "traits",
                             "martial_affinity_child_traits", "conqueror_gate",
                             "is_primary_heir", "is_child",
                             "education_focus", "education_focus_skill"],
            "phase2_unresolved": ["guardian_id", "guardian_martial",
                                  "projected_adult_martial_estimate"],
            "note": ("education_focus / education_focus_skill are now REAL reads "
                     "(query_heir_education §1a — childhood focus shares the adult focus "
                     "slot). guardian_id/_martial and projected_adult_martial are null: "
                     "no read locates a ward's guardian (re_heir_guardian_RE.md §1b)."),
        },
        # The acting half of AL-13 — which levers the agent can actually fire today.
        "levers": {
            "set_childhood_education_focus": {
                "today": ("do(intent='set_education_focus', character_id=<child>, "
                          "args={'focus': 'education_martial'}) — type-checked alias over the crash-hardened "
                          "SET_FOCUS_APPLY. Childhood and adult focus share ONE slot, so the "
                          "readback correctly detects success on a child (no false-negative). "
                          "Verify via inspect(subject='heir_education').education_focus."),
                "build": "re_heir_education_RE.md §2",
            },
            "assign_guardian": {
                "today": ("do(intent='interact', "
                          "args={'interaction': 'educate_child_interaction', 'target_id': <ward's "
                          "liege>, 'secondary_actor_id': <guardian>, 'secondary_recipient_id': "
                          "<ward>}) — the bridge writes both characters into the interaction "
                          "as the window does (secondary_actor = guardian, secondary_recipient = "
                          "ward, the script's own mapping). No read shows a ward's guardian: "
                          "inspect(subject='pending') says whether the engine took the send."),
                "build": "re_heir_guardian_RE.md §3, re_interactions.md §1",
            },
            "martial_marriage": {
                "today": ("breed a martial heir by marrying a child/courtier to a high-martial "
                          "spouse: do(intent='interact', args={'interaction': "
                          "'arrange_marriage_interaction', 'target_id': <their ruler>, "
                          "'secondary_actor_id': <your child>, 'secondary_recipient_id': "
                          "<martial spouse>, 'send_options': ['matrilineal'] to keep the "
                          "children in your dynasty when your child is the bride}). A queued "
                          "send returns verdict:pending; inspect(subject='pending') shows its "
                          "outcome and inspect(subject='character').spouses[].matrilineal the "
                          "marriage."),
                "candidates": "query(domain='marriage_candidates', sort='martial')",
            },
            "designate_heir": {
                "today": ("do(intent='interact', args={'interaction': 'designate_heir_interaction', "
                          "'target_id': <your child>}) — needs a realm law that allows it "
                          "(succession_plan levers.designate_heir says whether you have one); "
                          "read back with inspect(subject='character', character_id=<you>)"
                          ".designated_heir_id."),
            },
        },
    }
    if not rows:
        warnings.append({"code": "no_heirs_or_children", "is_known_limitation": False,
                         "detail": "no eligible heirs or children resolved — childless ruler "
                                   "or a read that returned no family; nothing to groom"})
    if rows and not has_conqueror_heir and groomable:
        warnings.append({"code": "no_conqueror_heir", "is_known_limitation": False,
                         "detail": f"{len(groomable)} groomable child(ren) but NO conqueror heir yet — "
                                   "set education_martial focus + a high-martial guardian YEARS before "
                                   "the trough (coming-of-age fixes the adult martial trait at age 16)"})
    return envelope("heir_plan", result=result, actor=actor, game=gw.game(),
                    warnings=warnings or None, bridge_health=gw.health)


async def good_guardian_candidates_impl(gw, character_id: int = 0,
                                        max_results: int = 10) -> dict[str, Any]:
    """High-martial guardian candidates for a child of the ruler. The education score
    weights the guardian's matching skill at 0.4x and learning at 0.2x per event
    (re_heir_guardian_mechanic.md §1), so candidates rank by that score.

    A guardian must share the ward's court. in_court_candidates are the council, the
    ruler's spouses and the courtiers search_characters returns for court_of=<ruler>;
    aspirational_global is the global high-martial list, who must be invited into the
    court before they can be assigned."""
    actor, err = await _resolve_actor(gw, character_id, "good_guardian_candidates")
    if err:
        return err

    cand: dict[int, dict] = {}

    def _add(sect: dict, source: str):
        # query_character and search_characters rows both carry `id`, and their
        # skills nested under `skills` (L15: martial/learning were read at the top
        # level, where neither reply writes them, so every candidate scored 0).
        cid = _as_int(sect.get("id"))
        if not cid or cid == _as_int(actor.get("id")):
            return
        martial = _skill(sect, "martial")
        traits = _trait_names(sect)
        learning = _skill(sect, "learning")
        # education score proxy (re_heir_guardian_mechanic.md §1c): 0.4*martial + 0.2*learning
        # + intellect-trait nudge. We only score what we can read.
        intellect_bonus = 5 if (traits & {"intellect_good_3", "intellect_good_2",
                                          "intellect_good_1", "shrewd"}) else 0
        intellect_penalty = 5 if (traits & {"dull", "intellect_bad_1", "intellect_bad_2",
                                            "intellect_bad_3"}) else 0
        score = 0.4 * martial + 0.2 * learning + intellect_bonus - intellect_penalty
        age = _f(sect.get("age"))
        row = {
            "id": cid,
            "name": sect.get("name"),
            "martial": martial,
            "learning": learning,
            "age": age,
            "is_adult": (age >= 16) if age else None,
            "good_intellect": bool(intellect_bonus),
            "bad_intellect": bool(intellect_penalty),
            "guardian_score": round(score, 2),
            "in_court": source != "global",
            "source": source,
            "assignable_note": ("in your court: assign as a guardian with "
                                "do(intent='interact', args={'interaction': "
                                "'educate_child_interaction', 'target_id': <ward's liege>, "
                                "'secondary_actor_id': <this id>, 'secondary_recipient_id': "
                                "<ward>})" if source != "global" else
                                "GLOBAL high-martial — must be recruited/invited into court "
                                "before guardianship is possible"),
        }
        prev = cand.get(cid)
        if prev is None or row["guardian_score"] > prev["guardian_score"] or (not prev["in_court"] and row["in_court"]):
            cand[cid] = row

    # (a) In-court reachable: council members (real courtiers) + ruler's spouse(s).
    # L15 A2: a seat is a `council` row whose holder is `holder_id`, 0xFFFFFF with
    # `vacant` when empty (query_handlers.cpp do_query_council :5703/:5797/:5803).
    # The reader took seats/character_id/councillor_id/id, none of which the bridge
    # writes, so no councillor was ever a candidate.
    council = await gw.read("query_council", {"character_id": character_id})
    council = council if isinstance(council, dict) else {}
    for seat in (council.get("council") or []):
        if not isinstance(seat, dict) or seat.get("vacant"):
            continue
        cid = _as_int(seat.get("holder_id"))
        if cid and cid != 0xFFFFFF:
            cm = _char_section(await gw.read("query_character", {"character_id": cid}))
            if cm:
                _add(cm, "council")

    ruler_sect = _char_section(await gw.read("query_character", {"character_id": character_id}))
    for sp in (ruler_sect.get("spouses") or []):        # {id, alive, ...} objects
        sid = _as_int(sp.get("id")) if isinstance(sp, dict) else 0
        if sid:
            sm = _char_section(await gw.read("query_character", {"character_id": sid}))
            if sm:
                _add(sm, "spouse")

    # (a2) §4: TRUE in-court courtiers via the new court_of= filter (immediate-liege
    # predicate over the global DB walk). This sees mentors the council/spouse arms miss.
    ruler_id = _as_int(actor.get("id"))
    if ruler_id:
        court = await gw.read("search_characters",
                              {"filter": "alive", "sort": "martial", "sort_order": "desc",
                               "max_results": max(10, max_results), "court_of": ruler_id})
        court = court if isinstance(court, dict) else {}
        for ch in (court.get("characters") or []):
            if isinstance(ch, dict):
                _add(ch, "court")

    # (b) Global high-martial pool (aspirational — recruit-first). sort=martial.
    glob = await gw.read("search_characters",
                         {"filter": "alive", "sort": "martial", "sort_order": "desc",
                          "max_results": max(10, max_results)})
    glob = glob if isinstance(glob, dict) else {}
    for ch in (glob.get("characters") or []):
        if isinstance(ch, dict):
            _add(ch, "global")

    rows = sorted(cand.values(),
                  key=lambda r: (r["in_court"], r["guardian_score"]), reverse=True)
    in_court = [r for r in rows if r["in_court"]][: max_results]
    aspirational = [r for r in rows if not r["in_court"]][: max_results]

    result = {
        "ruler_id": actor.get("id"),
        "scoring": "guardian_score = 0.4*martial + 0.2*learning + intellect_nudge "
                   "(re_heir_guardian_mechanic.md §1c) — rank by martial first",
        "in_court_candidates": in_court,
        "aspirational_global": aspirational,
        "court_roster_gap": {
            "issue": ("in-court candidates come from the council, the ruler's spouses and "
                      "search_characters(court_of=<ruler>), the immediate-liege predicate "
                      "over the character database (source='court')"),
            "built": "re_heir_guardian_RE.md §4 (search_characters court_of=)",
        },
        "assign_lever_status": ("UNVALIDATED: appoint a candidate with do(intent=\"interact\", "
                                "args={\"interaction\": \"educate_child_interaction\", \"target_id\": "
                                "<the ward's liege>, \"secondary_actor_id\": <guardian>, "
                                "\"secondary_recipient_id\": <ward>}). The bridge maps the two "
                                "secondaries to the engine's slots in an order no readback has "
                                "confirmed, and no read shows a ward's guardian: "
                                "inspect(subject=\"pending\") says whether the engine took it. "
                                "re_heir_guardian_RE.md §3/§3b"),
    }
    return envelope("good_guardian_candidates", result=result, actor=actor,
                    game=gw.game(), bridge_health=gw.health)


# --------------------------------------------------------------------------- set_phase
async def steer_impl(gw, character_id: int = 0, personality: dict = None) -> dict[str, Any]:
    """steer: read the personality weights, or write some of them, in the write
    envelope every other write answers (L14 finding 5).

    The write goes through settle_write like every do() write: set_ai_personality
    is synchronous (no intent), its own reply carries the axes read back after the
    write, and LEVERS ("steer", "") says the effect shows in the call. applied: the
    axes read back as written; rejected: the bridge refused (write_blocked), or the
    write ran and the axes do not read back as written (write_unconfirmed);
    pending: the reply never came (next_pass) or the read-back faulted (readback).
    A bad axis or value is refused before anything is sent, verdict rejected."""
    actor, err = await _resolve_actor(gw, character_id, "steer")
    if err:
        return err
    if personality:
        refusal = personality_refusal(personality)
        if refusal:
            return envelope("steer", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=why_blocked("invalid_argument", summary=refusal),
                            bridge_health=gw.health)
    if not personality:
        pers = await gw.read("get_ai_personality", {"character_id": character_id})
        if not isinstance(pers, dict) or not pers:
            return envelope("steer", ok=False, actor=actor, game=gw.game(),
                            blocked=why_blocked("read_timeout",
                                                summary="get_ai_personality returned no reply"),
                            bridge_health=gw.health)
        if pers.get("success") is False or (pers.get("error") and "success" not in pers):
            why = str(pers.get("reason") or pers.get("error") or "")
            # the read's refusal, typed by the one refusal builder (no game words: raw unset)
            return envelope("steer", ok=False, actor=actor, game=gw.game(), result={"raw": pers},
                            blocked=write_blocked(why, summary=f"get_ai_personality: {why or 'declined'}"),
                            bridge_health=gw.health)
        return envelope("steer", result={"personality": pers}, actor=actor, game=gw.game(),
                        bridge_health=gw.health)

    written = dict(personality)
    params = {"character_id": character_id, **written}
    submitted_on = gw.game().get("date")
    raw = await gw.write("set_ai_personality", params)
    raw = raw if isinstance(raw, dict) else {}

    async def _axes_back(r) -> Any:
        back = r.get("personality") if isinstance(r, dict) else None
        if not isinstance(back, dict):
            fresh = await gw.read("get_ai_personality", {"character_id": character_id})
            back = fresh.get("personality") if isinstance(fresh, dict) else None
            if not isinstance(back, dict):
                return UNREAD
        got = {k: back.get(k) for k in written}
        return {"axes": got} if got == written else None

    lever = lever_of("steer")[1]
    conf = await settle_write(None, gw.read, raw, _axes_back, game_date_at_submit=submitted_on,
                              max_polls=0)
    res = {"personality": raw, "written": written,
           "lever": {"shows": lever["shows"], "readback": lever["readback"]}}
    if conf.get("resolved") is not None:
        return envelope("steer", verdict="applied", actor=actor, game=gw.game(),
                        result=dict(res, readback=conf["resolved"]), bridge_health=gw.health)
    if conf.get("refused"):
        return envelope("steer", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        result=res, blocked=write_blocked(conf["refused"], conf=conf, raw=raw),
                        bridge_health=gw.health)
    if conf["sent"] and conf["drains_when"] != "readback":
        return envelope("steer", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        result=res,
                        blocked=write_blocked(gate="write_unconfirmed", raw_reason="readback_after_drain_absent",
                                            summary="the bridge ran the write and the axes do not read "
                                                    f"back as written ({sorted(written)})",
                                            fix={"verify_with": "steer()"}),
                        bridge_health=gw.health)
    why = ("the bridge ran the write and its read-back of the axes faulted"
           if conf["drains_when"] == "readback" else
           "the bridge did not confirm the write and a read did not show it")
    return envelope("steer", verdict="pending", actor=actor, game=gw.game(), result=res,
                    pending=conf_pending(conf, expected_change=f"the axes {sorted(written)} read back as written",
                                         why_pending=why, verify_with="steer()"),
                    bridge_health=gw.health)


def phase_of_genome(pers: dict) -> Optional[str]:
    """The phase whose genome a get_ai_personality reply's engine bytes hold: the
    one PHASE_GENOME entry that agrees with the engine on every axis the engine
    reports (the lock map, `personality`, is the write's own record, not a read
    of the game). None when the engine read is empty or no single phase matches."""
    engine = (pers or {}).get("engine") or {}
    if not isinstance(engine, dict) or not engine:
        return None
    hits = [name for name, genome in PHASE_GENOME.items()
            if any(k in engine for k in genome)
            and all(engine[k] == v for k, v in genome.items() if k in engine)]
    return hits[0] if len(hits) == 1 else None


#: The bridge's personality lock states (bridge/src/personality.cpp, L18-A:
#: get_ai_personality and set_ai_personality both emit `lock_state`), and the
#: reading when a reply carries none (a bridge before L18-A): never a default.
LOCK_STATES = ("live", "quarantined", "none")
LOCK_STATE_NOT_REPORTED = "not_reported"


def lock_state_of(reply: Any) -> str:
    """The personality lock state a bridge reply says, one reading for set_phase's
    reply and observe's posture.genome. A value outside LOCK_STATES is carried as
    said; a reply without the key reads not_reported."""
    if isinstance(reply, dict) and reply.get("lock_state") is not None:
        return str(reply["lock_state"])
    return LOCK_STATE_NOT_REPORTED


async def set_phase_impl(gw, phase: str = None, character_id: int = 0,
                         overrides: dict = None) -> dict[str, Any]:
    actor, err = await _resolve_actor(gw, character_id, "set_phase")
    if err:
        return err

    if not phase:
        # Read: the genome, and the phase whose genome the ENGINE holds now (the
        # personality bytes get_ai_personality reports as `engine`). No phase is
        # stored anywhere, so the name is derived from the bytes: None when no
        # phase matches (a hand-tuned genome, or a phase with overrides).
        pers = await gw.read("get_ai_personality", {"character_id": character_id})
        pers = dict(pers) if isinstance(pers, dict) else {}
        pers["lock_state"] = lock_state_of(pers)
        return envelope("set_phase", result={"phase": phase_of_genome(pers), "phase_read_from": "engine",
                                             "genome": pers, "lock_state": pers["lock_state"]},
                        actor=actor, game=gw.game(), bridge_health=gw.health)

    if phase not in PHASE_GENOME:
        return envelope("set_phase", ok=False, actor=actor, game=gw.game(),
                        blocked=why_blocked("invalid_argument", summary=f"unknown phase '{phase}'; valid: {', '.join(PHASES)}"),
                        bridge_health=gw.health)

    if overrides:
        refusal = personality_refusal(overrides)
        if refusal:
            return envelope("set_phase", ok=False, actor=actor, game=gw.game(),
                            blocked=why_blocked("invalid_argument",
                                                summary=f"overrides: {refusal}"),
                            bridge_health=gw.health)

    warnings = []
    genome = dict(PHASE_GENOME[phase])
    if overrides:
        genome.update(overrides)

    p = dict(genome)
    p["character_id"] = character_id
    pres = await gw.write("set_ai_personality", p)
    # L14 FF2 N-MAJ-1: the personality write IS the phase. Its refusal (a
    # `success: false`, the bridge's {"error":"memory_access_fault"} from
    # set_ai_personality, the hung gateway's bridge_disconnected) is the verdict,
    # and nothing more is written; it used to become a warning beside "applied".
    refused = reply_refusal(pres)
    if refused:
        return envelope("set_phase", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        result={"phase": phase, "genome": genome, "raw": pres},
                        blocked=write_blocked(refused, raw=pres if isinstance(pres, dict) else None),
                        bridge_health=gw.health)

    # ── ck-zgh4 deploy-51: THE WORLD-WIDE WRITE IS GONE. RULED by the
    # progenitor, "Remove the world-wide write": `set_phase` no longer calls
    # `toggle_ai_subsystem(DeclareWar)`, and so writes nothing outside the
    # calling seat's own character.
    #
    # WHAT IT USED TO DO, because the disclosure matters more than the diff.
    # Every phase pin flipped ONE engine byte that gates offensive war
    # declarations for EVERY AI ruler in the world. The code called it a PULSE
    # and surfaced a "⚠ WORLD-WIDE blast radius" note in the result, which is
    # honest labelling of a thing that should not have been on a per-seat verb.
    # Three consequences, all measured or structural:
    #   - SIX seats in arms A and B called this every cycle and ONE did in arm C,
    #     so the seats in a multi-seat arm were silently coupled through a global
    #     byte and were never independent; the coupling ALSO differed by arm.
    #   - it is the only ENGINE-ENFORCED declaration lever, so removing it is a
    #     change in what a seat can DO, not only in what it reads. Seats declare
    #     wars directly through `declare_war` instead.
    #   - therefore: disclosed, identical in every arm, and NO POOLING across
    #     this deploy boundary.
    #
    # The per-character subsystem mask the phase also wrote is deleted too
    # (cleanup 2026.11): the engine never read it.

    # ── ck-xwlg P2 hardening: re-assert the per-char AI gate (ai_ctrl+0x28) on
    # every phase pin. The gate stays untouched by default takes, but a leaked
    # disable (an earlier take with disable_native_ai=true, or a release/retake
    # race) silently kills ALL war prosecution — armies never re-plan, sieges
    # never chain (the archived M-003 class). Idempotent write; belt-and-braces.
    #
    # lane-query item 7: THE RE-ASSERT WAS REFUSED ON EVERY DEFAULT CALL, AND
    # THE REPLY SAID APPLIED. set_phase passes the tool's character_id through,
    # and the default is 0 ("the bound ruler"). set_ai_personality resolves 0
    # to the controlled ruler; the bridge's
    # set_character_ai deliberately REFUSES 0 (ck-vf92: 0 is not a character,
    # and it once reached the AI_CHAR_TOGGLE store). So every set_phase called
    # without an explicit id logged {"success": false, "ai_enabled": true} in
    # the action log, and this check — which looked only for `error`/`blocked`,
    # keys that reply never carries — let the phase report applied. Now the
    # write goes to the RESOLVED actor id, and a reply that is not an explicit
    # success is reported, in the warning and in result.ai_gate.
    ai_gate_cid = _as_int(character_id) or _as_int((actor or {}).get("id"))
    if ai_gate_cid > 0:
        ai_gate = await gw.write("set_character_ai",
                                 {"character_id": ai_gate_cid, "enabled": True})
    else:
        ai_gate = {"success": False, "reason": "actor_id_unresolved"}
    # F11 (cleanup 2026.11, lane l6): success is the bridge's read-back of the
    # per-character AI byte after the write (`applied`), not the write's return.
    ai_gate_ok = isinstance(ai_gate, dict) and ai_gate.get("success") is True \
        and ai_gate.get("applied") is not False and ai_gate.get("ai_enabled") is not False \
        and not ai_gate.get("error") and not ai_gate.get("blocked")
    if not ai_gate_ok:
        warnings.append({"code": "ai_gate_reassert_failed", "is_known_limitation": True,
                         # ck-zgh4 deploy-51: this used to name `set_character_ai`
                         # to the SEAT. That verb is not on the seat surface — not
                         # a tool and not a `do` intent — so a seat following it
                         # got `unknown_verb`. The SERVER re-asserts the gate on
                         # every set_phase call (just above); what the seat can do
                         # is READ whether it took.
                         "detail": "this seat's per-character AI tick could not be "
                                   "re-enabled. The server re-asserts it on every "
                                   "set_phase call, so retrying set_phase retries it. "
                                   "session(action='whoami') reports agency.ai_tick.enabled "
                                   "for the bound ruler; if armies park mid-war, that "
                                   "is the field to read."})
    # L14 finding 4: no phase arms the off-goal veto. It is an unvalidated hook
    # (the experimental_flags list says so), and neither the pre-registration nor
    # docs/pre-registration-conditions.md declares a phase genome or the veto, so
    # the default posture tool no longer turns it on process-wide; it stays
    # reachable only by session(action="set_experimental", flag="off_goal_veto",
    # enabled=true).
    # L18 ruling (b): the lock the write left, from the bridge's own reply (L18-A:
    # set_ai_personality says lock_state and the world_epoch it wrote in). The
    # 2026.11 pilots' first set_phase reverted in 12 of 32 seats while the reply
    # returned the declared genome, so `applied` alone could not show it.
    lock_state = lock_state_of(pres)
    if lock_state in ("quarantined", "none"):     # the bridge SAID it is not held
        warnings.append({"code": "personality_lock_not_live", "is_known_limitation": False,
                         "detail": (f"the bridge says lock_state={lock_state} after the write: "
                                    "the genome is not being held against the AI's own "
                                    "rewrites. observe's posture.genome.lock_state reads it "
                                    "again; call set_phase once more if it stays so.")})
    result = {"phase": phase, "genome": genome,
              "lock_state": lock_state,
              "world_epoch": (pres.get("world_epoch") if isinstance(pres, dict) else None),
              # rc2 B8: nothing re-applies the phase after a succession
              # (the server has no such path); this said True.
              "auto_repin": False, "repinned_this_reign": True,
              # lane-query item 7: what the per-character AI re-assert did,
              # read from its reply rather than assumed.
              "ai_gate": {"character_id": ai_gate_cid,
                          "reasserted": ai_gate_ok,
                          "ai_enabled_read_back": (ai_gate.get("ai_enabled")
                                                   if isinstance(ai_gate, dict) else None),
                          "reason": (None if ai_gate_ok else
                                     (ai_gate.get("reason") or ai_gate.get("error")
                                      if isinstance(ai_gate, dict) else "no_reply")
                                     or "no_confirmation")}}
    # A personality write the bridge did not confirm (sent_no_confirmation) is
    # not applied either: the session guard's one rule says what it is.
    return unwitnessed_applied(
        envelope("set_phase", verdict="applied", result=result, actor=actor,
                 game=gw.game(), warnings=warnings, bridge_health=gw.health),
        [("set_ai_personality", pres)])


# --------------------------------------------------------------------------- do
#: The game's tooltip markup (cleanup 2026.11, lane l6, live l6fix2-live-1): \x16<icon>!
#: inserts an icon, \x15TOOLTIP:<ref> / \x15ONCLICK:<ref> open a link (the text
#: follows a space), \x15<style>[;] sets a style (a letter, \x15X, or a word:
#: \x15high Serene Doge, l6fin-1 read "igh Serene Doge" while only a letter was
#: removed), \x15! closes one, \x15indent_newline:<n> breaks a line. The icons
#: that carry a word keep it.
def _build_refusal_summary(raw: dict) -> str:
    """A construct_building / construction_check refusal in the game's words: the
    construction validity's reasons, as the game's tooltip shows them."""
    given = [x for x in (raw.get("engine_reasons") or []) if x]
    lines = [x for x in (engine_text(y) for y in given) if x]
    if given and not lines:
        return "the game refuses: " + NO_GAME_DETAIL
    return ("the game refuses: " + "; ".join(lines)) if lines else ""


#: The game's imprison interaction. imprison_rows is its one builder: the
#: imprison_targets enumeration and discover(what='targets', action='imprison')
#: (imprison_target_rows) and the faction leader's lever all call it.
IMPRISON_INTERACTION = "imprison_interaction"

#: The send pipeline's refusal codes (ai_eval_hook.cpp, query_interaction_check's
#: refused_by), said as a player reads them; the game's own sentence follows. The
#: gate is the one the same code gets on a direct send (RAW_REASON_TO_GATE).
#:
#: The agent's rulers are AI-run in the game's eyes (the bridge plays an observer
#: game: no character is the player, every actor block reads is_player false), so
#: imprison_interaction's can_send takes its AI branch: grounds to imprison the
#: target (has_imprisonment_reason) OR involvement in a character struggle, and no
#: hostile act in the last 10 days. A ruler in a struggle needs no grounds: in runs
#: l10fx5-live-2/3 liege 11170 (Abd al-Aziz, an independent clan duke) sent and
#: jailed vassals he had no grounds against, which only the struggle branch allows
#: (deduced from can_send; his involvement was not read). The player branch
#: (is_ai = no: no grounds needed) is never the agent's.
_SEND_CHECK_SENTENCES: dict = {
    "interaction_not_valid_for_target": "the game does not allow this imprisonment now",
    "send_conditions_not_met": "the game will not let you send it (your ruler is run by the game's AI, so "
                               "it needs grounds to imprison him or a struggle you are involved in, and no "
                               "hostile act in the last 10 days)",
    "target_would_reject": "the game says he would refuse",
    "send_cost_unaffordable": "you cannot pay what it costs",
    "target_not_found": "this character no longer exists",
}


def _send_check_blocked(chk: Optional[dict]) -> Optional[dict]:
    """The blocked block for one query_interaction_check result, or None when the
    game would take the send and the recipient's answer is not a "no". The gate is
    the code's own (RAW_REASON_TO_GATE: interaction_invalid, recipient_refuses,
    cannot_afford, ...), the one a direct send of the same refusal gets."""
    if not isinstance(chk, dict):
        return why_blocked("read_timeout", summary="the game's imprison check did not answer, "
                                                   "so it is not offered", raw_reason="send_check_unread")
    if chk.get("sendable") is not True:
        code = str(chk.get("refused_by") or "unknown")
        said = _SEND_CHECK_SENTENCES.get(code, f"the game's check refused it ({code})")
        # The bridge drops a compound header with nothing under it (engine_text.h:
        # the imprison check's "All of these:", w-l10-factions-6), so an absent or
        # empty engine_reason is a refusal the game gave no detail for.
        why = chk.get("engine_reason") or NO_GAME_DETAIL
        return map_raw_reason(code, summary=f"{said}: {why}")
    if chk.get("answer") == "no":
        why = chk.get("answer_text")
        return why_blocked("recipient_refuses",
                           summary=f"the game says he would refuse: {why}" if why
                           else "the game says he would refuse", raw_reason="answer_no")
    return None


def _maybe_chance_pct(chk: dict) -> Optional[float]:
    """The one chance the game defines: a "maybe" whose score lies in the maybe band
    (above 0, below 100: ACCEPTANCE_DECISION mode 1 with the type's band flag) is the
    chance in percent that the recipient accepts. A "yes" (score 100 or more) and a
    "no" (0 or less) carry no chance: the answer and the score are given as they
    are, and neither is a promise (the target answers days later, on his state then)."""
    score = chk.get("accept_score")
    if chk.get("answer") == "maybe" and isinstance(score, (int, float)) and 0 < score < 100:
        return round(float(score), 1)
    return None


async def imprison_rows(gw, actor_id: int, target_ids: list) -> list:
    """The imprison do_call for each target, judged by the game itself.

    query_interaction_check runs the send's own checks (the game's validity with its
    sentence, the send conditions, the recipient's refusal, the cost) and reads the
    recipient's pre-answer, nothing sent. A row is offered (blocked None) only when
    the game would take the send and the answer is not "no". It carries the game's
    `answer` (yes / maybe / no, its pre-answer text in game_check.answer_text) and
    `accept_score` (the recipient's ai_accept sum), and `chance_pct` only for a
    "maybe" in the maybe band, the one chance the game defines. The answer is the
    target's at check time: the game asks him again when the send arrives and he
    answers 4 to 9 days later on his state then (gifts, opinion, wars and the days
    between move it), so a "yes" is not a promise (runs l10fx5-live-2/3: 13327 read
    "yes" at 239 and stayed free). Check right before sending. `grounds` are the
    reasons the game gives (query_imprisonment_reasons), for the record: the check
    is the game's verdict (an AI-run ruler needs grounds or a struggle, see
    _SEND_CHECK_SENTENCES)."""
    ids = [t for t in target_ids if isinstance(t, int) and t > 0]
    checks: dict = {}
    if ids:
        res = await gw.read("query_interaction_check",
                            {"actor_id": actor_id, "interaction_type": IMPRISON_INTERACTION,
                             "candidates": ids})
        res = res if isinstance(res, dict) else {}
        for c in ((res.get("result") or {}).get("results") or []):
            if isinstance(c, dict) and isinstance(c.get("target_id"), int):
                checks[c["target_id"]] = c
    rows = []
    for t in target_ids:
        ann = await gw.read("query_imprisonment_reasons", {"actor_id": actor_id, "target_id": t})
        ann = ann if isinstance(ann, dict) else {}
        chk = checks.get(t)
        row: dict[str, Any] = {
            "do_call": {"intent": "interact",
                        "args": {"interaction": IMPRISON_INTERACTION, "target_id": t}},
            "grounds": ann.get("reasons") or [],
            "blocked": _send_check_blocked(chk),
        }
        if isinstance(chk, dict):
            row["game_check"] = {k: chk[k] for k in ("sendable", "refused_by", "engine_reason", "answer",
                                                     "accept_score", "answer_text") if k in chk}
            if chk.get("sendable") is True:
                row["answer"] = chk.get("answer")
                row["accept_score"] = chk.get("accept_score")
                chance = _maybe_chance_pct(chk)
                if chance is not None:
                    row["chance_pct"] = chance
        rows.append(row)
    return rows


async def imprison_target_rows(gw, actor_id: int,
                               candidates: Optional[list] = None) -> tuple[list, list, Optional[dict]]:
    """(available, unavailable, refused) imprison rows for `actor_id`: the targets
    named in `candidates`, else his first 40 vassals; the likeliest first (the
    recipient's score). `refused` is the blocked block when the vassal read was
    declined or did not come back (no rows were judged), else None. The one builder
    of imprison targets: do(action_class='imprison_targets') and
    discover(what='targets', action='imprison') both call it."""
    if candidates:
        ids = [_as_int(c) for c in candidates if _as_int(c) > 0]
        names: dict = {}
    else:
        realm = await gw.read("query_realm", {"character_id": actor_id})
        realm = realm if isinstance(realm, dict) else {}
        raw = str(realm.get("reason") or realm.get("error") or "")
        if realm.get("success") is False or (raw and "vassals" not in realm):
            return [], [], map_raw_reason(raw or "declined",
                                          summary=f"the bridge declined the realm read: {raw or 'no reason'}")
        if "vassals" not in realm:
            # query_realm always lists vassals (an empty list for none): no list is
            # a read that did not come back, never "no vassal".
            return [], [], why_blocked("read_timeout", raw_reason="query_realm_unread",
                                       summary="the realm read did not come back, so no vassal was checked")
        vassals = [v for v in (realm.get("vassals") or [])[:40] if isinstance(v, dict)]
        ids = [v.get("id") for v in vassals]
        names = {v.get("id"): v.get("name") for v in vassals}
    available, unavailable = [], []
    for t, row in zip(ids, await imprison_rows(gw, actor_id, ids)):
        row = {"label": f"Imprison {names.get(t) or t}", **row}
        (available if row["blocked"] is None else unavailable).append(row)
    available.sort(key=lambda r: -(r.get("accept_score") or 0))
    return available, unavailable, None


def decision_eval_rows(res, requested) -> tuple[list, Optional[dict]]:
    """THE reading of a list_valid_decisions / list_decisions_annotated reply, for
    do(action_class="decisions") and discover(what="decisions_annotated"|
    "valid_decisions") alike (L18-B).

    The bridge evaluates only the decisions it is asked about, and its evaluated
    reply says so (`valid_only`, `considered`: enumerate_decisions_annotated). With
    no list it serves the LIGHT name+hash list of every decision instead
    (enumerate_decisions: `decisions`, `count`, nothing else). In the 2026.11 pilots
    every framed request reached the bridge as "no list" (submit_decision_eval read
    the list top-level only), so a request for 1 decision came back as 1,0xx
    unevaluated rows: 186K characters through do, 26K through discover, every row
    "blocked behind the frozen gate path". A reply that is not an evaluation is
    refused as not_evaluated, never passed through; an evaluated reply keeps only
    the rows that were asked for. Returns (rows, blocked|None)."""
    res = res if isinstance(res, dict) else {}
    if res.get("error"):
        return [], map_raw_reason(str(res.get("error")), summary=str(res.get("hint") or res.get("error")))
    if "valid_only" not in res and "considered" not in res:
        n = len(res.get("decisions") or []) if isinstance(res.get("decisions"), list) else 0
        return [], why_blocked(
            "not_evaluated",
            summary=(f"the bridge answered with its unevaluated list of {n} decision names, "
                     f"not an evaluation of the {len(requested or [])} asked for; this build "
                     "cannot evaluate decisions (re-check after a bridge update)"),
            measured={"requested": len(requested or []), "listed_unevaluated": n})
    want = {str(x) for x in (requested or [])}
    rows = [d for d in (res.get("decisions") or []) if isinstance(d, dict)
            and (not want or str(d.get("id") or d.get("name")) in want
                 or str(d.get("hash")) in want)]
    return rows, None


async def _do_enumerate(gw, action_class: str, character_id: int, candidates: list,
                        actor: dict, args: dict = None) -> dict[str, Any]:
    """Enumerate-mode: hand back self-describing do_call rows (SPEC §2 graft).

    `args` carries enumerate keys a class reads (buildings: location_id)."""
    args = dict(args or {})
    available: list = []
    unavailable: list = []
    warnings: list = []

    if action_class == "decisions":
        # #64 SAFETY: bounded only. Unbounded ⇒ refuse with unbounded_eval_refused.
        if not candidates:
            return envelope("do", ok=False, actor=actor, game=gw.game(),
                            blocked=why_blocked("unbounded_eval_refused",
                                                summary="decisions enumerate REQUIRES a bounded `candidates` list (#64 freeze safety)"),
                            bridge_health=gw.health)
        res = await gw.read("list_valid_decisions",
                            {"character_id": character_id, "decisions": candidates})
        rows, refused = decision_eval_rows(res, candidates)
        if refused is not None:
            return envelope("do", ok=False, actor=actor, game=gw.game(), blocked=refused,
                            bridge_health=gw.health)
        for d in rows:
            avail = d.get("available")
            row = {"label": d.get("id") or d.get("name"),
                   "do_call": {"intent": "take_decision", "args": {"decision": d.get("id") or d.get("name")}}}
            if avail:
                row["blocked"] = None
                available.append(row)
            else:
                fails = d.get("failures") or []
                row["blocked"] = why_blocked("gate_introspection_unavailable" if not fails else "missing_prerequisite_law",
                                             summary=", ".join(fails) if fails else "blocked behind the frozen gate path (O3)")
                unavailable.append(row)
    elif action_class == "interactions":
        res = await gw.read("list_interactions", {})
        for i in (res or {}).get("interactions", []) if isinstance(res, dict) else []:
            name = i.get("name") or i.get("type")
            available.append({"label": name,
                              "do_call": {"intent": "interact", "args": {"interaction": name, "target_id": 0}},
                              "blocked": None})
    elif action_class == "council_tasks":
        res = await gw.read("query_fabricate_targets",
                            {"actor_id": character_id, "task": "task_fabricate_claim", "max_results": 40})
        for t in (res or {}).get("targets", []) if isinstance(res, dict) else []:
            assignable = t.get("assignable")
            row = {"label": f"Fabricate claim on {t.get('title') or t.get('title_id')}",
                   # lane-query item 1: this row used to say seat "chancellor".
                   # task_fabricate_claim is a COURT CHAPLAIN task (dump
                   # ck3_20260625_152901_ivar867: tasktype+0x30 ->
                   # councillor_court_chaplain), so the printed call named a
                   # seat the game never uses for it, and a seat that read it
                   # went looking for its chancellor's skill.
                   "do_call": {"intent": "council_task",
                               "args": {"seat": FABRICATE_CLAIM_SEAT, "task": "task_fabricate_claim",
                                        "target_title_id": t.get("title_id")}},
                   "warnings": [{"code": "may_revert_ai_reasserts", "is_known_limitation": True}]}
            if assignable:
                row["blocked"] = None
                available.append(row)
            else:
                row["blocked"] = why_blocked("target_not_assignable",
                                             summary="target dropped off the eligible list this tick — re-enumerate, never reuse a stale target_id")
                unavailable.append(row)
        if not available and not unavailable:
            # L18-B item 2 (Cui 13191, an administrative ruler): the list came back
            # empty with no reason. The bridge evaluates the task on the council seat
            # whose current task type is the task's position (action_handlers.cpp
            # do_query_fabricate_targets), and the printed call names the court
            # chaplain; a ruler whose realm seats the task elsewhere (a ministry's
            # minister_justice) has no court-chaplain row. The empty reply names the
            # seat it used and what the bridge said, rather than an empty list.
            r = res if isinstance(res, dict) else {}
            said = {k: r.get(k) for k in ("reason", "requested", "eligible_count",
                                          "assignable_count", "counties_considered",
                                          "seat_vacant", "seat_warning", "scan_truncated")
                    if r.get(k) is not None}
            warnings.append({"code": "fabricate_targets_empty", "is_known_limitation": True,
                             "detail": (f"no fabricate-claim target for this ruler on the "
                                        f"{FABRICATE_CLAIM_SEAT} seat (task_fabricate_claim). "
                                        "A realm that seats the task elsewhere (an "
                                        "administrative ministry's minister_justice) is not "
                                        "looked up by the bridge yet. The bridge said: "
                                        + (", ".join(f"{k}={said[k]}" for k in sorted(said)) if said else "nothing")),
                             "seat": FABRICATE_CLAIM_SEAT, "bridge": said})
    elif action_class == "laws":
        # lane-query item 2: THE REPLY SHAPE WAS NEVER READ. The bridge answers
        # {groups: [{group_name, current_law, options: [{name, ...}]}]} and this
        # read `laws` / `available`, keys the bridge does not emit, so every
        # ruler got {"available": [], "unavailable": []} — at war or at peace.
        res = await gw.read("query_available_law_changes", {"character_id": character_id})
        res = res if isinstance(res, dict) else {}
        groups = res.get("groups") if isinstance(res.get("groups"), list) else []
        for g in groups:
            if not isinstance(g, dict):
                continue
            gname = g.get("group_name") or "?"
            cur = g.get("current_law")
            for o in (g.get("options") or []):
                name = o.get("name") if isinstance(o, dict) else o
                if not name:
                    continue
                available.append({"label": f"{gname}: {cur} -> {name}",
                                  "group": gname, "current_law": cur,
                                  "do_call": {"intent": "change_law", "args": {"law": name}},
                                  "blocked": None})
        warnings.append({"code": "law_options_unvalidated", "is_known_limitation": True,
                         "detail": ("each row is another law in a group this ruler has, not a "
                                    "verdict that the change is allowed now: the game checks "
                                    "cost, war and other conditions when change_law runs")})
        if not available:
            _why = (res.get("error") or ("the ruler has no law groups with alternatives"
                                         if groups else "the bridge returned no law groups"))
            warnings.append({"code": "no_law_options", "is_known_limitation": False,
                             "detail": f"no law changes listed: {_why}"})
    elif action_class == "fabricate_targets":
        return await _do_enumerate(gw, "council_tasks", character_id, candidates, actor)
    elif action_class == "mercenaries":
        # lane-query item 2: 562 rows, every label null, no size, 49 KB. The
        # bridge now labels each company (home county + men), gives troops and
        # the regiment mix, leaves out companies under contract, and with an
        # actor sorts nearest first; the cap keeps the reply small.
        res = await gw.read("query_hireable_mercenaries",
                            {"actor_id": character_id, "max_results": 25})
        res = res if isinstance(res, dict) else {}
        for m in (res.get("companies") or []):
            if not isinstance(m, dict):
                continue
            cid = m.get("company_id")
            row = {"label": m.get("label") or f"company {cid}",
                   "do_call": {"intent": "hire_merc", "args": {"company_id": cid}},
                   "troops": m.get("troops"),
                   "regiments": m.get("regiments"),
                   "home_county": m.get("home_county_name") or m.get("home_county_key"),
                   "cost": m.get("cost"),
                   "blocked": None}
            if m.get("distance_px") is not None:
                row["distance_px"] = m.get("distance_px")
            available.append(row)
        if res.get("cost_note"):
            warnings.append({"code": "merc_cost_not_read", "is_known_limitation": True,
                             "detail": res.get("cost_note")})
        if res.get("listed_before_cap") and res.get("count") is not None \
                and res.get("listed_before_cap") > res.get("count"):
            warnings.append({"code": "truncated", "is_known_limitation": False,
                             "detail": (f"{res.get('count')} of {res.get('listed_before_cap')} "
                                        f"free companies shown ({res.get('order')})")})
    elif action_class == "men_at_arms":
        # lane-query item 2: the enumerate do(intent="men_at_arms") pointed at.
        res = await gw.read("query_maa_types", {"actor_id": character_id})
        res = res if isinstance(res, dict) else {}
        types = [t for t in (res.get("types") or []) if isinstance(t, dict) and t.get("maa_type")]
        # fielded first (known available), then cheapest first; a type with no
        # recruit cost read (0) sorts last. Capped: 109 rows were 47 KB live.
        types.sort(key=lambda t: (-(_as_int(t.get("fielded_regiments"))),
                                  _f(t.get("recruit_cost")) <= 0,
                                  _f(t.get("recruit_cost"))))
        cap = max(1, len(candidates)) if candidates else MAA_ENUMERATE_CAP
        if candidates:
            want = {str(c) for c in candidates}
            types = [t for t in types if t.get("maa_type") in want]
        elif len(types) > cap:
            warnings.append({"code": "truncated", "is_known_limitation": False,
                             "detail": (f"{cap} of {len(types)} MaA types shown (fielded first, "
                                        "then cheapest); pass candidates=[<maa_type>, ...] "
                                        "to look up specific types")})
            types = types[:cap]
        for t in types:
            mt = t.get("maa_type")
            row = {"label": f"{mt} (recruit {t.get('recruit_cost')} gold)",
                   "do_call": {"intent": "men_at_arms", "args": {"action": "recruit", "maa_type": mt}},
                   "recruit_cost": t.get("recruit_cost"),
                   "upkeep_per_stack": t.get("upkeep_per_stack"),
                   "fielded_regiments": t.get("fielded_regiments")}
            if t.get("affordable") is False:
                row["blocked"] = why_blocked(
                    "insufficient_gold",
                    summary=f"recruit cost {t.get('recruit_cost')} > gold {res.get('gold')}",
                    measured={"need": t.get("recruit_cost"), "have": res.get("gold")})
                unavailable.append(row)
            else:
                row["blocked"] = None
                available.append(row)
        if res.get("note"):
            warnings.append({"code": "maa_unlock_not_checked", "is_known_limitation": True,
                             "detail": res.get("note")})
    elif action_class == "factions_to_join":
        # F16: the game's own verdict on every create/join/leave/press, with its reason.
        cid = character_id or _as_int((actor or {}).get("id"))
        res = await _factions.options(gw, cid)
        available, unavailable, warnings = _factions.option_rows(res)
        return envelope("do", result={"action_class": action_class,
                                      "liege_id": res.get("liege_id"),
                                      "liege_name": res.get("liege_name"),
                                      "available": available, "unavailable": unavailable},
                        actor=actor, game=gw.game(), warnings=warnings, bridge_health=gw.health)
    elif action_class == "buildings":
        # The building database (query_building_types). With args.location_id (the
        # barony title id of a holding) every row is the game's own verdict for
        # this ruler there (construction_check: the construction validity, which
        # holds the price against the treasury too) and its do_call is complete.
        loc = _as_int(args.get("location_id"))
        res = await gw.read("query_building_types", {})
        res = res if isinstance(res, dict) else {}
        rows = [b for b in (res.get("building_types") or []) if isinstance(b, dict) and b.get("name")]
        if candidates:
            want = {str(c) for c in candidates}
            rows = [b for b in rows if b.get("name") in want]
        elif len(rows) > BUILDING_ENUMERATE_CAP:
            warnings.append({"code": "truncated", "is_known_limitation": False,
                             "detail": (f"{BUILDING_ENUMERATE_CAP} of {len(rows)} building types shown; "
                                        "pass candidates=[<building>, ...] to look up specific ones")})
            rows = rows[:BUILDING_ENUMERATE_CAP]
        # One construction_check for every row: the bridge asks the game about the
        # whole list in one game-thread call (the validity runs under the random
        # scope, which is safe only there), not one engine round trip per building.
        verdicts: dict = {}
        no_verdict: dict = {}
        if loc > 0 and rows:
            res_chk = await gw.read("construction_check",
                                    {"actor_id": character_id, "location_id": loc,
                                     "buildings": [b.get("name") for b in rows]})
            res_chk = res_chk if isinstance(res_chk, dict) else {}
            if res_chk.get("success") is True:
                for v in res_chk.get("rows") or []:
                    if isinstance(v, dict) and v.get("building"):
                        verdicts[v["building"]] = v
            else:
                no_verdict = res_chk
        for bt in rows:
            bk = bt.get("name")
            row = {"label": f"{bk} (level {bt.get('level')})" if bt.get("level") is not None else bk,
                   "do_call": {"intent": "build", "args": {"building": bk}},
                   "construction_days": bt.get("construction_days")}
            if loc <= 0:
                row["needs"] = {"location_id": "the barony title id of a holding you hold "
                                               "(inspect(subject='county') holdings[].barony_title_id)"}
                row["blocked"] = None
                available.append(row)
                continue
            row["do_call"]["args"]["location_id"] = loc
            chk = verdicts.get(bk) or no_verdict
            row["price"] = chk.get("price")
            if chk.get("success") is not True:
                row["blocked"] = map_raw_reason(str(chk.get("reason") or chk.get("error") or "unrun"),
                                                summary=str(chk.get("reason") or chk.get("error") or ""))
                unavailable.append(row)
            elif not chk.get("buildable"):
                row["blocked"] = why_blocked("target_ineligible",
                                             summary=_build_refusal_summary(chk)
                                             or "the game's construction validity refuses, with no reason text",
                                             raw_reason="construction_refused")
                unavailable.append(row)
            else:
                row["blocked"] = None
                available.append(row)
    elif action_class == "imprison_targets":
        cid = character_id or _as_int((actor or {}).get("id"))
        available, unavailable, refused = await imprison_target_rows(gw, cid, candidates)
        if refused is not None:
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=refused, bridge_health=gw.health)
    elif action_class == "formable_titles":
        # GAP 1 (re_toolgap_mint.md): make the formable-title set DISCOVERABLE rather than
        # memorized. Pure composition of the already-shipping query_dejure_progress read
        # (which resolves UNCREATED keys + supplies the /93 meter) — zero new bridge surface,
        # mirrors the DeclareWar enumerate→do_call ergonomics. The agent reads this, picks an
        # `available` row, and fires its embedded do_call do(intent="mint_title", args=...).
        # Bounded BY DESIGN: only the keys the caller names (never a DB-wide scan).
        keys = list(candidates or [])
        warnings.append({"code": "control_requirements", "is_known_limitation": False,
                         "detail": ("Each row is the game's own verdict: the create-title command's "
                                    "validity (de-jure control, not already held, the game's "
                                    "can_create rules) and its price in gold (piety or treasury for "
                                    f"some governments), read before anything is sent. {MINT_DEJURE_RULE}; "
                                    "a titular kingdom (no de-jure "
                                    "counties, e.g. k_danelaw) is formed by its decision, not minted.")})
        for tkey in keys:
            prog = await gw.read("query_dejure_progress",
                                 {"character_id": character_id, "title_key": tkey})
            prog = prog if isinstance(prog, dict) else {}
            if str(prog.get("error") or "").startswith("title_key"):
                continue  # not a real landed_title — skip (never invent a row)
            held, total = _as_int(prog.get("held")), _as_int(prog.get("total"))
            short = mint_dejure_shortfall(tkey, held, total)
            row = {"label": f"Form {tkey} ({held}/{total} de-jure)",
                   "do_call": {"intent": "mint_title", "args": {"title_key": tkey}},
                   "measured": {"held": held, "total": total, "need": dejure_need(total),
                                "fraction": round(_f(prog.get("fraction")), 3)}}
            if short is not None:
                need = short["need"]
                row["blocked"] = why_blocked(
                    "de_jure_control",
                    summary=(f"hold {held}/{need} de-jure counties of {tkey} "
                             f"(need >={round(MINT_DEJURE_MIN_FRAC * 100)}% REALM-de-jure, "
                             "counted via transitive vassals — NOT personal demesne)"),
                    measured=row["measured"])
                unavailable.append(row)
            else:
                # The game's verdict and price (mint_title_price, the same check
                # do(intent="mint_title") makes before it sends).
                blk, price = await mint_title_price(gw, character_id, tkey)
                if price is not None:
                    row["price"] = price
                row["blocked"] = blk
                (unavailable if blk is not None else available).append(row)
    else:
        return envelope("do", ok=False, actor=actor, game=gw.game(),
                        # ck-ycim: SAY WHICH VOCABULARY IT IS. The census caught
                        # `men_at_arms` asked as an action_class five times, and
                        # it is a legitimate INTENT — a reasonable guess from an
                        # agent that has seen both plural-noun classes and a
                        # `men_at_arms` intent. The old message listed the valid
                        # classes and never said the name belonged to the other
                        # list, so there was nothing to correct the guess with.
                        blocked=why_blocked(
                            "unknown_verb",
                            summary=(
                                f"unknown action_class '{action_class}'. "
                                + (f"'{action_class}' is an INTENT, not a class of things to "
                                   "list — pass it as intent= (execute mode) instead of "
                                   "action_class= (enumerate mode). "
                                   if action_class in INTENTS else "")
                                + f"Valid classes: {', '.join(ACTION_CLASSES)}"),
                            raw_reason=f"unknown_action_class:{action_class}"),
                        bridge_health=gw.health)

    return envelope("do", result={"action_class": action_class, "available": available,
                                   "unavailable": unavailable},
                    actor=actor, game=gw.game(), warnings=warnings, bridge_health=gw.health)


def applied_result(result: dict) -> dict:
    """An applied verdict's result without the bridge's interim reply when that reply
    did not itself say success: the readback is the verdict, and a `raw` with
    success:false beside it is two polarities in one envelope (L14 S3, surface-eval
    finding 14: k_ireland applied under raw.success:false). The reply's request id
    stays, so the write can still be found in inspect(pending)."""
    raw = result.get("raw")
    if not isinstance(raw, dict) or raw.get("success") is True:
        return result
    out = {k: v for k, v in result.items() if k != "raw"}
    out["request_id"] = raw.get("intent_request_id") or raw.get("request_id")
    return out


async def mint_title_price(gw, character_id: int, tkey: str) -> tuple[Optional[dict], Optional[dict]]:
    """L14 S3 (surface-eval finding 14): the game's own answer to "can this ruler
    create tkey now, and at what price?" — query_create_title_cost, which runs the
    create-title command's IsValid (with its reasons) and the engine's cost function
    in the currency the engine charges, and reads what the ruler holds of it.

    Returns (blocked, measured). measured = {"currency", "cost", <currency>: have}
    when the price was read. blocked is None when the mint may be sent:
      * price read and have < cost      -> cannot_afford, carrying the price
      * IsValid said no                 -> create_title_engine_gate, the game's reasons
      * price read, or IsValid said yes -> None (the engine predicate is the
        fallback when the price functions are unmapped), except for a duchy or
        a key naming no tier, whose IsValid may skip the cost (m-3): refused
      * neither could be read           -> mint_cost_unverified (never sent: an
        unpriced mint is how k_ireland was created at 17.85 gold)
    One definition: do(intent="mint_title") and do(action_class="formable_titles")
    both call this."""
    chk = await _read_dict(gw, "query_create_title_cost",
                           {"actor_id": character_id, "title_key": tkey})
    ok = chk.get("success") is True
    price = chk.get("price") if isinstance(chk.get("price"), dict) else {}
    known = ok and price.get("known") is True
    measured = None
    if known:
        cur = str(price.get("currency") or "gold")
        cost, have = round(_f(price.get("cost")), 2), round(_f(price.get("have")), 2)
        measured = {"currency": cur, "cost": cost, cur: have}
        if price.get("affordable") is False or have < cost:
            return why_blocked("cannot_afford",
                               summary=f"creating {tkey} costs {cost:.2f} {cur}; you have {have:.2f}",
                               measured=measured, raw_reason="create_title_cannot_afford"), measured
    if ok and chk.get("engine_valid") is False:
        why = engine_text("\n".join(str(x) for x in (chk.get("engine_reasons") or [])))
        return why_blocked("create_title_engine_gate",
                           summary=f"the game refuses creating {tkey}: {why or NO_GAME_DETAIL}",
                           measured=measured, raw_reason="create_title_refused_by_engine_gate"), measured
    if known:
        return None, measured
    # IsValid stands in for an unread price only where its cost leg runs: the gate
    # skips it for a duchy (tier 3) in one government case while the apply charges
    # it always (re_create_title_action.md, "The cost leg"), so an unpriced duchy,
    # or a title whose tier the key does not name, is refused (L14 FF1 m-3).
    tier = title_tier_of_key(tkey)
    if ok and chk.get("engine_valid") is True and tier is not None and tier != 3:
        return None, measured
    cause = str(chk.get("reason") or price.get("reason") or chk.get("error") or "no_reply")
    if ok and chk.get("engine_valid") is True:
        cause = (f"{price.get('reason') or 'price_unknown'}; the game's check skips the cost "
                 "for a duchy, so its yes does not stand in for the price")
    return why_blocked("mint_cost_unverified",
                       summary=(f"the game's price for {tkey} could not be read ({cause}); "
                                "the mint was not sent"),
                       raw_reason=cause), measured


# FIX 1 — the do() intents that resolve by post-dispatch readback (not raw.success).
# mint_title: title now HELD. Genuinely
# deferred B* intents (scheme/faction/take_decision when enqueued) keep first-class
# pending (no synchronous readback) and are NOT in this set.
CONFIRMABLE_WRITES = {"mint_title"}


async def _confirm_intent_write(gw, intent: str, bridge_action: str, bridge_params: dict,
                                args: dict, character_id: int, actor: dict,
                                warnings: list) -> dict[str, Any]:
    """Dispatch a CONFIRMABLE_WRITES do() intent and resolve it by readback (#113)."""
    if intent == "mint_title":
        tkey = args.get("title_key") or args.get("title")

        async def _title_held(raw) -> Optional[dict]:
            # applied proof: the ruler now HOLDS the title, read off the title itself
            # (query_title_succession, query_handlers.cpp do_query_title_succession: it
            # writes `action` and `title_id` for every title it found and
            # `current_holder_id` only for a held one, line 12126) by the title_id the
            # create_title reply names. Never de-jure control: holding every de-jure
            # county is the mint's PRECONDITION, not its product (L14 FF1 B-1). Never
            # query_realm's primary title: a king who mints a duchy keeps his kingdom
            # primary, and query_realm carries no titles list (l14h geval, 2026-10-31:
            # Lothaire II minted d_bar, the drain ran it, his gold fell by the price, and
            # that readback saw nothing -> "rejected"). A reading that could not be made
            # is UNREAD, never "not held" (witness r4 N4-2: the same defect by another
            # door); settle_write then answers pending, drains_when "readback".
            tid = _as_int(raw.get("title_id")) if isinstance(raw, dict) else 0
            if tid <= 0:
                return UNREAD
            t = await _read_dict(gw, "query_title_succession", {"title_id": tid})
            if not isinstance(t, dict) or ("title_id" not in t and "action" not in t):
                return UNREAD
            if _as_int(t.get("current_holder_id")) == _as_int(character_id):
                return {"title_held": tkey, "title_id": tid, "via": "title_holder"}
            return None

        conf = await _confirm_write(gw, bridge_action, bridge_params,
                                    readback=_title_held, deadline_s=12.0)
        raw = conf.get("raw") or {}
        resolved = conf.get("resolved")
        # The refusal comes FIRST (B-1): the drain's, else the reply's own
        # (reply_refusal, the rule settle_write applies). A readback must never
        # outrank the game's no.
        reason = conf.get("refused") or reply_refusal(raw)
        if reason:
            counts = _parse_dejure_counts(reason)
            blk = write_blocked(reason, conf=conf if conf.get("refused") else None, raw=raw)
            if counts is not None:
                blk["measured"] = counts
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=blk, warnings=warnings, bridge_health=gw.health)
        if resolved is not None:
            # One polarity (L14 S3, applied_result): the readback is the verdict.
            return envelope("do", verdict="applied",
                            result=applied_result({"intent": intent, "title_key": tkey,
                                                   "readback": resolved, "raw": raw}),
                            actor=actor, game=gw.game(), warnings=warnings, bridge_health=gw.health)
        _key, lever = lever_of(intent)
        if (conf.get("sent") and conf.get("drains_when") != "readback"
                and lever["shows"] == "on_execute"):
            # The drain ran it and the title is not held after the pass: the
            # LEVERS row says what that is (mint_title: absent "unconfirmed").
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            result={"intent": intent, "title_key": tkey, "raw": raw,
                                    "sent": True},
                            blocked=lever_absent_block(lever, "the title held"),
                            warnings=warnings, bridge_health=gw.health)
        # Not held after the drain: the pending names its wait (confirm_write).
        return envelope("do", verdict="pending", actor=actor, game=gw.game(),
                        pending=conf_pending(
                            conf, expected_change="title held",
                            verify_with="inspect(subject='title_succession', title_id=<the reply's "
                                        "raw.title_id>).current_holder_id is you"),
                        result={"intent": intent, "title_key": tkey, "raw": raw,
                                "sent": conf.get("sent")},
                        warnings=warnings, bridge_health=gw.health)

    # Only CONFIRMABLE_WRITES route in, and each has its branch above. (Until L14 S3
    # an unhandled intent fell through to a bare write reported "applied" whatever
    # the bridge answered.)
    raise ValueError(f"_confirm_intent_write: no confirmation for intent {intent!r}")


#: do(intent="scheme", args.action): what the scheme intent can do. Asking whom
#: a scheme can target is discover(what="scheme_targets"), a read.
SCHEME_ACTIONS = ("start", "cancel")


#: What names one scheme across two reads. The game hands a freed scheme id to the
#: next scheme it creates, so an id alone can name the AI's new scheme in the read
#: after a cancel; the type, target and start date are the scheme's own.
SCHEME_IDENTITY_KEYS = ("scheme_id", "type", "target_id", "start_date")


def scheme_identity(row: Any) -> tuple:
    """The identity of a query_schemes row: its id, type, target and start date."""
    row = row if isinstance(row, dict) else {}
    return tuple(row.get(k) for k in SCHEME_IDENTITY_KEYS)


def _scheme_rows(reply: Any, scheme_id: int) -> list:
    """Every row holding scheme_id in a query_schemes reply."""
    rows = reply.get("schemes") if isinstance(reply, dict) else None
    return [row for row in rows or []
            if isinstance(row, dict) and _as_int(row.get("scheme_id")) == scheme_id]


def _scheme_brief(row: dict) -> dict:
    return {k: row.get(k) for k in SCHEME_IDENTITY_KEYS + ("target_name",) if k in row}


async def _do_scheme(gw, bridge_action: str, bridge_params: dict, actor: dict,
                     warnings: list) -> dict[str, Any]:
    """do(intent="scheme"): start or cancel, each confirmed by a second read.

    The bridge answers a start only after the game thread ran it: CanStartScheme
    (whose refusal carries engine_reasons), then CreateActiveScheme, then its own
    walk of the owner's list. The verdict here comes from a different read,
    inspect's query_schemes: `applied` means that list now holds the new scheme
    (start) or no longer holds it (cancel).

    A cancel whose id the game handed to a new scheme of the same owner between
    the bridge's cancel and the read after it is `applied`, with that scheme under
    result.id_reused_by. That case is covered by the unit test
    test_scheme_routes.py::test_a_freed_id_given_to_a_new_scheme_is_not_the_cancelled_scheme
    only; no live867 row reaches it. The reuse has to land inside this one call,
    after the write and before the read, and only the game's own AI makes it land
    there (run w-l3-schemes-1 saw it once, on learn_language, id 152): a scenario
    that cancels, starts a second scheme that takes the freed id, and cancels again
    sees two ordinary cancels, because its own start comes after the first call's
    read and before the second call's first read."""
    action = str(bridge_params.get("action") or "start")
    owner = _as_int(bridge_params.get("actor_id"))

    def _refuse(gate: str, summary: str, *, fix=None, raw_reason: str = "", measured=None):
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked(gate, summary=summary, fix=fix,
                                            raw_reason=raw_reason, measured=measured),
                        warnings=warnings, bridge_health=gw.health)

    if action not in SCHEME_ACTIONS:
        return _refuse("unknown_verb",
                       f"scheme action '{action}' is not one of {list(SCHEME_ACTIONS)}; "
                       "to see whom a scheme can target use discover(what='scheme_targets')",
                       raw_reason=f"unknown_scheme_action:{action}")
    if action == "start":
        missing = [k for k in ("scheme", "target_id") if not bridge_params.get(k)]
        if missing:
            return _refuse("missing_required_argument",
                           f"a scheme start needs {missing}; discover(what='scheme_types') "
                           "lists the schemes, discover(what='scheme_targets', scheme=...) "
                           "the characters each can target",
                           fix={"rejected_fields": missing,
                                "retry_with": {"scheme": "<key>", "target_id": "<id>"}})
    elif not bridge_params.get("scheme_id"):
        return _refuse("missing_required_argument",
                       "a scheme cancel needs scheme_id (inspect(subject='schemes') lists them)",
                       fix={"rejected_fields": ["scheme_id"], "retry_with": {"scheme_id": "<id>"}})

    # A cancel names the scheme it removes before it sends anything: the read
    # after it is matched on that scheme's identity, not on an id the game may
    # already have handed to a new scheme.
    cancelled: Optional[dict] = None
    if action == "cancel":
        cancel_id = _as_int(bridge_params.get("scheme_id"))
        before = await gw.read("query_schemes", {"character_id": owner})
        if not (isinstance(before, dict) and isinstance(before.get("schemes"), list)):
            return _refuse("read_timeout",
                           f"inspect(subject='schemes') for {owner} could not be read, so "
                           f"the cancel of scheme {cancel_id} could not be checked after it; "
                           "nothing was sent",
                           measured={"scheme_id": cancel_id, "owner_id": owner})
        rows = _scheme_rows(before, cancel_id)
        if not rows:
            return _refuse("name_not_found",
                           f"inspect(subject='schemes') for {owner} does not list scheme "
                           f"{cancel_id}; nothing was sent",
                           raw_reason="scheme_not_found",
                           measured={"scheme_id": cancel_id, "owner_id": owner})
        cancelled = rows[0]

    submitted_on = gw.game().get("date")
    raw = await gw.write(bridge_action, bridge_params)
    raw = raw if isinstance(raw, dict) else {}
    reason = str(raw.get("reason") or raw.get("error") or "")
    status = str(raw.get("status") or "")
    result = raw.get("result") if isinstance(raw.get("result"), dict) else {}
    if reason == "experimental_write_disabled" or status == "experimental_write_disabled":
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=experimental_gate_blocked(raw, bridge_action),
                        warnings=warnings, bridge_health=gw.health)
    drained = False
    if status in PENDING_STATUSES or reason == "intent_timeout_no_result":
        # The handler's own wait for the drain ran out: stage 1 reads the verdict
        # back (await_drain). A refusal is the engine's answer; a success goes on
        # to the second read below (a start is then found by type and target: the
        # reply that would have named its id never came); a verdict not read is
        # pending on the next pass.
        drain = await await_drain(gw.read, raw)
        no_verdict = (drain["status"] in DRAIN_REFUSED
                      and drain_refusal(drain) in DRAIN_RAN_NO_VERDICT)
        if drain["status"] in DRAIN_REFUSED and not no_verdict:
            why = drain_refusal(drain)
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            result={"intent": "scheme", "raw": raw, "drain": drain["row"]},
                            blocked=(title_scheme_blocked(str(bridge_params.get("scheme") or ""), why)
                                     if why == TITLE_SCHEME_REASON else
                                     write_blocked(why, conf={"refused": why, "drain": drain})),
                            warnings=warnings, bridge_health=gw.health)
        if drain["status"] != DRAIN_DONE and not no_verdict:
            return envelope("do", verdict="pending", actor=actor, game=gw.game(),
                            pending=write_pending(
                                "next_pass", game_date_at_submit=submitted_on,
                                intent_id=drain["intent_id"],
                                poll_token=drain["intent_id"] or raw.get("ticket") or "",
                                expected_change=f"scheme {action}",
                                why_pending=(f"the bridge reported {status or reason!r}; "
                                             f"the drain's verdict reads {drain['status']!r}"),
                                verify_with="inspect(subject='schemes')"),
                            result={"intent": "scheme", "raw": raw, "drain": drain["row"]},
                            warnings=warnings, bridge_health=gw.health)
        drained = True
    if not drained and not raw.get("success") and reason == TITLE_SCHEME_REASON:
        # L18 round 2 (witness item 7): the start refuses a title-target type as the
        # check does (scheme_engine scheme_target_refusal), with the same door.
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        result={"intent": "scheme", "raw": raw},
                        blocked=title_scheme_blocked(str(bridge_params.get("scheme") or ""), reason),
                        warnings=warnings, bridge_health=gw.health)
    if not drained and not raw.get("success"):
        blk = write_blocked(reason or status or "unmapped_refusal", summary=reason or status, raw=raw)
        raw_out = raw
        if result.get("engine_reasons"):
            blk["measured"] = {"engine_reasons": scheme_reasons(result["engine_reasons"])}
            # One shape per reply (L14 FF1 m-22): the reasons ride in
            # measured.engine_reasons as {check, summary}; the bridge's own
            # {check, text} copy is not carried beside them.
            raw_out = {**raw, "result": {k: v for k, v in result.items() if k != "engine_reasons"}}
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        result={"intent": "scheme", "raw": raw_out}, blocked=blk,
                        warnings=warnings, bridge_health=gw.health)

    scheme_id = _as_int(result.get("scheme_id") if action == "start"
                        else bridge_params.get("scheme_id"))
    after = await gw.read("query_schemes", {"character_id": owner})
    readable = isinstance(after, dict) and isinstance(after.get("schemes"), list)
    if not readable:
        # The write ran (the bridge or the drain said so) and its list could not
        # be read: pending readback, ok:true, never rejected (FF5 ruling).
        return envelope("do", verdict="pending", actor=actor, game=gw.game(),
                        pending=write_pending(
                            "readback", game_date_at_submit=submitted_on,
                            intent_id=intent_id_of(raw), raw_reason="write_unconfirmed",
                            expected_change=f"scheme {action}",
                            why_pending=f"the {action} ran and inspect(subject='schemes') for "
                                        f"{owner} could not be read",
                            verify_with="inspect(subject='schemes') before sending it again"),
                        result={"intent": "scheme", "raw": raw, "sent": True,
                                "scheme_id": scheme_id},
                        warnings=warnings, bridge_health=gw.health)
    same_id = _scheme_rows(after, scheme_id)
    reused = None
    if action == "start":
        # The new scheme: this id, of the type asked, on the target asked (any id
        # when the drain, not the reply, said it ran).
        want_type = bridge_params.get("scheme")
        want_target = _as_int(bridge_params.get("target_id"))
        if not scheme_id and drained and readable:
            same_id = [r for r in after["schemes"] if isinstance(r, dict)]
        row = next((r for r in same_id if r.get("type") == want_type
                    and _as_int(r.get("target_id")) == want_target), None)
    else:
        row = next((r for r in same_id
                    if scheme_identity(r) == scheme_identity(cancelled)), None)
        reused = next((r for r in same_id if r is not row), None)
    if readable and ((action == "start") == (row is not None)):
        out = {"intent": "scheme", "action": action, "scheme_id": scheme_id,
               "readback": "inspect(subject='schemes')"}
        if action == "start":
            out["scheme"] = row
            out["scheme_id"] = scheme_id or _as_int(row.get("scheme_id"))
        else:
            out["cancelled"] = _scheme_brief(cancelled)
            if reused is not None:
                out["id_reused_by"] = _scheme_brief(reused)
                out["note"] = (f"the game gave id {scheme_id} to a new scheme "
                               f"({reused.get('type')} on {reused.get('target_id')}, started "
                               f"{reused.get('start_date')}); the scheme you cancelled is gone, "
                               "and a cancel of this id now cancels the new one")
        return envelope("do", verdict="applied", result=out, actor=actor, game=gw.game(),
                        warnings=warnings, bridge_health=gw.health)
    return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                    result={"intent": "scheme", "raw": raw, "scheme_id": scheme_id},
                    blocked=write_blocked(gate=
                        "write_unconfirmed",
                        summary=(f"the bridge reported the {action} of scheme {scheme_id} done, and "
                                 f"inspect(subject='schemes') for {owner} "
                                 + ("could not be read" if not readable else
                                    "does not list it with that type and target"
                                    if action == "start" else
                                    "still lists it: the same type, target and start date")),
                        measured={"scheme_id": scheme_id, "owner_id": owner,
                                  "listed": row is not None, "list_readable": readable}),
                    warnings=warnings, bridge_health=gw.health)


async def scheme_types_impl(gw, target_type: str = "") -> dict[str, Any]:
    """discover(what="scheme_types"): every scheme in the engine's database, with what
    it targets, its category, whether it is secret and its maximum breaches."""
    raw = await gw.read("query_scheme_types", {"target_type": target_type} if target_type else {})
    if not isinstance(raw, dict) or not isinstance(raw.get("scheme_types"), list):
        return envelope("discover", ok=False, game=gw.game(), bridge_health=gw.health,
                        blocked=why_blocked("read_timeout" if not raw else "unmapped_refusal",
                                            summary="the scheme type database read returned "
                                                    + (str(raw.get("error")) if isinstance(raw, dict) and raw.get("error") else "nothing")))
    return envelope("discover", result=raw, game=gw.game(), bridge_health=gw.health)


#: How many characters discover(what="scheme_targets") checks when the caller
#: names none: the bridge checks a list, never the whole character database.
SCHEME_POOL_COURT = 60
SCHEME_POOL_LIEGE_COURT = 30
SCHEME_POOL_CLAIM_HOLDERS = 3


async def _scheme_target_pool(gw, owner: int) -> tuple[list, list]:
    """The characters a ruler deals with: his court and direct vassals, his liege
    and the liege's court, the holders of titles he claims and their courts
    (search_characters court_of over the game's liege links; query_claims)."""
    pool: list = []
    sources: list = []

    def _add(rows, label):
        n = 0
        for c in (rows or {}).get("characters") or [] if isinstance(rows, dict) else []:
            cid = _as_int((c or {}).get("id"))      # search_characters rows: id (:11346)
            if cid > 0 and cid not in pool:
                pool.append(cid)
                n += 1
        sources.append({"from": label, "count": n})

    _add(await gw.read("search_characters", {"filter": "alive", "court_of": owner,
                                             "max_results": SCHEME_POOL_COURT}),
         f"court_of:{owner}")
    me = await gw.read("query_character", {"character_id": owner})
    me = _char_section(me)
    liege = _as_int(me.get("liege_id"))
    if liege > 0 and liege != owner:
        pool.append(liege)
        sources.append({"from": "liege", "count": 1})
        _add(await gw.read("search_characters", {"filter": "alive", "court_of": liege,
                                                 "max_results": SCHEME_POOL_LIEGE_COURT}),
             f"court_of:{liege}")
    # The holders of the titles he claims, and their courts: the rivals a hostile
    # scheme is usually aimed at.
    claims = await gw.read("query_claims", {"character_id": owner})
    holders = []
    for c in (claims.get("claims") or []) if isinstance(claims, dict) else []:
        h = _as_int((c or {}).get("holder_id"))
        if h > 0 and h != owner and h not in holders:
            holders.append(h)
    for h in holders[:SCHEME_POOL_CLAIM_HOLDERS]:
        if h not in pool:
            pool.append(h)
        _add(await gw.read("search_characters", {"filter": "alive", "court_of": h,
                                                 "max_results": SCHEME_POOL_LIEGE_COURT}),
             f"court_of:{h}")
    if holders:
        sources.append({"from": "claim_holders", "count": len(holders[:SCHEME_POOL_CLAIM_HOLDERS])})
    return pool, sources


def scheme_reasons(reasons) -> list:
    """The game's reasons a scheme may not start, as a player reads them: each
    {check, summary} and nothing else. `summary` is the game's own words (the
    bridge's `text`, cleaned). Without words:
      - a failing trigger block (SCHEME_UNWORDED_CHECKS: allow, valid, trigger)
        is NO_BRIDGE_WORDS: the bridge does not ask the game for that tooltip,
        so nobody looked;
      - sink_method_not_modelled, owner_invalid and target_invalid say what they
        are (SCHEME_CHECK_WORDS);
      - an engine entry (an existing scheme, a cooldown) is NO_GAME_DETAIL. The
        bridge sends an engine entry only with non-empty text (emit_reasons), so
        this is the case where the game's text is markup with no words in it (a
        compound header such as "All of these:", an icon): the game was asked
        and its answer says nothing;
      - any other check is SCHEME_CHECK_UNKNOWN."""
    out = []
    for r in reasons if isinstance(reasons, list) else []:
        if not isinstance(r, dict):
            continue
        text = r.get("text")
        words = engine_text(text) if isinstance(text, str) else ""
        check = r.get("check")
        if not words:
            words = (NO_BRIDGE_WORDS if check in SCHEME_UNWORDED_CHECKS
                     else SCHEME_CHECK_WORDS.get(check)
                     or (NO_GAME_DETAIL if check == "engine_entry" else SCHEME_CHECK_UNKNOWN))
        out.append({"check": check, "summary": words})
    return out


async def scheme_targets_impl(gw, scheme: str, character_id: int = 0, target_id: int = 0,
                              candidates: list = None, max_results: int = 0) -> dict[str, Any]:
    """discover(what="scheme_targets"): the engine's own start check (CanStartScheme)
    for one target or a candidate list; with neither, the actor's court and vassals,
    his liege and the liege's court (result.candidates_from). Nothing is started. A
    refused row carries the engine's reasons."""
    actor, err = await _resolve_actor(gw, character_id, "discover")
    if err:
        return err
    if not scheme:
        return envelope("discover", ok=False, actor=actor, game=gw.game(), bridge_health=gw.health,
                        blocked=why_blocked("missing_required_argument",
                                            summary="scheme_targets needs scheme=<key>; "
                                                    "discover(what='scheme_types') lists them",
                                            fix={"rejected_fields": ["scheme"],
                                                 "retry_with": {"scheme": "<key>"}}))
    owner = _as_int(character_id) or _as_int((actor or {}).get("id"))
    params: dict[str, Any] = {"actor_id": owner, "action": "check", "scheme": scheme}
    pool_from = None
    if target_id:
        params["target_id"] = _as_int(target_id)
    else:
        if not candidates:
            candidates, pool_from = await _scheme_target_pool(gw, owner)
        params["candidates"] = [_as_int(c) for c in candidates or [] if _as_int(c) > 0]
    if max_results:
        params["max_results"] = _as_int(max_results)
    raw = await gw.read("scheme_action", params)
    raw = raw if isinstance(raw, dict) else {}
    if raw.get("success") and isinstance(raw.get("result"), dict):
        out = dict(raw["result"])
        if "reasons" in out:
            out["reasons"] = scheme_reasons(out["reasons"])
        if isinstance(out.get("refused"), list):
            out["refused"] = [{**row, "reasons": scheme_reasons(row.get("reasons"))}
                              if isinstance(row, dict) else row for row in out["refused"]]
        if pool_from:
            out["candidates_from"] = pool_from
        return envelope("discover", result=out, actor=actor, game=gw.game(),
                        bridge_health=gw.health)
    reason = str(raw.get("reason") or raw.get("status") or "")
    if reason == TITLE_SCHEME_REASON:
        return envelope("discover", ok=False, actor=actor, game=gw.game(), bridge_health=gw.health,
                        blocked=title_scheme_blocked(scheme, reason))
    return envelope("discover", ok=False, actor=actor, game=gw.game(), bridge_health=gw.health,
                    blocked=map_raw_reason(reason or "unmapped_refusal", summary=reason))


#: L18-B: the door for a claim on a TITLE. 23 pilot calls of
#: discover(what="scheme_targets", scheme="generate_claim") were refused
#: scheme_type_targets_a_title (scheme_engine.cpp: the check scans CHARACTER targets
#: only) with nothing naming the call that works, and the seats then tried
#: do(scheme start), start_generate_claim_interaction and buy_claim_interaction in
#: turn. The game's generate_claim is started only by start_generate_claim_interaction,
#: whose is_shown needs an active laamp_help_fabricate_claim_contract
#: (common/character_interactions/06_ep3_scheme_interactions.txt): a landless
#: adventurer's contract scheme, never a landed ruler's. A landed ruler's claim on a
#: The council seat task_fabricate_claim is printed and evaluated on (dump
#: ck3_20260625_152901_ivar867: tasktype+0x30 -> councillor_court_chaplain). One
#: definition: the fabricate_targets rows' do_call and its empty reply name it.
FABRICATE_CLAIM_SEAT = "court_chaplain"
#: title is the council task task_fabricate_claim (do(action_class='fabricate_targets')).
TITLE_CLAIM_DOOR = {"tool": "do", "args": {"action_class": "fabricate_targets"}}
#: The bridge's refusal of a scheme type whose target is not a character, from the
#: start and the check alike (scheme_engine.cpp scheme_target_refusal).
TITLE_SCHEME_REASON = "scheme_type_targets_a_title"


def title_scheme_blocked(scheme: str, raw_reason: str = TITLE_SCHEME_REASON) -> dict:
    """The refusal of a title-target scheme, for discover(scheme_targets) and
    do(intent="scheme") alike: the door is TITLE_CLAIM_DOOR."""
    return why_blocked(
        "target_ineligible", raw_reason=raw_reason,
        summary=(f"'{scheme}' targets a TITLE, and the bridge checks and starts character-"
                 "target schemes only. A landed ruler's claim on a title is the council task "
                 "task_fabricate_claim: do(action_class='fabricate_targets') lists the titles "
                 "it can take. generate_claim itself is a landless adventurer's contract "
                 "scheme (start_generate_claim_interaction needs an active "
                 "laamp_help_fabricate_claim_contract)"),
        fix=TITLE_CLAIM_DOOR)


async def _do_cancel_pending(gw, a: dict, actor: dict) -> dict[str, Any]:
    """Withdraw one queued write by its request id (the bridge's cancel_intent)."""
    rid = str(a.get("request_id") or a.get("poll_token") or "")
    if not rid:
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("missing_required_argument",
                                            summary="cancel_pending requires args.request_id "
                                                    "(a do reply's pending.intent_id)"),
                        bridge_health=gw.health)
    raw = await gw.write("cancel_intent", {"request_id": rid})
    raw = raw if isinstance(raw, dict) else {}
    if raw.get("status") == "sent_no_confirmation":
        return envelope("do", ok=False, verdict="pending", actor=actor, game=gw.game(),
                        blocked=write_blocked(gate="write_unconfirmed",
                                            summary="cancel_intent was sent and the bridge did not confirm it; "
                                                    "check inspect(subject='pending')"),
                        result={"intent": _CANCEL_PENDING_INTENT, "request_id": rid, "raw": raw},
                        bridge_health=gw.health)
    if reply_refusal(raw):
        reason = str(raw.get("reason") or raw.get("error") or "not_found")
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        result={"intent": _CANCEL_PENDING_INTENT, "request_id": rid, "raw": raw},
                        blocked=write_blocked(reason, summary=f"cancel of {rid} refused: {reason}", raw=raw),
                        bridge_health=gw.health)
    return envelope("do", verdict="applied", actor=actor, game=gw.game(),
                    result={"intent": _CANCEL_PENDING_INTENT, "request_id": rid, "raw": raw},
                    bridge_health=gw.health)


# --------------------------------------------------------------------------- war intents (F8)
#: do(intent="make_peace") terms → the end-war interaction the seat sends, by the
#: side the seat leads. The game names all three from the attacker's view
#: (common/character_interactions/00_war.txt): a DEFENDER who surrenders sends
#: end_war_attacker_victory_interaction, a defender enforcing his demands sends
#: end_war_attacker_defeat_interaction. The recipient is always the enemy war
#: leader, and the engine finds the war from the pair (re/findings/re_war.md §1).
PEACE_TERMS: dict[str, dict[str, str]] = {
    "white_peace":     {"attacker": "end_war_attacker_white_peace_interaction",
                        "defender": "end_war_attacker_white_peace_interaction"},
    "surrender":       {"attacker": "end_war_attacker_defeat_interaction",
                        "defender": "end_war_attacker_victory_interaction"},
    "enforce_demands": {"attacker": "end_war_attacker_victory_interaction",
                        "defender": "end_war_attacker_defeat_interaction"},
}

#: The outcome the bridge's war-end hook records when each interaction ends a
#: war (query_war_history `outcome`; the spellings are war_history.cpp's
#: war_outcome_name, and test_war_intents checks them against that file).
PEACE_OUTCOME: dict[str, str] = {
    "end_war_attacker_white_peace_interaction": "white_peace",
    "end_war_attacker_defeat_interaction": "defender_victory",
    "end_war_attacker_victory_interaction": "attacker_victory",
}

#: The intents _do_war_intent runs.
WAR_INTENTS: frozenset = frozenset({"make_peace", "call_ally", "join_war"})

#: THE LEVER TABLE: per write lever, the server's readback and WHEN its effect
#: or the recipient's answer shows. ONE definition for the do() tail, the war
#: intents and the live867 scenarios; levers.md "When the answer shows"
#: restates it. Key (intent, sub): sub is the interaction for interact, the
#: terms for make_peace, the scope for change_law, "" otherwise; ("interact",
#: "*") is every interaction not named. A drain "success" means the write was
#: SENT; `applied` needs the readback (settle_write).
#:   shows "on_execute": the effect, or the recipient's answer, is visible after
#:       the dispatch pass, so stage 1 reads it back in either clock state and
#:       no day is waited. Still absent after the pass: `absent` says what that
#:       is: "declined" (the recipient said no: rejected recipient_refuses) or
#:       "unconfirmed" (a command that should have landed: rejected
#:       write_unconfirmed, with what was read).
#:   shows "days": the answer comes on a later day: stage 2, polled while the
#:       clock runs, pending clock_advances paused (result.sent), never a refusal.
#:   readback None: the server reads nothing back; the write is pending
#:       clock_advances with result.sent once the drain ran it. "unproven" in
#:       `evidence` marks a row nobody has measured: treated as days.
#: The synchronous handlers (set_council_task, set_obligation, construct_building,
#: change_realm_law, set_lifestyle_focus, ...) run on the game thread and answer
#: with their own verdict and readback in the reply; they are not queued and
#: are not rows here. One synchronous write IS a row: ("steer", ""), because
#: steer_impl reads its readback and its absent outcome from this table like a
#: queued lever (set_ai_personality writes in the call and reads the axes back
#: into its reply; settle_write re-reads them when the reply lacks them).
LEVERS: dict[tuple, dict] = {
    ("make_peace", "surrender"): {
        "shows": "days", "readback": "the war-end ledger (query_war_history)",
        "evidence": "live867 l13ff2a-p/r war_defender, war_lifecycle: applied in the call; a "
                    "surrender cannot be refused, so a missing ledger row waits as days"},
    ("make_peace", "white_peace"): {
        "shows": "days", "readback": "the war-end ledger (query_war_history)",
        "evidence": "live867 l13ff2a-p war_outcomes: all 31 sent offers ended white_peace 3-10 "
                    "days after the send; l13ff2a-r: applied by the stage-2 poll in 3-8 s"},
    ("make_peace", "enforce_demands"): {
        "shows": "days", "readback": "the war-end ledger (query_war_history)",
        "evidence": "unproven, treated as days (both runs: applied in the call at 100 war score, "
                    "the game's auto_accept; no in-call refusal seen past the pre-check)"},
    ("call_ally", ""): {
        "shows": "on_execute", "absent": "declined",
        "readback": "the ally on the leader's side of the war (query_wars all, verbose)",
        "evidence": "live867 l13ff2a-p/r call_allies: applied in the call. PROVEN FOR ACCEPT "
                    "ONLY: no refusal has been observed, so an ally whose yes landed after the "
                    "stage-1 re-reads would read as declined"},
    ("join_war", ""): {
        "shows": "days", "readback": "the joiner on the leader's side of the war",
        "evidence": "unproven, treated as days"},
    ("mint_title", ""): {
        "shows": "on_execute", "absent": "unconfirmed",
        "readback": "the title held (query_title_succession current_holder_id of the title the reply names)",
        "evidence": "create_title's intent dispatches the engine command in the drain "
                    "(action_handlers.cpp do_create_title); _confirm_intent_write"},
    ("change_law", "title"): {
        "shows": "on_execute", "absent": "unconfirmed",
        "readback": "the title's own laws (query_title_succession title_laws)",
        "evidence": "set_title_law queues the title-law window's command, executed on the next "
                    "pass (action_handlers.cpp do_set_title_law); live867 l4_succession_levers "
                    "_title_law reads title_laws within passes, no advance"},
    ("interact", "gift_interaction"): {
        "shows": "on_execute", "absent": "unconfirmed",
        "readback": "the exact amount: the sender's gold fell by what the recipient's rose, "
                    "within one game day (query_character gold and game_date, both sides)",
        "evidence": "live867 l6_gift: both purses move within passes of the send, no advance"},
    ("interact", "disinherit_interaction"): {
        "shows": "days", "readback": None,
        "evidence": "the game applies an own-court interaction on a daily tick (live867 _l4 "
                    "OWN_DAYS; l4_succession_levers reads it back over days)"},
    ("interact", "designate_heir_interaction"): {
        "shows": "days", "readback": None,
        "evidence": "own-court, applied on a daily tick (live867 _l4 OWN_DAYS, l4_succession_levers)"},
    ("interact", "grant_titles_interaction"): {
        "shows": "days",
        "readback": "the title's current holder is the recipient (query_title_succession "
                    "current_holder_id)",
        "evidence": "L14 minor 9: live eval-paused/running and evalc-paused/running (L13 bridge): the "
                    "grant to an own courtier read back with no day passed (paused, tick 4 both "
                    "sides) and running; kept `days` so a holder not yet changed after the pass "
                    "waits for the clock instead of reading as refused (the l4 proof's declaration, "
                    "_l4 OWN_DAYS, names no measured day)"},
    ("interact", "arrange_marriage_interaction"): {
        "shows": "days", "readback": None,
        "evidence": "own-court, applied on a daily tick (live867 _l4 OWN_DAYS, l4_send_options)"},
    ("interact", "revoke_title_interaction"): {
        "shows": "days", "readback": None,
        "evidence": "a vassal's AI answers in 4-9 days (ai_min/max_reply_days; live867 "
                    "l4_succession_levers REPLY_DAYS)"},
    ("interact", "negotiate_alliance_interaction"): {
        "shows": "days", "readback": None,
        "evidence": "the AI answers in 4-9 days (live867 l5_call_allies reads the alliance over days)"},
    ("faction", "*"): {
        "shows": "on_execute", "absent": "unconfirmed",
        "readback": "the faction's member list on its liege, or the leader's demand row "
                    "(factions._second_read; needs the faction the reply names)",
        "evidence": "live867 l10_factions_liege / l10_factions_vassal: create, join, leave and "
                    "press_demand read back in the same call"},
    ("scheme", "*"): {
        "shows": "on_execute", "absent": "unconfirmed",
        "readback": "the owner's scheme list (query_schemes; _do_scheme's second read)",
        "evidence": "live867 l3_schemes_matrix: start and cancel read back in the same call"},
    ("steer", ""): {
        "shows": "on_execute", "absent": "unconfirmed",
        "readback": "the axes as the bridge reads them back after the write "
                    "(set_ai_personality's own reply, else get_ai_personality)",
        "evidence": "personality.cpp do_set_ai_personality writes the bytes and the locks in "
                    "the call and reads the axes back into its reply; live eval-paused/running "
                    "steer write read back in the same call"},
    ("interact", "*"): {
        "shows": "days", "readback": None,
        "evidence": "unproven, treated as days"},
    ("*", "*"): {
        "shows": "days", "readback": None,
        "evidence": "unproven, treated as days (a queued write of an intent not named here)"},
}


def lever_of(intent: str, bridge_params: dict = None, *, sub: str = None) -> tuple:
    """(key, row) of LEVERS for a write: the named row, else the interaction
    default, else the catch-all. Refuses a row with a value it does not know."""
    p = bridge_params if isinstance(bridge_params, dict) else {}
    if sub is None:
        if intent == "interact":
            sub = str(p.get("interaction_type") or p.get("interaction") or "")
        elif intent == "change_law":
            sub = "title" if "title_id" in p else "realm"
        else:
            sub = ""
    for key in ((intent, sub), (intent, "*"), ("*", "*")):
        if key in LEVERS:
            row = LEVERS[key]
            if row["shows"] not in ("on_execute", "days") or row.get("absent") not in (
                    None, "declined", "unconfirmed"):
                raise ValueError(f"LEVERS{key} is malformed: {row}")
            return key, row
    raise KeyError((intent, sub))


def lever_absent_block(lever: dict, what: str) -> dict:
    """THE refusal of an "on_execute" lever whose effect is still absent after the
    dispatch pass ran it: the LEVERS row's `absent` says which ("declined" ->
    recipient_refuses, "unconfirmed" -> write_unconfirmed). One builder for the
    do() tail and the mint (_confirm_intent_write)."""
    if lever.get("absent") == "declined":
        return why_blocked("recipient_refuses", raw_reason="recipient_declined_on_execute",
                           summary=f"the recipient declined: {what} did not happen when the "
                                   "game ran it (the game gives no reason text)")
    return why_blocked("write_unconfirmed", raw_reason="readback_after_drain_absent",
                       summary=(f"the drain ran it and {lever.get('readback')} does not "
                                "show it after the pass"),
                       fix={"verify_with": "inspect(subject='pending')"})

def _gold_of(reply) -> Optional[float]:
    g = _char_section(reply).get("gold")
    return float(g) if isinstance(g, (int, float)) and not isinstance(g, bool) else None


async def _purses(gw, p: dict) -> dict:
    """The sender's and the recipient's gold, and the game date each reply was
    stamped with (every bridge reply carries game_date)."""
    out = {}
    for side, key in (("sender", "actor_id"), ("recipient", "target_id")):
        r = await gw.read("query_character", {"character_id": _as_int(p.get(key))})
        out[side] = _gold_of(r)
        out[side + "_date"] = (r or {}).get("game_date") if isinstance(r, dict) else None
    return out


async def _gift_before(gw, p: dict) -> dict:
    return await _purses(gw, p)


#: Gold the two purses may disagree by in a pure transfer (the game rounds).
GIFT_TOLERANCE = 0.05


async def _gift_landed(gw, p: dict, before: dict) -> Optional[dict]:
    """The gift's EXACT amount read back (witness r4 m1): the sender's purse fell
    by what the recipient's rose, with no game day between the two readings (a
    daily tick can pay either side income or take an expense, which a "the
    sender's gold fell" proxy reads either way). A day between them, or a purse
    not read, is UNREAD (not "absent"): the transfer cannot be separated from
    income. Stage 1 reads again (a later after-pair can share the before-pair's
    date only if no day passed since the send); still unread when the schedule
    ends, the gift that ran is pending drains_when "readback", ok:true, never
    rejected (FF5 ruling). Paused, no day passes and the reading is always made."""
    now = await _purses(gw, p)
    b = before or {}
    vals = (b.get("sender"), b.get("recipient"), now["sender"], now["recipient"])
    if any(v is None for v in vals):
        return UNREAD
    dates = {b.get("sender_date"), b.get("recipient_date"), now["sender_date"],
             now["recipient_date"]} - {None}
    if len(dates) > 1:
        # a daily tick between the readings: not separable from income. The
        # stage-1 schedule reads again; ended so, the gift is pending readback.
        return UNREAD
    paid = b["sender"] - now["sender"]
    got = now["recipient"] - b["recipient"]
    if paid > 0.5 and abs(paid - got) <= GIFT_TOLERANCE:
        return {"gold_before": b["sender"], "gold_after": now["sender"], "amount": round(paid, 3),
                "recipient_before": b["recipient"], "recipient_after": now["recipient"]}
    return None


async def _title_law_landed(gw, p: dict, before: dict) -> Optional[dict]:
    t = await _read_dict(gw, "query_title_succession", {"title_id": _as_int(p.get("title_id"))})
    if not isinstance(t.get("title_laws"), list):
        return UNREAD
    laws = t["title_laws"]
    law = str(p.get("law_name") or "")
    return {"title_laws": laws, "law": law} if law and law in laws else None


async def _grant_landed(gw, p: dict, before: dict) -> Optional[dict]:
    """The granted title's holder is the recipient (L14 minor 9: read once before
    answering; the evaluator saw it land with no day passed)."""
    t = await _read_dict(gw, "query_title_succession", {"title_id": _as_int(p.get("title_id"))})
    if "current_holder_id" not in t:
        return UNREAD
    holder, to = _as_int(t.get("current_holder_id")), _as_int(p.get("target_id"))
    return {"title_id": _as_int(p.get("title_id")), "holder_id": holder} if to and holder == to else None


#: The do() tail's readbacks, by LEVERS key: (read before the send or None, the
#: readback). Every do() row of LEVERS with a readback has one here (checked by
#: test_queued_is_not_applied); the war intents and mint_title read back in
#: their own paths.
LEVER_READBACKS: dict[tuple, tuple] = {
    ("interact", "gift_interaction"): (_gift_before, _gift_landed),
    ("change_law", "title"): (None, _title_law_landed),
    ("interact", "grant_titles_interaction"): (None, _grant_landed),
}


def _war_rows(raw) -> list:
    return [w for w in ((raw or {}).get("wars") or []) if isinstance(w, dict)]


def _war_pick(rows: list, war_id: int) -> Optional[dict]:
    return next((w for w in rows if _as_int(w.get("war_id")) == war_id), None)


def _war_view(w: dict) -> dict:
    """One war of query_wars(all, verbose): its leaders and both participant lists."""
    return {"war_id": _as_int(w.get("war_id")), "name": w.get("name"),
            "attacker_id": _as_int(w.get("primary_attacker")),
            "defender_id": _as_int(w.get("primary_defender")),
            "attackers": [_as_int(x) for x in (w.get("attackers") or [])],
            "defenders": [_as_int(x) for x in (w.get("defenders") or [])]}


async def _all_wars(gw) -> list:
    """Every active war (query_wars all=true, verbose: leaders + participants). A
    character's own war list can miss a war he fights, so wars are found here."""
    return [_war_view(w) for w in _war_rows(await gw.read("query_wars", {"all": True, "verbose": True}))]


def _intent_outcome(results_raw, rid: str) -> Optional[dict]:
    for r in ((results_raw or {}).get("results") or []):
        if isinstance(r, dict) and rid and r.get("intent_id") == rid:
            return r
    return None


def _war_refused(gate: str, summary: str, actor, gw, **kw) -> dict[str, Any]:
    return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                    blocked=why_blocked(gate, summary=summary, **kw), bridge_health=gw.health)


async def _do_war_intent(gw, intent: str, p: dict, me: int, actor: dict) -> dict[str, Any]:
    """make_peace / call_ally / join_war: find the war and the recipient from the
    game's war list, send the one interaction through the bridge's direct path (the
    game's own validity and acceptance checks decide), and confirm by reading the war
    back: for peace, the war-end ledger records its outcome and the war is gone from
    the war list; for allies, the joiner is in the war's participant list."""
    war_id = p.get("war_id")
    if war_id is None:
        return _war_refused("missing_required_argument",
                            f"{intent} needs args.war_id (inspect(subject='wars') lists them)",
                            actor, gw, fix={"rejected_fields": ["war_id"]})
    wars = await _all_wars(gw)
    w = _war_pick(wars, war_id)
    if w is None:
        return _war_refused("war_not_found", f"no active war {war_id}", actor, gw)
    atk, dfn = w["attacker_id"], w["defender_id"]
    warnings: list = []
    terms = None
    expected = None
    if intent == "join_war":
        leader = _as_int(p.get("target_id"))
        if not leader:
            # the war leader you are allied to
            al = await gw.read("query_alliances", {"character_id": me})
            al = al if isinstance(al, dict) else {}
            allies = {_as_int(x.get("ally_id")) for x in (al.get("alliances") or [])
                      + (al.get("forced_allies") or []) if isinstance(x, dict)}
            leader = next((x for x in (atk, dfn) if x in allies), 0)
        if not leader:
            return _war_refused("missing_required_argument",
                                "join_war needs args.leader_id: neither leader of that war is your ally",
                                actor, gw, fix={"rejected_fields": ["leader_id"]},
                                measured={"attacker_id": atk, "defender_id": dfn})
        if leader not in (atk, dfn):
            return _war_refused("not_war_leader", f"{leader} does not lead a side of war {war_id}",
                                actor, gw, measured={"attacker_id": atk, "defender_id": dfn})
        side = "attacker" if leader == atk else "defender"
        recipient = leader
        interaction = "join_war_interaction"
    else:
        if me not in (atk, dfn):
            fights = me in w["attackers"] or me in w["defenders"]
            return _war_refused(
                "not_war_leader" if fights else "war_not_found",
                (f"{me} fights in war {war_id} but does not lead a side; " if fights
                 else f"{me} is not in war {war_id}; ")
                + f"its leaders are {atk} (attacker) and {dfn} (defender)",
                actor, gw, measured={"attacker_id": atk, "defender_id": dfn})
        side = "attacker" if me == atk else "defender"
        if intent == "make_peace":
            terms = str(p.get("terms") or "")
            if terms not in PEACE_TERMS:
                return _war_refused("invalid_argument", f"args.terms must be one of {sorted(PEACE_TERMS)}",
                                    actor, gw, fix={"rejected_fields": ["terms"]})
            recipient = dfn if side == "attacker" else atk
            interaction = PEACE_TERMS[terms][side]
            expected = PEACE_OUTCOME[interaction]
            if terms == "surrender":
                # L18-C: 6 of 12 budget-pilot seats surrendered right after a refused white
                # peace believing it free ("surrendering costs me nothing", Eung-ryeom e2/s4
                # 0869.05.30); each paid 15-350 prestige and up to 590 gold. The engine's
                # CB penalty is script (on_defeat effects), not a value the bridge reads, so
                # the reply says what a surrender is instead of a number.
                warnings.append({"code": "surrender_is_your_defeat", "is_known_limitation": False,
                                 "detail": ("a surrender ends the war as your defeat: the game charges "
                                            "the loser's penalty of the war's casus belli (prestige, "
                                            "often gold; the war's goal is lost). The bridge cannot read "
                                            "the amount before the send; result.before/after show what "
                                            "it took. A stalled war that costs nothing is cheaper "
                                            "left open.")})
            same_pair = [x for x in wars if {x["attacker_id"], x["defender_id"]} == {atk, dfn}]
            if len(same_pair) > 1:
                warnings.append({"code": "several_wars_with_this_enemy", "is_known_limitation": True,
                                 "detail": (f"you fight {len(same_pair)} wars against {recipient}; the game "
                                            "ends the one it finds first between you, which may not be "
                                            f"war {war_id}: the reply's war_ended says which")})
        else:  # call_ally
            recipient = _as_int(p.get("target_id"))
            if not recipient:
                return _war_refused("missing_required_argument",
                                    "call_ally needs args.ally_id (inspect(subject='diplomacy') lists your allies)",
                                    actor, gw, fix={"rejected_fields": ["ally_id"]})
            interaction = "call_ally_interaction"

    params = {"interaction_type": interaction, "actor_id": me, "target_id": recipient}
    if intent in ("call_ally", "join_war"):
        params["war_id"] = war_id

    def _purse(ch) -> dict:
        sect = _char_section(ch)
        return {"gold": sect.get("gold"), "prestige": sect.get("prestige")}

    before = _purse(await gw.read("query_character", {"character_id": me})) \
        if intent == "make_peace" else None

    # call_ally / join_war: the joiner shows on the leader's side of the war.
    joiner = recipient if intent == "call_ally" else me
    key = "attackers" if side == "attacker" else "defenders"
    seen = None

    async def _readback(raw):
        # The intent's own verdict first (the drain ran it: confirm_write's stage
        # 1), then the world it changes: peace ends the war (the war-end ledger),
        # a call/join puts the joiner on the leader's side. When the drain ran
        # and the world has not moved yet, the outcome needs game days (the AI's
        # reply): confirm_write's stage 2.
        nonlocal seen
        if raw.get("success") is False:
            return {"refused": str(raw.get("reason") or raw.get("error") or "refused")}
        rid = intent_id_of(raw)
        res = _intent_outcome(await _read_dict(gw, "query_intent_results", {"max_count": 64}), rid)
        if res and res.get("status") in ("rejected", "crashed"):
            return {"refused": str(res.get("rejection_reason") or res.get("status")), "intent": res}
        if intent == "make_peace":
            hist = await _read_dict(gw, "query_war_history", {"war_id": war_id})
            ended = (hist.get("wars") or [None])[0]
            return {"ended": ended, "intent": res} if isinstance(ended, dict) else None
        seen = _war_pick(await _all_wars(gw), war_id)
        if seen and joiner in seen.get(key, []):
            return {"joined": seen, "intent": res}
        return None

    shows = lever_of(intent, sub=terms or "")[1]["shows"]
    # An answer that shows on the dispatch pass needs no day: no world poll.
    conf = await gw.confirmed_write("send_interaction", params, readback=_readback, deadline_s=12.0,
                                    max_polls=0 if shows == "on_execute" else 8)
    raw = conf.get("raw") or {}
    got = conf.get("resolved")
    if conf.get("refused") and got is None:
        got = {"refused": conf["refused"], "summary": refusal_summary(conf),
               "intent": (conf.get("drain") or {}).get("row")}
    if got is None and conf.get("sent") and shows == "on_execute" \
            and conf.get("drains_when") != "readback":
        # The drain ran it and its effect is absent after the pass: the
        # recipient said no when the game ran the interaction.
        got = {"refused": "recipient_declined_on_execute", "declined": True,
               "intent": (conf.get("drain") or {}).get("row")}
    base = {"intent": intent, "war_id": war_id, "war_name": w.get("name"),
            "interaction": interaction, "side": side, "recipient_id": recipient}
    if terms:
        base["terms"] = terms

    if isinstance(got, dict) and got.get("refused"):
        reason = got["refused"]
        blocked = write_blocked(reason.split(";", 1)[0].strip(), summary=got.get("summary") or reason,
                                conf=conf if conf.get("refused") else None)
        words = blocked.get("detail")
        if got.get("declined"):
            blocked = write_blocked(gate=
                "recipient_refuses", raw_reason=reason,
                summary=(f"{recipient} declined: the game ran {interaction} and its effect is "
                         "absent after that pass (the game gives no reason text)"))
        if intent == "make_peace":
            # A refused send ends nothing; a war the ledger shows ended was ended by
            # the game between the war-list read and the send (live: m-l12-gl5-r1,
            # war 33554432 ended defender_victory in the tick the enforce was queued,
            # and the send was refused for want of a casus belli). Say that.
            hist = await gw.read("query_war_history", {"war_id": war_id})
            gone = ((hist or {}).get("wars") or [None])[0] if isinstance(hist, dict) else None
            if isinstance(gone, dict):
                blocked = write_blocked(gate=
                    "war_not_found", raw_reason=reason,
                    summary=(f"war {war_id} had already ended ({gone.get('outcome')}, "
                             f"{gone.get('end_date')}) when the peace was sent; "
                             f"the game refused the send: {reason}"))
                if words is not None:
                    blocked["detail"] = words
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        result=dict(base, raw=raw, intent_result=got.get("intent")),
                        blocked=blocked, warnings=warnings, bridge_health=gw.health)

    if intent == "make_peace":
        ended = got.get("ended") if isinstance(got, dict) else None
        if ended is None and _war_pick(await _all_wars(gw), war_id) is None:
            # gone from the war list after the poll window: the ledger has it by now
            hist = await gw.read("query_war_history", {"war_id": war_id})
            ended = ((hist or {}).get("wars") or [None])[0] if isinstance(hist, dict) else None
        if not isinstance(ended, dict):
            return envelope("do", verdict="pending", actor=actor, game=gw.game(),
                            pending=conf_pending(
                                conf, expected_change=f"war {war_id} ends ({expected})",
                                verify_with="inspect(subject='wars'); inspect(subject='pending')"),
                            result=dict(base, raw=raw, sent=conf.get("sent")),
                            warnings=warnings, bridge_health=gw.health)
        after = _purse(await gw.read("query_character", {"character_id": me}))
        outcome = ended.get("outcome")
        if outcome != expected:
            warnings.append({"code": "war_ended_otherwise", "is_known_limitation": False,
                             "detail": f"the war ended as {outcome}, not {expected}"})
        return envelope("do", verdict="applied", actor=actor, game=gw.game(),
                        result=dict(base, war_ended={"war_id": ended.get("war_id"),
                                                     "name": ended.get("war_name"),
                                                     "outcome": outcome,
                                                     "end_date": ended.get("end_date"),
                                                     "target_titles": ended.get("target_titles")},
                                    expected_outcome=expected, before=before, after=after),
                        warnings=warnings, bridge_health=gw.health)

    joined = got.get("joined") if isinstance(got, dict) else None
    if joined is not None:
        return envelope("do", verdict="applied", actor=actor, game=gw.game(),
                        result=dict(base, joined_id=joiner, participants=joined),
                        warnings=warnings, bridge_health=gw.health)
    return envelope("do", verdict="pending", actor=actor, game=gw.game(),
                    pending=conf_pending(
                        conf, expected_change=f"{joiner} among war {war_id}'s {key}",
                        verify_with="query(domain='wars', verbose=true); inspect(subject='pending')"),
                    result=dict(base, raw=raw, participants=seen, sent=conf.get("sent")),
                    warnings=warnings, bridge_health=gw.health)


async def do_impl(gw, intent: str = None, action_class: str = None,
                  candidates: list = None, character_id: int = 0,
                  args: dict = None) -> dict[str, Any]:
    actor, err = await _resolve_actor(gw, character_id, "do",
                                      war_id=args.get("war_id") if isinstance(args, dict) else None)
    if err:
        return err

    # Enumerate mode.
    if not intent:
        if not action_class:
            return envelope("do", ok=False, actor=actor, game=gw.game(),
                            blocked=why_blocked("missing_required_argument", summary="enumerate mode needs action_class; execute mode needs intent"),
                            bridge_health=gw.health)
        return await _do_enumerate(gw, action_class, character_id, candidates or [], actor, args)

    # Execute mode.
    if intent == _CANCEL_PENDING_INTENT:
        return await _do_cancel_pending(gw, dict(args or {}), actor)

    if intent == _FACTION_INTENT:
        return await _factions.do_faction(gw, dict(args or {}), character_id, actor)

    if intent not in INTENT_TO_ACT:
        return envelope("do", ok=False, actor=actor, game=gw.game(),
                        # ck-ycim: and the mirror of it. `set_character_ai` was
                        # asked as an intent 14 times; it is a REAL BRIDGE ACTION
                        # that no MCP tool wraps, and the generated
                        # knowledge/tool-reference.md listed it under "AI
                        # Subsystem Control" marked `_raw_`. An agent reading a
                        # document called Tool Reference reasonably read a listed
                        # action as callable. The reference now says unwrapped
                        # means NOT CALLABLE, and this message says so too.
                        blocked=why_blocked(
                            "unknown_verb",
                            summary=(
                                f"unknown intent '{intent}'. "
                                + (f"'{intent}' is a class of things to LIST — pass it as "
                                   "action_class= (enumerate mode) instead of intent=. "
                                   if intent in ACTION_CLASSES else "")
                                + "A bridge action that no tool wraps is NOT callable by an "
                                  "agent. "
                                + f"Valid intents: {', '.join(INTENTS)}"),
                            raw_reason=f"unknown_intent:{intent}"),
                        bridge_health=gw.health)
    a = dict(args or {})
    warnings: list = []

    # Lane l4: the kin guard (KIN_GUARDED_INTERACTIONS) — disinherit only down the
    # actor's own line. Checked here, before the bridge sees anything.
    if intent == "interact":
        itype = (a.get("interaction") or a.get("interaction_type") or "").lower()
        _actor_id = character_id or _as_int((actor or {}).get("id"))
        _guard = await _kin_guard(gw, itype, _actor_id, _as_int(a.get("target_id")))
        if _guard is not None:
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=_guard, bridge_health=gw.health)

    # A7: mint_title pre-check via query_dejure_progress (NEVER create_title as a meter).
    # The read routes PAUSED now (mcp_server gateway) → stable held/total (no mid-tick 0/0).
    # Mirror the bridge's FIXED #131 gate (intent_dispatch.cpp, 2026-06-25): only a
    # POSITIVE de-jure tree (total>0) gets the >=51% territorial bar; a total==0 on a
    # RESOLVED title is a TITULAR kingdom (k_danelaw) — no territorial bar — so DEFER to
    # the engine can_create gate instead of pre-rejecting. Only a genuine resolution
    # failure (title_key_not_found) is a hard reject here. (re_dejure_meter_and_mint.md bug #2)
    if intent == "mint_title":
        tkey = a.get("title_key") or a.get("title")
        prog = await gw.read("query_dejure_progress", {"character_id": character_id, "title_key": tkey})
        prog = prog if isinstance(prog, dict) else {}
        if (prog.get("error") == "title_key_not_found"
                or str(prog.get("error") or "").startswith("title_key")):
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=why_blocked("target_not_assignable",
                                                summary=f"title key '{tkey}' did not resolve to a landed_title"),
                            bridge_health=gw.health)
        held, total = _as_int(prog.get("held")), _as_int(prog.get("total"))
        # The #131 bar, as the bridge applies it (mint_dejure_shortfall: tier >= 4
        # only; a titular or unnamed-tier title is left to the bridge's gate).
        short = mint_dejure_shortfall(tkey, held, total)
        if short is not None:
            need = short["need"]
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=why_blocked("de_jure_control",
                                                summary=(f"hold {held}/{need} de-jure counties of {tkey} "
                                                         f"(need >={round(MINT_DEJURE_MIN_FRAC * 100)}%: "
                                                         f"{MINT_DEJURE_RULE})"),
                                                measured={"held": held, "need": need, "total": total,
                                                          "fraction": round(_f(prog.get('fraction')), 3)}),
                            bridge_health=gw.health)
        # L14 S3: the game's verdict and price before anything is sent (mint_title_price).
        _blk, _price = await mint_title_price(gw, character_id, tkey)
        if _blk is not None:
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=concrete_next(_blk, intent), bridge_health=gw.health)

    # #64: take_decision must be bounded (no unbounded annotated eval here — single
    # named decision is fine; it dispatches send_decision, not the eval path).
    # lane-scheme 2026-10-18: the scheme and faction handlers REFUSE actor_id 0
    # (`actor_id_required`, action_handlers.cpp) rather than fall back to the
    # bridge's process-wide controlled primary, which may be another seat's
    # character and which the ownership gate does not check. So a seat that
    # calls do(intent="scheme") with no character_id -- the normal single-ruler
    # call, and the persona test's -- was refused for a missing argument it
    # never had to pass. Measured live once the write gate was armed: six of six
    # do(intent="scheme") calls refused actor_id_required. Send the actor this
    # call already resolved instead. Every other intent keeps actor_id 0 and the
    # bridge's own default, unchanged.
    dispatch_actor = character_id
    if not dispatch_actor and intent in _EXPLICIT_ACTOR_INTENTS:
        dispatch_actor = _as_int((actor or {}).get("id"))
    act_params = _intent_to_act_params(intent, a, dispatch_actor)
    if intent in WAR_INTENTS:
        return await _do_war_intent(gw, intent, act_params,
                                    character_id or _as_int((actor or {}).get("id")), actor)

    # ── ck-ba35: REFUSE A MALFORMED CALL BEFORE IT COSTS A FLUSH ───────────
    #
    # The census found 103 refusals across 15 fields where the BRIDGE told the
    # agent a required argument was absent — `target_required` 50,
    # `maa_type_required` 11, `missing_decision_type` 6 and the rest. Each cost
    # a ticket, a flush, a bridge_action_record and a round trip to learn that
    # a field was missing, and ck-3mz0 found them all coming back
    # `retryable: true`.
    #
    # THE VALIDATOR ALREADY EXISTED AND THIS SURFACE DID NOT USE IT.
    # `action_selector.validate_act_params` has exactly the right shape —
    # `reason`, `rejected_fields`, `hint`, `retry_with`, "stable so the LLM can
    # pattern-match and self-correct without parsing prose" — and
    # mcp_server.py's legacy `act` tool calls it at :2256. The redesigned `do`
    # tool, which is what the arms actually run, never did. So this is not a new
    # table: it is the existing one, reached through `INTENT_TO_ACT`, which
    # already carries each intent's act type. One derivation, no second list to
    # drift.
    #
    # WHAT IT DOES AND DOES NOT CATCH, measured rather than hoped: 8 of the 20
    # intents are refused here; the other 12 are passed through BY THE
    # VALIDATOR'S OWN DESIGN — its `_ACT_TYPES` comment says the write tools are
    # "validated bridge-side (their handlers return explicit reject reasons like
    # position_id_required / maa_type_required), so they pass the pre-flight
    # gate". That covers roughly 16 of the 103; the largest single cause,
    # `target_required` on set_council_task, CANNOT be checked here at all
    # because the bridge decides it from live engine data
    # (TASKTYPE_TARGET_DISC), not from a static field list. Those remain
    # bridge-side refusals, now typed by ck-3mz0 rather than "unknown, retry".
    #
    # AN UNKNOWN act type is NOT treated as a rejection. `set_education_focus`
    # is a real intent whose type the validator does not know, and failing it
    # here would refuse a VALID call — the first draft of this did exactly that.
    # So only an explicit `valid: false` with a reason refuses.
    if intent in INTENT_TO_ACT:
        _verdict = _validate_act_params(act_params)
        if _verdict.get("valid") is False and _verdict.get("reason") != "unknown_type":
            return envelope(
                "do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                blocked=concrete_next(why_blocked(
                    "missing_required_argument",
                    summary=_verdict.get("hint")
                    or f"{_verdict.get('reason')}: {_verdict.get('rejected_fields')}",
                    raw_reason=str(_verdict.get("reason") or ""),
                    fix={"rejected_fields": _verdict.get("rejected_fields") or [],
                         "retry_with": _verdict.get("retry_with") or {}}), intent),
                bridge_health=gw.health)

    try:
        resolved = _tools.resolve_act(act_params)
    except ValueError as e:
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("missing_required_argument", summary=str(e)), bridge_health=gw.health)
    if resolved is None:
        # resolve_act returns None (not raises) for an unmapped act type.
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("unknown_verb",
                                            summary=f"intent '{intent}' did not resolve to a bridge action"),
                        bridge_health=gw.health)
    bridge_action, bridge_params = resolved

    if intent == "scheme":
        return await _do_scheme(gw, bridge_action, bridge_params, actor, warnings)

    # FIX 1 — deferred-write confirmation (#113). Confirmable writes (mint_title)
    # route through _confirm_write: dispatch → readback → applied/rejected, never a bare
    # dispatched_pending surfaced and never a success:false for an object that landed.
    if intent in CONFIRMABLE_WRITES:
        return await _confirm_intent_write(gw, intent, bridge_action, bridge_params,
                                           a, character_id, actor, warnings)

    # experimental gate: scheme/faction → honest gate if not enabled (bridge enforces,
    # but surface a typed code immediately rather than a silent no-op).
    lever_key, lever = lever_of(intent, bridge_params)
    before_fn, landed_fn = LEVER_READBACKS.get(lever_key, (None, None))
    lever_before = await before_fn(gw, bridge_params) if before_fn else {}
    submitted_on = gw.game().get("date")
    raw = await gw.write(bridge_action, bridge_params)
    raw = raw if isinstance(raw, dict) else {}
    reason = raw.get("reason") or raw.get("error") or ""
    status = raw.get("status") or ""

    if reason == "experimental_write_disabled" or status == "experimental_write_disabled":
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=experimental_gate_blocked(raw, bridge_action),
                        bridge_health=gw.health)

    # Stickiness honesty (G1) for council_task — warn it may revert.
    if intent == "council_task":
        warnings.append({"code": "may_revert_ai_reasserts", "is_known_limitation": True,
                         "detail": "council task store is transient; re-verify via inspect(subject='council')"})

    # ── ck-zgh4 deploy-51 item (3): NEVER "applied" FOR A QUEUED ACTION ─────
    #
    # THE DEFECT, and the bridge was the honest party. `do_send_interaction`
    # replies, on its enqueue path (action_handlers.cpp:531-535):
    #
    #     success: true   status: "queued"   method: "intent_dispatch"
    #
    # which says exactly what happened: accepted INTO THE QUEUE, not executed.
    # This tail then checked `status in ("dispatched_pending",
    # "sent_no_confirmation")` — "queued" is in neither — so `success: true`
    # fell through to `verdict="applied"`. The designer measured the
    # consequence: 449 deferred REJECTIONS against 203 successes, arriving
    # after the seat had been told the action applied.
    #
    # TWO FIXES, both evidenced:
    #
    # 1. THE PENDING SET WAS INCOMPLETE. "queued" (the intent-dispatch path) and
    #    "pending" (action_handlers.cpp:375) are both the bridge saying "later",
    #    and both read as applied. They are named here now.
    #
    # 2. `raw.get("success", True)` DEFAULTED TO TRUE. A reply carrying no
    #    `success` key at all was a success — the missing state falling into the
    #    good bucket, which is a claim and not a reading. An absent `success` is
    #    now UNCONFIRMED, and unconfirmed is pending, not applied.
    #
    # L13 amendment 3 and the FF2/FF3 rulings: "later" is the NEXT PASS, and
    # applied needs an effect read back. The bridge drains a queued intent on the
    # session thread's next pass, paused or running; settle_write reads its
    # verdict back, then the lever's readback (LEVERS, LEVER_READBACKS): visible
    # after the pass is applied in either clock state; a refusal is rejected
    # with the game's reason; an "on_execute" effect still absent after the pass
    # is rejected as the row says (declined / unconfirmed); a "days" lever, or
    # one with no readback, that ran is pending until the clock advances, with
    # result.sent; a verdict not read in the drain budget is pending next_pass.
    _success_declared = "success" in raw
    success = bool(raw.get("success")) and not reason
    if status in PENDING_STATUSES or (not _success_declared and not reason):
        readback = ((lambda _raw: landed_fn(gw, bridge_params, lever_before))
                    if landed_fn else None)
        on_exec = lever["shows"] == "on_execute"
        conf = await settle_write(None if (on_exec or readback is None) else gw.clock,
                                  gw.read, raw, readback, game_date_at_submit=submitted_on,
                                  max_polls=0 if on_exec else 8)
        drain = conf["drain"]
        res_base = {"intent": intent, "raw": raw, "sent": conf["sent"], "drain": drain["row"],
                    "lever": {"shows": lever["shows"], "readback": lever.get("readback")}}
        if conf.get("resolved") is not None:
            # One polarity (L14 S3): the readback is the verdict, so the bridge's
            # interim reply (written before the drain ran: queued, pending, or
            # success:false + dispatched_pending) is not carried under "applied".
            return envelope("do", verdict="applied", actor=actor, game=gw.game(),
                            result=dict(applied_result(res_base), readback=conf["resolved"]),
                            warnings=warnings, bridge_health=gw.health)
        if conf.get("refused"):
            why = conf["refused"]
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            result=res_base,
                            blocked=write_blocked(why, conf=conf, raw=raw, intent=intent),
                            warnings=warnings, bridge_health=gw.health)
        what = queued_expected_change(intent, bridge_params)
        if conf["drains_when"] == "readback":
            # It ran, and the reading that would show it was never made: pending,
            # ok:true, never rejected (FF5 ruling).
            return envelope("do", verdict="pending", actor=actor, game=gw.game(),
                            pending=conf_pending(
                                conf,
                                expected_change=what,
                                raw_reason="write_unconfirmed",
                                why_pending=(f"the drain ran it (result.sent) and "
                                             f"{lever.get('readback') or 'its effect'} could not be "
                                             "read within the drain budget"),
                                verify_with=(f"read {lever.get('readback') or 'the effect'} before "
                                             "sending it again")),
                            result=res_base, warnings=warnings, bridge_health=gw.health)
        if conf["sent"] and on_exec and readback is not None:
            blk = lever_absent_block(lever, what)
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            result=res_base, blocked=blk, warnings=warnings,
                            bridge_health=gw.health)
        if conf["drains_when"] == "clock_advances":
            why_pending = ("the drain ran it (the write was sent); "
                           + ("its readback does not show it yet; it lands as game days pass"
                              if readback is not None else
                              "the server reads no effect of it back") + f": {what}")
        else:
            # B* deferred (scheme/faction/decision), the intent-dispatch queue, and
            # any reply that never said whether it worked, with no verdict read.
            why_pending = ((f"the bridge reported status={status!r}" if status else
                            "the bridge reply carried no `success` field, so "
                            "whether this landed is unknown")
                           + (f"; the drain's verdict reads {drain['status']!r}"
                              if drain["intent_id"] else ""))
        return envelope("do", verdict="pending", actor=actor, game=gw.game(),
                        pending=conf_pending(
                            conf,
                            expected_change=what, why_pending=why_pending,
                            verify_with="inspect(subject='pending') has the drain's record; "
                                        "read the effect where expected_change says"),
                        result=res_base, warnings=warnings, bridge_health=gw.health)
    if not success:
        _summary = reason or status
        _result = None
        if intent == "council_task":
            # lane-query item 1: the bridge's refusal names the seat it used or
            # the seat the task belongs to; surface that instead of the bare code.
            _summary = raw.get("warning") or _summary
            _result = {"intent": intent, "raw": raw}
        elif intent == "build":
            _summary = _build_refusal_summary(raw) or _summary
            _result = {"intent": intent, "raw": raw}
        elif intent == "set_obligation":
            if raw.get("reason") == "raise_requires_negotiation":
                _summary = (f"a raise ({raw.get('old_level')} -> {raw.get('requested_level')}) needs the "
                            "vassal's consent and costs opinion in the game; only lowering is done here")
            elif raw.get("available_obligations"):
                _summary = f"{reason}: this contract has {raw.get('available_obligations')}"
            _result = {"intent": intent, "raw": raw}
        elif intent == "change_law" and raw.get("engine_reason"):
            # Lane l4: a title law the engine refuses carries the engine's own text
            # (the title-law window's tooltip); that is the player's answer.
            _summary = f"{reason}: {engine_text(raw.get('engine_reason')) or NO_GAME_DETAIL}"
            _result = {"intent": intent, "raw": raw}
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        result=_result,
                        blocked=write_blocked(reason or status, summary=_summary, raw=raw,
                                              intent=intent),
                        warnings=warnings, bridge_health=gw.health)
    return envelope("do", verdict="applied", result={"intent": intent, "raw": raw},
                    actor=actor, game=gw.game(), warnings=warnings, bridge_health=gw.health)


# --------------------------------------------------------------------------- inspect
# subject → (bridge_action, param_builder). Routes to the existing TIER-2 reads.
_INSPECT_SUBJECTS: dict[str, Any] = {
    "character":  ("query_character",          lambda p: {"character_id": p["cid"]}),
    "realm":      ("query_realm",              lambda p: {"character_id": p["cid"]}),
    "council":    ("query_council",            lambda p: {"character_id": p["cid"]}),
    "succession": ("query_succession",         lambda p: {"character_id": p["cid"]}),
    # (succession_preview was deleted in L14, finding 8: succession_plan carries the
    # verdict and the per-title map.)
    "claims":     ("query_claims",             lambda p: {"character_id": p["cid"]}),
    "wars":       ("query_wars",               lambda p: {"character_id": p["cid"]}),
    "armies":     ("query_armies",             lambda p: {"character_id": p["cid"]}),
    "vassals":    ("query_vassal_loyalty",     lambda p: {"character_id": p["cid"]}),
    "diplomacy":  ("query_alliances",          lambda p: {"character_id": p["cid"]}),
    "title":      ("query_dejure_progress",    lambda p: {"character_id": p["cid"], "title_key": p["title_key"], "verbose": 1}),
    "dynasty":    ("query_dynasty",            lambda p: {"character_id": p["cid"], "dynasty_id": p["dynasty_id"]}),
    "laws":       ("query_realm_laws",         lambda p: {"character_id": p["cid"]}),
    "schemes":    ("query_schemes",            lambda p: {"character_id": p["cid"]}),
    "marriage":   ("query_marriage_candidates", lambda p: {"character_id": p["cid"]}),
    "economy":    ("query_realm",              lambda p: {"character_id": p["cid"], "verbose": True}),
    "research":   ("query_research_speed",     lambda p: {"character_id": p["cid"]}),
    "county":     ("query_county_details",     lambda p: {"character_id": p["cid"]}),
    # Phase-2 PORT (_PHASE2_PARITY.md): SPEC §3 inspect subjects the code omitted — the
    # legibility long-tail that retiring `query` would otherwise silently drop. All route
    # to existing bridge actions.
    "opinion":            ("query_opinion",            lambda p: {"character_id": p["cid"], "target_id": p["target_id"]}),
    "opinion_breakdown":  ("query_opinion_breakdown",  lambda p: {"character_id": p["cid"], "target_id": p["target_id"]}),
    "relations":          ("query_relations",          lambda p: {"character_id": p["cid"]}),
    "hooks":              ("query_hooks",               lambda p: {"character_id": p["cid"]}),
    "title_hierarchy":    ("query_title_hierarchy",    lambda p: {"character_id": p["cid"], "title_id": p["title_id"]}),
    "title_succession":   ("query_title_succession",   lambda p: {"character_id": p["cid"], "title_id": p["title_id"]}),
    "claimants":          ("query_title_claimants",    lambda p: {"character_id": p["cid"], "title_id": p["title_id"]}),
    "war_score":          ("query_war_score_breakdown", lambda p: {"character_id": p["cid"], "war_id": p["war_id"]}),
    "call_to_arms":       ("query_call_to_arms",        lambda p: {"character_id": p["cid"]}),
    "development":        ("query_development_growth",  lambda p: {"character_id": p["cid"], "title_id": p["title_id"]}),
    "incoming_schemes":   ("query_incoming_schemes",    lambda p: {"character_id": p["cid"]}),
    "lifestyle":          ("query_lifestyle",           lambda p: {"character_id": p["cid"]}),
    # §1a: per-child/heir education-focus read (education_focus real; guardian null §1b).
    "heir_education":     ("query_heir_education",      lambda p: {"character_id": p["cid"]}),
}


#: inspect subjects composed from several reads rather than one bridge action.
_INSPECT_COMPOSED = ("war_stance", "heir_plan", "guardian_candidates", "pending", "factions")

#: Every subject inspect answers: the `subject` enum the tool advertises.
INSPECT_SUBJECTS: list = sorted(set(_INSPECT_SUBJECTS) | set(_INSPECT_COMPOSED))


#: The most drained verdicts inspect(subject="pending") lists. The bridge keeps
#: more (intent_queue.h MAX_COMPLETED, 128; the protocol and the inspect
#: docstring state both numbers, checked by test_pending_ring_doc.py).
PENDING_LIST_MAX = 64


def _pending_row(row):
    """One drained verdict as the player reads it. A row's `detail` is the engine's
    own tooltip text (which condition failed), cleaned to words; the bridge's markup
    stays under raw. A rejected row always says why: the game's reason, or
    NO_GAME_DETAIL when the bridge sent no detail or the detail cleans to nothing.
    Any other row keeps its detail as cleaned words, NO_GAME_DETAIL only when a
    detail is present and cleans to nothing, and gains none when it had none. A
    rejected row the bridge stored without words says the words the write's own
    reply carried (_REPLY_REFUSAL_WORDS, L14 finding 1: set_title_law)."""
    if not isinstance(row, dict):
        return row
    present = isinstance(row.get("detail"), str) and bool(row.get("detail"))
    if not present and row.get("status") == "rejected":
        # the write's own reply said it and the bridge stored the row without it
        said = _REPLY_REFUSAL_WORDS.get(str(row.get("intent_id") or row.get("request_id") or ""))
        if said:
            return {**row, "detail": said}
    present = isinstance(row.get("detail"), str)
    if present or row.get("status") == "rejected":
        words = engine_text(row["detail"]) if present else ""
        return {**row, "detail": words or NO_GAME_DETAIL}
    return row


async def _inspect_pending(gw, max_results: int) -> dict[str, Any]:
    """The bridge's intent queue: what is still pending and what drained, with the
    per-request verdict (success | rejected | crashed, and the rejection reason).
    Writes only: a check the bridge runs on the game thread (discover's
    scheme_targets, an interaction check) is never listed. Match a do reply's
    pending.intent_id against a completed row's intent_id."""
    queued = await gw.read("query_intents", {})
    done = await gw.read("query_intent_results",
                         {"max_count": max(1, min(int(max_results or 16), PENDING_LIST_MAX))})
    errors = []
    for name, res in (("query_intents", queued), ("query_intent_results", done)):
        if not isinstance(res, dict) or not res:
            errors.append({"code": "read_timeout", "detail": f"{name} returned no reply"})
    queued = queued if isinstance(queued, dict) else {}
    done = done if isinstance(done, dict) else {}
    completed = [_pending_row(row) for row in done.get("results", done.get("completed", [])) or []]
    return envelope("inspect", result={
        "subject": "pending",
        "queued": queued.get("pending", []),
        "completed": completed,
        "raw": {"query_intents": queued, "query_intent_results": done},
    }, errors=errors, game=gw.game(), bridge_health=gw.health)


async def inspect_impl(gw, subject: str, character_id: int = 0, target_id: int = 0,
                       title_id: int = 0, war_id: int = 0, title_key: str = "",
                       dynasty_id: int = 0,
                       max_results: int = 20) -> dict[str, Any]:
    if not subject:
        return envelope("inspect", ok=False, game=gw.game(),
                        blocked=why_blocked("missing_required_argument",
                                            summary=f"subject is required; valid: {', '.join(INSPECT_SUBJECTS)}"),
                        bridge_health=gw.health)
    if subject not in INSPECT_SUBJECTS:
        valid = INSPECT_SUBJECTS
        return envelope("inspect", ok=False, game=gw.game(),
                        blocked=why_blocked("unknown_subject",
                                            summary=f"unknown subject '{subject}'; valid: {', '.join(valid)}"),
                        bridge_health=gw.health)
    # rc2 B6: every subject reads "the ruler" when character_id is omitted. With
    # none bound, the reads decline and came back ok=true as empty readings
    # (war_score's declined reply, war_stance's wars=[]). heir_plan and
    # guardian_candidates run this check themselves through _resolve_actor.
    if not character_id and subject not in ("heir_plan", "guardian_candidates", "pending"):
        unbound = unbound_actor_block(await gw.read("query_character", {"character_id": 0}))
        if unbound is not None:
            return envelope("inspect", ok=False, result={"subject": subject}, blocked=unbound,
                            game=gw.game(), bridge_health=gw.health)
    if subject == "pending":
        return await _inspect_pending(gw, max_results)
    # factions (F16): the liege's seat — members, threat, demand state, the standing
    # answer to a demand, and the levers against each leader.
    if subject == "factions":
        return await _factions.inspect_factions(gw, character_id)
    # war_stance (item 3): a SYNTHESIZED view (no single bridge action) — per-active-war
    # side/stance/occupation + a stall verdict + the white-peace exit lever, so the agent
    # SEES stuck offensive wars and abandons them. Special-cased before the action map.
    if subject == "war_stance":
        view = await _war_stance_view(gw, character_id)
        warns = []
        if view.get("any_stalled"):
            warns.append({"code": "war_stalled", "is_known_limitation": False,
                          "detail": f"{len(view['stalled'])} attacker war(s) stuck at occupation 0 / losing — "
                                    "abandon via the per-war exit_lever (white-peace) before they bleed your army "
                                    "across a succession"})
        return envelope("inspect", result={"subject": "war_stance", **view},
                        warnings=warns, game=gw.game(), bridge_health=gw.health)
    # heir_plan / guardian_candidates: multi-read compositions (per-child query_character),
    # not a single bridge action — special-cased before the action map, like war_stance.
    if subject == "heir_plan":
        env = await heir_plan_impl(gw, character_id=character_id, max_results=max_results)
        if isinstance(env, dict) and isinstance(env.get("result"), dict):
            env["result"] = {"subject": "heir_plan", **env["result"]}
            env["tool"] = "inspect"
        return env
    if subject == "guardian_candidates":
        env = await good_guardian_candidates_impl(gw, character_id=character_id,
                                                  max_results=max_results)
        if isinstance(env, dict) and isinstance(env.get("result"), dict):
            env["result"] = {"subject": "guardian_candidates", **env["result"]}
            env["tool"] = "inspect"
        return env
    action, builder = _INSPECT_SUBJECTS[subject]
    # ck-zgh4 deploy-51: this DEFAULTED the de-jure subject to the previous
    # project's goal title, so a seat that asked "de jure of what?" without
    # naming one was answered about e_britannia. No default: the caller names
    # the title, and an unnamed one is the bridge's own problem to refuse.
    p = {"cid": character_id, "title_key": title_key or "",
         "dynasty_id": dynasty_id, "target_id": target_id, "title_id": title_id,
         "war_id": war_id}
    res = await gw.read(action, builder(p))
    res = res if isinstance(res, dict) else {"value": res}

    # L18-B: war_score without war_id. query_war_score_breakdown answers every war
    # in the world (`count`, `breakdowns`): 21 at 0867.06, 394 by 0873 — 54-119K
    # characters, over the CLI's cap, and every seat that got one then jq'd out its
    # own war_id. Without war_id the subject is YOUR wars (get_active_wars of the
    # actor); a war_id still reads that one war, whoever fights it.
    if subject == "war_score" and not _as_int(war_id) and isinstance(res.get("breakdowns"), list):
        mine = await gw.read("get_active_wars", {"character_id": character_id})
        mine = mine if isinstance(mine, dict) else {}
        my_ids = {_as_int(w.get("war_id")) for w in (mine.get("wars") or []) if isinstance(w, dict)}
        rows = [b for b in res["breakdowns"] if isinstance(b, dict) and _as_int(b.get("war_id")) in my_ids]
        res = dict(res, breakdowns=rows, count=len(rows),
                   scope=("your wars (get_active_wars); pass war_id=<id> for any one war "
                          f"of the {_as_int(res.get('count'))} the game holds"))

    # title subject → summary + per-county holders (O2 verbose), reshaped.
    if subject == "title":
        counties = res.get("counties") or []
        res = {"title_key": res.get("title_key"), "title_id": res.get("title_id"),
               "summary": {"held": res.get("held"), "total": res.get("total"),
                           "fraction": res.get("fraction")},
               "counties": [{"county": c.get("county_id"), "holder_id": c.get("holder_id"),
                             "you_control": c.get("you_control")} for c in counties]}
        if not counties:
            res["counties_note"] = "per-county list unavailable (O2 fallback) — summary only"

    warns: list = []
    return envelope("inspect", result={"subject": subject, **res}, game=gw.game(),
                    warnings=warns or None, bridge_health=gw.health)


# --------------------------------------------------------------------------- experimental flags
# ★ EXPERIMENTAL-UNVALIDATED runtime flags (default-OFF on the bridge). These arm RISKY
# engine hooks/writes that are SHIPPED INERT behind a default-false atomic and must NOT
# be relied on until a live disposable-save smoke passes (re/findings FINAL specs below).
# The agent surface here only ARMS the flag; it does NOT itself perform the gated write.
# Each maps a friendly name → the bridge IPC setter the corresponding FINAL spec defines:
#
#   off_goal_veto   → set_offgoal_veto {enabled}      (re_offgoal_veto_FINAL.md §5/§6)
#       Per-character routine-war veto: the controlled British ruler stops squandering its
#       army on AI-chosen OFF-GOAL wars (steered bypass=1 wars are never affected). Default
#       OFF; one live tick (no-crash + per-char scoping + steered-bypass-intact) gates trust.
#
# Honest-refusal contract: a bridge build may lack these setters (the hooks are FINAL-spec
# and DISABLED-by-default; the IPC setter ships with its hook). If the bridge
# returns unknown_action / unknown_command, we surface a typed `experimental_flag_unavailable`
# blocked verdict — never a silent ok:true. We NEVER claim the flag is validated.
_EXPERIMENTAL_FLAGS: dict[str, dict[str, str]] = {
    "off_goal_veto": {
        "setter": "set_offgoal_veto",
        "spec": "re_offgoal_veto_FINAL.md",
        "summary": "off-goal war veto: AI-chosen wars are suppressed for every controlled character (steered bypass=1 unaffected). Armed only by session(action=\"set_experimental\", flag=\"off_goal_veto\", enabled=true).",
    },
    # REMOVED (cleanup 2026.11, owner ruling): force_offensive_stance. The engine's own
    # coordinator already prosecutes a steered war offensively (live867 l5fx6-p2-live);
    # the game's AI prosecutes a declared war on its own (re_steered_war_offensive_root.md §9).
    # REMOVED: move_army experimental flag deleted per design (agent does not drive army micro).
}

#: rc2 B8: what the veto actually covers (offgoal_war_veto_hook.cpp): one
#: process-wide flag; the setter ignores character_id.
OFFGOAL_VETO_SCOPE = ("process-wide: one bridge flag covering every controlled character "
                      "of every session; character_id is ignored; it stays on until "
                      "set_experimental disarms it or the game exits")

#: rc2 B8: the bridge has no readback for these flags, so the only live
#: evidence this server has is what IT wrote. Keyed by flag name.
_FLAG_WRITES: dict[str, dict[str, Any]] = {}


def _note_flag_write(flag: str, enabled: bool, by: str) -> None:
    _FLAG_WRITES[flag] = {"enabled": bool(enabled), "by": by}


def flag_write_confirmed(reply, enabled: bool) -> bool:
    """rc2 B8: True only when the bridge says the flag write landed.

    The setters reply {success: true, enabled: <bool>}. Anything else is not a
    confirmation: {success: false, reason: ...} (ownership refusal),
    {reason: bridge_disconnected}, {status: sent_no_confirmation}, {} or None.
    An echoed `enabled` that differs from the request is a refusal too.
    """
    if not isinstance(reply, dict) or reply.get("success") is not True:
        return False
    if "enabled" in reply and bool(reply.get("enabled")) is not bool(enabled):
        return False
    return True

# Bridge tokens that mean "this build does not know that command" — surface as a typed
# experimental_flag_unavailable rather than a false success or an opaque error.
_UNKNOWN_CMD_TOKENS = ("unknown_action", "unknown_command", "unknown_type",
                       "unsupported", "not_found", "no_such_command")


async def _set_experimental_flag(gw, flag: str, enabled: bool) -> dict[str, Any]:
    """Arm/disarm ONE default-OFF experimental bridge flag. Clearly EXPERIMENTAL-unvalidated.
    Returns a typed envelope; honest experimental_flag_unavailable if the bridge lacks the
    setter (the hook+setter may not be landed yet — agent surface is ready ahead of it)."""
    meta = _EXPERIMENTAL_FLAGS.get(flag)
    if meta is None:
        return envelope("session", ok=False, game=gw.game(),
                        blocked=why_blocked(
                            "invalid_argument",
                            summary=f"unknown experimental flag '{flag}'; valid: {', '.join(sorted(_EXPERIMENTAL_FLAGS))}"),
                        bridge_health=gw.health)
    raw = await gw.write(meta["setter"], {"enabled": bool(enabled)})
    raw = raw if isinstance(raw, dict) else {}
    err = str(raw.get("error") or raw.get("reason") or raw.get("status") or "").lower()
    # Bridge does not know this setter yet → honest unavailable, never a false ok.
    if any(tok in err for tok in _UNKNOWN_CMD_TOKENS) and not raw.get("success"):
        return envelope("session", ok=False, game=gw.game(),
                        blocked=write_blocked(gate=
                            "experimental_write_disabled",
                            summary=(f"experimental flag '{flag}' is not available on this bridge "
                                     f"(setter '{meta['setter']}' not served — the hook ships DISABLED "
                                     f"and the IPC setter lands with it; see {meta['spec']})"),
                            raw_reason=err or "unknown_action"),
                        bridge_health=gw.health)
    # rc2 B8: this read a missing `enabled` as 0, so any reply to a DISARM,
    # a disconnect included, counted as applied.
    applied = flag_write_confirmed(raw, enabled)
    if applied:
        _note_flag_write(flag, enabled, "set_experimental")
    elif reply_refusal(raw):
        _why = reply_refusal(raw)
        return envelope("session", ok=False, verdict="rejected", game=gw.game(),
                        result={"experimental_flag": flag, "enabled": bool(enabled), "raw": raw},
                        blocked=write_blocked(_why, summary=_why or "the bridge refused the flag write"),
                        bridge_health=gw.health)
    return envelope(
        "session",
        verdict="applied" if applied else "pending",
        result={
            "experimental_flag": flag,
            "setter": meta["setter"],
            "enabled": bool(enabled),
            "status": "EXPERIMENTAL-unvalidated",
            "validation": "unvalidated: no live run has shown what this flag does; use it on a disposable save only",
            "spec": meta["spec"],
            "summary": meta["summary"],
            "raw": raw,
        },
        warnings=[
            f"EXPERIMENTAL-unvalidated: '{flag}' arms a default-OFF engine hook/write that has "
            f"NOT passed live validation ({meta['spec']}). Use only for validation on a "
            f"disposable observer save — never rely on its behaviour on the live campaign."
        ],
        game=gw.game(), bridge_health=gw.health)


# --------------------------------------------------------------------------- session
#: Every reason `do_set_controlled_character` can refuse with, enumerated from
#: deploy-27's source rather than grepped for: `ownership_divergence` is not a
#: literal but a ternary at dispatch_router.cpp:807-808, so a literal grep does
#: not find it and a count does not catch its absence. Nine distinct, with
#: `observe_mode` reachable from BOTH the handler and the registry under one
#: spelling — so nothing may depend on which gate refused.
CONTROL_REFUSAL_REASONS = frozenset((
    "no_session", "invalid_character", "observe_mode", "owned_by_other",
    "watchlist_full", "character_not_found", "character_dead",
    "cannot_act_as_player", "ownership_divergence",
))


#: What each refusal MEANS to an agent reading it. One sentence, naming the
#: thing to do differently — a code with no sentence is a code the agent will
#: guess at.
CONTROL_REFUSAL_SUMMARIES = {
    "no_session": "the bridge has no session for this client — reconnect, then retry",
    "invalid_character": "the character id is not one the bridge will accept",
    "observe_mode": "this episode is an OBSERVER: it may read the world and may not claim a seat",
    "owned_by_other": "another session already holds that character — it is not free to claim",
    "watchlist_full": "every controlled slot is taken: a seat must RELEASE before this one can claim",
    "character_not_found": "no such character in the world",
    "character_dead": "that character is dead — claim the heir, not the corpse",
    "cannot_act_as_player": "cannot control the human player — player is target-only",
    "ownership_divergence": "the registry disagrees with itself about who holds this character (§7.3g)",
    "refusal_reason_unknown": ("the bridge refused control and named a reason this client "
                               "does not know: treat as UNFORESEEN, not as a near-miss"),
}


#: Every reason `do_release_character` can refuse with (dispatch_router.cpp).
#: Four, and only `observe_mode` is shared with control's nine — a release is
#: not a control call with the sign flipped.
RELEASE_REFUSAL_REASONS = frozenset((
    "missing_character_id", "not_in_roster", "not_owned_by_session",
    "observe_mode",
    # ck-cwal (progenitor ruling 2026-09-24). A CONTROL seat's release of its
    # LAST character is refused by the bridge. The literal is ck-760i's, agreed
    # before either side shipped, because the two halves meet at exactly this
    # string: a mismatch routes a real refusal into `refusal_reason_unknown`
    # and the agent is told the bridge named something this client does not
    # know — safe, and not the explanation the ruling asks for.
    #
    # The bridge emits it (dispatch_router.cpp, the release of a control
    # session's last claim). It needs the GATE_TABLE row above as well as this
    # member: a member here with no row there is explained in prose and marked
    # RETRYABLE, and the agent reads the retry flag, not the prose.
    "last_claim_of_control_session",
))

RELEASE_REFUSAL_SUMMARIES = {
    "missing_character_id": "no character id was sent with the release",
    "not_in_roster": "that character is not in the controlled roster — nothing to release",
    "not_owned_by_session": "this seat does not hold that character, so it cannot release it",
    "observe_mode": "this episode is an OBSERVER: it holds no seat to release",
    # THE EXPLANATION THE RULING ASKS FOR, in the agent's own terms: it is not
    # being told it cannot act, it is being told acting is not what holding the
    # seat is for.
    # THE CARVE-OUT IS NOW STATED, and it is ONE, not the two I expected.
    # ck-760i answered bridge_init.cpp:1575 from the handler (2026-09-25): the
    # direct assignment there only refreshes the session's cached primary — the
    # claim itself is made in `swap_character`, which the handoff always goes
    # through and which moves the id between `claimed_character_ids` under one
    # lock. So a succession-seated character is an ORDINARY claim, and there is
    # no second seating path: `last_steered_character_id` only recovers WHICH
    # corpse to run the handoff for, after which the ordinary path continues.
    #
    # The corpse carve-out I had planned DISSOLVED rather than being confirmed:
    # the corpse is removed by `swap_character` inside the handoff and never
    # reaches do_release_character, so there is no release request to exempt.
    # Stated positively, per ck-760i: the refusal applies to EVERY release an
    # agent can make, and the only thing it lets through is the switch's own
    # internal second half — which is not an agent's release either.
    "last_claim_of_control_session": (
        "this is a CONTROL seat's LAST character and releasing it is refused. "
        "You steer; the AI runs regardless — an empty seat does not pause the "
        "world, it removes your influence over a character that keeps playing. "
        "This holds however you came to hold the character, including one a "
        "succession handed you: an inherited claim is a claim. Switching to "
        "another character is still allowed: control the next one directly "
        "rather than releasing this one first."),
}


def control_refusal_reason(res) -> "Optional[str]":
    """The refusal code, or None when the bridge WITNESSED a success. (ck-switch)

    Three-valued on purpose, because `success` has three states and the old code
    read none of them: it inspected `res` for exactly one condition
    (`cannot_act_as_player`) and never read `success` at all, so every other
    refusal — the seat already held by another agent, the cap reached, the
    character dead — returned ok=True, verdict "applied", and an actor echo for
    a character the agent DOES NOT HOLD. That is the defect Ruling Three names.

    `success is False` is a refusal, and a refusal whose spelling this list does
    not name is `refusal_reason_unknown`, never the nearest member.
    `success is True` is a success. `success` ABSENT is neither: a recognised
    code still means refused, and otherwise the caller is told the outcome was
    UNWITNESSED rather than being handed a default.

    READS BOTH `error` AND `reason`, and that is not the ledger's rule. On the
    live wire `reject_player_actor` (dispatch_router.cpp:238) puts the CODE
    under `error` and a prose sentence under `reason`, while every other gate
    puts the code under `reason`. The ledger has no `error` key at all, so
    piece 7's consumer reads `reason` alone; these are two different surfaces
    and the difference is deliberate, not an oversight in either.
    """
    if not isinstance(res, dict):
        return "refusal_reason_unknown"
    code = None
    for field in ("error", "reason"):
        v = res.get(field)
        if isinstance(v, str) and v in CONTROL_REFUSAL_REASONS:
            code = v
            break
    success = res.get("success")
    if success is False:
        return code or "refusal_reason_unknown"
    if success is True:
        return None
    return code


#: Where the gateway keeps the bridge client: mcp_server.BridgeGateway holds it
#: at gw.state._client (BridgeState._client), an AsyncBridgeClient whose
#: `session_id` the v2 handshake mints. test_own_session_id asserts this path
#: against the REAL class, so a rename reds a row rather than silently costing
#: every seat its identity.
OWN_SESSION_CLIENT_PATHS = (("state", "_client"),)


def own_session_id(gw) -> "Optional[str]":
    """THIS session's id, or None when it cannot be determined. (ck-switch)

    None is a STATE, not a failure to be papered over. Every consumer here must
    say "I do not know which seat I am" rather than fall back to the global
    primary — falling back is the defect this bead exists to fix, and it is the
    same shape as the ledger's character_id fallback and as the succession
    condition that required the GLOBAL primary to move.
    """
    for path in OWN_SESSION_CLIENT_PATHS:
        obj = gw
        for attr in path:
            obj = getattr(obj, attr, None)
            if obj is None:
                break
        sid = getattr(obj, "session_id", None) if obj is not None else None
        if isinstance(sid, str) and sid:
            return sid
    return None


def own_session_row(bridge_state, sid):
    """The asking session's OWN row from `get_bridge_state.sessions`.

    `bridge_state.controlled_primary` is the GLOBAL primary — the front of the
    claim order — so in a six-seat arm every seat asking "who am I" was told the
    same character, and it was not theirs. Keyed by session_id it is right for
    N=1 and for N=6 alike.
    """
    if not isinstance(bridge_state, dict) or not sid:
        return None
    for row in (bridge_state.get("sessions") or []):
        if isinstance(row, dict) and row.get("session_id") == sid:
            return row
    return None


def roster_ids(reply) -> "Optional[list]":
    """The controlled roster from a `get_controlled_roster` reply, or None when
    the reply carries none. The bridge writes the ids under `roster`
    (dispatch_router.cpp do_get_controlled_roster, `r.key("roster")`); it never
    wrote `characters` or `controlled`, which is why the several-rulers guard
    below never fired (L15 witness M1)."""
    if not isinstance(reply, dict):
        return None
    ids = reply.get("roster")
    return list(ids) if isinstance(ids, list) else None


#: The bridge's "no primary" values for a session's primary_character_id
#: (dispatch_router.cpp get_controlled_character: `cid == 0 || cid == 0xFFFFFFFF`
#: falls through to the GLOBAL primary). session.cpp Session::to_json writes the
#: field unmasked, so both can reach the reader.
_NO_PRIMARY = (0, 0xFFFFFFFF)


async def actor_gate_of(gw) -> Optional[dict]:
    """The refusal a call with character_id=0 answers before it reaches the
    bridge, or None when that call resolves to THIS seat's own ruler. The one
    definition both gateways' `actor_gate()` call (SPEC §5).

    The bridge resolves character_id=0 to the asking session's own primary, and
    when that is 0 to the GLOBAL primary, which is another seat's ruler
    (dispatch_router.cpp get_controlled_character). So the id-less call is
    trusted only when this seat's own row (`get_bridge_state.sessions`,
    session.cpp Session::to_json) is read and names exactly one ruler as its
    primary. `get_controlled_roster` is the GLOBAL roster (`roster`,
    dispatch_router.cpp do_get_controlled_roster), the union of every seat's
    claims:

    - roster unread (the read raised, refused, or carries no `roster`):
      actor_required. Whether this seat holds several rulers is unknown, so the
      call names its actor (L15 witness m9; it used to answer "not multi").
    - roster empty: None. Nobody holds a ruler, so there is no fallback to
      leak; the bridge declines query_character(0) and unbound_actor_block
      answers no_ruler_bound.
    - own row unreadable (no session id, read failed, row absent, primary not a
      number): actor_required, the same rule.
    - own primary 0 while the roster is not empty: no_ruler_bound, the answer
      whoami gives that row (unbound_summary / unbound_measured), and the one the
      same seat gets when nobody else holds a ruler. Not actor_required: this
      seat holds no actor, and naming another seat's ruler cannot make it one
      (the bridge refuses the write, session.cpp actor_not_claimed); the step
      that helps is to bind one (L15 witness m8; it used to answer "not multi"
      and the read fell through to the global primary).
    - more than one claim: actor_required (multi-actor mode).
    """
    try:
        ids = roster_ids(await gw.read("get_controlled_roster", {}))
    except Exception:
        ids = None
    if ids is None:
        return why_blocked(
            "actor_required",
            summary=("the controlled roster could not be read, so whether this seat holds "
                     "one ruler or several is unknown: name the actor with character_id"),
            measured={"roster": "unread"})
    if not ids:
        return None
    try:
        row = own_session_row(await gw.read("get_bridge_state", {}), own_session_id(gw))
    except Exception:
        row = None
    primary = row.get("primary_character_id") if row is not None else None
    if row is None or isinstance(primary, bool) or not isinstance(primary, int):
        return why_blocked(
            "actor_required",
            summary=("this seat's own session row could not be read, and character_id=0 may "
                     "resolve to another seat's ruler: name the actor with character_id"),
            measured={"roster": len(ids), "own_row": "unread"})
    if primary in _NO_PRIMARY:
        return why_blocked("no_ruler_bound", summary=unbound_summary(row),
                           measured=unbound_measured(row))
    claims = row.get("claimed_character_ids")
    if not isinstance(claims, list) or len(claims) > 1:
        return why_blocked(
            "actor_required",
            summary="multi-actor mode: character_id is REQUIRED on every actor-bearing call",
            measured={"roster": len(ids),
                      "own_claims": len(claims) if isinstance(claims, list) else "unread"})
    return None


class _WitnessingGateway:
    """Delegates to the real gateway and REMEMBERS the last write reply.

    So that ONE guard can see what the bridge answered, whichever branch of
    session_impl returns. Everything else passes straight through — this is a
    recorder, not a policy.
    """

    def __init__(self, gw):
        self._gw = gw
        self.writes = []                # [(action, reply), ...] in order

    async def write(self, action, params=None):
        res = await self._gw.write(action, params)
        # A LIST, NOT A LAST-WRITE-WINS SLOT (ck-zgh4). Every branch performs
        # exactly one write today, so a single slot would be correct — and
        # "one guard, one recorded reply" would be a load-bearing assumption
        # nothing states. A branch that ever writes twice would leave the FIRST
        # reply unwitnessed, and the source check cannot catch it: that checks
        # the write VERBS against the branches it drives, so `pause_game` then
        # `set_speed` uses two verbs already listed and reds nothing. Recording
        # all of them costs a line and removes the assumption instead of
        # documenting it.
        self.writes.append((action, res))
        return res

    def __getattr__(self, name):        # read, game, health, everything else
        return getattr(self._gw, name)


def unwitnessed_applied(env, writes):
    """Downgrade an envelope that claims `applied` over a reply that does not
    support it. Returns the envelope unchanged when there is nothing to fix.

    Considers EVERY write the branch made, not just the last: if any of them
    refused or went unconfirmed, the envelope cannot claim `applied`. The FIRST
    such reply wins the report, because it is the one that failed earliest and
    anything after it ran on a premise that had already broken.

    THE CLASS, NOT THE INSTANCE (ck-zgh4's sweep). session_impl has ten action
    branches, six of which write. Three of us fixed instances of one pattern and
    stopped: ck-zgh4 found `control`, I found `release`, and four remained in the
    same 218 lines — `speed` and `auto_events` carrying `raw: res` into a result
    whose verdict contradicts it, `unpause` re-reading gw.game() so only its
    verdict is wrong, and `pause` assigning the reply to `res`, never mentioning
    `res` again, and hard-coding `{"paused": True}`.

    `pause` is the one that shows why this is reachable rather than theoretical.
    All six verbs (five since L14 deleted `auto_events`) go out on the gateway's action path, and there `gw.write`
    answers a timeout with `{"status": "sent_no_confirmation"}` — the bridge's explicit
    "I do not know if this landed". The one shape it takes trouble to report
    honestly was the one the caller overwrote with a literal.

    So the rule is asserted ONCE, here, for every branch including ones still to
    be written: a reply that refuses, or that declines to confirm, cannot reach a
    verdict of `applied`. Four more `if not ok` blocks would have closed four
    holes and left the seventh branch to whoever adds it.
    """
    if not isinstance(env, dict) or env.get("verdict") != "applied":
        return env
    # The predicate is reply_refusal's (L14 FF2 N-MAJ-1): ONE rule, so the
    # bridge's fault and timeout replies and the hung gateway's
    # bridge_disconnected, none of which carries `success`, are refusals here too.
    action, res = next(
        ((a, r) for a, r in (writes or [])
         if isinstance(r, dict)
         and (reply_refusal(r) or r.get("status") == "sent_no_confirmation")),
        (None, None))
    if res is None:
        return env

    if reply_refusal(res):
        raw = res.get("error") if isinstance(res.get("error"), str) else res.get("reason")
        if not isinstance(raw, str) or not raw or reply_refusal(res) == DISCONNECTED_IN_FLIGHT:
            # L14 FF3 N3-3: the listener's in-flight disconnect is the predicate's
            # own code (may have landed), not the retryable bridge_disconnected.
            raw = reply_refusal(res)
        # ── ck-ie6o: RESOLVE THROUGH THE ONE RESOLVER ─────────────────────
        #
        # This asked `raw in GATE_TABLE` — a membership test against the gate
        # ENUM — so a raw reason that RAW_REASON_TO_GATE maps to a gate under a
        # different spelling became `refusal_reason_unknown` anyway:
        # `clock_owned_by_pipeline` did for the whole of the census, until
        # ck-3mz0 happened to add it as a GATE_TABLE key and not merely as a
        # mapping. That is two resolvers for one question, and the narrower one
        # decided. `map_raw_reason` is the resolver every other path uses: exact
        # map, then the engine-prose table, then the substring pass, then
        # `unknown`. Asked here too, so a reason this client CAN name is never
        # reported as one it cannot.
        #
        # `refusal_reason_unknown` is kept for the genuinely unnamed, because
        # "the bridge said something I do not know" and "the bridge said nothing
        # I could read" are different findings and this path is the one that can
        # tell them apart.
        gate = "refusal_reason_unknown"
        if isinstance(raw, str) and raw:
            if raw in GATE_TABLE:
                gate = raw
            else:
                mapped = map_raw_reason(raw).get("gate")
                if mapped and mapped != "unmapped_refusal":
                    gate = mapped
        out = dict(env)
        out["ok"] = False
        out.pop("verdict", None)
        out.pop("applied", None)
        out["blocked"] = why_blocked(
            gate,
            summary=(CONTROL_REFUSAL_SUMMARIES.get(gate)
                     or RELEASE_REFUSAL_SUMMARIES.get(gate)
                     or f"the bridge refused `{action}` and this client does not "
                        f"know the reason it named"),
            raw_reason=raw if isinstance(raw, str) else "",
            measured={"requested": env.get("result")} if env.get("result") else None)
        # ── ck-ie6o: A REFUSED WRITE MUST NOT LEAVE A RESULT SAYING IT HAPPENED
        #
        # The branches hardcode their intent: `pause` returns
        # `result={"paused": True}` and never reads the reply at all. So a
        # refused pause came back `ok: false`, a real gate, AND
        # `result={"paused": true}` — an envelope contradicting itself, with the
        # false half in the field an agent reads for the outcome. The census has
        # 7 of them.
        #
        # The requested value is not discarded: it moves into `blocked.measured`
        # as `requested`, which is where a declaration belongs when the
        # observation disagrees with it. `result` is dropped rather than
        # corrected, because this function does not know what the world now says
        # — only that the write did not take.
        out["result"] = None
        return out

    if res.get("status") == "sent_no_confirmation":
        # NOT a refusal and NOT an application: the write was dispatched and the
        # bridge could not confirm it. `confirmation_pending` is the vocabulary's
        # own word for that — severity "pending", retryable False: VERIFY, do
        # not re-declare.
        out = dict(env)
        out.pop("verdict", None)
        out.pop("applied", None)
        out["ok"] = False
        out["blocked"] = why_blocked(
            "confirmation_pending",
            summary=(f"`{action}` was dispatched and the bridge did NOT confirm it. "
                     "The result below is what was REQUESTED, not what was "
                     "observed — read the state back before acting on it."),
            raw_reason="sent_no_confirmation")
        return out
    return env


#: The session actions that write a switch: `enabled` has no default for them.
ENABLED_REQUIRED = ("set_experimental",)


def _enabled_required(gw, action: str) -> dict[str, Any]:
    """L14 finding 4: a switch call without `enabled` armed the flag (it defaulted
    to true). The value is required; nothing is written without it."""
    assert action in ENABLED_REQUIRED, action
    return envelope("session", ok=False, verdict="rejected", game=gw.game(),
                    blocked=why_blocked(
                        "invalid_argument",
                        summary=(f"session(action='{action}') requires enabled=true or "
                                 "enabled=false; nothing was written")),
                    bridge_health=gw.health)


async def session_impl(gw, action: str, character_id: int = 0, speed: int = SPEED_MAX,
                       game_days: int = 0,
                       enabled: Optional[bool] = None, disable_native_ai: bool = False,
                       flag: str = "", seconds: int = 60) -> dict[str, Any]:
    """Thin wrapper: run the branches, then apply the one guard to whatever they
    returned. See `unwitnessed_applied` for why the rule lives in one place."""
    wgw = _WitnessingGateway(gw)
    env = await _session_impl_branches(wgw, action, character_id=character_id,
                                       speed=speed, enabled=enabled, game_days=game_days,
                                       disable_native_ai=disable_native_ai,
                                       flag=flag, seconds=seconds)
    return unwitnessed_applied(env, wgw.writes)


# session(action="wait"): the server owns the wait between cycles. Claude Code
# refuses a foreground `sleep` of 30 s or more (measured 2026-10-18 on 2.1.241 and
# 2.1.302: "Blocked: standalone sleep 30"), and an allow-list entry does not
# override it, so the protocol's `sleep 60` never ran and every multi-cycle run
# stopped after one cycle. Claude Code also hands an MCP call still running after
# 120 s to a background task (CLAUDE_CODE_MCP_AUTO_BACKGROUND_MS); the cap keeps a
# wait under that, and the run harness and play directory turn the hand-off off.
WAIT_CAP_S = 110
import cycle_cadence as _cc            # noqa: E402  (v3 §7.2: the cycle in game-days, one definition)
_SLEEP = asyncio.sleep          # tests replace it


async def _fresh_tick(gw) -> dict:
    """get_tick_info straight from the bridge: the gateway's cached tick_start is
    not refreshed by a management write, so after unpause_game it still says
    paused until some other call happens to update it (the `paused: true` reply
    to a successful unpause, 2026-10-18)."""
    try:
        ti = await gw.read("get_tick_info", {})
    except Exception:  # noqa: BLE001
        return {}
    return _unwrap_reply(ti)


async def _settle_tick(gw, settled, timeout_s: float = 3.0) -> dict:
    """Poll get_tick_info until `settled(ti)` holds (None: one fresh read) or
    `timeout_s` passes; the last reading either way. The one settle loop for the
    session clock verbs (pause, unpause, speed, wait)."""
    deadline = asyncio.get_running_loop().time() + timeout_s
    ti = await _fresh_tick(gw)
    while settled is not None and not settled(ti):
        if asyncio.get_running_loop().time() >= deadline:
            break
        await _SLEEP(0.2)
        ti = await _fresh_tick(gw)
    return ti


async def _settle_pause(gw, want_paused, timeout_s: float = 3.0) -> dict:
    """Poll the bridge until `paused` reads `want_paused` (None: one fresh read)."""
    return await _settle_tick(
        gw, None if want_paused is None else (lambda ti: ti.get("paused") is want_paused),
        timeout_s)


def _game_from_tick(gw, ti: dict) -> dict:
    g = dict(gw.game())
    if ti.get("game_date"):
        g["date"] = ti["game_date"]
    for k in ("tick", "tick_id"):
        if isinstance(ti.get(k), int):
            g["tick"] = ti[k]
            break
    if ti.get("paused") is not None:
        g["paused"] = bool(ti["paused"])
    if ti.get("game_speed") is not None:
        g["speed"] = ti["game_speed"]
    return g


async def _session_clock_write(gw, action: str) -> dict[str, Any]:
    """session(pause|unpause): write, then read the clock back (L14 FF1 M-5). The
    bridge clears or sets the flag and queues the real PAUSE_SET for the next
    tick, and the gateway's cached snapshot is not refreshed by the write: ask the
    bridge until it reads the wanted state (or 3 s pass). A refusal is rejected
    (write_blocked); a state read back is applied, with the reading; a state not
    read back within 3 s is pending next_pass, never applied."""
    want_paused = action == "pause"
    submitted_on = (gw.game() or {}).get("date")
    res = await gw.write("pause_game" if want_paused else "unpause_game", {})
    refused = reply_refusal(res)
    if refused:
        return envelope("session", ok=False, verdict="rejected", game=gw.game(),
                        result={"raw": res}, blocked=write_blocked(refused, raw=res),
                        bridge_health=gw.health)
    ti = await _settle_pause(gw, want_paused, 3.0)
    g = _game_from_tick(gw, ti)
    if ti.get("paused") is not want_paused:
        state = "paused" if want_paused else "running"
        return envelope("session", verdict="pending", game=g,
                        result={"paused": ti.get("paused"), "settled": False},
                        pending=write_pending(
                            "next_pass", game_date_at_submit=submitted_on,
                            expected_change=f"the game reads {state}",
                            why_pending=(f"get_tick_info did not read {state} within 3 s of "
                                         "the write (paused reads "
                                         f"{ti.get('paused')!r})"),
                            verify_with="session(action='health') paused"),
                        bridge_health=gw.health)
    return envelope("session", verdict="applied", result={"paused": want_paused, "settled": True},
                    game=g, bridge_health=gw.health)

async def _session_speed_write(gw, speed: int) -> dict:
    """session(speed): write, then read the speed back, like pause/unpause (L14
    FF2 M-5). The bridge stores the speed synchronously (clock_write ->
    speed_primitive, event_handler.cpp) and get_tick_info reads it back as
    `game_speed` (speed_read.h emit; null with speed_read_status naming the
    absence when the read faulted). A refusal (reply_refusal: success false, or
    the bridge's {"error":"memory_access_fault"} from case_set_speed) is
    rejected; the speed read back is applied, with the reading; a speed not read
    back within 3 s is pending next_pass, never applied."""
    submitted_on = (gw.game() or {}).get("date")
    res = await gw.write("set_speed", {"speed": speed})
    refused = reply_refusal(res)
    if refused:
        return envelope("session", ok=False, verdict="rejected", game=gw.game(),
                        result={"raw": res}, blocked=write_blocked(refused, raw=res),
                        bridge_health=gw.health)
    ti = await _settle_tick(gw, lambda t: t.get("game_speed") == speed, 3.0)
    g = _game_from_tick(gw, ti)
    if ti.get("game_speed") != speed:
        return envelope("session", verdict="pending", game=g,
                        result={"speed": ti.get("game_speed"), "requested": speed,
                                "settled": False, "raw": res},
                        pending=write_pending(
                            "next_pass", game_date_at_submit=submitted_on,
                            expected_change=f"the game reads speed {speed}",
                            why_pending=(f"get_tick_info did not read speed {speed} within 3 s "
                                         f"of the write (game_speed reads {ti.get('game_speed')!r}, "
                                         f"speed_read_status {ti.get('speed_read_status')!r})"),
                            verify_with="session(action='health') speed"),
                        bridge_health=gw.health)
    return envelope("session", verdict="applied",
                    result={"speed": speed, "settled": True, "raw": res},
                    game=g, bridge_health=gw.health)


async def _session_impl_branches(gw, action: str, character_id: int = 0, speed: int = SPEED_MAX,
                                 game_days: int = 0,
                       enabled: Optional[bool] = None, disable_native_ai: bool = False,
                       flag: str = "", seconds: int = 60) -> dict[str, Any]:
    if action == "experimental_flags":
        # READ the registry — what experimental flags exist + their validation status.
        return envelope("session", result={
            # rc2 B8: `default` is the boot value. The bridge has no readback,
            # so `live` is unknown; `last_written_here` is this server's own
            # write, if any.
            "flags": [{"name": k, "setter": v["setter"], "spec": v["spec"],
                       "summary": v["summary"], "status": "EXPERIMENTAL-unvalidated",
                       "default": "OFF", "live": None,
                       "live_unavailable": "the bridge exposes no readback for this flag",
                       "last_written_here": _FLAG_WRITES.get(k),
                       "last_written_here_scope": ("this MCP server process only; another "
                                                   "seat's server may have written the flag, "
                                                   "so null does not mean off")}
                      for k, v in sorted(_EXPERIMENTAL_FLAGS.items())],
            "note": ("arm one with session(action='set_experimental', flag=<name>, enabled=true). "
                     "All boot OFF and unvalidated live; nothing else arms them."),
        }, game=gw.game(), bridge_health=gw.health)
    if action == "set_experimental":
        if not flag:
            return envelope("session", ok=False, game=gw.game(),
                            blocked=why_blocked(
                                "missing_required_argument",
                                summary=("set_experimental requires flag=<name>; valid: "
                                         + ", ".join(sorted(_EXPERIMENTAL_FLAGS))
                                         + ". Read them via session(action='experimental_flags').")),
                            bridge_health=gw.health)
        if not isinstance(enabled, bool):
            return _enabled_required(gw, action)
        return await _set_experimental_flag(gw, flag, enabled)
    if action == "whoami":
        roster = await gw.read("get_controlled_roster", {})
        bs = await gw.read("get_bridge_state", {})
        # ck-switch: THIS SEAT'S OWN CLAIMS, not the global primary.
        # `controlled_primary` is the FRONT OF THE CLAIM ORDER, so in a six-seat
        # arm every seat asking "who am I" was told the same character and it
        # was not theirs. A true switch depends on a seat knowing what it holds,
        # so this had to be right before `control` could be.
        _sid = own_session_id(gw)
        _mine = own_session_row(bs, _sid)
        if _mine is not None:
            primary = _mine.get("primary_character_id")
        else:
            # UNKNOWN IS A STATE, and so is SEATED-BUT-HOLDING-NOTHING. Both
            # give no character, so they share this branch rather than two
            # identical assignments pretending to be a distinction; what tells
            # them apart is `session_id` in the result, null for the first and a
            # string for the second. Falling back to the global primary is the
            # defect this bead exists to fix.
            primary = None
        # Every seat's held rulers, as the bridge writes them (roster_ids), or
        # None when the read did not come back; `characters` was never written.
        _roster_out = roster_ids(roster)
        _claims_out = (list(_mine.get("claimed_character_ids") or ())
                       if _mine is not None else None)
        # rc2 B6: NOTHING BOUND IS NOT ok=true. This seat's row says it holds no
        # primary: say so, and do not echo id 0 as a controlled, living ruler.
        if _mine is not None and _as_int(primary) <= 0:
            return envelope("session", ok=False,
                            result={"controlled": None, "session_id": _sid,
                                    "my_claims": _claims_out, "roster": _roster_out},
                            blocked=why_blocked(
                                "no_ruler_bound",
                                summary=unbound_summary(_mine),
                                measured=unbound_measured(_mine)),
                            game=gw.game(), bridge_health=gw.health)
        # rc2 B6: the REVERSE succession. The human's character died and the
        # human inherited this seat's ruler: the seat still holds the id, but it
        # is now the player's and every write is refused (cannot_act_as_player).
        _player = _as_int((bs or {}).get("player_character_id")) & 0xFFFFFF \
            if isinstance(bs, dict) and "player_character_id" in bs else 0
        if _mine is not None and 0 < _player < 0xFFFFFF \
                and (_as_int(primary) & 0xFFFFFF) == _player:
            return envelope("session", ok=False,
                            result={"controlled": None, "session_id": _sid,
                                    "my_claims": _claims_out, "roster": _roster_out},
                            blocked=why_blocked(
                                "cannot_act_as_player",
                                summary=("this seat's ruler is now the human's character (the "
                                         "human inherited it); every write is refused. Release "
                                         "it and bind an AI ruler."),
                                measured={"primary_character_id": _as_int(primary),
                                          "player_character_id": _player}),
                            game=gw.game(), bridge_health=gw.health)
        chr_ = await gw.read("query_character", {"character_id": _as_int(primary)})
        _warn: list = []
        if _mine is None:
            # Seat unknown: character_id=0 is whatever the bridge resolves it
            # to, which falls back to the GLOBAL primary (another seat's ruler
            # when several are bound). Report it, labelled, or report nothing.
            unbound = unbound_actor_block(chr_)
            if unbound is not None:
                return envelope("session", ok=False,
                                result={"controlled": None, "session_id": _sid,
                                        "my_claims": None, "roster": _roster_out},
                                blocked=unbound, game=gw.game(), bridge_health=gw.health)
            _warn.append({"code": "seat_unknown",
                          "detail": "this session's own row was not found; `controlled` "
                                    "is the bridge's resolution of character_id=0, not a confirmed claim"})
        ctrl = actor_echo(chr_ if isinstance(chr_, dict) else {}, _as_int(primary),
                          is_controlled=True if _mine is not None else None)
        agent_mode = (bs or {}).get("agent_mode") if isinstance(bs, dict) else None
        _agency = await agency_block(gw, _as_int(primary))
        return envelope("session", warnings=_warn, result={"controlled": ctrl,
                                           # ck-zgh4 deploy-51 item (4): what
                                           # this seat actually controls, stated
                                           # once and measured, not inferred.
                                           "agency": _agency,
                                           # ck-switch: what THIS seat holds,
                                           # beside the host-wide roster. `null`
                                           # for either means it could not be
                                           # determined, never "the global one".
                                           "session_id": _sid,
                                           "my_claims": _claims_out,
                                           "roster": _roster_out},
                        game=gw.game(), bridge_health=gw.health)
    if action == "health":
        bs = await gw.read("get_bridge_state", {})
        ov = await gw.read("get_offset_validation", {})
        return envelope("session", result={"state": gw.health,
                                           "peer": (await gw.read("who_holds_the_lock", {})),
                                           "offset_health": ov, "agent_mode": (bs or {}).get("agent_mode") if isinstance(bs, dict) else None},
                        game=gw.game(), bridge_health=gw.health)
    if action == "control":
        # rc2 B6: the bridge reads character_id<=0 as "drop this seat's primary"
        # and replies success, so an omitted id silently UNBOUND the seat and
        # came back `applied`. Unbinding is `release`; control needs an id.
        if _as_int(character_id) <= 0:
            return envelope("session", ok=False, game=gw.game(),
                            blocked=why_blocked(
                                "missing_character_id",
                                summary=("control needs character_id=<id>; it was omitted or 0, "
                                         "which the bridge would treat as unbinding. To unbind, "
                                         "use session(action='release', character_id=<id>).")),
                            bridge_health=gw.health)
        res = await gw.write("set_controlled_character",
                             {"character_id": character_id, "disable_native_ai": disable_native_ai})
        res = res if isinstance(res, dict) else {}
        # ck-switch / Ruling Three: read `success`, and return the TYPED reason
        # with NO actor echo. Echoing a character the agent does not hold is
        # what made a refusal indistinguishable from a seat change.
        _refusal = control_refusal_reason(res)
        if _refusal is not None:
            _raw = res.get("error") if isinstance(res.get("error"), str) else res.get("reason")
            return envelope("session", ok=False, game=gw.game(),
                            blocked=write_blocked(gate=
                                _refusal,
                                summary=CONTROL_REFUSAL_SUMMARIES.get(
                                    _refusal,
                                    "the bridge refused control and named a reason this "
                                    "client does not know: treat as UNFORESEEN, not as a "
                                    "near-miss of one it does"),
                                raw_reason=_raw if isinstance(_raw, str) else ""),
                            bridge_health=gw.health)
        _witnessed = res.get("success") is True
        # ── the echo is what the seat HOLDS, not what it ASKED FOR (Ruling Three)
        # Building it from `character_id` is what let 2,578 retries happen: the
        # agent was handed back the character it had named, observed it was not
        # steering that character, and called again. Not irrational — misinformed.
        # The bridge's own reply is the first authority; its `controlled_primary`
        # is this seat's primary AFTER the routing decided swap-or-claim.
        _held = res.get("controlled_primary")
        if not isinstance(_held, int) or _held <= 0:
            # A FALLBACK, not a guard: the reply did not carry it, so ask. This
            # is the same question whoami asks, keyed on THIS session rather than
            # on the global primary.
            _bs = await gw.read("get_bridge_state", {})
            _row = own_session_row(_bs, own_session_id(gw))
            _held = _row.get("primary_character_id") if _row is not None else None
        _held_known = isinstance(_held, int) and _held > 0
        _echo_id = _held if _held_known else character_id
        chr_ = await gw.read("query_character", {"character_id": _as_int(_echo_id)})
        # RELEASE-DURING-WAR doctrine (re_control_suppresses_war.md): disable_native_ai=true
        # SKIPS this char in the engine AI tick → its OFFENSIVE wars never siege
        # (occupation_score stays 0). Surface a warning so the agent doesn't silently
        # strand a conquest. Fix = release during the war, or keep disable_native_ai=false.
        warnings = None
        if disable_native_ai:
            # ck-zgh4 deploy-51: ck-4b41's BRIDGE finding is kept verbatim in
            # substance — `disable_native_ai=false` genuinely undoes nothing,
            # because the disable has no else-branch. What is removed is the
            # remedy it named: `set_character_ai` is not callable from the seat
            # surface, so a seat that followed this got `unknown_verb`. The two
            # remedies left are both on the surface: `session(action='release')`
            # and `set_phase`, which re-asserts the gate every time it runs.
            warnings = ["disable_native_ai=true disables this character's war AI: an "
                        "OFFENSIVE war will not siege (occupation_score stays 0). "
                        "Re-controlling with disable_native_ai=false does NOT undo it — "
                        "the disable has no else-branch, so false only skips it. "
                        "What does: session(action='release') during the war, which the "
                        "war survives, or set_phase, which re-asserts this character's "
                        "AI tick every time it runs."]
        if _held_known and _echo_id != character_id:
            # The call was not refused and the seat holds SOMEBODY ELSE. That is
            # the discrepancy the agent must see rather than infer three retries
            # later, so it is a warning and the echo names who is really held.
            warnings = (warnings or []) + [
                f"control was not refused, but this seat holds character {_echo_id}, "
                f"NOT the requested {character_id} — the echo below is what is HELD. "
                "Read session(action='whoami') before acting as the requested one."]
        if not _held_known:
            warnings = (warnings or []) + [
                "the bridge did not say which character this seat holds after the "
                f"call, so the echo below is the REQUESTED {character_id} and has "
                "not been confirmed against the seat."]
        if not _witnessed:
            # `success` absent and no reason given: an OLD bridge, or a reply
            # shape this client has not seen. The call may well have worked —
            # but "probably" is not a witness, and a seat that believes it holds
            # a character it does not is the failure this whole piece is about.
            warnings = (warnings or []) + [
                "the bridge did not report `success` for this control call, so the "
                "seat change is UNWITNESSED: the echo below is what was REQUESTED, "
                "not what was confirmed. Call session(action='whoami') to read what "
                "this seat actually holds."]
        return envelope("session", verdict="applied",
                        result={"controlled": actor_echo(chr_ if isinstance(chr_, dict) else {}, character_id),
                                # ck-zgh4 deploy-51 item (4): RENAMED. This is
                                # `bool(disable_native_ai)` — THE PARAMETER THE
                                # CALLER PASSED — and it was named as though it
                                # were a measured state. d50 read it `true` 210
                                # times and d49's king concluded "I never had
                                # hands on the army". The measured per-character
                                # fact is `agency.ai_tick` from
                                # session(action='whoami').
                                "native_ai_disable_requested": bool(disable_native_ai),
                                "control_witnessed": _witnessed,
                                # what the echo IS: the seat's own holding, or
                                # the unconfirmed request. Never left to be
                                # inferred from whether the ids happen to match.
                                "controlled_is_held": bool(_held_known),
                                "requested_character_id": character_id,
                                },
                        warnings=warnings,
                        game=gw.game(), bridge_health=gw.health)
    if action == "add":
        # lane-multi: ONE MORE RULER for this session. `control` SWITCHES (the
        # bridge's swap, Ruling Three); `add` claims the character beside the
        # ones already held and keeps the primary (`add: true` on the bridge's
        # set_controlled_character). With nothing held it is a first claim.
        if _as_int(character_id) <= 0:
            return envelope("session", ok=False, game=gw.game(),
                            blocked=why_blocked(
                                "missing_character_id",
                                summary="add needs character_id=<id>; it was omitted or 0."),
                            bridge_health=gw.health)
        res = await gw.write("set_controlled_character",
                             {"character_id": character_id,
                              "disable_native_ai": disable_native_ai, "add": True})
        res = res if isinstance(res, dict) else {}
        _refusal = control_refusal_reason(res)
        if _refusal is not None:
            _raw = res.get("error") if isinstance(res.get("error"), str) else res.get("reason")
            return envelope("session", ok=False, game=gw.game(),
                            blocked=write_blocked(gate=
                                _refusal,
                                summary=CONTROL_REFUSAL_SUMMARIES.get(
                                    _refusal,
                                    "the bridge refused the add and named a reason this "
                                    "client does not know: treat as UNFORESEEN"),
                                raw_reason=_raw if isinstance(_raw, str) else ""),
                            bridge_health=gw.health)
        if res.get("success") is True and res.get("added") is not True:
            # An older bridge ignores `add` and SWITCHES: the seat may have let
            # go of a ruler it held. Say so; whoami reads what it holds now.
            return envelope("session", ok=False, game=gw.game(),
                            blocked=write_blocked(gate=
                                "add_unsupported_by_bridge",
                                summary=("this bridge does not support add: it treated the call "
                                         "as control and may have SWITCHED away from a ruler "
                                         "this session held. Read session(action='whoami').")),
                            bridge_health=gw.health)
        _claims = res.get("claimed_character_ids")
        if not isinstance(_claims, list):
            _row = own_session_row(await gw.read("get_bridge_state", {}), own_session_id(gw))
            _claims = list(_row.get("claimed_character_ids") or ()) if _row is not None else None
        _witnessed = res.get("success") is True and isinstance(_claims, list) \
            and _as_int(character_id) in [_as_int(c) for c in _claims]
        chr_ = await gw.read("query_character", {"character_id": _as_int(character_id)})
        warnings = None
        if not _witnessed:
            warnings = ["the bridge did not confirm that this session now holds "
                        f"{character_id}; call session(action='whoami') before acting as it."]
        if disable_native_ai:
            warnings = (warnings or []) + [
                "disable_native_ai=true disables this character's war AI: an OFFENSIVE war "
                "will not siege. set_phase re-asserts its AI tick."]
        return envelope("session", verdict="applied",
                        result={"added": actor_echo(chr_ if isinstance(chr_, dict) else {},
                                                    character_id),
                                "add_witnessed": _witnessed,
                                "primary_character_id": res.get("controlled_primary"),
                                "my_claims": _claims,
                                "native_ai_disable_requested": bool(disable_native_ai),
                                "note": ("pass character_id on every call for each ruler: "
                                         "a call without it acts as the primary")},
                        warnings=warnings, game=gw.game(), bridge_health=gw.health)
    if action == "release":
        # ck-switch: THE SIBLING OF THE CONTROL DEFECT, one branch below it.
        # This discarded the reply outright — not even assigned — and answered
        # `verdict: "applied"` with `released: <id>` unconditionally. A refused
        # release therefore read exactly like a successful one, and the seat
        # went on holding the character while the agent believed it had let go.
        # That is load-bearing for the experiment, not only for the agent: the
        # `seat_vacated` criterion invalidates an episode whose control seat was
        # seated and later sat empty, so "did this seat really release" is a
        # question the evidence asks.
        res = await gw.write("release_character", {"character_id": character_id})
        res = res if isinstance(res, dict) else {}
        if res.get("success") is False:
            _r = res.get("reason")
            _gate = _r if isinstance(_r, str) and _r in RELEASE_REFUSAL_REASONS \
                else "refusal_reason_unknown"
            return envelope("session", ok=False, game=gw.game(),
                            blocked=write_blocked(gate=
                                _gate,
                                summary=RELEASE_REFUSAL_SUMMARIES.get(
                                    _gate,
                                    "the bridge refused the release and named a reason "
                                    "this client does not know: treat as UNFORESEEN"),
                                raw_reason=_r if isinstance(_r, str) else ""),
                            bridge_health=gw.health)
        return envelope("session", verdict="applied",
                        result={"released": character_id,
                                # Same three-valued honesty as control: absent
                                # `success` is not success, it is unwitnessed.
                                "release_witnessed": res.get("success") is True},
                        warnings=None if res.get("success") is True else [
                            "the bridge did not report `success` for this release, so "
                            "the seat may still hold the character. Call "
                            "session(action='whoami') before assuming it is free."],
                        game=gw.game(), bridge_health=gw.health)
    if action == "speed":
        # The bridge clamps every speed write to 1..SPEED_MAX (game_set_speed,
        # event_handler.cpp). This mirrors it so the reply says what was set, and
        # adds the one rule the bridge does not have: above the cap at war is
        # refused, not quietly clamped.
        try:
            speed = int(speed)
        except (TypeError, ValueError):
            speed = 0
        if speed < SPEED_MIN:
            return envelope("session", ok=False, game=gw.game(),
                            blocked=why_blocked("invalid_argument",
                                                summary=f"speed must be {SPEED_MIN}-{SPEED_MAX}; got {speed}"),
                            bridge_health=gw.health)
        if speed > SPEED_MAX:
            wars = await gw.read("get_active_wars", {})
            if isinstance(wars, dict) and wars.get("wars"):
                return envelope("session", ok=False, game=gw.game(),
                                blocked=why_blocked("speed_refused_at_war",
                                                    summary=f"speed {speed} refused while at war; the cap is {SPEED_MAX}"),
                                bridge_health=gw.health)
            speed = SPEED_MAX
        return await _session_speed_write(gw, speed)
    if action == "pause":
        return await _session_clock_write(gw, action)
    if action == "unpause":
        return await _session_clock_write(gw, action)
    if action == "wait":
        # Let time pass, server-side: unpause if paused, block, pause, report the
        # dates. Claude cannot sleep in Bash (see WAIT_CAP_S above).
        try:
            secs = int(seconds)
        except (TypeError, ValueError):
            secs = 60
        secs = max(1, min(secs, WAIT_CAP_S))
        # v3 §7.2 (Study 0 round 2): past the registered horizon no wait is made.
        # Read only when a horizon is registered, so free play makes no extra read.
        hz_ord, hz_raw, hz_err = _cc.registered_horizon_end()
        if hz_err:
            return envelope("session", ok=False, game=gw.game(), bridge_health=gw.health,
                            blocked=why_blocked(_cc.R_HORIZON_UNREADABLE, summary=hz_err))
        if hz_ord is not None:
            now = _game_from_tick(gw, await _fresh_tick(gw))
            reached = _cc.horizon_reached(now.get("date"), hz_ord)
            if reached is None:
                return envelope("session", ok=False, game=now, bridge_health=gw.health,
                                blocked=why_blocked(
                                    _cc.R_HORIZON_UNREADABLE,
                                    summary=(f"the game date {now.get('date')!r} could not be read "
                                             f"against the registered horizon {hz_raw}; nothing was waited")))
            if reached:
                return envelope("session", ok=False, game=now, bridge_health=gw.health,
                                blocked=why_blocked(
                                    _cc.R_HORIZON_REACHED,
                                    summary=(f"the game date {now.get('date')} is at or past the episode's "
                                             "horizon; no further wait is made. Pause and report "
                                             "(session(action='pause'))."),
                                    measured={"game_date": now.get("date")}))
        try:
            days = int(game_days)
        except (TypeError, ValueError):
            days = -1
        if days < 0 or days > _cc.HARD_GAME_DAYS_CEILING:
            return envelope("session", ok=False, game=gw.game(), bridge_health=gw.health,
                            blocked=why_blocked("invalid_argument",
                                                summary=(f"game_days must be 0..{_cc.HARD_GAME_DAYS_CEILING} "
                                                         f"(0 = wait `seconds` once); got {game_days!r}")))
        if days > 0:
            # v3 §7.2 / owner ruling 2026-11-04: THE CYCLE IS MEASURED IN GAME-DAYS.
            cap_days, cap_err = _cc.registered_cycle_game_days()
            if cap_err:
                return envelope("session", ok=False, game=gw.game(), bridge_health=gw.health,
                                blocked=why_blocked(_cc.R_CADENCE_UNREADABLE, summary=cap_err))
            if cap_days is not None and days > cap_days:
                return envelope("session", ok=False, game=gw.game(), bridge_health=gw.health,
                                blocked=why_blocked(
                                    _cc.R_OVER_CADENCE,
                                    summary=(f"game_days={days} exceeds the registered cycle cadence "
                                             f"({cap_days} game-days, {_cc.CYCLE_GAME_DAYS_ENV}); "
                                             f"ask for at most {cap_days}. Refused, not clamped."),
                                    measured={"game_days": days, "cadence_game_days": cap_days}))
            return await _session_wait_game_days(gw, days, secs, cap_days)
        return await _session_wait_once(gw, secs)
    return envelope("session", ok=False, game=gw.game(),
                    blocked=why_blocked("unknown_verb" if action else "missing_required_argument",
                                        summary=(f"unknown session action '{action}'" if action
                                                 else "action is required")
                                                + f"; valid: {', '.join(SESSION_ACTIONS)}"),
                    bridge_health=gw.health)


async def _session_wait_once(gw, secs: int):
    """ONE bounded wait: unpause if paused, sleep `secs`, pause, report both dates.
    The body `session(action="wait")` always had; `_session_wait_game_days` calls it in a loop."""
    if True:
        before = await _settle_pause(gw, None)
        if before.get("paused") is not False:
            await gw.write("unpause_game", {})
            running = await _settle_pause(gw, False, 3.0)
            if running.get("paused") is not False:
                return envelope("session", ok=False, game=_game_from_tick(gw, running),
                                blocked=write_blocked(gate=
                                    "game_did_not_unpause",
                                    summary=("the game did not unpause within 3 s; nothing was "
                                             "waited. session(action='health'), then retry once.")),
                                bridge_health=gw.health)
        await _SLEEP(secs)
        paused_on = (gw.game() or {}).get("date")
        pres = await gw.write("pause_game", {})
        refused = reply_refusal(pres)
        # L14 FF3 N3-2: a REFUSED closing pause is a refusal, read once for the
        # record. It is never pending: nothing will drain, and the game is left
        # running at the speed it waited at.
        after = await (_fresh_tick(gw) if refused else _settle_pause(gw, True, 3.0))
        g = _game_from_tick(gw, after)
        waited = {
            "waited_s": secs, "cap_s": WAIT_CAP_S,
            "start_date": before.get("game_date"), "end_date": after.get("game_date"),
            "clock_ran": bool(before.get("game_date")) and before.get("game_date") != after.get("game_date"),
            "paused": after.get("paused"),
        }
        if refused:
            return envelope("session", ok=False, verdict="rejected", result=waited, game=g,
                            blocked=write_blocked(
                                refused, raw=pres,
                                summary=(f"the closing pause was refused ({refused}): the game "
                                         f"is still running (paused reads {after.get('paused')!r}); "
                                         "session(action='pause') once session(action='health') "
                                         "answers")),
                            bridge_health=gw.health)
        if after.get("paused") is not True:
            # L14 FF2 N-MAJ-1 sweep: the closing pause is a write like any other;
            # a pause not read back is pending, never applied. A refused one is
            # rejected just above (the session guard acts only on `applied`).
            return envelope("session", verdict="pending", result=waited, game=g,
                            pending=write_pending(
                                "next_pass", game_date_at_submit=paused_on,
                                expected_change="the game reads paused",
                                why_pending=("get_tick_info did not read paused within 3 s of "
                                             f"the closing pause (paused reads {after.get('paused')!r})"),
                                verify_with="session(action='health') paused"),
                            bridge_health=gw.health)
        return envelope("session", verdict="applied", result=waited, game=g,
                        bridge_health=gw.health)


async def _session_wait_game_days(gw, days: int, secs: int, cap_days):
    """v3 §7.2: let the clock run until the game date has advanced ≥ `days`
    game-days since this call began — bounded waits of at most `secs`
    (≤ WAIT_CAP_S) each, the next one planned from the rate measured so far
    (`cycle_cadence.plan_wait`), at most MAX_WAIT_CALLS, and stopped after
    MAX_NO_PROGRESS waits with no date movement. The reply carries what was
    asked, what was covered, how many calls it took and why it stopped; a
    refused or pending inner wait is returned as that wait's own envelope.
    """
    start_date = None
    start_ord = None
    elapsed = 0.0
    advanced = 0
    calls = 0
    no_progress = 0
    stopped = _cc.STOP_MAX_CALLS
    last = None
    while calls < _cc.MAX_WAIT_CALLS:
        remaining = days - advanced
        step = _cc.plan_wait(remaining, elapsed_s=elapsed, advanced_days=advanced, cap_s=secs)
        last = await _session_wait_once(gw, step)
        calls += 1
        res = (last or {}).get("result") or {}
        if not last.get("ok", True) or last.get("verdict") == "pending":
            return _with_cycle(last, days, advanced, calls, elapsed, start_date, cap_days,
                               stopped="inner_wait_" + str(last.get("verdict") or "refused"))
        if start_date is None:
            start_date = res.get("start_date")
            start_ord = _cc.ordinal(start_date)
        elapsed += float(res.get("waited_s") or step)
        end_ord = _cc.ordinal(res.get("end_date"))
        before_adv = advanced
        if start_ord is not None and end_ord is not None:
            advanced = max(advanced, end_ord - start_ord)
        if advanced >= days:
            stopped = _cc.STOP_REACHED
            break
        no_progress = no_progress + 1 if advanced == before_adv else 0
        if no_progress >= _cc.MAX_NO_PROGRESS:
            stopped = _cc.STOP_NO_PROGRESS
            break
    out = _with_cycle(last, days, advanced, calls, elapsed, start_date, cap_days, stopped=stopped)
    if stopped != _cc.STOP_REACHED:
        out["ok"] = False
        out["verdict"] = "rejected"
        out["blocked"] = why_blocked(
            "cycle_not_reached",
            summary=(f"the clock covered {advanced} of {days} game-days in {calls} wait(s) "
                     f"({elapsed:.0f} s) and stopped: {stopped}. session(action='health'), then "
                     "ask again for the remaining days."),
            measured={"game_days_requested": days, "game_days_advanced": advanced, "calls": calls})
    return out


def _with_cycle(env_: dict, days, advanced, calls, elapsed, start_date, cap_days, *, stopped):
    res = dict((env_ or {}).get("result") or {})
    res.update({"game_days_requested": days, "game_days_advanced": advanced,
                "cycle_start_date": start_date, "calls": calls,
                "waited_s_total": round(elapsed, 1), "cadence_game_days": cap_days,
                "stopped": stopped})
    out = dict(env_ or {})
    out["result"] = res
    return out
