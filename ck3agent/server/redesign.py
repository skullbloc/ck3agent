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
  - This module holds NO bridge I/O. mcp_server.py owns the socket and pause-flip;
    it passes a `BridgeGateway` object that exposes `read(action, params)` /
    `write(action, params)` coroutines plus pause helpers. Each tool here builds the
    bridge calls and shapes the result. This keeps the contract (which bridge actions
    are reachable) inspectable in one file.
  - Backward-compat: the old 11 tools (observe-old/query/discover/act/steer/intent/
    game/control/bridge/pass_turn) remain registered in mcp_server.py unchanged. These
    8 are ADDITIVE (SPEC §6.2 Phase 1).

Every bridge action referenced below maps 1:1 to a §3 dispatch_router entry — see the
TOOL_BRIDGE_ACTIONS conformance map at the bottom of this file (the §3 checklist).
"""

from __future__ import annotations

import asyncio
import re
from typing import Any, Optional

# Reuse the existing resolvers — the redesign tools delegate dispatch to them so the
# agent<->bridge contract (inv_bridge-map.md) stays single-sourced.
import tools as _tools
#: ck-ba35. THE pre-dispatch validator, not a second copy of it: the
#: legacy `act` tool in mcp_server.py calls this same function, so the two
#: surfaces cannot disagree about which field a verb requires.
from action_selector import validate_act_params as _validate_act_params


# ===========================================================================
# §1.4 the gate enum + §1.2 why_blocked model
# ===========================================================================

# Closed gate set (SPEC §1.4). Each maps a documented bridge refusal reason to a
# stable machine enum carrying class/severity/retryable. An UNMAPPED refusal becomes
# gate:"unknown" with the raw reason in `summary` (the unmapped-refusal rule).
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
    "requires_paused":           {"class": "pause",          "severity": "transient", "retryable": True},
    "requires_unpaused":         {"class": "pause",          "severity": "transient", "retryable": True},
    "ai_refuses_self_disadvantaging": {"class": "steer_vs_control", "severity": "invalid", "retryable": False, "is_known_limitation": True},
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
    # paused/timeout ones as `drain_timeout` through the substring pass ("timeout").
    #   scheme_start_refused    the ENGINE's own start checks said no (CreateActiveScheme
    #                           returned its null sentinel): invalid/hidden target, the
    #                           schemer cannot start this type, or an existing scheme or
    #                           cooldown against that target. A re-plan, not a retry.
    #   scheme_type_unsupported every engine start of this type seen this session
    #                           carried a task contract or artifact the bridge cannot
    #                           supply.
    #   scheme_bridge_fault     the bridge's own call faulted or could not be prepared
    #                           (crash-net fired, null objects unreadable). Contained,
    #                           and not something a retry fixes.
    "scheme_start_refused":      {"class": "target_state",   "severity": "invalid", "retryable": False},
    "scheme_type_unsupported":   {"class": "bridge",         "severity": "invalid", "retryable": False, "is_known_limitation": True},
    "scheme_bridge_fault":       {"class": "bridge",         "severity": "invalid", "retryable": False, "is_known_limitation": True},
    # Write-action-legitimacy: a state-/resource-granting verb the agent must NEVER
    # mint (the scripted_effect EXECUTE allow-list refuses it; the gate keeps its old name). A
    # hard, non-retryable refusal — earn the resource in-game, do not cheat it.
    "console_command_denied":    {"class": "legitimacy",     "severity": "invalid", "retryable": False, "is_known_limitation": True},
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
    "actor_dead_or_stale":       {"class": "bridge",         "severity": "transient", "retryable": True},
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
    # A steered-war intent that dispatched but has not yet been confirmed persisted.
    # The war often LANDS a tick or two later (the intent drains asynchronously) — this
    # is NOT a hard block. severity:"pending", retryable:False (VERIFY, do not re-declare).
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
    "unknown":                   {"class": "unknown",        "severity": "blocked", "retryable": False},
}

# Maps documented raw bridge refusal strings → a gate enum. Maintained next to the
# dispatch resolver (the unmapped-refusal rule: anything not here ⇒ gate:"unknown").
RAW_REASON_TO_GATE: dict[str, str] = {
    "game_paused": "requires_unpaused",
    "game_unpaused": "requires_paused",
    "cannot_act_as_player": "cannot_act_as_player",
    "experimental_write_disabled": "experimental_write_disabled",
    "no_living_actor": "actor_dead_or_stale",
    "actor_dead": "actor_dead_or_stale",
    "claim_not_pressed": "claim_not_pressed",
    "target_not_independent": "target_not_independent",
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
    "bridge_disconnected": "bridge_hung",
    "not_implemented_pending_re": "gate_introspection_unavailable",
    "refused_not_buildable": "target_not_assignable",
    # A dispatch that drained without a same-call confirmation — the steered war often
    # lands a tick later. Map to confirmation_pending (pending, NOT blocked) so no path
    # mislabels an async-landing war as a hard rejection.
    "intent_not_completed_within_5s": "confirmation_pending",
    "sent_no_confirmation": "confirmation_pending",
    # create_title (#113) typed rejections — the bridge self-polls 12s then reports
    # status:"rejected" + one of these reasons (intent_dispatch.cpp). The de-jure-control
    # refusal carries the measured X_of_Y counts (parsed in _confirm_create_title).
    "create_title_refused_insufficient_de_jure_control": "de_jure_control",
    "create_title_refused_by_engine_gate": "create_title_engine_gate",
    "title_key_not_found": "target_not_assignable",
    "not_confirmed_within_12s": "confirmation_pending",
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
    "already_present_or_constructing": "already_in_that_state",
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
    "scheme_type_needs_contract_or_artifact": "scheme_type_unsupported",
    "scheme_type_not_found": "name_not_found",
    "scheme_type_required": "missing_required_argument",
    "target_id_required": "missing_required_argument",
    "scheme_id_required": "missing_required_argument",
    "cannot_scheme_against_self": "target_ineligible",
    "not_scheme_owner": "target_ineligible",
    "scheme_not_found": "name_not_found",
    "owner_not_found": "actor_dead_or_stale",
    "owner_not_alive": "actor_dead_or_stale",
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
    "name_not_found": "the name given does not exist in the game's database",
    "decision_unavailable": "the decision exists but is not available to this character now",
    "ai_potential_not_met": "the game's own ai_potential gate on this action is not satisfied",
    "no_change_already_set": "the write was a no-op: the state already matched",
    "already_in_that_state": "the write was a no-op: the state already matched",
    "experimental_write_disabled": "this write is off by default in the bridge and an agent cannot enable it",
    "unknown_verb": "no tool wraps that action, so it is not callable however it is named elsewhere",
    "owned_by_other": "another control session already holds that character",
    "last_claim_of_control_session": "this is the seat's last character and releasing it is refused",
    "clock_owned_by_pipeline": "the clock belongs to the harness; a session cannot pause, unpause or set speed",
    "actor_not_claimed": "the acting character is not claimed by this session",
    "actor_dead_or_stale": "the acting character id is dead or stale and must be re-resolved",
    "no_ruler_bound": "no ruler is bound to this session, so there is no actor to read or act for",
    "actor_unresolved": "character_id=0 got no usable reply; the actor is unknown, which does not prove nothing is bound",
    "target_ineligible": "the target was rejected by the game's own eligibility check",
    "confirmation_pending": "the call was accepted and its verdict has not arrived yet",
    "cb_query_busy": "the shared casus-belli slot stayed busy for the whole retry budget",
    "de_jure_control": "the title requires de-jure counties the character does not hold",
    "not_at_peace": "the action requires peace and the character is at war",
    "scheme_start_refused": "the game's own scheme-start checks refused it: the target is not valid for this scheme, the schemer cannot start it, or a scheme or cooldown against that target already exists",
    "scheme_type_unsupported": "the game only starts this scheme type with a task contract or artifact, which the bridge cannot supply",
    "scheme_bridge_fault": "the bridge's call into the game's scheme code faulted or could not be prepared; it was contained, so check query(domain='schemes') before assuming nothing changed",
}

#: Gates that fire REGARDLESS of the arguments. `scope: "generic"` tells a seat
#: that re-sending with different arguments cannot help, which is precisely the
#: distinction the 80 byte-identical retries were missing.
GENERIC_GATES: frozenset = frozenset({
    "experimental_write_disabled", "unknown_verb", "clock_owned_by_pipeline",
    "ai_controller_unbound", "last_claim_of_control_session",
    "gate_introspection_unavailable", "actor_not_claimed",
})

#: Where a next step is genuinely derivable from the gate. Anything not here
#: gets NO `next` key.
GATE_NEXT: dict[str, str] = {
    "missing_required_argument": "call do(action_class=<this intent's class>) to enumerate valid arguments",
    "name_not_found": "call do(action_class=<this intent's class>) to enumerate valid names",
    "decision_unavailable": "call do(action_class='decisions') to see which decisions are available now",
    "unknown_verb": "the callable intents are the `intent` enum on this tool",
    "actor_not_claimed": "claim the character with session(action='claim') first",
    "actor_dead_or_stale": "re-resolve the character id and retry",
    "no_ruler_bound": "bind one: session(action='control', character_id=<id>), then session(action='whoami')",
    "actor_unresolved": "check session(action='health'), then session(action='whoami')",
    "owned_by_other": "that character is not free; choose one this seat holds",
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

    ck-r3fs: A LATE `unknown` IS RESOLVED HERE, at the choke point, because not
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

    Only an exact code match resolves. Prose stays `unknown`, loudly: a
    `summary` of "faction power 80% (>=80 tripwire)" is not a code and must not
    be bucketed by a substring. When a late resolve happens it is RECORDED
    (`gate_resolved_from`), so a census can tell a gate the caller knew from one
    this function recovered.
    """
    resolved_from = ""
    if gate == "unknown":
        for candidate in (raw_reason, summary):
            key = str(candidate or "").strip()
            if key and key in RAW_REASON_TO_GATE:
                gate = RAW_REASON_TO_GATE[key]
                resolved_from = key
                break
    meta = GATE_TABLE.get(gate, GATE_TABLE["unknown"])
    out: dict[str, Any] = {
        "gate": gate,
        "class": meta["class"],
        "severity": severity or meta["severity"],
        "retryable": meta["retryable"],
        "is_known_limitation": bool(meta.get("is_known_limitation", False)),
        "summary": summary or (raw_reason if gate == "unknown" else gate),
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
EXPERIMENTAL_WRITES: tuple = ("faction_action", "break_alliance")
# scheme_action left the list 2026-10-18 after live validation (lane-scheme).


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
    summary = (f"'{bridge_action}' is off by default in the bridge (not yet validated "
               f"crash-free). An operator can arm it for a controlled run by adding the "
               f"line '{bridge_action}' to {path or 'the ck3-enabled-writes file in the bridge state dir'}; "
               f"the seat cannot.")
    return why_blocked("experimental_write_disabled", summary=summary, fix=fix,
                       raw_reason="experimental_write_disabled")


def map_raw_reason(raw_reason: str, summary: str = "") -> dict[str, Any]:
    """Map a documented bridge refusal string to a why_blocked. Unmapped ⇒ gate:unknown
    carrying the raw reason in `summary` (loud, never silently bucketed)."""
    if not raw_reason:
        return why_blocked("unknown", summary=summary or "bridge refused with no reason")
    key = str(raw_reason).strip()
    gate = RAW_REASON_TO_GATE.get(key)
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
        return why_blocked("unknown", summary=summary or key, raw_reason=key)
    return why_blocked(gate, summary=summary or key, raw_reason=key)


# ===========================================================================
# §1.1 the uniform envelope
# ===========================================================================

def envelope(tool: str, *, ok: bool = True, result: Any = None,
             verdict: Optional[str] = None, blocked: Optional[dict] = None,
             actor: Optional[dict] = None, game: Optional[dict] = None,
             delta: Optional[list] = None, pending: Optional[dict] = None,
             warnings: Optional[list] = None, errors: Optional[list] = None,
             bridge_health: str = "ok") -> dict[str, Any]:
    """Build the uniform top-level envelope (SPEC §1.1). `data` is always a parsed
    object; a partial read never masquerades as success (errors[] non-empty ⇒ ok:false)."""
    env: dict[str, Any] = {
        "ok": ok and not errors,
        "tool": tool,
        "result": result if result is not None else {},
        "blocked": blocked,
        "warnings": warnings or [],
        "errors": errors or [],
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
        sect = char.get("character") if isinstance(char.get("character"), dict) else char
        cid = _as_int(sect.get("character_id") or sect.get("id") or requested_id) or requested_id
        # query_character emits the name at the top level via emit_char_names ("name"),
        # but some read paths nest or use first_name/full_name — accept any of them so
        # actor.name is never falsely null (the live envelope bug).
        name = (sect.get("name") or sect.get("full_name") or sect.get("first_name")
                or char.get("name"))
        if "alive" in sect:
            alive = bool(sect.get("alive"))
        # lane-namekey: a name the bridge has not localized yet is PENDING, not
        # absent. The bridge omits `name` and sends `name_key` (the game's
        # localization key, `E_thelred` for Æthelred — not a name) plus
        # `name_pending: true` until the game thread resolves it on a tick; a
        # paused game resolves nothing. Carry that state through so the reader
        # of this echo knows a null name means "not yet", never "unnamed".
        if not name and (sect.get("name_pending") or char.get("name_pending")):
            pending = True
            pending_key = sect.get("name_key") or char.get("name_key")
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
    observe_v2 reported zeros as the ruler's state with ok=true.
    """
    sect = reply.get("character") if isinstance(reply, dict) and isinstance(
        reply.get("character"), dict) else reply
    if isinstance(sect, dict) and _as_int(sect.get("character_id") or sect.get("id")) > 0:
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

# 10 axes: honor, greed, rationality, energy, boldness, zeal, vengefulness,
# compassion, sociability, dominance. Values -100..100. Tables encode the memory
# doctrine (expansion = bold/energetic; trough = defensive/dominant; etc.).
PHASE_GENOME: dict[str, dict[str, int]] = {
    "expansion": {
        "boldness": 85, "energy": 90, "greed": -20, "dominance": 60,
        "rationality": 50, "vengefulness": 30, "compassion": -10,
        "honor": 20, "zeal": 40, "sociability": 30,
    },
    "rebuild": {
        "boldness": 45, "energy": 55, "greed": 0, "dominance": 60,
        "rationality": 70, "vengefulness": 10, "compassion": 30,
        "honor": 40, "zeal": 30, "sociability": 50,
    },
    "succession_trough": {
        "boldness": 40, "energy": 55, "greed": 0, "dominance": 90,
        "rationality": 80, "vengefulness": 10, "compassion": 20,
        "honor": 40, "zeal": 30, "sociability": 40,
    },
    "stalled_siege": {
        # Unstick a stalled siege: max energy/boldness, patience (no white-peace).
        "boldness": 95, "energy": 100, "greed": -20, "dominance": 70,
        "rationality": 40, "vengefulness": 40, "compassion": -20,
        "honor": 10, "zeal": 50, "sociability": 20,
    },
    "fragile_minority": {
        # Turtle a fragile minority — never raid, crush threats.
        "boldness": 30, "energy": 50, "greed": 0, "dominance": 90,
        "rationality": 80, "vengefulness": 10, "compassion": 40,
        "honor": 50, "zeal": 30, "sociability": 50,
    },
    "consolidate": {
        "boldness": 35, "energy": 60, "greed": -10, "dominance": 70,
        "rationality": 70, "vengefulness": 20, "compassion": 30,
        "honor": 40, "zeal": 40, "sociability": 50,
    },
}

# Subsystem toggles per phase. NOTE (SPEC §2 docstring): the engine does NOT enforce
# CallAllies — these are advisory; the genome does the real work.
PHASE_SUBSYSTEMS: dict[str, dict[str, bool]] = {
    "expansion":          {"DeclareWar": True,  "Decisions": True},
    "rebuild":            {"DeclareWar": False, "Decisions": True},
    "succession_trough":  {"DeclareWar": False, "Decisions": True},
    "stalled_siege":      {"DeclareWar": True,  "Decisions": True},
    "fragile_minority":   {"DeclareWar": False, "Decisions": True},
    "consolidate":        {"DeclareWar": False, "Decisions": True},
}

PHASES = list(PHASE_GENOME.keys())


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
    "muster":         {"type": "raise_armies"},   # mode switch below
    # REMOVED: order_siege intent def deleted per design (agent does not drive army micro).
    "men_at_arms":    {"type": "maa"},
    "hire_merc":      {"type": "hire_mercenary"},
    "move_capital":   {"type": "move_capital"},
    "set_obligation": {"type": "contract"},
    "respond_event":  {"type": "event"},
    "scheme":         {"type": "scheme"},
    "faction":        {"type": "faction"},
}

# ck-7fyl / #75 — scripted_effect is handled by a DEDICATED branch in do_impl (it dispatches
# the bridge `execute_scripted_effect` action directly, NOT via resolve_act, which has no
# scripted_effect act-type). It is INTENTIONALLY NOT in INTENT_TO_ACT so resolve_act never
# sees it. Listed in INTENTS so the schema/enumeration advertises it as available.
_SCRIPTED_EFFECT_INTENT = "scripted_effect"

# GAP 3 (re_toolgap_break_alliance.md) — break_alliance is a DEDICATED branch in do_impl
# (resolve_act in tools.py hard-raises for it; the bridge `break_alliance` action is a
# DIRECT command, not the resolve_act act-type path). INTENTIONALLY NOT in INTENT_TO_ACT so
# resolve_act never sees it. Listed in INTENTS so the schema advertises it. The bridge handler
# is built but DEFAULT-OFF + observer-save-gated (g_enable_break_alliance) — while OFF the
# agent gets the bridge's honest break_alliance_disabled refusal surfaced as a typed blocked
# verdict, NEVER a silent ok. Arm via session(action="set_experimental", flag="break_alliance").
_BREAK_ALLIANCE_INTENT = "break_alliance"

# CAIWarCoordinator offensive-stance DRIVE (re_caiwarcoordinator_FINAL.md §5 /
# re_steered_war_offensive_root.md) — a DEDICATED branch in do_impl (resolve_act has no
# such act-type; the bridge `force_offensive_stance` action is a DIRECT command). This is
# the per-war DRIVE that enqueues WAR_FORCE_OFFENSIVE_STANCE on the engine's coordinator
# tick so a force-declared war lands an ATTACKER-OFFENSIVE stance and actually sieges. It
# is distinct from the SETTER (set_force_offensive_stance / session set_experimental
# force_offensive_stance) which only ARMS the default-OFF g_force_offensive_stance flag.
# While the flag is OFF the bridge rejects the drive (disabled_flag_off) and we surface a
# typed experimental_write_disabled blocked verdict — never a silent ok. INTENTIONALLY NOT
# in INTENT_TO_ACT (so resolve_act never sees it); listed in INTENTS so the schema
# advertises it. Args: {war_id, attacker_id} (attacker_id defaults to character_id).
_FORCE_OFFENSIVE_STANCE_INTENT = "force_offensive_stance"

INTENTS = list(INTENT_TO_ACT.keys()) + [_SCRIPTED_EFFECT_INTENT, _BREAK_ALLIANCE_INTENT,
                                        _FORCE_OFFENSIVE_STANCE_INTENT]

# enumerate-mode action_class → the bridge enumeration call(s).
#: ck-ycim: FROM tools.py, which is where the agent-facing schema is built, so
#: the enum an agent reads and the list this module validates against cannot
#: disagree. They did: the schema had nine of these and this had ten.
ACTION_CLASSES = list(_tools.ACTION_CLASSES)

# The formable-title candidate set. Bounded BY DESIGN (mirrors the
# decisions-enumerate bound) — never a DB-wide scan.
#
# ck-zgh4 deploy-51: this used to BE the previous project's win-line — k_danelaw,
# k_england, k_ireland, k_scotland, k_wales, e_britannia — as the DEFAULT set
# every seat was shown when it asked what it could form. That is the objective
# wearing a candidate list. The default is now empty: a caller that wants
# candidates tested passes them (`do(action_class="formable_titles",
# candidates=[...])`), and the enumerate path says so rather than answering
# about six British titles the seat may have no relation to.
FORMABLE_TITLE_CANDIDATES: list[str] = []

# O6: the documented self-disadvantage interaction class the AI refuses forever.
# Conservative denylist (only the documented A6 class) → immediate invalid, never
# enqueues a never-draining intent.
SELF_DISADVANTAGE_INTERACTIONS = {
    "disinherit", "disinherit_interaction",
    "grant_titles", "grant_title", "grant_title_interaction",
}


#: Intents whose bridge handler requires an explicit actor_id > 0 (no controlled-
#: primary fallback): do_dispatch_scheme_intent, do_dispatch_faction_intent.
_EXPLICIT_ACTOR_INTENTS: frozenset = frozenset({"scheme", "faction"})


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
        # BOTH SPELLINGS ARE STILL ACCEPTED FROM THE AGENT — `target_id` is what
        # do(action_class="council_tasks") prints in its own do_call rows, and a
        # printed call that its executor rejects is the defect the lords spent a
        # year on. The exact name wins when both are given.
        base["target_title_id"] = _as_int(a.get("target_title_id") or a.get("target_id"))
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
    elif intent == "muster":
        if (a.get("mode") or "armies") == "raiders":
            base["type"] = "raid"
        else:
            base["type"] = "raise_armies"
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
    elif intent == "respond_event":
        base["event_id"] = a.get("event_id", "")
        base["option"] = _as_int(a.get("option"))
        base["instance_id"] = _as_int(a.get("instance_id"))
    elif intent == "scheme":
        base["action"] = a.get("action", "start")
        base["target_id"] = _as_int(a.get("target_id"))
        base["scheme"] = a.get("scheme_type") or a.get("scheme") or "murder"
        base["scheme_id"] = _as_int(a.get("scheme_id"))
    elif intent == "faction":
        base["faction_action"] = a.get("action", "create")
        base["faction_id"] = _as_int(a.get("faction_id"))
        base["target_id"] = _as_int(a.get("target_id"))
        base["faction_type"] = a.get("faction_type", "independence_faction")
    return base


# ===========================================================================
# Tool conformance map (SPEC §3) — every existing bridge action each tool composes.
# This is the auditable non-regression checklist; it asserts NO invented capability.
# ===========================================================================

TOOL_BRIDGE_ACTIONS: dict[str, list[str]] = {
    "observe": [
        "query_dejure_progress", "query_succession", "query_succession_simulation",
        "query_title_succession_map",
        "query_faction_threat", "query_realm", "get_active_wars", "query_character",
        "query_county_details", "query_council", "get_ai_personality", "get_tick_info",
        "query_war_declarations", "query_scheme_inits", "query_council_tasks",
        "get_ai_evaluations", "get_character_subsystems",
    ],
    "plan_war": [
        "query_conquest_targets", "query_valid_war_targets", "query_cb_validity_strict",
        "query_cbs_for_target", "query_claims", "query_alliances", "query_call_to_arms",
        "query_armies", "query_levy_readiness", "query_dejure_progress", "query_succession",
    ],
    "cbs": ["query_cbs_for_target"],
    "declare_war": [
        "declare_war", "call_war_create_core", "set_cb_selector_override",
        "query_cb_validity_strict", "query_claims", "get_active_wars",
    ],
    "succession_plan": [
        "query_succession", "query_succession_simulation", "query_partition_preview",
        "query_title_succession_map", "query_title_succession", "get_active_wars",
    ],
    "set_phase": ["set_ai_personality", "set_character_subsystems", "get_tick_info",
                  "get_ai_personality"],
    "do": [
        # execute → resolve_act dispatch targets:
        "create_title", "send_decision", "send_interaction", "set_character_faith",
        "set_character_culture", "set_doctrine", "change_realm_law", "set_title_law",
        "set_council_task", "appoint_councillor", "set_lifestyle_focus",
        "construct_building", "raise_armies", "raise_raiders", "maa_recruit",
        "maa_station", "maa_unstation", "hire_mercenary", "move_capital",
        "set_obligation_level", "respond_event", "scheme_action", "faction_action",
        # REMOVED: move_army verb deleted per design (agent does not drive army micro).
        # enumerate:
        "list_interactions", "query_interaction_acceptance", "list_valid_decisions",
        "list_decisions_annotated", "query_council_tasks", "query_available_law_changes",
        "query_fabricate_targets", "query_hireable_mercenaries", "query_joinable_factions",
        "query_maa_types",
        "query_building_types", "construction_check", "query_imprisonment_reasons",
        "query_dejure_progress",
        # ck-7fyl/#75 scripted_effect intent (resolve-only validated; EXECUTE experimental):
        "execute_scripted_effect",
    ],
    "inspect": [
        "query_character", "query_opinion", "query_opinion_breakdown", "query_relations",
        "query_lifestyle", "query_hooks", "query_realm", "query_vassal_loyalty",
        "query_realm_laws", "query_succession", "query_succession_simulation",
        "query_title_succession", "query_claims", "query_title_claimants",
        "query_title_hierarchy", "query_wars", "query_war_score_breakdown",
        "query_call_to_arms", "query_armies", "query_factions", "query_faction_threat",
        "query_joinable_factions", "query_marriage_candidates", "query_county_details",
        "query_county_neighbors", "query_development_growth", "query_dynasty",
        "query_dejure_progress", "query_schemes", "query_incoming_schemes",
        "query_alliances", "query_council", "query_research_speed", "query_lifestyle",
        "query_title_succession_map",
    ],
    "session": [
        "set_controlled_character", "release_character", "get_controlled_roster",
        "pause_game", "unpause_game", "set_speed", "set_auto_respond_events",
        "get_bridge_state", "who_holds_the_lock", "get_offset_validation", "get_tick_info",
        # EXPERIMENTAL-unvalidated default-OFF flag setters (FINAL specs; bridge ships them
        # INERT and the IPC setter lands with the hook — agent surface ready ahead of it,
        # honest experimental_flag_unavailable until then):
        "set_offgoal_veto", "set_break_alliance_enabled", "set_force_offensive_stance",
        # REMOVED: set_move_army_enabled deleted per design (agent does not drive army micro).
    ],
}


# ===========================================================================
# Async tool implementations.
#
# Each takes a `gw` (gateway) object supplied by mcp_server.py exposing:
#   await gw.read(action, params)            → TIER-2 read (gw owns pause→read→restore)
#   await gw.write(action, params)           → TIER-3 / B* write (gw owns unpause→drain→restore)
#   await gw.read_unpaused(action, params)   → B* deferred read needing UNPAUSED (strict-CB etc.)
#   gw.game()                                 → {tick, date, paused, speed}
#   gw.health                                 → "ok" | "degraded" | "hung"
#   await gw.delta_since_last()              → list[str] of since-tick events (gw tracks tick)
#   await gw.multi_actor()                   → bool (multi-controlled / campaign overlay)
# All bridge I/O lives in the gateway; this module only composes + shapes.
# ===========================================================================


def _f(v, default=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


# --------------------------------------------------------------------------- de-jure settle
# re_observe_meter_0_0.md fix (B) — the agent-side settle layer for the e_brit /93 meter.
#
# ROOT (HIGH confidence): game_pause() only flips a flag byte; it does NOT barrier an
# in-flight game tick that is mid-recompute of the de-jure children vector (CPdxVector
# clear→refill). The gateway's PAUSED scope reads sub-ms after the flag flip, catching a
# transient SUB_TITLE_COUNT(0x264)==0 → total=0 → a FALSE 0/0. The bridge's tier>=4
# retry-on-zero is ~2ms; the real repopulate window is ~1.8s, so it always exhausts
# mid-window. Fix (A) lives in the bridge (settle-until-stable). Fix (B), here, makes
# the AGENT PATH specifically use the settled value even against a bridge lacking (A):
# re-issue the read on a transient 0 after a short asyncio.sleep, taking the first
# non-zero/stable result. Scoped to the meter so the common non-zero case pays nothing.
#
# A GENUINELY titular kingdom (k_danelaw) converges on a real 0 and we must NOT loop
# forever: a resolution error (title_key_not_found) or the bridge's own settled note
# ("titular_or_transient_empty_tree" with tier<4 / no tree) breaks immediately. For a
# tier-5 empire with a known 93-county tree a 0 is ALWAYS the transient, never titular,
# so the bounded poll is correct there. Budget ≤ ~1.5s (6 × 250ms) — under the ~1.8s
# settle but enough to span most of it; with bridge fix (A) landed this almost never
# loops because the bridge already returns the settled value.
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


def dejure_keys_for(realm: dict | None) -> list[str]:
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
    in. Primary title first, then the rest, de-duplicated and order-stable.
    """
    out: list[str] = []
    r = realm if isinstance(realm, dict) else {}
    pk = r.get("primary_title_key")
    if isinstance(pk, str) and pk:
        out.append(pk)
    for t in (r.get("titles") or []):
        if not isinstance(t, dict):
            continue
        k = t.get("title_key") or t.get("key")
        if isinstance(k, str) and k and k not in out:
            out.append(k)
    return out


async def _settled_dejure_pair(gw, cid: int, keys: tuple[str, ...], *,
                               max_polls: int = 6, interval_s: float = 0.25
                               ) -> list[dict[str, Any]]:
    """Both goal meters, ISSUED TOGETHER, then polled only where unsettled.

    ck-p9vt. THE MEASURED COST. `observe_v2`'s p50 is 50-52 s (about 43
    game-days) in every set, and it is the tool the seats are told to call
    first; they route around it (3,284 `query` calls against 582 `observe_v2`).
    This function is where the time was.

    `_settled_dejure` issues one read and then polls up to `max_polls` more,
    so it is 1-7 bridge reads. `observe_impl` called it TWICE IN SEQUENCE, for
    `k_ireland` and `e_britannia` — so up to FOURTEEN reads, each paying a span,
    before the main batch had started. At the 2-4 s per span ck-zgh4 measured
    live (5 verbs: 20.01 s sequential, 9.83 s batched) that alone brackets the
    observed p50.

    THE TWO READS ARE AT ONE DEPENDENCY LEVEL, verified rather than assumed:
    each takes only `character_id` and its own `title_key`, both known before
    either runs, and neither consumes the other's reply. Same test the
    `read_many` batch below documents, and the same discipline as ck-abaa:
    phase by what is actually dependent, not by the order the lines were
    written in.

    SO: one `read_many` for both first reads, then poll ONLY the keys that came
    back unsettled. The common case — both settle immediately — goes from two
    spans to ONE. The bad case stays bounded by `max_polls` per key, and a key
    that settles early stops being asked about, which the serial version could
    not do either.

    The retry exists for a real race (re_observe_meter_0_0.md fix B): a
    mid-repopulate 0/0 read during a PAUSED scope, before the tick settles,
    must not be reported as a false 0/0. That behaviour is preserved exactly —
    this changes WHEN the reads are issued, never which value is accepted.
    """
    want = list(keys)
    out: dict[str, dict[str, Any]] = {}
    batch = await gw.read_many([("query_dejure_progress",
                                 {"character_id": cid, "title_key": k}) for k in want])
    for k, g in zip(want, batch):
        out[k] = g if isinstance(g, dict) else {}

    def _settled(g: dict) -> bool:
        # Identical acceptance test to the serial version: a real resolution
        # error never settles to a tree, and a positive total is a real value.
        return g.get("error") == "title_key_not_found" or _as_int(g.get("total")) > 0

    for _ in range(max_polls):
        pending = [k for k in want if not _settled(out[k])]
        if not pending:
            break
        await asyncio.sleep(interval_s)
        again = await gw.read_many([("query_dejure_progress",
                                     {"character_id": cid, "title_key": k}) for k in pending])
        for k, g in zip(pending, again):
            if isinstance(g, dict):
                out[k] = g
    return [out[k] for k in want]


async def _settled_dejure(gw, cid: int, title_key: str, *, max_polls: int = 6,
                          interval_s: float = 0.25) -> dict[str, Any]:
    g = await gw.read("query_dejure_progress", {"character_id": cid, "title_key": title_key})
    g = g if isinstance(g, dict) else {}
    for _ in range(max_polls):
        # A real resolution error never settles to a tree — stop (do not loop pointlessly).
        if g.get("error") == "title_key_not_found":
            break
        # Already a real value — done.
        if _as_int(g.get("total")) > 0:
            break
        await asyncio.sleep(interval_s)
        g2 = await gw.read("query_dejure_progress", {"character_id": cid, "title_key": title_key})
        if isinstance(g2, dict):
            g = g2
    return g


# --------------------------------------------------------------------------- succession preview
# The per-title partition MAP folded into a concise at-a-glance summary (ck-succprev).
#
# Source: the bridge query_title_succession_map READ — for each held title it reports which
# heir gets it and under which succession law (partition vs single-heir). This consumer
# helper degrades HONESTLY when the bridge lacks the new query (unlanded / no-heirs /
# older bridge): every field is guarded; a missing/error payload yields a None-shaped
# summary with `available=False`, never a guess. No bridge writes, no engine reentry —
# it is a pure shape over the read.
#
# The summary the agent reads each reign:
#   scatters            — bool: does the NEXT succession split titles across >1 heir?
#   passes_single_heir  — bool: inverse (the whole realm passes to ONE heir)
#   eligible_heir_count — how many heirs land at least one title
#   primary_heir        — {id, name, title_count}
#   scatter_breaker     — the first cadet who peels a title off (id, name, takes[])
#   partition_titles    — [{title_id, key, name, tier}] held titles still on PARTITION law
#   single_heir_titles  — [{...}] held titles already on a single-heir law (won't split)
#   top_title           — the primary (highest-tier) title + its law
def _succession_preview_summary(sp: dict) -> dict[str, Any]:
    """Shape the bridge query_title_succession_map payload into the concise reign-watch
    summary. Guarded: any missing/error payload → available=False with a reason, never a
    fabricated split.

    The bridge emits a TITLE-keyed shape (matching do_query_succession), not an heir-keyed
    distribution:
        {
          action, character_id,
          primary_title_id, primary_title_tier, primary_heir_id, primary_heir_name,
          titles: [ {title_id, title_key, title_name, tier, succession_law,
                     title_division, partition_rank?, projected_heir_id,
                     projected_heir_name, splits_away}, ... ],
          any_splits: bool, status: "ok"
        }
    `splits_away` is true when a title's projected heir differs from the primary title's
    heir. We derive the per-heir scatter view + the partition/single-heir law buckets from
    that flat list. No guessing: a title with an unknown law/heir stays unclassified.
    """
    if not isinstance(sp, dict) or sp.get("error"):
        reason = (sp or {}).get("error") if isinstance(sp, dict) else "no_response"
        return {"available": False, "reason": reason or "no_response"}

    titles = sp.get("titles")
    # Older bridge / unlanded / unknown action: the handler is absent or returned no list.
    if not isinstance(titles, list):
        return {"available": False, "reason": "no_titles"}

    primary_heir_id = sp.get("primary_heir_id")
    if isinstance(primary_heir_id, int) and primary_heir_id < 0:
        primary_heir_id = None
    primary_title_id = sp.get("primary_title_id")

    rows = [t for t in titles if isinstance(t, dict)]

    # Per-heir scatter view, derived from the per-title projected heirs.
    by_heir: dict[int, dict] = {}
    primary_title_count = 0
    for t in rows:
        hid = t.get("projected_heir_id")
        if not isinstance(hid, int) or hid <= 0:
            continue  # unknown projected heir for this title — don't attribute it
        slot = by_heir.setdefault(hid, {"id": hid, "name": t.get("projected_heir_name"),
                                        "title_count": 0, "titles": []})
        if not slot.get("name") and t.get("projected_heir_name"):
            slot["name"] = t.get("projected_heir_name")
        slot["title_count"] += 1
        slot["titles"].append({"title_id": t.get("title_id"), "key": t.get("title_key"),
                               "name": t.get("title_name"), "tier": t.get("tier")})
        if isinstance(primary_heir_id, int) and hid == primary_heir_id:
            primary_title_count += 1

    eligible_heir_count = len(by_heir)
    primary = None
    if isinstance(primary_heir_id, int) and primary_heir_id in by_heir:
        ph = by_heir[primary_heir_id]
        primary = {"id": ph["id"], "name": ph["name"], "title_count": ph["title_count"]}
    elif sp.get("primary_heir_name"):
        primary = {"id": primary_heir_id, "name": sp.get("primary_heir_name"),
                   "title_count": primary_title_count}

    # scatter_breaker: the first title that splits_away, attributed to its heir.
    scatter_breaker = None
    for hid, slot in by_heir.items():
        if isinstance(primary_heir_id, int) and hid == primary_heir_id:
            continue
        scatter_breaker = {"id": slot["id"], "name": slot["name"], "takes": slot["titles"]}
        break
    # any_splits is the bridge's authoritative verdict; fall back to the derived view.
    scatters = bool(sp.get("any_splits")) or scatter_breaker is not None or eligible_heir_count > 1

    # Per-title law buckets, straight from the bridge's decoded succession_law /
    # title_division (no inference — the handler resolves the effective rule).
    partition_titles: list = []
    single_heir_titles: list = []
    for t in rows:
        row = {"title_id": t.get("title_id"), "key": t.get("title_key"),
               "name": t.get("title_name"), "tier": t.get("tier"),
               "law": t.get("succession_law")}
        div = t.get("title_division")
        if div == "partition":
            if t.get("partition_rank"):
                row["partition_rank"] = t.get("partition_rank")
            partition_titles.append(row)
        elif div == "single_heir":
            single_heir_titles.append(row)
        # "unset"/"n/a"/None → unclassified (don't guess).

    top_title = None
    for t in rows:
        if t.get("title_id") == primary_title_id:
            top_title = {"title_id": primary_title_id, "key": t.get("title_key"),
                         "name": t.get("title_name"), "tier": t.get("tier"),
                         "law": t.get("succession_law")}
            break

    return {
        "available": True,
        "scatters": scatters,
        "passes_single_heir": not scatters,
        "eligible_heir_count": eligible_heir_count,
        "primary_heir": primary,
        "scatter_breaker": scatter_breaker,
        "partition_titles": partition_titles,
        "single_heir_titles": single_heir_titles,
        "top_title": top_title,
        "any_splits": bool(sp.get("any_splits")),
    }


async def _resolve_actor(gw, character_id: int, tool: str):
    """§5: resolve + validate the actor. Returns (actor_echo_dict, error_envelope|None).

    Multi-actor mode REQUIRES character_id (hard actor_required). Single-controlled
    defaults to the primary but validates alive+landed (actor_dead_or_stale)."""
    multi = await gw.multi_actor()
    if multi and not character_id:
        return None, envelope(
            tool, ok=False,
            blocked=why_blocked("actor_required",
                                summary="multi-actor mode: character_id is REQUIRED on every actor-bearing call"),
            game=gw.game(), bridge_health=gw.health)
    raw_char = await gw.read("query_character", {"character_id": character_id})
    if not character_id:
        unbound = unbound_actor_block(raw_char)
        if unbound is not None:
            return None, envelope(tool, ok=False, blocked=unbound,
                                  game=gw.game(), bridge_health=gw.health)
    char = raw_char if isinstance(raw_char, dict) else {}
    is_player = bool((char.get("character") or char).get("is_player")) if isinstance(char, dict) else False
    actor = actor_echo(char, character_id)
    if character_id and not actor.get("alive", True):
        return actor, envelope(
            tool, ok=False, actor=actor,
            blocked=why_blocked("actor_dead_or_stale",
                                summary="actor is dead or stale — re-pin control via session(action='control')"),
            game=gw.game(), bridge_health=gw.health)
    if is_player:
        actor["is_player"] = True
        return actor, envelope(
            tool, ok=False, actor=actor,
            blocked=why_blocked("cannot_act_as_player",
                                summary="the human player is a valid TARGET, never an ACTOR"),
            game=gw.game(), bridge_health=gw.health)
    return actor, None


# --------------------------------------------------------------------------- observe
async def observe_impl(gw, character_id: int = 0, depth: str = "brief") -> dict[str, Any]:
    actor, err = await _resolve_actor(gw, character_id, "observe_v2")
    if err:
        return err

    warnings: list = []
    errors: list = []

    # DE-JURE COUNTY COUNTS, for the titles THIS SEAT HOLDS. query_dejure_progress
    # is the meter; both reads go through _settled_dejure so a transient
    # mid-repopulate 0/0 (the paused-scope-reads-before-the-tick-settles race) is
    # retried out rather than reported as a false 0/0.
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
        # ck-p9vt: `get_ai_personality` was a span of its own on EVERY call,
        # 150 lines below. 93% of observe_v2 calls are depth="brief" (539 of
        # 582 over d45-d50), so it was on the hot path every time. Its params
        # are built from this function's argument and nothing between here and
        # its old site consumed `pers` — read, not assumed.
        ("get_ai_personality",         {"character_id": character_id}),
    ])
    realm, ft, wars, chr, sp_raw, pers = _batch
    realm = realm if isinstance(realm, dict) else {}
    ft = ft if isinstance(ft, dict) else {}
    wars = wars if isinstance(wars, dict) else {}
    chr_sect = (chr.get("character") if isinstance(chr, dict) else None) or (chr if isinstance(chr, dict) else {})

    # ── ck-p9vt: THE KEYS COME OUT OF THE BATCH ABOVE, NOT A READ OF THEIR OWN
    #
    # ck-zgh4 left this read for me to subsume and said so: its fix needed
    # `query_realm` to derive the held-title keys, and `query_realm` was ALREADY
    # in the batch — so the shipped shape paid a span for a reply it already
    # had. The batch is simply lifted above the meter block; nothing between the
    # two positions consumed a meter, which is why it moves safely. Verified by
    # reading the span, the way this file's other notes do it.
    _dj_keys = dejure_keys_for(realm)
    _UNAVAIL_NONE = {"title": None, "have": None, "need": None, "total": None,
                     "fraction": None, "met": False,
                     "unavailable": "no held title key could be resolved for this seat"}
    _UNAVAIL_ONE = {"title": None, "have": None, "need": None, "total": None,
                    "fraction": None, "met": False,
                    "unavailable": "this seat holds only one resolvable title"}
    # ONE span for both meters instead of two serial poll loops of up to seven
    # reads each. `_settled_dejure` stays for its single-key callers.
    if len(_dj_keys) >= 2:
        sub_goal, empire = await _settled_dejure_pair(
            gw, character_id, (_dj_keys[0], _dj_keys[1]))
    elif len(_dj_keys) == 1:
        sub_goal = (await _settled_dejure_pair(gw, character_id, (_dj_keys[0],)))[0]
        empire = dict(_UNAVAIL_ONE)
    else:
        # NO FALLBACK LITERAL, which is ck-zgh4's rule and the reason the
        # objective got in: an undetermined set reports unavailable.
        sub_goal, empire = dict(_UNAVAIL_NONE), dict(_UNAVAIL_NONE)
    sub_goal = sub_goal if isinstance(sub_goal, dict) else {}
    empire = empire if isinstance(empire, dict) else {}

    def _goal_block(g, need_frac=0.51):
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
    # (ck-zgh4, live). `read_many` collapses them into one paused span while
    # keeping any ANY-polarity member on its inline fast path.
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

    gold = _f(realm.get("gold", chr_sect.get("gold")))
    debt = -gold if gold < 0 else 0

    # ── ck-m3xd: THE MONEY, AND WHAT IT CANNOT SAY ──────────────────────────
    #
    # MEASURED over d45-d50, 1,983 observations carrying `income_breakdown`:
    #   1,952 (98.4%)  EVERY component is zero -- gross_domain, net_domain,
    #                  total_expenses and net_gold -- while `total_monthly`
    #                  carries a real number. The seat is shown a breakdown
    #                  that breaks nothing down, and four zeros read as "no
    #                  expenses" rather than as "not populated".
    #      761 (38%)   `income_breakdown.total_monthly` DISAGREES with
    #                  `monthly_income`. Two fields for one quantity, differing,
    #                  with nothing saying which to believe. This is the
    #                  "income fields contradict each other" the design counted.
    #
    # THE LIMIT, STATED RATHER THAN PAPERED OVER. The design asked for
    # "income/expense lines that sum". THERE IS NO EXPENSE SIDE IN THIS DATA:
    # `total_expenses` is 0 in every one of the 1,983 observations. So a
    # reconciliation cannot be computed, and inventing one -- deriving expenses
    # as income minus a delta, say -- would be a fabricated line on a screen
    # the seat is meant to trust. What this block does instead is report the
    # figures that are real, and SAY what is missing and when the two totals
    # disagree. A field that cannot be sourced is absent with a reason, never a
    # zero: four zeros are indistinguishable from a realm with no costs.
    _bd = realm.get("income_breakdown")
    _bd = _bd if isinstance(_bd, dict) else {}
    _mi = realm.get("monthly_income")
    _tm = _bd.get("total_monthly")
    _components = ("gross_domain", "net_domain", "total_expenses", "net_gold")
    _populated = [k for k in _components
                  if isinstance(_bd.get(k), (int, float)) and _bd.get(k) != 0]
    money: dict[str, Any] = {
        "gold": round(gold, 2),
        "in_debt": gold < 0,
        "monthly_income": round(_f(_mi), 2) if isinstance(_mi, (int, float)) else None,
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
    # TWO TOTALS: name the disagreement rather than silently preferring one.
    if (isinstance(_mi, (int, float)) and isinstance(_tm, (int, float))
            and abs(_f(_mi) - _f(_tm)) > 0.051):
        money["totals_disagree"] = {
            "monthly_income": round(_f(_mi), 2),
            "income_breakdown.total_monthly": round(_f(_tm), 2),
            "note": ("two fields report the monthly total and they differ; "
                     "neither is preferred here because nothing in the reply "
                     "says which the engine means"),
        }
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
    at_war = bool(wars.get("wars")) or bool(chr_sect.get("is_at_war"))
    age = _f(chr_sect.get("age"))
    # O5 fallback: no engine death-ETA → age heuristic, with a warning.
    eta = None
    if age:
        eta = max(0, int(65 - age))
        warnings.append({"code": "war_duration_unavailable", "is_known_limitation": True,
                         "detail": "death ETA is an age-derived heuristic (O5); no engine estimate"})
    g = gw.game()
    speed = _as_int(g.get("speed"))
    speed_safe = not (speed >= 4 and (at_war or (eta is not None and eta <= 3)))

    reasons: list = []
    if debt > 0:
        reasons.append(why_blocked("insufficient_gold", summary=f"in debt {int(debt)}"))
    if faction_max >= 80:
        reasons.append(why_blocked("unknown", summary=f"faction power {int(faction_max)}% (>=80 tripwire)"))
    if stress >= 150:
        reasons.append(why_blocked("unknown", summary=f"stress {int(stress)} (mental break risk)"))
    in_trough = bool(reasons) or (at_war and eta is not None and eta <= 3)

    # ck-zgh4 deploy-51: `kill_zone` was a single survival VERDICT named after
    # the previous project's doctrine ("the trough"). The facts it was computed
    # from are what a ruler can see in the game; the verdict is a conclusion
    # drawn for the seat. Facts stay, under a name that states what they are.
    standing = {
        "succession_eta_years": eta,
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
        if not sub_b["met"]:
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
    # inside `depth == "full"`, and 93% of observe_v2 calls are `brief` (539 of
    # 582, measured over d45-d50). So the war reached the seat as ONE BOOLEAN,
    # `standing.at_war`, on the path it actually took. The information existed;
    # it was off the hot path.
    #
    # COSTS NO SPAN. Every field below is already in the `get_active_wars` reply
    # fetched by the batch above — war_score, war_score_source, battle_score,
    # ticking_score, sieges, my_side. Nothing new is read.
    #
    # AND IT SAYS WHEN THE SCORE CANNOT BE TRUSTED, which is the whole point. A
    # `war_score` of 0 is reported as 0 — never massaged — but when the score is
    # 0 while OTHER evidence of movement exists (sieges in progress, a battle
    # score, a ticking score, or a source that is cached/absent), the entry
    # carries `score_is_uninformative: true` and names what it saw. That is a
    # statement about the INSTRUMENT, not advice about the war: no "abandon
    # this", no recommended lever. Parity with a player's screen, which always
    # shows the wars you are in.
    wars_list = wars.get("wars") if isinstance(wars.get("wars"), list) else []
    war_rows = []
    for w in wars_list:
        if not isinstance(w, dict):
            continue
        score = _as_int(w.get("war_score"))
        src = w.get("war_score_source")
        sieges = w.get("sieges") if isinstance(w.get("sieges"), list) else []
        battle = _as_int(w.get("battle_score"))
        ticking = _as_int(w.get("ticking_score"))
        # The evidence that contradicts a zero. Each is a FACT from the reply.
        contrary = []
        if sieges:
            contrary.append(f"{len(sieges)} siege(s) in progress")
        if battle:
            contrary.append(f"battle_score {battle}")
        if ticking:
            contrary.append(f"ticking_score {ticking}")
        if not src:
            contrary.append("war_score_source absent")
        row = {
            "war_id": w.get("war_id"),
            "name": w.get("name"),
            "my_side": w.get("my_side") or w.get("side"),
            "war_score": score,
            "war_score_source": src,
            "sieges_in_progress": len(sieges),
            "battle_score": battle,
            "ticking_score": ticking,
        }
        if score == 0 and contrary:
            row["score_is_uninformative"] = True
            row["score_caveat"] = (
                "war_score is 0 while other evidence of movement exists: "
                + "; ".join(contrary)
                + ". Do not read 0 as parity.")
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
    sp_sum = _succession_preview_summary(sp_raw if isinstance(sp_raw, dict) else {})
    succession_block = {
        "scatters": sp_sum.get("scatters"),
        "passes_single_heir": sp_sum.get("passes_single_heir"),
        "eligible_heir_count": sp_sum.get("eligible_heir_count"),
        "primary_heir": sp_sum.get("primary_heir"),
        "scatter_breaker": sp_sum.get("scatter_breaker"),
        "succession_eta_years": eta,
        "near_trough": bool(eta is not None and eta <= 3),
        "available": sp_sum.get("available", False),
    }
    if not sp_sum.get("available"):
        succession_block["note"] = (
            "per-title partition map unavailable (" + str(sp_sum.get("reason", "unknown")) +
            ") — use succession_plan / inspect(subject='succession_preview')")
    elif sp_sum.get("scatters"):
        # A SCATTER warning the cycle loop will surface — act to reduce eligible heirs.
        warnings.append({"code": "succession_scatters", "is_known_limitation": False,
                         "detail": "next succession SCATTERS titles across "
                                   f"{sp_sum.get('eligible_heir_count')} heirs — drive eligible-heir count "
                                   "toward 1 BEFORE the trough (single_heir law / land spares as in-realm "
                                   "vassals / consolidate). See inspect(subject='succession_preview') + succession_plan."})

    # posture
    # `pers` came from the batch above (ck-p9vt); no span of its own.
    posture = {"genome": pers if isinstance(pers, dict) else {}}

    result: dict[str, Any] = {"de_jure": de_jure, "standing": standing,
                              "wars": war_block, "money": money,
                              "succession": succession_block, "posture": posture}

    if depth == "full":
        result["self"] = {
            "counties_held": sub_b["have"] + max(0, emp_b["have"] - sub_b["have"]),
            "titles_held": _as_int(realm.get("titles_held") or realm.get("title_count")),
            "gold": int(gold), "prestige": int(_f(chr_sect.get("prestige"))),
            "piety": int(_f(chr_sect.get("piety"))), "stress": int(stress),
            "government": chr_sect.get("government"), "faith": chr_sect.get("faith"),
            "culture": chr_sect.get("culture"),
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
        # Phase-2 PORT (_PHASE2_PARITY.md): the legacy `observe` evaluations+subsystems
        # sections, ported into depth='full' so retiring nothing here regresses (legacy
        # `observe` stays registered; this just gives observe_v2 parity coverage).
        evals = await gw.read("get_ai_evaluations", {"character_id": character_id})
        result["evaluations"] = evals if isinstance(evals, dict) else {}
        subs = await gw.read("get_character_subsystems", {"character_id": character_id})
        result["subsystems"] = subs if isinstance(subs, dict) else {}

    # ── ck-s8vr: `since` IS ABOUT THE SEAT, AND SAYS WHAT IT WITHHELD ───────
    #
    # MEASURED over d49+d50: `delta` is 75.5% of observe_v2's BYTES. Of its
    # 5,640 entries, 14 (0.25%) mention the actor and 100% have empty names —
    # they are Python-repr dicts of foreign wars and schemes
    # ("war: {'tick': 61, 'attacker_id': 30966, ...}") because
    # `delta_since_last` falls back to str(rec) when a record has no `summary`
    # or `name`, which the bridge's war and scheme records never do.
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
    _recs = []
    try:
        _recs = await gw.delta_records_since_last()
    except AttributeError:
        # An older gateway without the structured method. Fall back to the
        # string form rather than returning nothing, and SAY which shape it is.
        _legacy = await gw.delta_since_last()
        _recs = [{"kind": "legacy_string", "text": s} for s in (_legacy or [])]
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
        if rec.get("kind") == "legacy_string":
            _unclassified += 1
            _foreign.append(rec)
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
    return envelope("observe_v2", result=result, actor=actor, game=gw.game(),
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
def _is_de_jure_cb(cb_name) -> bool:
    """True iff a CB key belongs to the de-jure family (de_jure_cb / individual_*_de_jure_cb)."""
    return "de_jure" in str(cb_name or "").lower()

# Affordability: declaring a war costs PRESTIGE (the engine gates on it; war_validity A-class).
# The bridge does not surface a per-CB prestige cost, so we tag each row affordable-now vs
# needs-prestige from a CONSERVATIVE per-CB-family floor (vanilla base prestige costs; the
# exact number scales with tier/target but the floor is a safe "can I even afford to start
# this class of war" gate so the agent raids first instead of a guaranteed-reject declare).
# County/claim wars are cheap; subjugation/de-jure/holy wars are the expensive class.
_CB_PRESTIGE_FLOOR: dict[str, int] = {
    "county_conquest_cb": 0,
    "claim_cb": 0,
    # FIXPLAN A1 (CB-audit G1): keyed on the REAL CASUS_BELLI_DB engine names.
    # The old keys `de_jure_county_cb` / `de_jure_duchy_cb` / `holy_war_cb` are
    # PHANTOM — no such engine CB exists, so every de-jure / holy-war declare fell
    # through to _CB_PRESTIGE_FLOOR_DEFAULT (50) and got a mislabeled affordability
    # tag. The real names (cross-checked against bridge cb_semantics_for): take-all
    # de-jure = `de_jure_cb`; per-title = `individual_county_de_jure_cb` /
    # `individual_duchy_de_jure_cb`; holy war = `religious_war` (+ minor/major
    # variants). "holy_war" is only a piety-cost variable, never a CB. The substr
    # fallthrough in _cb_prestige_floor then catches the minor/major prefixes.
    "individual_county_de_jure_cb": 0,
    "duchy_conquest_cb": 50,
    "individual_duchy_de_jure_cb": 100,
    "de_jure_cb": 100,
    "religious_war": 100,
    "tribal_subjugation_cb": 300,
    "subjugation_cb": 300,
    "vassalization_cb": 100,
}
_CB_PRESTIGE_FLOOR_DEFAULT = 50   # unknown CB family: a modest non-zero floor.


def _cb_prestige_floor(cb_name: str) -> int:
    """Conservative prestige floor to START a war of this CB family (affordability tag)."""
    key = str(cb_name or "").lower()
    if key in _CB_PRESTIGE_FLOOR:
        return _CB_PRESTIGE_FLOOR[key]
    for fam, cost in _CB_PRESTIGE_FLOOR.items():
        if fam in key:
            return cost
    if "subjugation" in key:
        return 300
    return _CB_PRESTIGE_FLOOR_DEFAULT


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
        }
        all_opts.append(opt)
        if opt["valid"] and opt["dispatch_supported"]:
            valid_opts.append(opt)
    return valid_opts, all_opts


# --------------------------------------------------------------------------- CB busy-retry
# A CB-validity / cbs-for-target read can come back TRANSIENTLY busy: the bridge's
# single-flight g_cb_query slot is shared with the engine's OWN AI CB evaluations,
# and when the deferred-drain coordinator pulses the game unpaused to service our
# query the engine's concurrent eval can hold the slot → the bridge returns
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
    if status in ("ok", "needs_unpaused", "bilateral_filter_reject"):
        return False  # a real verdict (or a polarity signal the gateway handles)
    if status in ("busy", "timeout", "drain_unavailable", "busy_deferred"):
        return True
    return any(tok in err for tok in _CB_BUSY_TOKENS)


async def _cb_read_with_retry(gw, action: str, params: dict,
                              *, attempts: int = 3, backoff_s: float = 0.4) -> dict:
    """read_unpaused(action, params) with bounded retry-on-busy. Rides out a
    transient g_cb_query slot collision (engine AI vs our query) before giving up,
    so a single hot-tick contention does not make the agent fail-closed and HOLD.
    Returns the last response dict (its status/error tells the caller busy vs real)."""
    res: dict = {}
    for i in range(max(1, attempts)):
        raw = await gw.read_unpaused(action, params)
        res = raw if isinstance(raw, dict) else {}
        if not _cb_read_is_busy(res):
            return res
        if i + 1 < attempts:
            await asyncio.sleep(backoff_s)
    return res  # exhausted retries — last (still-busy) response surfaced to caller


async def _cb_read_with_retry_inline(gw, action: str, params: dict,
                                     *, attempts: int = 3, backoff_s: float = 0.4) -> dict:
    """Same bounded retry-on-busy as _cb_read_with_retry, but issues the read via
    gw.read_inline — for use INSIDE an open unpaused_window() where the single-flight lock
    and the unpause flip are already held by the window scope. Re-flipping/re-locking per
    call (read_unpaused) would re-pay the pulse the window exists to amortize. The retry
    loop is otherwise identical: it rides out a transient g_cb_query slot collision before
    surfacing the still-busy response to the caller (the #95 fail-closed-HOLD guard)."""
    res: dict = {}
    for i in range(max(1, attempts)):
        raw = await gw.read_inline(action, params)
        res = raw if isinstance(raw, dict) else {}
        if not _cb_read_is_busy(res):
            return res
        if i + 1 < attempts:
            await asyncio.sleep(backoff_s)
    return res


async def _cb_options_for_inline(gw, character_id: int, target_id: int) -> tuple[list, list, dict]:
    """Inline (in-window) variant of _cb_options_for — identical shaping, but the
    query_cbs_for_target read is issued via gw.read_inline (no per-call flip/lock) because
    the enclosing unpaused_window() already holds both. Returns (valid, all, target_state)."""
    cbs_raw = await _cb_read_with_retry_inline(
        gw, "query_cbs_for_target",
        {"attacker_id": character_id, "target_id": target_id, "timeout_ms": 6000})
    cbs_raw = cbs_raw if isinstance(cbs_raw, dict) else {}
    valid_opts, all_opts = _shape_cb_options(cbs_raw, target_id=target_id)
    return valid_opts, all_opts, (cbs_raw.get("target_state") or {})


async def _cbs_for_targets_batched(gw, character_id: int, target_ids: list[int]) -> Optional[dict]:
    """OPTIONAL batched CB-options fetch: ONE query_cbs_for_targets round-trip for ALL
    targets, when the bridge serves it. Returns {target_id: cbs_raw_dict} on success, or
    None when the handler is unserved (graceful fallback to per-target read_inline). The
    bridge handler must be crash-hardened identically to query_cbs_for_target (safe_read/
    is_mapped, same g_cb_query single-flight discipline, no name-cache drain); until it
    lands this probes once and returns None, so behavior is UNCHANGED on shipping builds."""
    if not target_ids:
        return {}
    raw = await gw.read_inline("query_cbs_for_targets",
                               {"attacker_id": character_id,
                                "target_ids": list(target_ids), "timeout_ms": 9000})
    raw = raw if isinstance(raw, dict) else {}
    err = str(raw.get("error") or raw.get("reason") or "").lower()
    if any(tok in err for tok in _UNKNOWN_CMD_TOKENS):
        return None
    rows = raw.get("targets") or raw.get("results") or raw.get("by_target")
    if rows is None:
        # Not a recognized batched shape (and not a clean busy/empty for THIS schema) →
        # treat as unserved so the caller falls back per-target. Never invents data.
        if not (raw.get("status") or raw.get("cbs")):
            return None
    out: dict = {}
    if isinstance(rows, dict):
        for k, v in rows.items():
            out[_as_int(k)] = v if isinstance(v, dict) else {}
    elif isinstance(rows, list):
        for entry in rows:
            if isinstance(entry, dict):
                out[_as_int(entry.get("target_id"))] = entry
    return out


async def _strict_validity_batched(gw, character_id: int, target_ids: list[int]) -> Optional[dict]:
    """OPTIONAL batched strict-CB validity (R2): ONE query_cb_validity_strict_batch
    round-trip for ALL targets, when the bridge serves it. Returns {target_id:
    strict_raw_dict} (each value shaped exactly like a single query_cb_validity_strict
    response, i.e. carrying `results`), or None when the handler is unserved (graceful
    fallback to per-target read_inline). Must be crash-hardened identically to the
    single-target handler (safe_read/is_mapped, same g_cb_query single-flight discipline,
    no name-cache drain); until it lands this probes once and returns None so behavior is
    UNCHANGED on shipping builds. Issued via read_inline inside the open unpaused_window
    (the flip + single-flight lock are already held by the window)."""
    if not target_ids:
        return {}
    raw = await gw.read_inline("query_cb_validity_strict_batch",
                               {"actor_id": character_id,
                                "target_ids": list(target_ids),
                                "include_ineligible": True, "timeout_ms": 12000})
    raw = raw if isinstance(raw, dict) else {}
    err = str(raw.get("error") or raw.get("reason") or "").lower()
    if any(tok in err for tok in _UNKNOWN_CMD_TOKENS):
        return None
    rows = raw.get("targets") or raw.get("by_target") or raw.get("verdicts")
    if rows is None:
        return None  # not a recognized batched shape → fall back per-target
    out: dict = {}
    if isinstance(rows, dict):
        for k, v in rows.items():
            out[_as_int(k)] = v if isinstance(v, dict) else {}
    elif isinstance(rows, list):
        for entry in rows:
            if isinstance(entry, dict):
                # accept either {target_id, results:[...]} or {target_id, ...nested strict}
                out[_as_int(entry.get("target_id"))] = entry
    return out


async def _cb_options_for(gw, character_id: int, target_id: int) -> tuple[list, list, dict]:
    """Fetch + shape the CB tradeoff list for ONE (actor,target). UNPAUSED engine read:
    query_cbs_for_target is serviced on the game thread (g_cb_query slot) and times out
    while paused, exactly like query_cb_validity_strict — so route it through
    read_unpaused (single-flight) explicitly, NOT plain read (the _AgentGateway adapter
    has no _UNPAUSED_READS routing). Returns (valid_options, all_options, target_state)."""
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
# needs a one-shot live probe on an at-war record — FIXPLAN B2). This Python layer does NOT
# arm that flag (arming + the "prefer engine stance over synthesis" switch is FLAG-GATED B2
# work that needs an isolated live restart; deferred here), so the flag-OFF served response
# carries occupation but no stance label, and we layer it onto the SYNTHESIZED agent-facing
# "war stance" view from the two reads the bridge serves UNGATED: get_active_wars (side +
# war_score) + query_war_score_breakdown (occupation_score, the siege signal).
# query_war_stance is classified `is_game_state_query` (snapshot.cpp) so it is served INLINE
# while the game is PAUSED — the agent's observe state. (It was previously unclassified →
# queued to the game-thread queue → never drained while paused → MCP timeout → cached
# permanently into _WAR_STANCE_UNSERVED, which silently degraded this gateway to synthesis-
# only for its whole life. Fixed in the bridge.) _native_war_stance below already prefers
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
# NOTE: with the pause-classification fix (query_war_stance is now is_game_state_query),
# a PAUSED call is served inline instead of timing out — so the timeout branch below no
# longer false-caches a capable build into synthesis-only.
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
    if not (raw.get("wars") or raw.get("stances") or raw.get("success")):
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
            # White-peace is the clean exit (defender accepts if not winning); unilateral
            # defeat (truce) is the always-works fallback. Agent fires via do(intent=interact).
            exit_lever = {
                "preferred": "end_war_attacker_white_peace_interaction",
                "fallback": "end_war_attacker_defeat_interaction",
                "how": "do(intent='interact', args={'interaction': '<lever>', 'target_id': <defender_id>})",
                "why": ("attacker occupation_score=0 (siege never started — the controlled-ruler "
                        "impasse / defensive stance)" if occ0 else f"losing as attacker (war_score {war_score})"),
            }

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
                 "army across a succession. To make a controlled ruler actually siege, RELEASE "
                 "control for the war's duration (re_control_suppresses_war.md) or arm the "
                 "EXPERIMENTAL force_offensive_stance flag."),
    }


# --------------------------------------------------------------------------- cbs
async def cbs_impl(gw, target_id: int = 0, character_id: int = 0) -> dict[str, Any]:
    """'Show me EVERY casus belli I can use against this target, with tradeoffs.'
    Returns the full curated CB list (county/duchy/realm/faith_counties scope, what each
    GRANTS on victory, why-unmet, dispatch-marshalling hint) so the agent can weigh
    e.g. tribal_subjugation (whole realm, once/lifetime) vs county_conquest (one county).
    UNPAUSED engine read."""
    actor, err = await _resolve_actor(gw, character_id, "cbs")
    if err:
        return err
    if not target_id:
        return envelope("cbs", ok=False, actor=actor, game=gw.game(),
                        blocked=why_blocked("unknown", summary="target_id is required"),
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
async def plan_war_impl(gw, character_id: int = 0, goal_scoped: bool = True,
                        ally_free_only: bool = True, exclude_straddling: bool = True,
                        max_results: int = 8) -> dict[str, Any]:
    actor, err = await _resolve_actor(gw, character_id, "plan_war")
    if err:
        return err
    warnings: list = []

    # Actor prestige (for the affordability tag). A single read up front; declaring a
    # war is prestige-gated, so each row is tagged affordable-now vs needs-prestige
    # against a conservative per-CB-family floor (the agent raids to fund before a
    # guaranteed-reject declare). Cheap read — falls back to 0 (= tag everything
    # needs-prestige for the expensive classes) if the char read degrades.
    _chr = await gw.read("query_character", {"character_id": character_id})
    _chr_sect = (_chr.get("character") if isinstance(_chr, dict) else None) or (_chr if isinstance(_chr, dict) else {})
    actor_prestige = _f(_chr_sect.get("prestige"))

    # ── War lifecycle gates (item 2): succession-trough + stress-aware pacing ───
    # Reuse the same character read for stress/age (no extra read). The planner does
    # NOT fire interactions — it produces an HONEST lifecycle advisory + a declare-gate
    # the agent honors: never declare across a succession trough; back off when stress
    # is climbing; abandon stalled occ-0 wars first.
    actor_stress = _f(_chr_sect.get("stress"))
    actor_age = _f(_chr_sect.get("age"))
    death_eta_years = max(0, int(65 - actor_age)) if actor_age else None
    # Succession trough: ruler near death (war would resolve across the partition) →
    # do not OPEN a new war (the at-peace-single-heir-succession doctrine). Mirrors the
    # declare_war age>=62 guard so plan_war and declare_war agree.
    near_succession = (death_eta_years is not None and death_eta_years <= 3) or actor_age >= 62
    # Stress pacing: above the soft line, opening more wars compounds stress (acting
    # against coping traits) → recommend backing off. Above the hard line it's a break risk.
    _STRESS_SOFT, _STRESS_HARD = 100, 150
    stress_climbing = actor_stress >= _STRESS_SOFT
    stress_break_risk = actor_stress >= _STRESS_HARD
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
    # This is a PAUSED read — issue it OUTSIDE the unpaused window so the window stays
    # tight around only the deferred game-thread reads.
    vt = await gw.read("query_valid_war_targets", {"actor_id": character_id})
    vt = vt if isinstance(vt, dict) else {}
    rows = vt.get("targets") or vt.get("valid_targets") or []
    if not any("conquest" in str(r.get("cb_type", "")).lower()
               or "subjugation" in str(r.get("cb_type", "")).lower() for r in rows):
        warnings.append({"code": "enumerator_omits_conquest_cb", "is_known_limitation": True,
                         "detail": "O1: enumerator omits conquest/subjugation CB classes; "
                                   "falling back to claim-CB-only + per-target strict-CB"})

    targets: list = []
    excluded: list = []
    seen = set()

    # ── ONE unpaused window (R1) ────────────────────────────────────────────────
    # Every deferred game-thread read in plan_war — conquest_targets, the per-target
    # strict-CB gate, and the per-target cbs tradeoff list — runs INSIDE this single
    # UNPAUSED_TICK scope. The window flips pause→unpause + acquires the g_cb_query
    # single-flight lock ONCE for the whole enumeration, then restores tempo once on exit.
    # Previously each of those (1 + N + K ≈ 17) reads independently flipped pause and
    # re-acquired the lock — 17 pulses / 34 tempo IPCs. Now: 1 pulse / 2 tempo IPCs.
    # Pause-transparent: when called from the agent's PAUSED observe window the window
    # unpauses on entry and re-pauses on exit, identically to before — just amortized.
    # Crash-safety UNCHANGED: the lock is held for the whole window so every inline CB
    # read still serializes through the same slot (no overlap with the engine's AI CB
    # evals), and read_inline does no name-cache drain.
    async with gw.unpaused_window():
        # Augment with conquest_targets (goal-ladder duchies) when goal-scoped.
        # ★ cb_title_id baking: county_conquest_cb is COUNTY-targeted — its cb_title_id
        # is the COUNTY id (rc.county_id), NOT the duchy. The bridge already hands back
        # the authoritative declare block (declare.cb_title_id = county_id) on a
        # conquerable_now county; carry that VERBATIM so declare_war(declare_args=row)
        # fires the exact county the engine validated. target_in_truce/alliance carried
        # from the holder relation flags so the row's tags are correct without a 2nd read.
        # R4: route the conquest read through the busy-retry wrapper (it was a plain read
        # before, so a transient busy silently yielded zero conquest rows → the #95
        # empty-plan pathology). Inline (in-window) so it shares the one flip + lock.
        if goal_scoped:
            ct = await _cb_read_with_retry_inline(gw, "query_conquest_targets",
                                                  {"character_id": character_id})
            ct = ct if isinstance(ct, dict) else {}
            for d in (ct.get("duchies") or ct.get("targets") or []):
                for rc in (d.get("remaining_counties") or []):
                    decl = rc.get("declare") or {}
                    # The county is the CB title for county_conquest_cb (engine-authoritative
                    # via the declare block; fall back to county_id when not conquerable_now).
                    county_cb_title = _as_int(decl.get("cb_title_id") or rc.get("county_id"))
                    rows.append({
                        "target_id": _as_int(decl.get("target_id")
                                             or rc.get("holder_top_liege_id") or rc.get("holder_id")),
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
            for r in pruned_rows[_PROBE_CAP:]:
                excluded.append({"target_name": r.get("target_name"),
                                 "blocked": why_blocked("unknown", severity="transient",
                                                        summary="pruned_low_priority: beyond the per-cycle "
                                                                "strict-CB probe cap; re-plan to surface")})
            pruned_rows = pruned_rows[:_PROBE_CAP]

        # ── Batched strict-CB prefetch (R2), graceful fallback to per-target ────────
        # If the bridge serves query_cb_validity_strict_batch (target_ids:[...]), fetch
        # ALL strict verdicts in ONE round-trip; otherwise fall back to per-target inline.
        probe_tids = [_as_int(r.get("target_id")) for r in pruned_rows]
        strict_by_tid = await _strict_validity_batched(gw, character_id, probe_tids)

        # ── Batched cbs prefetch (R3), graceful fallback to per-target ──────────────
        # One query_cbs_for_targets round-trip for all kept targets when served; else
        # per-target inline below. Prefetch for ALL pruned targets (cheap when batched);
        # the per-target shaping consumes whichever is available.
        cbs_by_tid = await _cbs_for_targets_batched(gw, character_id, probe_tids)

        for r in pruned_rows:
            tid = _as_int(r.get("target_id"))
            # Strict-CB authoritative declarability. Batched verdict when available; else
            # per-target inline read (in-window — shares the one flip + held lock).
            if strict_by_tid is not None and tid in strict_by_tid:
                strict = strict_by_tid.get(tid) or {}
            else:
                strict = await _cb_read_with_retry_inline(
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
            # pick the best declarable CB row (engine-authoritative inner gate).
            best = None
            for cr in results:
                if cr.get("declarable") or (cr.get("outer_status") == "pass" and cr.get("inner_status") == "pass"):
                    best = cr
                    break

            # ── Full CB tradeoff list for THIS target (fetched UP-FRONT so the non-adjacency
            # steer below can consult it). query_cbs_for_target — UNPAUSED engine read; each
            # option carries scope/grants/tradeoffs + the dispatch-marshalling hint. Batched
            # prefetch when served; else in-window inline (shares the one flip + held lock).
            if cbs_by_tid is not None and tid in cbs_by_tid:
                _craw = cbs_by_tid.get(tid) or {}
                cb_options, _all_cb = _shape_cb_options(_craw, target_id=tid)
                tstate = _craw.get("target_state") or {}
            else:
                cb_options, _all_cb, tstate = await _cb_options_for_inline(gw, character_id, tid)

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
            win_prob = round(_f(r.get("win_prob"), 0.5), 2)
            straddles = bool(r.get("straddles_succession"))
            if exclude_straddling and straddles:
                excluded.append({"target_name": r.get("target_name"),
                                 "blocked": why_blocked("succession_straddle",
                                                        summary="war expected to cross a death ETA")})
                continue
            score = round(win_prob * (dejure_gain or 1), 3)

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
            if chosen is not None:
                # Honor the chosen CB's dispatch shape: drop a title_id the CB doesn't take
                # (target_mode 0 subjugation), keep claimant only for claim CBs.
                if not chosen.get("needs_cb_title_id"):
                    cb_title_id = 0
                if not chosen.get("needs_claimant"):
                    claimant = None

            # ready_to_declare per (target, cb): a CB is ready when it is in the valid set.
            ready_by_cb = {o["cb"]: bool(o["valid"]) for o in cb_options}

            # ── truce tag (item 1) ─────────────────────────────────────────────────
            # Surface whether this target is truced so the agent plans AROUND it instead of
            # "no targets" stalling. (ally_free_only already excludes alliances; a truce is
            # carried here both from the enumerator row flag and the strict-CB target_state.)
            in_truce = bool(r.get("target_in_truce") or best.get("target_in_truce")
                            or (tstate.get("in_truce") if isinstance(tstate, dict) else False))

            # ── affordability tag (item 1) ─────────────────────────────────────────
            # Declaring is prestige-gated; tag affordable-now vs needs-prestige against a
            # conservative per-CB-family floor + a raid hint when short. NOT a hard block
            # (the engine is authoritative) — a planning hint so the agent funds first.
            prestige_floor = _cb_prestige_floor(best_cb_name)
            affordable = actor_prestige >= prestige_floor
            affordability = {
                "affordable_now": affordable,
                "actor_prestige": int(actor_prestige),
                "cb_prestige_floor": prestige_floor,
                "hint": (None if affordable
                         else f"needs ~{prestige_floor} prestige to start a {best_cb_name} war "
                              f"(have {int(actor_prestige)}); raid/host-feasts to fund first"),
            }

            # A row is DECLARE-READY only when nothing structural blocks it. We keep the row
            # (it is engine-declarable per the strict gate) but flag truce/affordability so
            # the agent never blind-fires a known-blocked declare.
            declare_ready = (not in_truce) and affordable
            notes = "Independent, declarable per strict-CB inner gate. See cb_options to weigh scope."
            if in_truce:
                notes += " ⚠ TRUCE active — wait for expiry or pick another target."
            if not affordable:
                notes += f" ⚠ {affordability['hint']}."

            targets.append({
                "declare_args": {
                    "target_id": tid,
                    "cb": best_cb_name,
                    "cb_title_id": cb_title_id,
                    "claimant_id": claimant,
                },
                "target_name": r.get("target_name"),
                "title": r.get("title"),
                "dejure_gain": dejure_gain,
                "win_prob": win_prob,
                "ally_free": ally_free,
                "truce": in_truce,
                "affordability": affordability,
                "claim_pressed": bool(r.get("pressed") or best.get("claim_pressed")),
                "straddles_succession": straddles,
                "score": score,
                # The full CB tradeoff list — weigh scope/grants and re-point declare_args.cb
                # at any option here (the marshalling fields tell you what each CB needs).
                "cb_options": cb_options,
                # ready_to_declare is now an HONEST verdict: engine-declarable AND not truced
                # AND affordable. (ready_to_declare_by_cb stays the raw per-CB valid set.)
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
                dj_floor = _cb_prestige_floor(dj.get("cb"))
                dj_affordable = actor_prestige >= dj_floor
                # de-jure CBs are title-targeted; carry the option's own title when the
                # shaped row provides one, else fall back to the target's row title_id.
                dj_title_id = _as_int(dj.get("cb_title_id") or r.get("title_id") or best.get("cb_title_id"))
                dj_ready = (not in_truce) and dj_affordable
                dj_notes = (f"DE-JURE war ({dj.get('scope')}): actor is the de-jure liege of "
                            f"{r.get('target_name')}'s title — usurp by de-jure right "
                            "(the k_england-plurality win-line). See cb_options for scope.")
                if in_truce:
                    dj_notes += " ⚠ TRUCE active — wait for expiry."
                if not dj_affordable:
                    dj_notes += (f" ⚠ needs ~{dj_floor} prestige (have {int(actor_prestige)}).")
                targets.append({
                    "declare_args": {
                        "target_id": tid,
                        "cb": dj.get("cb"),
                        "cb_title_id": dj_title_id if dj.get("needs_cb_title_id") else 0,
                        "claimant_id": None,
                    },
                    "target_name": r.get("target_name"),
                    "title": dj.get("grants") or r.get("title"),
                    # de-jure scope folds in MANY counties at once — weight it above a
                    # single county_conquest so the planner ranks the win-line CB higher.
                    "dejure_gain": max(dejure_gain, 2),
                    "win_prob": win_prob,
                    "ally_free": ally_free,
                    "truce": in_truce,
                    "affordability": {
                        "affordable_now": dj_affordable,
                        "actor_prestige": int(actor_prestige),
                        "cb_prestige_floor": dj_floor,
                        "hint": (None if dj_affordable
                                 else f"needs ~{dj_floor} prestige to start a {dj.get('cb')} war "
                                      f"(have {int(actor_prestige)}); raid/host-feasts to fund first"),
                    },
                    "claim_pressed": False,
                    "straddles_succession": straddles,
                    "score": round(win_prob * max(dejure_gain, 2), 3),
                    "cb_options": cb_options,
                    "ready_to_declare": dj_ready,
                    "ready_to_declare_by_cb": ready_by_cb,
                    "is_de_jure": True,
                    "notes": dj_notes,
                })

    # Rank declare-ready rows first (truced/unaffordable rows sink but are NOT dropped —
    # the agent still sees them to plan around the truce / fund the prestige), then by score.
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
        empty_reason = why_blocked(
            "target_allied" if excluded else "no_valid_targets",
            summary=("all goal targets are allied/truced/non-declarable right now — "
                     "this is NOT a structural wall (re-check next reign / after a truce expires)"
                     if excluded else "no candidate targets enumerated"))

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
                        summary=f"ruler near death (~{death_eta_years}y, age {int(actor_age)}) — a new war "
                                "would resolve ACROSS the succession trough (the at-peace-single-heir "
                                "doctrine); do NOT open one now (declare_war still needs confirm_straddle)"))
    if stress_break_risk:
        lifecycle_reasons.append(
            why_blocked("unknown",
                        summary=f"stress {int(actor_stress)} >= {_STRESS_HARD} (mental-break risk) — opening "
                                "more wars compounds it; relieve stress first"))
    elif stress_climbing:
        lifecycle_reasons.append(
            why_blocked("unknown", severity="transient",
                        summary=f"stress {int(actor_stress)} climbing (>= {_STRESS_SOFT}) — pace declarations; "
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
            "death_eta_years": death_eta_years,
            "age": int(actor_age),
            "doctrine": "AT-PEACE single-heir successions — never be at war across a succession.",
        },
        "stress": {
            "value": int(actor_stress),
            "climbing": stress_climbing,
            "break_risk": stress_break_risk,
            "soft_cap": _STRESS_SOFT, "hard_cap": _STRESS_HARD,
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
# path, three callers (steered war, mint_title, muster).
# ===========================================================================

async def _inline_read(gw, action: str, params: dict) -> dict:
    """ANY-polarity readback read usable INSIDE a confirmed_write poll (the game is
    already unpaused + the single-flight lock is held by the confirming scope, so this
    must not re-flip / re-lock). Prefers gw.read_inline (BridgeGateway); falls back to
    gw.read for the _AgentGateway adapter, which does not own a scoped flip."""
    fn = getattr(gw, "read_inline", None)
    if fn is not None:
        res = await fn(action, params)
    else:
        res = await gw.read(action, params)
    return res if isinstance(res, dict) else {}


async def _confirm_write(gw, action: str, params: dict, *, readback,
                         deadline_s: float = 12.0) -> dict:
    """Dispatch a write and resolve it by readback. Returns {"raw": dict,
    "resolved": obj|None, "polls": int}. Uses gw.confirmed_write (one scope spanning
    dispatch→poll→restore — no tempo leak, game keeps ticking) when the gateway exposes
    it; otherwise falls back to gw.write + a poll loop over gw.read (the _AgentGateway
    path — behavior-preserving)."""
    confirmer = getattr(gw, "confirmed_write", None)
    if confirmer is not None:
        out = await confirmer(action, params, readback=readback, deadline_s=deadline_s)
        return out if isinstance(out, dict) else {"raw": {}, "resolved": None, "polls": 0}
    # Fallback: dispatch (gw.write owns unpause→tick→restore), then poll via gw.read.
    raw = await gw.write(action, params)
    raw = raw if isinstance(raw, dict) else {}
    resolved = await readback(raw)
    polls = 0
    if resolved is None:
        deadline = asyncio.get_event_loop().time() + deadline_s
        while resolved is None and asyncio.get_event_loop().time() < deadline and polls < 8:
            adv = getattr(gw, "_await_tick_advance", None)
            if adv is not None:
                await adv(timeout_s=2.5)
            else:
                await asyncio.sleep(1.0)
            resolved = await readback(raw)
            polls += 1
    return {"raw": raw, "resolved": resolved, "polls": polls}


def _parse_dejure_counts(reason: str) -> Optional[dict]:
    """Parse 'create_title_refused_insufficient_de_jure_control_<held>_of_<total>_counties'
    → {'held': h, 'need': ceil(0.51*total), 'total': t}."""
    import re
    m = re.search(r"de_jure_control_(\d+)_of_(\d+)", str(reason or ""))
    if not m:
        return None
    held, total = int(m.group(1)), int(m.group(2))
    need = int(total * 0.51 + 0.999) if total else 0
    return {"held": held, "need": need, "total": total}


# --------------------------------------------------------------------------- declare_war
async def declare_war_impl(gw, character_id: int = 0, declare_args: dict = None,
                           target_id: int = 0, cb: str = "", cb_title_id: int = 0,
                           claimant_id: int = 0, confirm_straddle: bool = False) -> dict[str, Any]:
    actor, err = await _resolve_actor(gw, character_id, "declare_war")
    if err:
        return err

    da = dict(declare_args or {})
    tid = _as_int(da.get("target_id") or target_id)
    cb_name = da.get("cb") or cb
    cbt = _as_int(da.get("cb_title_id") or cb_title_id)
    clm = _as_int(da.get("claimant_id") or claimant_id)
    if not tid or not cb_name:
        return envelope("declare_war", ok=False, actor=actor, game=gw.game(),
                        blocked=why_blocked("unknown", summary="target_id and cb (or declare_args) are required"),
                        bridge_health=gw.health)

    # Crash-vector guardrail (inv_bridge-map §5): cap simultaneous wars.
    wars0 = await gw.read("get_active_wars", {"character_id": character_id})
    pre_wars = (wars0 or {}).get("wars", []) if isinstance(wars0, dict) else []
    active = len(pre_wars)
    if active >= 20:
        return envelope("declare_war", ok=False, actor=actor, game=gw.game(),
                        blocked=why_blocked("unknown", summary=f"refusing: {active} simultaneous wars (heap-corruption crash vector)"),
                        bridge_health=gw.health)

    def _is_my_attacker_war_vs(w: dict, against: int = 0, cb_match: str = "") -> bool:
        """A war with THIS actor as primary attacker (optionally vs `against` / `cb_match`)."""
        if _as_int(w.get("attacker_id")) != character_id:
            return False
        if against and _as_int(w.get("defender_id")) != against:
            return False
        if cb_match and str(w.get("cb_type", "")) and str(w.get("cb_type")) != cb_match:
            return False
        return True

    def _war_result(w: dict) -> dict:
        return {"war_id": w.get("war_id"), "verified_active": True,
                "target_id": _as_int(w.get("defender_id")) or tid,
                "target": da.get("target_name") or w.get("defender_id") or tid,
                "cb": w.get("cb_type") or cb_name, "name": w.get("name")}

    pre_war_ids = {_as_int(w.get("war_id")) for w in pre_wars}

    # (1) Idempotency: if a war with this actor as attacker vs the same target (and CB,
    # when both carry one) is ALREADY active, return it WITHOUT re-dispatching. This
    # stops the duplicate-declaration churn the live agent fell into (re-declaring a war
    # that already landed because the prior call false-rejected on the 5s window).
    for w in pre_wars:
        if _is_my_attacker_war_vs(w, against=tid, cb_match=cb_name):
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
                        blocked=map_raw_reason(cull, summary=cull), bridge_health=gw.health)

    # confirm_straddle hard guard.
    if not confirm_straddle:
        sp = await gw.read("query_succession", {"character_id": character_id})
        chr_ = await gw.read("query_character", {"character_id": character_id})
        age = _f(((chr_ or {}).get("character") or chr_ or {}).get("age"))
        if age >= 62:
            return envelope("declare_war", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=why_blocked("succession_straddle",
                                                summary="ruler near death; war may resolve across the succession — pass confirm_straddle=true to proceed"),
                            bridge_health=gw.health)

    # great_holy_war is religious-head-lifecycle-only — the WCC resolves the name but a
    # hand-declared GHW is not the engine's real path (inv_gamecb.md §7). Reject up front
    # rather than dispatch a war that won't stick.
    cb_lower = str(cb_name).lower()
    if "great_holy_war" in cb_lower:
        return envelope("declare_war", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("custom_scope_bridge_cannot_fire",
                                            summary="great_holy_war uses the religious-head CGreatHolyWar lifecycle, not a declarable CB — unsupported via steered WCC"),
                        bridge_health=gw.health)

    # Per-CB marshalling (inv_gamecb.md call table). Mode-0 CBs (tribal_subjugation_cb,
    # vassalization) take NO cb_title_id and NO claimant. claim_cb takes claimant_id
    # (default = self-press to the actor). Title-targeted conquest/de_jure/holy_war CBs
    # take cb_title_id but NO claimant. We detect mode-0/claim by CB-name family so the
    # marshalling is right whether declare_args came from plan_war or the escape hatch.
    is_subjugation = "subjugation" in cb_lower or "vassalization" in cb_lower
    is_claim = cb_lower == "claim_cb" or cb_lower.endswith("_claim_cb")
    if is_subjugation:
        cbt = 0          # target_mode 0: no title
        clm = 0          # no claimant
    elif is_claim:
        clm = clm or character_id   # self-press defaults claimant to the actor
    else:
        clm = 0          # title-targeted conquest/de_jure/holy_war: no claimant

    # Dispatch via the steered path (resolve_act → call_war_create_core), confirmed by
    # readback (FIX 1 §1.4 — ONE scope spans dispatch+poll+restore, game keeps ticking
    # the whole window then tempo is restored). The steered-war intent drains
    # ASYNCHRONOUSLY — the war frequently lands a tick or two AFTER the write returns
    # (the old "intent_not_completed_within_5s" report was a false-negative). The
    # readback scans get_active_wars for a NEW attacker-war (match target_id/cb when
    # possible, else any new attacker-war since the pre-dispatch snapshot).
    act_params = {"type": "war", "method": "steered", "actor_id": character_id,
                  "target_id": tid, "casus_belli": cb_name, "cb_title_id": cbt,
                  "claimant_id": clm}
    bridge_action, bridge_params = _tools.resolve_act(act_params)

    async def _scan_for_landed_war(_raw) -> Optional[dict]:
        wars_now = await _inline_read(gw, "get_active_wars", {"character_id": character_id})
        wn = (wars_now or {}).get("wars", []) if isinstance(wars_now, dict) else []
        for w in wn:                                          # exact target+CB match
            if _is_my_attacker_war_vs(w, against=tid, cb_match=cb_name):
                return w
        for w in wn:                                          # any attacker-war vs target
            if _is_my_attacker_war_vs(w, against=tid):
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

    # (3) Window expired with no new war. This is NOT a hard rejection — a steered war
    # very often lands a tick later than this window. Report confirmation_pending and
    # tell the agent to VERIFY (observe_v2/get_active_wars), not blindly retry.
    raw_reason = str(raw.get("reason") or raw.get("error") or raw.get("status") or "")
    # A genuine engine cull surfaces a target-state reason — surface THAT as a real gate.
    if raw_reason and raw_reason not in (
            "", "intent_not_completed_within_5s", "sent_no_confirmation",
            "dispatched_pending", "timeout", "sent",
            "create_title_command_dispatched") and not raw.get("success", False):
        mapped = map_raw_reason(raw_reason, summary=raw_reason)
        # If the raw reason mapped to confirmation_pending, fall through to the pending
        # envelope below; only a hard gate short-circuits to rejected.
        if mapped.get("gate") != "confirmation_pending":
            return envelope("declare_war", ok=False, verdict="rejected", actor=actor,
                            game=gw.game(), blocked=mapped, bridge_health=gw.health)
    return envelope("declare_war", ok=False, verdict="dispatched_pending", actor=actor,
                    game=gw.game(),
                    blocked=why_blocked(
                        "confirmation_pending",
                        summary=("War dispatched but not yet confirmed; it often lands a tick "
                                 "later. VERIFY with observe_v2/get_active_wars before "
                                 "re-declaring — do NOT blindly retry."),
                        raw_reason=raw_reason or "intent_not_completed_within_5s"),
                    pending={"poll_token": raw.get("request_id", ""), "drains_when": "unpaused",
                             "expected_change": "new attacker-war vs target", "timeout_s": 15,
                             "verify_with": "get_active_wars / observe_v2"},
                    result={"target_id": tid, "cb": cb_name}, bridge_health=gw.health)


# --------------------------------------------------------------------------- succession_plan
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

    # Per-title partition MAP (ck-succprev): which heir gets which title under which law —
    # the concrete "which titles SCATTER" detail behind the passes_intact verdict. Pure
    # crash-hardened READ; degrades to available=False on an older bridge (verdict below
    # still derives from the simulation, so this is purely additive detail).
    sp_raw = await gw.read("query_title_succession_map", {"character_id": character_id})
    sp_sum = _succession_preview_summary(sp_raw if isinstance(sp_raw, dict) else {})

    # REINTERPRET: compute the real split from per-title simulation, NOT is_partition (B2).
    heirs = sim.get("eligible_heirs") or succ.get("heirs") or []
    eligible_heir_count = sim.get("eligible_heir_count")
    if eligible_heir_count is None:
        eligible_heir_count = len(heirs) if heirs else _as_int(succ.get("heir_count"))
    primary = (heirs[0] if heirs else None) or {}
    primary_keeps = sim.get("primary_keeps_kingdom")
    breaker = None
    splits = False
    for h in heirs[1:]:
        takes = h.get("takes") or h.get("titles") or []
        if takes:
            breaker = {"id": h.get("id"), "name": h.get("name"), "takes": takes}
            splits = True
            break
    if eligible_heir_count and eligible_heir_count > 1:
        splits = splits or True
    passes_intact = not splits

    age = None
    chr_ = await gw.read("query_character", {"character_id": character_id})
    age = _f(((chr_ or {}).get("character") or chr_ or {}).get("age"))
    eta = max(0, int(65 - age)) if age else None

    result = {
        "passes_intact": passes_intact,
        "eligible_heir_count": eligible_heir_count,
        "primary_heir": {"id": primary.get("id"), "name": primary.get("name"),
                         "keeps_titles": primary.get("keeps_titles") or primary.get("titles") or []},
        "primary_keeps_kingdom": bool(primary_keeps),
        "breaker": breaker,
        "realm_above_partition_floor": sim.get("realm_above_partition_floor", True),
        "succession_eta_years": eta,
        "at_war": at_war,
        # Per-title partition map (ck-succprev): the concrete which-title-to-which-heir
        # split + which titles ride PARTITION law vs single-heir. available=False on an
        # older bridge (verdict above still holds — this is additive detail).
        "per_title_map": {
            "available": sp_sum.get("available", False),
            "scatters": sp_sum.get("scatters"),
            "scatter_breaker": sp_sum.get("scatter_breaker"),
            "primary_heir": sp_sum.get("primary_heir"),
            "partition_titles": sp_sum.get("partition_titles"),
            "single_heir_titles": sp_sum.get("single_heir_titles"),
            "top_title": sp_sum.get("top_title"),
            **({"note": "per-title map unavailable (" + str(sp_sum.get("reason", "unknown")) +
                        ") — verdict derived from the simulation"} if not sp_sum.get("available") else {}),
        },
        "fix": {
            "lever": "reduce_eligible_heirs",
            "options": [
                "single_heir law (if reachable — check do(intent=None, action_class='laws'/'decisions'))",
                "land a spare son as a VASSAL inside the realm (stays in-realm at partition)",
            ],
            "wont_work": [
                "disinherit an adult landed son (AI refuses; gate ai_refuses_self_disadvantaging)",
            ],
        },
    }
    warnings = []
    if eta is not None:
        warnings.append({"code": "war_duration_unavailable", "is_known_limitation": True,
                         "detail": "succession_eta is an age-derived heuristic (O5)"})
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
# WHAT IS NOT EXPOSED BY ANY CURRENT BRIDGE READ (→ emitted null + flagged
# "unresolved", never invented): education_focus, guardian_id, guardian_martial,
# projected_adult_martial. Those need the child-education/CWardenLink RE — SPEC in
# re_heir_toolchain_spec.md. Honest degradation per project convention.
# ===========================================================================

# The childhood AFFINITY traits that bias a child toward education_martial (each
# +20/event in the education score, re_heir_education_mechanic.md §1a). Read-only
# hint surfaced when present on a child — we never assume it; we report what
# query_character's trait walk actually shows.
_MARTIAL_AFFINITY_CHILD_TRAITS = {"rowdy", "bossy"}


def _char_section(raw: dict | None) -> dict:
    """Lift the per-character section out of a query_character envelope. Accepts the
    nested {"character": {...}} shape or a flat dict; never raises."""
    if not isinstance(raw, dict):
        return {}
    sect = raw.get("character")
    return sect if isinstance(sect, dict) else raw


def _trait_names(sect: dict) -> set[str]:
    """Lower-cased trait-name set from a query_character section. Accepts both the
    string-array and dict-entry ({"name": ...}) trait shapes."""
    out: set[str] = set()
    traits_raw = sect.get("traits") if isinstance(sect.get("traits"), list) else []
    for t in traits_raw:
        if isinstance(t, str):
            out.add(t.lower())
        elif isinstance(t, int):
            out.add(str(t).lower())
        elif isinstance(t, dict):
            nm = t.get("name") or t.get("key") or t.get("trait")
            if nm:
                out.add(str(nm).lower())
    return out


def _conqueror_gate(sect: dict) -> dict[str, Any]:
    """The AL-13 conqueror-gate verdict for one character section. Mirrors
    the same predicate the removed expansion_engine_health used (martial>=18 OR a
    conqueror/viking trait) so the
    heir read and the engine-health read agree on what 'a conqueror' is."""
    martial = _as_int(sect.get("martial"))
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
    guardian_* stay NULL+unresolved (§1b DEEP-RE — relation-index offset unpinned)."""
    martial = _as_int(sect.get("martial"))
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
        rec = ("groom NOW: set childhood focus education_martial + assign a HIGH-MARTIAL "
               "guardian (see good_guardian_candidates) "
               + ("(child already has the rowdy/bossy affinity → +20/event, focus will take strongly)"
                  if affinity else
                  "(no martial-affinity child trait visible — the focus+guardian is the only lever)"))
    else:
        rec = ("adult below the gate: top up via adult martial LIFESTYLE focus "
               "(act type=lifestyle focus=martial_*) + a martial marriage for the next gen")
    return {
        "id": _as_int(sect.get("character_id") or sect.get("id")),
        "name": (sect.get("name") or sect.get("full_name") or sect.get("first_name")),
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
        # §1b DEEP-RE — guardian relation-index offset unpinned; honest null+unresolved.
        # re_heir_guardian_RE.md §1b (needs ONE live probe of FUN_06777940(ward,8)).
        "guardian_id": (edu or {}).get("guardian_id"),
        "guardian_martial": (edu or {}).get("guardian_martial"),
        "projected_adult_martial_estimate": None,
        # education_focus is resolved; guardian remains the unresolved half.
        "education_unresolved": (edu or {}).get("education_focus") is None,
        "guardian_unresolved": True,
    }


async def heir_plan_impl(gw, character_id: int = 0, max_results: int = 12) -> dict[str, Any]:
    """★ LEGIBILITY READ (Phase 1) — compose existing safe reads into a per-child /
    per-eligible-heir heir-planning roster the agent can steer the conqueror-heir
    doctrine against. PURE reads (query_succession_simulation + query_character per
    child/heir); observer-safe; no bridge change. Education-focus/guardian fields are
    NULL + flagged unresolved (no bridge read exposes them; see re_heir_toolchain_spec.md)."""
    actor, err = await _resolve_actor(gw, character_id, "heir_plan")
    if err:
        return err

    sim = await gw.read("query_succession_simulation", {"character_id": character_id})
    sim = sim if isinstance(sim, dict) else {}
    chr_ = await gw.read("query_character", {"character_id": character_id})
    ruler_sect = _char_section(chr_)

    # Ordered eligible-heir line (from the partition simulation). Primary heir = the
    # first in the simulation order (the one who keeps the kingdom under single-heir).
    sim_heirs = sim.get("eligible_heirs") or []
    heir_ids: list[int] = []
    heir_meta: dict[int, dict] = {}
    for idx, h in enumerate(sim_heirs if isinstance(sim_heirs, list) else []):
        hid = _as_int(h.get("id") or h.get("character_id"))
        if hid:
            heir_ids.append(hid)
            heir_meta[hid] = {"is_primary_heir": idx == 0, "in_heir_line": True}

    # Children (the grooming pool — bare uint IDs in query_character.children).
    child_ids: list[int] = []
    raw_children = ruler_sect.get("children")
    if isinstance(raw_children, list):
        for c in raw_children:
            cid = _as_int(c if not isinstance(c, dict) else (c.get("id") or c.get("character_id")))
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
        if not sect:
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
        "succession_law": sim.get("succession_law") or sim.get("law"),
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
                     "slot). guardian_id/_martial and projected_adult_martial remain NULL: "
                     "§1b is DEEP-RE (relation-index sub-object offset unpinned, needs one "
                     "live probe of FUN_06777940(ward,8)) — re_heir_guardian_RE.md §1b."),
        },
        # The acting half of AL-13 — which levers the agent can actually fire today.
        "levers": {
            "set_childhood_education_focus": {
                "status": "BUILT (§2) — EXPERIMENTAL until one child smoke",
                "today": ("act(type='set_education_focus', focus='education_martial', "
                          "character_id=<child>) — type-checked alias over the crash-hardened "
                          "SET_FOCUS_APPLY. Childhood and adult focus share ONE slot, so the "
                          "readback correctly detects success on a child (no false-negative). "
                          "Verify via query_heir_education.education_focus."),
                "build": "re_heir_education_RE.md §2",
            },
            "assign_guardian": {
                "status": "BUILT-EXPERIMENTAL (§3) — secondary-scope branch wired; slot map unconfirmed",
                "today": ("intent(action='queue', interaction_type='educate_child_interaction', "
                          "secondary_actor_id=<guardian>, secondary_recipient_id=<ward>) — the "
                          "ai_eval_hook educate_child branch now writes both scopes (marriage "
                          "mapping: ward->0x2DC, guardian->0x2E4) under the same sigsetjmp guard. "
                          "act(type=interaction) ALSO forwards secondaries now (§3b). ★ CONFIRM "
                          "the guardian relation actually set via a live readback before trusting "
                          "ok:true — slot order is the one unconfirmed variable."),
                "build": "re_heir_guardian_RE.md §3 / §3b",
            },
            "martial_marriage": {
                "status": "WORKS via intent-queue AND act(type=interaction) (§3b fixed)",
                "today": ("breed a martial heir by marrying a child/courtier to a high-martial "
                          "spouse: intent(action='queue', "
                          "interaction_type='arrange_marriage_interaction', "
                          "secondary_actor_id=<child>, secondary_recipient_id=<martial spouse>). "
                          "The act(type=interaction) path now forwards the secondaries too "
                          "(§3b — was a foot-gun, fixed)."),
                "candidates": "query(domain='marriage_candidates', sort='martial')",
            },
            "designate_heir": {
                "status": "EXISTS (generic interaction); INERT under partition/primogeniture",
                "today": ("act(type=interaction, interaction_type='designate_heir_interaction', "
                          "target_id=<chosen heir>) — only matters under elective/appointment "
                          "succession; for a partition realm drive heir-count→1 instead "
                          "(succession_plan / single_heir law)."),
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
    """★ GOOD-GUARDIAN FINDER — return high-martial guardian candidates for a child of
    the ruler. The education score weights the guardian's matching skill at 0.4× +
    learning at 0.2× per event (re_heir_guardian_mechanic.md §1), so we rank by martial.

    HONEST SCOPE GAP: there is NO court-roster bridge read with a court_of/realm_of filter
    — search_characters scans the GLOBAL char DB. A valid guardian MUST be
    is_in_the_same_court_as the ward, which the global scan cannot guarantee. So we:
      (a) surface the ruler's OWN family/court members we CAN reach (children, spouse,
          council via query_council) scored by martial — these ARE in-court; and
      (b) include the global high-martial list as 'aspirational' (need a recruit/invite
          first, NOT directly assignable as a guardian).
    The court_of filter is the SPEC fix (re_heir_toolchain_spec.md §4)."""
    actor, err = await _resolve_actor(gw, character_id, "good_guardian_candidates")
    if err:
        return err

    cand: dict[int, dict] = {}

    def _add(sect: dict, source: str):
        cid = _as_int(sect.get("character_id") or sect.get("id"))
        if not cid or cid == _as_int(actor.get("id")):
            return
        martial = _as_int(sect.get("martial"))
        traits = _trait_names(sect)
        learning = _as_int(sect.get("learning"))
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
            "name": (sect.get("name") or sect.get("full_name") or sect.get("first_name")),
            "martial": martial,
            "learning": learning,
            "age": age,
            "is_adult": (age >= 16) if age else None,
            "good_intellect": bool(intellect_bonus),
            "bad_intellect": bool(intellect_penalty),
            "guardian_score": round(score, 2),
            "in_court": source != "global",
            "source": source,
            "assignable_note": ("in your court (reachable as a guardian once the assign lever "
                                "lands)" if source != "global" else
                                "GLOBAL high-martial — must be recruited/invited into court "
                                "before guardianship is possible"),
        }
        prev = cand.get(cid)
        if prev is None or row["guardian_score"] > prev["guardian_score"] or (not prev["in_court"] and row["in_court"]):
            cand[cid] = row

    # (a) In-court reachable: council members (real courtiers) + ruler's spouse(s).
    council = await gw.read("query_council", {"character_id": character_id})
    council = council if isinstance(council, dict) else {}
    for seat in (council.get("seats") or council.get("council") or []):
        if not isinstance(seat, dict):
            continue
        cid = _as_int(seat.get("character_id") or seat.get("councillor_id") or seat.get("id"))
        if cid:
            cm = _char_section(await gw.read("query_character", {"character_id": cid}))
            if cm:
                _add(cm, "council")

    ruler_sect = _char_section(await gw.read("query_character", {"character_id": character_id}))
    for sp in (ruler_sect.get("spouses") or []):
        sid = _as_int(sp if not isinstance(sp, dict) else (sp.get("id") or sp.get("character_id")))
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
        for ch in (court.get("characters") or court.get("results") or []):
            if isinstance(ch, dict):
                _add(ch, "court")

    # (b) Global high-martial pool (aspirational — recruit-first). sort=martial.
    glob = await gw.read("search_characters",
                         {"filter": "alive", "sort": "martial", "sort_order": "desc",
                          "max_results": max(10, max_results)})
    glob = glob if isinstance(glob, dict) else {}
    for ch in (glob.get("characters") or glob.get("results") or []):
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
            "issue": ("RESOLVED (§4): search_characters now supports court_of=<ruler_id>, an "
                      "immediate-liege predicate (courtier_data+0xC8 back-ref + landed-vassal "
                      "arm) over the DB walk — true in-court courtiers are enumerated (source="
                      "'court'), no longer limited to council+spouse."),
            "built": "re_heir_guardian_RE.md §4 (search_characters court_of=)",
        },
        "assign_lever_status": ("BUILT-EXPERIMENTAL (§3/§3b): educate_child secondary-scope branch "
                                "is wired in ai_eval_hook.cpp (marriage slot mapping) and the act/"
                                "intent paths forward secondaries — but the slot ORDER is unconfirmed; "
                                "CONFIRM the guardian relation via a live readback before trusting "
                                "ok:true. re_heir_guardian_RE.md §3/§3b"),
    }
    return envelope("good_guardian_candidates", result=result, actor=actor,
                    game=gw.game(), bridge_health=gw.health)


# --------------------------------------------------------------------------- set_phase
async def set_phase_impl(gw, phase: str = None, character_id: int = 0,
                         overrides: dict = None) -> dict[str, Any]:
    actor, err = await _resolve_actor(gw, character_id, "set_phase")
    if err:
        return err

    if not phase:
        # read current genome.
        pers = await gw.read("get_ai_personality", {"character_id": character_id})
        return envelope("set_phase", result={"phase": None, "genome": pers if isinstance(pers, dict) else {}},
                        actor=actor, game=gw.game(), bridge_health=gw.health)

    if phase not in PHASE_GENOME:
        return envelope("set_phase", ok=False, actor=actor, game=gw.game(),
                        blocked=why_blocked("unknown", summary=f"unknown phase '{phase}'; valid: {', '.join(PHASES)}"),
                        bridge_health=gw.health)

    warnings = []
    genome = dict(PHASE_GENOME[phase])
    if overrides:
        if "CallAllies" in overrides:
            warnings.append({"code": "callallies_not_enforced", "is_known_limitation": True,
                             "detail": "set_character_subsystems CallAllies is advisory; the engine does NOT suppress call-to-arms"})
        genome.update({k: v for k, v in overrides.items() if isinstance(v, int)})

    p = dict(genome)
    p["character_id"] = character_id
    pres = await gw.write("set_ai_personality", p)
    subs = dict(PHASE_SUBSYSTEMS[phase])
    s = dict(subs)
    s["character_id"] = character_id
    await gw.write("set_character_subsystems", s)

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
    # The per-character `set_character_subsystems` write above STAYS exactly as
    # it was. It is engine_enforced:false — a bookkeeping map no dispatch hook
    # consumes — and the ruling was explicit not to route the phase through it
    # instead, because that would move a dead lever rather than remove a live
    # one. What the phase intends is reported; what the engine enforces is not
    # claimed.
    declare_war = bool(subs.get("DeclareWar", False))

    # ── ck-xwlg P2 hardening: re-assert the per-char AI gate (ai_ctrl+0x28) on
    # every phase pin. The gate stays untouched by default takes, but a leaked
    # disable (an earlier take with disable_native_ai=true, or a release/retake
    # race) silently kills ALL war prosecution — armies never re-plan, sieges
    # never chain (the archived M-003 class). Idempotent write; belt-and-braces.
    #
    # lane-query item 7: THE RE-ASSERT WAS REFUSED ON EVERY DEFAULT CALL, AND
    # THE REPLY SAID APPLIED. set_phase passes the tool's character_id through,
    # and the default is 0 ("the bound ruler"). set_ai_personality and
    # set_character_subsystems resolve 0 to the controlled ruler; the bridge's
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
    ai_gate_ok = isinstance(ai_gate, dict) and ai_gate.get("success") is True \
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
                                   "inspect(subject='war_score') reports ai_enabled "
                                   "for the character; if armies park mid-war, that "
                                   "is the field to read."})
    offgoal_armed = False
    if phase == "expansion":
        veto = await gw.write("set_offgoal_veto", {"enabled": True, "character_id": character_id})
        # rc2 B8: armed only on the bridge's own confirmation. A reply with no
        # `error` key is not one: an ownership refusal, a disconnect and an
        # unconfirmed send all lack it, and all three used to read as armed.
        if flag_write_confirmed(veto, True):
            offgoal_armed = True
            _note_flag_write("off_goal_veto", True, "set_phase('expansion')")
            warnings.append({"code": "offgoal_veto_experimental", "is_known_limitation": True,
                             "detail": "armed the off-goal war veto, an EXPERIMENTAL hook "
                                       "not yet live-validated: " + OFFGOAL_VETO_SCOPE})
        else:
            _raw = veto if isinstance(veto, dict) else {}
            _why = (_raw.get("error") or _raw.get("reason") or _raw.get("status")
                    or ("success=false" if _raw.get("success") is False else "no confirmation"))
            warnings.append({"code": "offgoal_veto_unavailable", "is_known_limitation": True,
                             "detail": f"the bridge did not confirm set_offgoal_veto ({_why}); "
                                       "the veto is not known to be armed and nothing else "
                                       "stands in for it"})

    result = {"phase": phase, "genome": genome, "subsystems": subs,
              # ck-zgh4: what the phase RECORDS for this character, and the
              # honest note that nothing in the engine reads it. No global byte
              # is written and none is reported.
              "declare_war_recorded": declare_war,
              "declare_war_note": ("recorded on this character only; the engine does not "
                                   "enforce it. To start a war, use declare_war."),
              "offgoal_veto_armed": offgoal_armed,
              "offgoal_veto_scope": OFFGOAL_VETO_SCOPE,
              # rc2 B8: nothing re-applies the phase after a succession
              # (the server has no such path); this said True.
              "auto_repin": False, "repinned_this_reign": True,
              # lane-query item 7: what the per-character AI re-assert did,
              # read from its reply rather than assumed.
              "ai_gate": {"character_id": ai_gate_cid,
                          "reasserted": ai_gate_ok,
                          "reason": (None if ai_gate_ok else
                                     (ai_gate.get("reason") or ai_gate.get("error")
                                      if isinstance(ai_gate, dict) else "no_reply")
                                     or "no_confirmation")}}
    if isinstance(pres, dict) and pres.get("error"):
        warnings.append({"code": "personality_write_warning", "detail": str(pres.get("error"))})
    return envelope("set_phase", verdict="applied", result=result, actor=actor,
                    game=gw.game(), warnings=warnings, bridge_health=gw.health)


# --------------------------------------------------------------------------- do
async def _do_enumerate(gw, action_class: str, character_id: int, candidates: list,
                        actor: dict) -> dict[str, Any]:
    """Enumerate-mode: hand back self-describing do_call rows (SPEC §2 graft)."""
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
        res = await gw.read_unpaused("list_valid_decisions",
                                     {"character_id": character_id, "decisions": candidates})
        for d in (res or {}).get("decisions", []) if isinstance(res, dict) else []:
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
                               "args": {"seat": "court_chaplain", "task": "task_fabricate_claim",
                                        "target_id": t.get("title_id")}},
                   "warnings": [{"code": "may_revert_ai_reasserts", "is_known_limitation": True}]}
            if assignable:
                row["blocked"] = None
                available.append(row)
            else:
                row["blocked"] = why_blocked("target_not_assignable",
                                             summary="target dropped off the eligible list this tick — re-enumerate, never reuse a stale target_id")
                unavailable.append(row)
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
                                        f"free companies shown ({res.get('order')}); "
                                        "discover(what='hireable_mercenaries', max_results=N) lists more")})
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
        res = await gw.read("query_joinable_factions", {"character_id": character_id})
        for fac in (res or {}).get("factions", []) if isinstance(res, dict) else []:
            ft = fac.get("faction_type") if isinstance(fac, dict) else fac
            available.append({"label": ft,
                              "do_call": {"intent": "faction", "args": {"action": "create", "faction_type": ft}},
                              "blocked": None})
    elif action_class == "buildings":
        res = await gw.read("query_building_types", {"category": 0})
        for b in (res or {}).get("buildings", res.get("types", []) if isinstance(res, dict) else []) if isinstance(res, dict) else []:
            bk = b.get("building") or b.get("key") if isinstance(b, dict) else b
            available.append({"label": bk,
                              "do_call": {"intent": "build", "args": {"building": bk, "location_id": 0}},
                              "blocked": None})
    elif action_class == "imprison_targets":
        realm = await gw.read("query_realm", {"character_id": character_id})
        for v in (realm or {}).get("vassals", [])[:40] if isinstance(realm, dict) else []:
            ann = await gw.read("query_imprisonment_reasons",
                                {"actor_id": character_id, "target_id": v.get("id")})
            ok = bool((ann or {}).get("has_imprisonment_reason")) if isinstance(ann, dict) else False
            row = {"label": f"Imprison {v.get('name')}",
                   "do_call": {"intent": "interact",
                               "args": {"interaction": "imprison_interaction", "target_id": v.get("id")}}}
            if ok:
                row["blocked"] = None
                available.append(row)
            else:
                row["blocked"] = why_blocked("target_not_assignable", summary="no imprisonment reason")
                unavailable.append(row)
    elif action_class == "formable_titles":
        # GAP 1 (re_toolgap_mint.md): make the formable-title set DISCOVERABLE rather than
        # memorized. Pure composition of the already-shipping query_dejure_progress read
        # (which resolves UNCREATED keys + supplies the /93 meter) — zero new bridge surface,
        # mirrors the DeclareWar enumerate→do_call ergonomics. The agent reads this, picks an
        # `available` row, and fires its embedded do_call do(intent="mint_title", args=...).
        keys = candidates or FORMABLE_TITLE_CANDIDATES
        warnings.append({"code": "control_requirements", "is_known_limitation": False,
                         "detail": ("Two DISTINCT 'control' gates: (1) DUCHY/county tier — the engine "
                                    "can_create trigger requires you to PERSONALLY hold (demesne or direct "
                                    "de-jure control) the title's de-jure counties; the bridge does NOT "
                                    "pre-check this, it surfaces the engine refusal verbatim. (2) KINGDOM/"
                                    "EMPIRE tier (>=4) — the #131 integrity gate requires >=51% REALM-"
                                    "INCLUSIVE de-jure control (ruler + transitive vassals, NOT personal "
                                    "demesne). A titular kingdom (k_danelaw, total==0) bypasses the #131 bar "
                                    "and defers to the engine can_create gate.")})
        for tkey in keys:
            prog = await gw.read("query_dejure_progress",
                                 {"character_id": character_id, "title_key": tkey})
            prog = prog if isinstance(prog, dict) else {}
            if str(prog.get("error") or "").startswith("title_key"):
                continue  # not a real landed_title — skip (never invent a row)
            held, total = _as_int(prog.get("held")), _as_int(prog.get("total"))
            need = int(total * 0.51 + 0.999) if total else 0
            row = {"label": f"Form {tkey} ({held}/{total} de-jure)",
                   "do_call": {"intent": "mint_title", "args": {"title_key": tkey}},
                   "measured": {"held": held, "total": total, "need": need,
                                "fraction": round(_f(prog.get("fraction")), 3)}}
            if total > 0 and held < need:
                row["blocked"] = why_blocked(
                    "de_jure_control",
                    summary=(f"hold {held}/{need} de-jure counties of {tkey} (need >=51% REALM-de-jure, "
                             "counted via transitive vassals — NOT personal demesne)"),
                    measured=row["measured"])
                unavailable.append(row)
            else:
                # total==0 (titular like k_danelaw) OR majority held → defer to the engine
                # can_create gate (gold + not-extant + the DUCHY-tier PERSONAL-DEMESNE county
                # requirement). Surface as available; the engine gate is the final authority.
                row["blocked"] = None
                available.append(row)
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


# FIX 1 — the do() intents that resolve by post-dispatch readback (not raw.success).
# mint_title: title now HELD. muster: a raised/mustering regiment delta. Genuinely
# deferred B* intents (scheme/faction/take_decision when enqueued) keep first-class
# pending (no synchronous readback) and are NOT in this set.
CONFIRMABLE_WRITES = {"mint_title", "muster"}


async def _confirm_intent_write(gw, intent: str, bridge_action: str, bridge_params: dict,
                                args: dict, character_id: int, actor: dict,
                                warnings: list) -> dict[str, Any]:
    """Dispatch a CONFIRMABLE_WRITES do() intent and resolve it by readback (#113)."""
    if intent == "mint_title":
        tkey = args.get("title_key") or args.get("title")

        async def _title_held(_raw) -> Optional[dict]:
            # applied proof: the ruler now HOLDS the title (de-jure progress flips to
            # held==total for the minted tier, or query_realm primary_title matches).
            prog = await _inline_read(gw, "query_dejure_progress",
                                      {"character_id": character_id, "title_key": tkey})
            # The minted title is fully de-jure-held by definition once it exists.
            held, total = _as_int(prog.get("held")), _as_int(prog.get("total"))
            realm = await _inline_read(gw, "query_realm", {"character_id": character_id})
            prim = str((realm.get("primary_title") or realm.get("primary_title_key") or "")).lower()
            titles = [str(t).lower() for t in (realm.get("titles") or realm.get("titles_held") or [])
                      if isinstance(t, (str,))]
            tkl = str(tkey or "").lower()
            if tkl and (tkl == prim or tkl in titles):
                return {"title_held": tkey, "via": "realm_titles"}
            if total and held >= total:
                return {"title_held": tkey, "via": "dejure_full", "held": held, "total": total}
            return None

        conf = await _confirm_write(gw, bridge_action, bridge_params,
                                    readback=_title_held, deadline_s=12.0)
        raw = (conf.get("raw") if isinstance(conf, dict) else {}) or {}
        resolved = conf.get("resolved") if isinstance(conf, dict) else None
        status = str(raw.get("status") or "")
        reason = str(raw.get("reason") or raw.get("error") or "")
        if resolved is not None:
            return envelope("do", verdict="applied",
                            result={"intent": intent, "title_key": tkey, "readback": resolved, "raw": raw},
                            actor=actor, game=gw.game(), warnings=warnings, bridge_health=gw.health)
        if status == "rejected" and reason:
            counts = _parse_dejure_counts(reason)
            blk = map_raw_reason(reason, summary=reason)
            if counts is not None:
                blk["measured"] = counts
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=blk, warnings=warnings, bridge_health=gw.health)
        # dispatched without same-call confirmation AND readback still false → honest
        # pending with a poll token (the rare last resort, not the default).
        return envelope("do", verdict="pending", actor=actor, game=gw.game(),
                        pending={"poll_token": raw.get("intent_request_id") or raw.get("request_id", ""),
                                 "drains_when": "unpaused", "expected_change": "title held",
                                 "timeout_s": 15, "verify_with": "inspect(subject='realm') primary_title"},
                        result={"intent": intent, "title_key": tkey, "raw": raw},
                        warnings=warnings, bridge_health=gw.health)

    if intent == "muster":
        # applied proof: a raised/mustering regiment delta vs the pre-dispatch snapshot.
        pre = await gw.read("query_armies", {"character_id": character_id})
        pre = pre if isinstance(pre, dict) else {}

        def _raised_count(armies: dict) -> int:
            regs = armies.get("armies") or armies.get("regiments") or []
            if isinstance(regs, list):
                return sum(1 for r in regs if isinstance(r, dict)
                           and (r.get("is_raised") or r.get("mustering") or r.get("raised")))
            return _as_int(armies.get("raised_count"))

        pre_n = _raised_count(pre)

        async def _army_delta(_raw) -> Optional[dict]:
            now = await _inline_read(gw, "query_armies", {"character_id": character_id})
            now_n = _raised_count(now)
            if now_n > pre_n:
                return {"raised_before": pre_n, "raised_after": now_n}
            return None

        conf = await _confirm_write(gw, bridge_action, bridge_params,
                                    readback=_army_delta, deadline_s=8.0)
        raw = (conf.get("raw") if isinstance(conf, dict) else {}) or {}
        resolved = conf.get("resolved") if isinstance(conf, dict) else None
        reason = str(raw.get("reason") or raw.get("error") or "")
        status = str(raw.get("status") or "")
        if resolved is not None:
            return envelope("do", verdict="applied",
                            result={"intent": intent, "readback": resolved, "raw": raw},
                            actor=actor, game=gw.game(), warnings=warnings, bridge_health=gw.health)
        if reason == "experimental_write_disabled" or status == "experimental_write_disabled":
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=why_blocked("experimental_write_disabled",
                                                summary="add the action to the ck3-enabled-writes file in the bridge state dir (enabled_writes_file in this reply) to enable"),
                            warnings=warnings, bridge_health=gw.health)
        if status not in ("dispatched_pending", "sent_no_confirmation") and reason and not raw.get("success", False):
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=map_raw_reason(reason or status, summary=reason or status),
                            warnings=warnings, bridge_health=gw.health)
        return envelope("do", verdict="pending", actor=actor, game=gw.game(),
                        pending={"poll_token": raw.get("request_id", ""), "drains_when": "unpaused",
                                 "expected_change": "raised regiment delta", "timeout_s": 10,
                                 "verify_with": "inspect(subject='armies')"},
                        result={"intent": intent, "raw": raw},
                        warnings=warnings, bridge_health=gw.health)

    # Should not reach here (only CONFIRMABLE_WRITES route in).
    raw = await gw.write(bridge_action, bridge_params)
    raw = raw if isinstance(raw, dict) else {}
    return envelope("do", verdict="applied", result={"intent": intent, "raw": raw},
                    actor=actor, game=gw.game(), warnings=warnings, bridge_health=gw.health)


# --------------------------------------------------------------------------- scripted_effect
# ck-7fyl / #75 — agent path for the bridge `execute_scripted_effect` action.
#
# TWO halves, with very different validation status:
#   (1) RESOLVE-ONLY (resolve_only=true) — SAFE + VALIDATED. The by-name template resolver
#       (linear scan over the scripted_effect DB) landed faff2143 and is core-dump-verified
#       (re_scripted_effect_dispatch.md / re_scripted_effect_execute.md §0). It only LOOKS UP
#       the effect template; it runs NOTHING. This is the agent-reachable default.
#   (2) EXECUTE (resolve_only=false) — ★ EXPERIMENTAL, VALIDATION-PENDING. The hardened
#       scope_ctx-pinned tree-walk (EFFECT_TREE_WALK + the fake-RNG / watchdog / sigsetjmp
#       crash guards) is MEDIUM confidence — composed from two independently-proven pieces
#       but NOT yet run together on a live observer save (re_scripted_effect_execute.md §0,
#       confidence header). The CURRENT bridge Execute path still has the compound-key /
#       raw-actor-as-scope_ctx bug that FAULTS (doc §0). So the EXECUTE half is gated:
#       the agent must pass experimental=true to even ATTEMPT it, and the bridge's own
#       default-OFF allow-list (experimental_write_disabled) is the real gate — we surface
#       that honestly, never a silent success.
#
# This keeps the surface additive + safe: resolve-only confirmation is freely usable;
# the crash-risky Execute stays behind an explicit experimental opt-in + the bridge gate,
# and is clearly marked validation-pending. We NEVER claim the Execute landed unconfirmed.
#
# ── LEGITIMACY (write-action-legitimacy audit) ──────────────────────────────
# scripted_effect EXECUTE runs a PDXScript effect WITHOUT the trigger that vanilla
# normally fronts it with — so it is a back door around every other gate. Run
# `add_gold` "as a scripted effect" and it mints resources with no cost. The
# release ships no console path to compare against: execute_console_command is
# compiled out of release bridges and is not an MCP tool, so THIS allow-list is
# the agent-side gate, not a mirror of one. It is deliberately the SMALL safe set:
# the audit-blessed reversible/benign primitives (set_variable_conqueror = the
# ally-call-refusal posture char var; noop/bridge_noop = observability probes).
# Everything else is refused before it reaches the bridge. (The bridge keeps its
# OWN default-OFF gate; this is defense-in-depth at the only layer this file owns.)
_SCRIPTED_EFFECT_EXECUTE_ALLOW = frozenset({
    "set_variable_conqueror",   # audit-blessed: sets the `conqueror` flag (ally-call-refusal posture)
    "noop", "bridge_noop",      # bridge-local no-op / observability probes
})

# Resource-/state-granting scripted effects. Listed only so the refusal names the
# legitimacy reason rather than a generic "not allow-listed"; the allow-list above
# already refuses them.
_SCRIPTED_EFFECT_DENY = frozenset({
    "add_gold", "add_prestige", "add_piety",
    "add_dynasty_prestige", "add_realm_law", "add_doctrine",
    "add_claim", "add_pressed_claim", "add_title_claim", "give_title", "add_trait",
})


def scripted_effect_is_allow_listed(name: str) -> bool:
    """True iff this scripted_effect may be EXECUTED through the agent path.

    The allow-list is deliberately tiny (the audit-blessed reversible/benign
    primitives only). Everything else — and the resource-grant family in
    particular — is refused at the agent layer so execute_scripted_effect cannot
    mint resources (write-action-legitimacy audit)."""
    if not name:
        return False
    return name in _SCRIPTED_EFFECT_EXECUTE_ALLOW


async def _do_scripted_effect(gw, a: dict, character_id: int, actor: dict) -> dict[str, Any]:
    effect = a.get("effect") or a.get("effect_name") or a.get("name") or ""
    if not effect:
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("unknown",
                                            summary="scripted_effect requires args.effect (the scripted_effect name)"),
                        bridge_health=gw.health)
    # Default = resolve-only (safe). Execute requires BOTH resolve_only=false AND an
    # explicit experimental=true opt-in.
    want_execute = (a.get("resolve_only") is False) or bool(a.get("execute"))
    experimental = bool(a.get("experimental"))

    # ── LEGITIMACY GATE (agent-layer, defense-in-depth) ────────────────────────
    # Only EXECUTE is gated; resolve-only just looks up the template and runs
    # nothing (safe). A non-allow-listed EXECUTE — and the resource-grant family
    # in particular — is REFUSED here. This refusal precedes the experimental
    # opt-in so even experimental=true cannot run add_gold/add_prestige/add_piety.
    if want_execute and not scripted_effect_is_allow_listed(effect):
        denied = effect in _SCRIPTED_EFFECT_DENY
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked(
                            "console_command_denied" if denied else "experimental_write_disabled",
                            severity="invalid",
                            summary=(
                                f"scripted_effect EXECUTE of '{effect}' is REFUSED: "
                                + ("it is a state-/resource-granting effect: earn resources "
                                   "in-game, do not mint them (write-action-legitimacy audit). "
                                   if denied else
                                   "it is not on the agent EXECUTE allow-list. ")
                                + f"Only {sorted(_SCRIPTED_EFFECT_EXECUTE_ALLOW)} may be executed; "
                                  "use resolve_only=true to safely RESOLVE the template instead."),
                            raw_reason="scripted_effect_not_allow_listed"),
                        bridge_health=gw.health)

    if want_execute and not experimental:
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked(
                            "experimental_write_disabled",
                            summary=("scripted_effect EXECUTE is EXPERIMENTAL/validation-pending "
                                     "(re_scripted_effect_execute.md §0 — the hardened scope_ctx walk "
                                     "is unproven on a live save; the current bridge Execute path can "
                                     "FAULT). Pass experimental=true to attempt it (still gated by the "
                                     "bridge's default-OFF allow-list), or omit resolve_only to just "
                                     "RESOLVE the template (safe).")),
                        bridge_health=gw.health)

    bridge_params = {
        "actor_id": character_id,
        "character_id": character_id,
        "effect_name": effect,
        "effect": effect,
        # resolve_only defaults TRUE here unless the agent explicitly armed Execute.
        "resolve_only": not want_execute,
    }
    raw = await gw.write("execute_scripted_effect", bridge_params)
    raw = raw if isinstance(raw, dict) else {}
    reason = str(raw.get("reason") or raw.get("error") or "")
    status = str(raw.get("status") or "")

    # Bridge default-OFF allow-list gate (the real gate for the Execute write).
    if reason == "experimental_write_disabled" or status == "experimental_write_disabled":
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("experimental_write_disabled",
                                            summary=("scripted_effect EXECUTE is bridge-gated (default OFF) "
                                                     "— add the action to the ck3-enabled-writes file in the bridge state dir (enabled_writes_file in this reply) to enable; "
                                                     "validation still PENDING (re_scripted_effect_execute.md)")),
                        bridge_health=gw.health)
    # Bridge does not know the action (old/parallel build) → honest unavailable.
    if any(tok in reason.lower() for tok in _UNKNOWN_CMD_TOKENS) and not raw.get("success"):
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("experimental_write_disabled",
                                            summary=f"execute_scripted_effect not available on this bridge ({reason})",
                                            raw_reason=reason),
                        bridge_health=gw.health)

    resolved_ok = bool(raw.get("success")) or bool(raw.get("resolved")) or bool(raw.get("found"))
    warnings = []
    if want_execute:
        warnings.append(
            "EXPERIMENTAL-unvalidated: scripted_effect EXECUTE ran via an UNPROVEN hardened "
            "scope_ctx walk (re_scripted_effect_execute.md §0). VERIFY the world-state change "
            "by readback (inspect) — do NOT trust the dispatch verdict. Disposable-save only."
        )
    verdict = "applied" if resolved_ok else "dispatched_pending"
    return envelope("do",
                    verdict=verdict,
                    result={"intent": "scripted_effect", "effect": effect,
                            "mode": "execute" if want_execute else "resolve_only",
                            "status": ("EXPERIMENTAL-unvalidated" if want_execute else "validated_safe"),
                            "raw": raw},
                    actor=actor, game=gw.game(), warnings=warnings, bridge_health=gw.health)


async def _do_break_alliance(gw, a: dict, character_id: int, actor: dict) -> dict[str, Any]:
    """GAP 3 (re_toolgap_break_alliance.md) — dissolve a real alliance via the bridge's
    engine-legitimate break_alliance_cmd (CanExecute→command→consequences). The bridge handler
    is BUILT + crash-guarded but DEFAULT-OFF (g_enable_break_alliance) and observer-save-gated:
    arm it first via session(action="set_experimental", flag="break_alliance", enabled=true),
    and the FIRST live dissolve must run on a DISPOSABLE observer save with
    confirm_observer_save=true (re_break_alliance_FINAL.md §5). While the bridge flag is OFF the
    write returns an HONEST gated refusal that we surface as a typed blocked verdict — never a
    false ok. Stops FUTURE off-goal ally-drags (#125); the current war still runs its course."""
    ally_id = _as_int(a.get("ally_id") or a.get("target_id"))
    if ally_id <= 0:
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("actor_required",
                                            summary="break_alliance requires args.target_id (the ally to break with)"),
                        bridge_health=gw.health)
    bridge_params = {
        "actor_id": character_id,
        "character_id": character_id,
        "ally_id": ally_id,
        "target_id": ally_id,
        # disposable-observer-save smoke gate (re_break_alliance_FINAL.md §5). NEVER fire the
        # first dissolve on the live campaign — pass through the agent's explicit opt-in only.
        "confirm_observer_save": bool(a.get("confirm_observer_save")),
    }
    raw = await gw.write("break_alliance", bridge_params)
    raw = raw if isinstance(raw, dict) else {}
    reason = str(raw.get("reason") or raw.get("error") or "")
    status = str(raw.get("status") or "")
    low = (reason + " " + status).lower()

    # Default-OFF gate → honest typed refusal (the bridge returns break_alliance_disabled).
    if "break_alliance_disabled" in low or "disabled" in low:
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("experimental_write_disabled",
                                            summary=("break_alliance is bridge-gated (default OFF + observer-save "
                                                     "smoke pending): arm it via session(action='set_experimental', "
                                                     "flag='break_alliance', enabled=true), then run the FIRST dissolve "
                                                     "on a DISPOSABLE observer save with confirm_observer_save=true "
                                                     "(re_break_alliance_FINAL.md §5)"),
                                            raw_reason=reason or status),
                        bridge_health=gw.health)
    # Bridge does not know the command (old/parallel build) → honest unavailable.
    if any(tok in low for tok in _UNKNOWN_CMD_TOKENS) and not raw.get("success"):
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("experimental_write_disabled",
                                            summary=f"break_alliance not available on this bridge ({reason or status})",
                                            raw_reason=reason or status),
                        bridge_health=gw.health)
    # Not allied / target invalid → surface the bridge reason, never a false applied.
    if reason and not raw.get("success"):
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=map_raw_reason(reason, summary=reason), bridge_health=gw.health)

    applied = bool(raw.get("success")) or str(raw.get("allied_after", "")).lower() in ("false", "0")
    warnings = [
        "EXPERIMENTAL/observer-save-gated: break_alliance dissolves a REAL alliance via the "
        "engine command pipeline (re_break_alliance_FINAL.md). VERIFY by readback "
        "(inspect(subject='alliances') / query_alliances → allied_after=false). The current "
        "ally-war (if any) still runs its course — the engine does not retroactively eject a "
        "co-belligerent; this only stops FUTURE drags."
    ]
    return envelope("do", verdict="applied" if applied else "dispatched_pending",
                    result={"intent": "break_alliance", "ally_id": ally_id, "raw": raw},
                    actor=actor, game=gw.game(), warnings=warnings, bridge_health=gw.health)


async def _do_force_offensive_stance(gw, a: dict, character_id: int, actor: dict) -> dict[str, Any]:
    """CAIWarCoordinator offensive-stance DRIVE (re_caiwarcoordinator_FINAL.md §5 /
    re_steered_war_offensive_root.md). Enqueues the bridge `force_offensive_stance`
    {war_id, attacker_id} intent (drained on the GAME thread by
    execute_war_force_offensive_stance_intent), which drives the engine's OWN per-coordinator
    stance tick on our war's record so a FORCE-DECLARED war lands an ATTACKER-OFFENSIVE stance
    and actually SIEGES (occupation_score rises) instead of sitting defensive. This is the
    per-war DRIVE; the SETTER (session set_experimental force_offensive_stance) only ARMS the
    default-OFF g_force_offensive_stance flag. While the flag is OFF the bridge rejects the
    drive (disabled_flag_off) and we surface a typed experimental_write_disabled blocked
    verdict — never a false ok. NEEDS-LIVE-VALIDATION (attempt-25)."""
    war_id = _as_int(a.get("war_id"))
    attacker_id = _as_int(a.get("attacker_id") or a.get("actor_id")) or character_id
    if war_id <= 0:
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("unknown",
                                            summary="force_offensive_stance requires args.war_id (the force-declared war)"),
                        bridge_health=gw.health)
    if attacker_id <= 0:
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("actor_required",
                                            summary="force_offensive_stance requires args.attacker_id (or a pinned character_id)"),
                        bridge_health=gw.health)
    bridge_params = {
        "war_id": war_id,
        "attacker_id": attacker_id,
        "actor_id": attacker_id,
        "character_id": attacker_id,
    }
    raw = await gw.write("force_offensive_stance", bridge_params)
    raw = raw if isinstance(raw, dict) else {}
    reason = str(raw.get("reason") or raw.get("error") or "")
    status = str(raw.get("status") or "")
    low = (reason + " " + status).lower()

    # Default-OFF gate → honest typed refusal (the intent executor rejects disabled_flag_off).
    if "disabled_flag_off" in low or "experimental_write_disabled" in low or "disabled" in low:
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("experimental_write_disabled",
                                            summary=("force_offensive_stance is bridge-gated (default OFF + "
                                                     "NEEDS-LIVE-VALIDATION): arm it via session(action='set_experimental', "
                                                     "flag='force_offensive_stance', enabled=true) first "
                                                     "(re_caiwarcoordinator_FINAL.md §5)"),
                                            raw_reason=reason or status),
                        bridge_health=gw.health)
    # Bridge does not know the command (old/parallel build) → honest unavailable.
    if any(tok in low for tok in _UNKNOWN_CMD_TOKENS) and not raw.get("success"):
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("experimental_write_disabled",
                                            summary=f"force_offensive_stance not available on this bridge ({reason or status})",
                                            raw_reason=reason or status),
                        bridge_health=gw.health)
    # War culled / coordinator record not found / attacker no longer primary → surface verbatim.
    if reason and not raw.get("success"):
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=map_raw_reason(reason, summary=reason), bridge_health=gw.health)

    applied = bool(raw.get("success"))
    warnings = [
        "EXPERIMENTAL/NEEDS-LIVE-VALIDATION (attempt-25): force_offensive_stance drives the "
        "engine's own coordinator stance re-eval on a force-declared war so it sieges "
        "(re_caiwarcoordinator_FINAL.md). VERIFY by readback (plan_war / war_score "
        "occupation_score rising over the next ticks), never the immediate dispatch flag."
    ]
    return envelope("do", verdict="applied" if applied else "dispatched_pending",
                    result={"intent": "force_offensive_stance", "war_id": war_id,
                            "attacker_id": attacker_id, "raw": raw},
                    actor=actor, game=gw.game(), warnings=warnings, bridge_health=gw.health)


async def do_impl(gw, intent: str = None, action_class: str = None,
                  candidates: list = None, character_id: int = 0,
                  args: dict = None) -> dict[str, Any]:
    actor, err = await _resolve_actor(gw, character_id, "do")
    if err:
        return err

    # Enumerate mode.
    if not intent:
        if not action_class:
            return envelope("do", ok=False, actor=actor, game=gw.game(),
                            blocked=why_blocked("unknown", summary="enumerate mode needs action_class; execute mode needs intent"),
                            bridge_health=gw.health)
        return await _do_enumerate(gw, action_class, character_id, candidates or [], actor)

    # Execute mode.
    # ck-7fyl / #75 — scripted_effect: a DEDICATED branch (resolve_act has no such type).
    if intent == _SCRIPTED_EFFECT_INTENT:
        return await _do_scripted_effect(gw, dict(args or {}), character_id, actor)

    # GAP 3 (re_toolgap_break_alliance.md) — break_alliance: a DEDICATED branch (resolve_act
    # hard-raises for it). Dispatches the bridge `break_alliance` DIRECT command. Default-OFF
    # + observer-save-gated in the bridge → honest blocked verdict while disabled, never a
    # silent ok. This is the #122/#125 exit lever to stop FUTURE off-goal ally-drags.
    if intent == _BREAK_ALLIANCE_INTENT:
        return await _do_break_alliance(gw, dict(args or {}), character_id, actor)

    # CAIWarCoordinator offensive-stance DRIVE — a DEDICATED branch (resolve_act has no such
    # act-type). Dispatches the bridge `force_offensive_stance` DIRECT command {war_id,
    # attacker_id}. Default-OFF behind g_force_offensive_stance (arm via session
    # set_experimental force_offensive_stance) → honest blocked verdict while disabled, never
    # a silent ok. This is the per-war drive the agent previously had to deliver via raw socket.
    if intent == _FORCE_OFFENSIVE_STANCE_INTENT:
        return await _do_force_offensive_stance(gw, dict(args or {}), character_id, actor)

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
                                  "agent, whatever knowledge/tool-reference.md lists. "
                                + f"Valid intents: {', '.join(INTENTS)}"),
                            raw_reason=f"unknown_intent:{intent}"),
                        bridge_health=gw.health)
    a = dict(args or {})
    warnings: list = []

    # O6: self-disadvantage short-circuit (A6) — never enqueue a never-draining intent.
    if intent == "interact":
        itype = (a.get("interaction") or a.get("interaction_type") or "").lower()
        if itype in SELF_DISADVANTAGE_INTERACTIONS:
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=why_blocked("ai_refuses_self_disadvantaging",
                                                summary=f"AI refuses '{itype}' on an adult landed heir — this never drains via steering"),
                            bridge_health=gw.health)

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
        need = int(total * 0.51 + 0.999) if total else 0
        # total==0 (titular OR resolved-empty tree) → DEFER to the bridge #131/engine gate.
        # Only pre-reject when there IS a tree and we hold < the 51% majority.
        if total > 0 and held < need:
            return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                            blocked=why_blocked("de_jure_control",
                                                summary=f"hold {held}/{need} de-jure counties of {tkey} (need >=51%)",
                                                measured={"held": held, "need": need, "total": total,
                                                          "fraction": round(_f(prog.get('fraction')), 3)}),
                            bridge_health=gw.health)
        # GAP 1 (re_toolgap_mint.md) — deferring to the engine can_create gate: surface the
        # TWO distinct control requirements so a duchy-tier mint does not hit the demesne gate
        # blind. Duchy/county tier: engine requires you PERSONALLY hold the de-jure counties
        # (not pre-checked here — engine refusal is surfaced verbatim). Kingdom/empire titular
        # (total==0): no #131 territorial bar, just gold + not-extant.
        warnings.append({"code": "mint_control_requirement", "is_known_limitation": False,
                         "detail": ("DUCHY/county tier: the engine can_create gate requires you to "
                                    "PERSONALLY hold (demesne or direct de-jure control) the title's "
                                    "de-jure counties — the bridge does NOT pre-check this, a refusal "
                                    "surfaces as create_title_engine_gate. KINGDOM/EMPIRE titular "
                                    f"({tkey}, total={total}): no >=51% territorial bar, only gold + "
                                    "not-already-extant. Verify the mint via query_realm primary_title "
                                    "readback, never the immediate dispatch flag.")})

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
                        blocked=why_blocked("unknown", summary=str(e)), bridge_health=gw.health)
    if resolved is None:
        # resolve_act returns None (not raises) for an unmapped act type.
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        blocked=why_blocked("unknown",
                                            summary=f"intent '{intent}' did not resolve to a bridge action"),
                        bridge_health=gw.health)
    bridge_action, bridge_params = resolved

    # FIX 1 — deferred-write confirmation (#113). Confirmable writes (mint_title, muster)
    # route through _confirm_write: dispatch → readback → applied/rejected, never a bare
    # dispatched_pending surfaced and never a success:false for an object that landed.
    if intent in CONFIRMABLE_WRITES:
        return await _confirm_intent_write(gw, intent, bridge_action, bridge_params,
                                           a, character_id, actor, warnings)

    # experimental gate: scheme/faction → honest gate if not enabled (bridge enforces,
    # but surface a typed code immediately rather than a silent no-op).
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
    PENDING_STATUSES = ("dispatched_pending", "sent_no_confirmation",
                        "queued", "pending")
    _success_declared = "success" in raw
    success = bool(raw.get("success")) and not reason
    if status in PENDING_STATUSES or (not _success_declared and not reason):
        # B* deferred (scheme/faction/decision), the intent-dispatch queue, and
        # any reply that never said whether it worked — all first-class pending.
        return envelope("do", verdict="pending", actor=actor, game=gw.game(),
                        pending={"poll_token": (raw.get("intent_request_id")
                                                or raw.get("request_id", "")),
                                 "drains_when": "unpaused",
                                 "expected_change": intent, "timeout_s": 15,
                                 "why_pending": (
                                     f"the bridge reported status={status!r}"
                                     if status else
                                     "the bridge reply carried no `success` field, so "
                                     "whether this landed is unknown")},
                        result={"intent": intent, "raw": raw}, warnings=warnings, bridge_health=gw.health)
    if not success:
        _summary = reason or status
        _result = None
        if intent == "council_task":
            # lane-query item 1: the bridge's refusal names the seat it used or
            # the seat the task belongs to; surface that instead of the bare code.
            _summary = raw.get("warning") or _summary
            _result = {"intent": intent, "raw": raw}
        return envelope("do", ok=False, verdict="rejected", actor=actor, game=gw.game(),
                        result=_result,
                        blocked=concrete_next(
                            map_raw_reason(reason or status or "unknown", summary=_summary),
                            intent),
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
    # succession_preview (ck-succprev): the per-title partition MAP — which heir gets which
    # title under which succession law (partition vs single-heir), and whether the next
    # succession SCATTERS. Reshaped below into the concise reign-watch summary.
    "succession_preview": ("query_title_succession_map", lambda p: {"character_id": p["cid"]}),
    "claims":     ("query_claims",             lambda p: {"character_id": p["cid"]}),
    "wars":       ("query_wars",               lambda p: {"character_id": p["cid"]}),
    "armies":     ("query_armies",             lambda p: {"character_id": p["cid"]}),
    "factions":   ("query_factions",           lambda p: {"character_id": p["cid"]}),
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
    # to existing bridge actions already listed in TOOL_BRIDGE_ACTIONS["inspect"].
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
# borders/de_jure are KNOWN_BROKEN — surfaced as a warning, never data (B7).
_INSPECT_KNOWN_BROKEN = {"borders", "de_jure"}


async def inspect_impl(gw, subject: str, character_id: int = 0, target_id: int = 0,
                       title_id: int = 0, war_id: int = 0, title_key: str = "",
                       dynasty_id: int = 0, include: list = None,
                       max_results: int = 20) -> dict[str, Any]:
    if subject in _INSPECT_KNOWN_BROKEN:
        return envelope("inspect", result={"subject": subject},
                        warnings=[{"code": "domain_known_broken", "is_known_limitation": True,
                                   "detail": f"{subject} returns memory_access_fault — reconstruct adjacency from plan_war targets"}],
                        game=gw.game(), bridge_health=gw.health)
    _composed = ("war_stance", "heir_plan", "guardian_candidates")
    if subject not in _INSPECT_SUBJECTS and subject not in _composed:
        valid = sorted(list(_INSPECT_SUBJECTS) + list(_composed))
        return envelope("inspect", ok=False, game=gw.game(),
                        blocked=why_blocked("unknown",
                                            summary=f"unknown subject '{subject}'; valid: {', '.join(valid)}"),
                        bridge_health=gw.health)
    # rc2 B6: every subject reads "the ruler" when character_id is omitted. With
    # none bound, the reads decline and came back ok=true as empty readings
    # (war_score's declined reply, war_stance's wars=[]). heir_plan and
    # guardian_candidates run this check themselves through _resolve_actor.
    if not character_id and subject not in ("heir_plan", "guardian_candidates"):
        unbound = unbound_actor_block(await gw.read("query_character", {"character_id": 0}))
        if unbound is not None:
            return envelope("inspect", ok=False, result={"subject": subject}, blocked=unbound,
                            game=gw.game(), bridge_health=gw.health)
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

    # succession_preview subject (ck-succprev): reshape the bridge's flat per-title map into
    # the concise at-a-glance summary (scatters?/per-title law buckets/breaker) + keep the
    # full per-title list under `titles` for drill-down. Pure read; degrades honestly.
    warns: list = []
    if subject == "succession_preview":
        sp_sum = _succession_preview_summary(res)
        titles = res.get("titles") if isinstance(res, dict) else None
        res = {"summary": sp_sum}
        if isinstance(titles, list):
            res["titles"] = titles
        if not sp_sum.get("available"):
            warns.append({"code": "succession_preview_unavailable", "is_known_limitation": True,
                          "detail": "per-title partition map unavailable (" +
                                    str(sp_sum.get("reason", "unknown")) +
                                    ") — fall back to succession_plan (simulation-derived verdict)"})
        elif sp_sum.get("scatters"):
            warns.append({"code": "succession_scatters", "is_known_limitation": False,
                          "detail": "next succession SCATTERS titles across "
                                    f"{sp_sum.get('eligible_heir_count')} heirs — drive eligible-heir "
                                    "count toward 1 BEFORE the trough (single_heir law / land spares as "
                                    "in-realm vassals / consolidate)"})

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
#   break_alliance  → set_break_alliance_enabled {enabled}  (re_break_alliance_FINAL.md §5)
#       Arms the bridge-internal break_alliance_cmd write (dissolve a real alliance via the
#       engine CanExecute→command→consequences pipeline). The break ACTION itself stays
#       bridge-internal-only until 2 clean disposable-save smokes (break + negative) pass —
#       this only flips g_enable_break_alliance so that validation can run.
#   force_offensive_stance → set_force_offensive_stance {enabled}  (re_caiwarcoordinator_FINAL.md §5)
#       Makes a FORCE-DECLARED war land an ATTACKER-OFFENSIVE stance so the engine actually
#       sieges (occupation_score rises) instead of sitting defensive. Writes live AI state;
#       opt-in per write-action-legitimacy. Default OFF.
#
# Honest-refusal contract: the live bridge may NOT yet expose these setters (the hooks are
# FINAL-spec, landed DISABLED-by-default; the IPC setter lands with the hook). If the bridge
# returns unknown_action / unknown_command, we surface a typed `experimental_flag_unavailable`
# blocked verdict — never a silent ok:true. We NEVER claim the flag is validated.
_EXPERIMENTAL_FLAGS: dict[str, dict[str, str]] = {
    "off_goal_veto": {
        "setter": "set_offgoal_veto",
        "spec": "re_offgoal_veto_FINAL.md",
        "summary": "off-goal war veto: AI-chosen wars are suppressed for every controlled character (steered bypass=1 unaffected). set_phase('expansion') arms it.",
    },
    "break_alliance": {
        "setter": "set_break_alliance_enabled",
        "spec": "re_break_alliance_FINAL.md",
        "summary": "arm the bridge-internal break_alliance_cmd write (dissolve a real alliance via the engine command pipeline); action stays bridge-internal-only until 2 disposable-save smokes pass",
    },
    "force_offensive_stance": {
        "setter": "set_force_offensive_stance",
        "spec": "re_caiwarcoordinator_FINAL.md",
        "summary": "force-declared wars land an ATTACKER-OFFENSIVE stance so the engine actually sieges (occupation_score rises)",
    },
    # REMOVED: move_army experimental flag deleted per design (agent does not drive army micro).
}

#: rc2 B8: what the veto actually covers (offgoal_war_veto_hook.cpp): one
#: process-wide flag; the setter ignores character_id.
OFFGOAL_VETO_SCOPE = ("process-wide: one bridge flag covering every controlled character "
                      "of every session; character_id is ignored; no phase disarms it; it "
                      "stays on until set_experimental disarms it or the game exits")

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
                            "unknown", severity="invalid",
                            summary=f"unknown experimental flag '{flag}'; valid: {', '.join(sorted(_EXPERIMENTAL_FLAGS))}"),
                        bridge_health=gw.health)
    raw = await gw.write(meta["setter"], {"enabled": bool(enabled)})
    raw = raw if isinstance(raw, dict) else {}
    err = str(raw.get("error") or raw.get("reason") or raw.get("status") or "").lower()
    # Bridge does not know this setter yet → honest unavailable, never a false ok.
    if any(tok in err for tok in _UNKNOWN_CMD_TOKENS) and not raw.get("success"):
        return envelope("session", ok=False, game=gw.game(),
                        blocked=why_blocked(
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
    elif raw.get("success") is False:
        _why = str(raw.get("reason") or raw.get("error") or "")
        return envelope("session", ok=False, verdict="rejected", game=gw.game(),
                        result={"experimental_flag": flag, "enabled": bool(enabled), "raw": raw},
                        blocked=map_raw_reason(_why, summary=_why or "the bridge refused the flag write"),
                        bridge_health=gw.health)
    return envelope(
        "session",
        verdict="applied" if applied else "dispatched_pending",
        result={
            "experimental_flag": flag,
            "setter": meta["setter"],
            "enabled": bool(enabled),
            "status": "EXPERIMENTAL-unvalidated",
            "validation": "PENDING — do NOT rely on this until the disposable-save live smoke passes",
            "spec": meta["spec"],
            "summary": meta["summary"],
            "raw": raw,
        },
        warnings=[
            f"EXPERIMENTAL-unvalidated: '{flag}' arms a default-OFF engine hook/write that has "
            f"NOT passed live validation ({meta['spec']}). Use only for validation on a "
            f"disposable observer save — never rely on its behaviour on the live campaign."
            + (" set_phase('expansion') arms this same flag in normal play; the caveat "
               "applies there too." if flag == "off_goal_veto" else "")
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
    # LANDED AHEAD OF THE BRIDGE ON PURPOSE. The emitting half is not written
    # yet, and until it is this member is simply never seen. Shipping the
    # client first means the bridge's first refusal is explained rather than
    # unforeseen — which needs the GATE_TABLE row above as well as this one:
    # a member here with no row there is explained in prose and marked
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


#: The two gateway shapes redesign.py is called with, and where each one keeps
#: the bridge client. TRACED, NOT GUESSED — the first list I wrote had
#: ("_a", "_client"), which resolves on NEITHER, so the autonomous agent (the
#: path that actually drives an arm episode) would have reported "I do not know
#: which seat I am" in exactly the configuration the bead exists for. It also
#: had a bare ("client",): on CK3Agent that name is the ANTHROPIC client, and a
#: path that can reach the wrong object is worse than one that reaches nothing.
#:
#:   mcp_server.BridgeGateway   gw.state._client   (BridgeState._client, :614)
#:   agent._AgentGateway        gw._a.bridge       (CK3Agent.bridge, :642)
#:
#: Both land on AsyncBridgeClient, whose `session_id` property delegates to the
#: sync BridgeClient's, minted by the v2 handshake. test_own_session_id asserts
#: these paths against the REAL classes, so a rename breaks the row rather than
#: silently costing every seat its identity.
OWN_SESSION_CLIENT_PATHS = (("state", "_client"), ("_a", "bridge"))


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
    All six verbs are in `_ANY_ACTIONS`, and for those `gw.write` answers a
    timeout with `{"status": "sent_no_confirmation"}` — the bridge's explicit
    "I do not know if this landed". The one shape it takes trouble to report
    honestly was the one the caller overwrote with a literal.

    So the rule is asserted ONCE, here, for every branch including ones not yet
    written: a reply that refuses, or that declines to confirm, cannot reach a
    verdict of `applied`. Four more `if not ok` blocks would have closed four
    holes and left the seventh branch to whoever adds it.
    """
    if not isinstance(env, dict) or env.get("verdict") != "applied":
        return env
    action, res = next(
        ((a, r) for a, r in (writes or [])
         if isinstance(r, dict)
         and (r.get("success") is False or r.get("status") == "sent_no_confirmation")),
        (None, None))
    if res is None:
        return env

    if res.get("success") is False:
        raw = res.get("error") if isinstance(res.get("error"), str) else res.get("reason")
        # ── ck-ie6o: RESOLVE THROUGH THE ONE RESOLVER ─────────────────────
        #
        # This asked `raw in GATE_TABLE` — a membership test against the gate
        # ENUM — so a raw reason that RAW_REASON_TO_GATE maps to a gate under a
        # different spelling became `refusal_reason_unknown` anyway. `game_paused`
        # is mapped to `requires_unpaused` and would have landed here as
        # UNFORESEEN; so did `clock_owned_by_pipeline` for the whole of the census,
        # until ck-3mz0 happened to add it as a GATE_TABLE key and not merely as a
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
                if mapped and mapped != "unknown":
                    gate = mapped
        out = dict(env)
        out["ok"] = False
        out.pop("verdict", None)
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
        out["ok"] = False
        out["blocked"] = why_blocked(
            "confirmation_pending",
            summary=(f"`{action}` was dispatched and the bridge did NOT confirm it. "
                     "The result below is what was REQUESTED, not what was "
                     "observed — read the state back before acting on it."),
            raw_reason="sent_no_confirmation")
        return out
    return env


async def session_impl(gw, action: str, character_id: int = 0, speed: int = 3,
                       enabled: bool = True, disable_native_ai: bool = False,
                       flag: str = "", seconds: int = 60) -> dict[str, Any]:
    """Thin wrapper: run the branches, then apply the one guard to whatever they
    returned. See `unwitnessed_applied` for why the rule lives in one place."""
    wgw = _WitnessingGateway(gw)
    env = await _session_impl_branches(wgw, action, character_id=character_id,
                                       speed=speed, enabled=enabled,
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
    if isinstance(ti, dict) and isinstance(ti.get("data"), dict):
        ti = {**ti["data"], **ti}
    return ti if isinstance(ti, dict) else {}


async def _settle_pause(gw, want_paused, timeout_s: float = 3.0) -> dict:
    """Poll the bridge until `paused` reads `want_paused` (None: one fresh read)."""
    deadline = asyncio.get_running_loop().time() + timeout_s
    ti = await _fresh_tick(gw)
    while want_paused is not None and ti.get("paused") is not want_paused:
        if asyncio.get_running_loop().time() >= deadline:
            break
        await _SLEEP(0.2)
        ti = await _fresh_tick(gw)
    return ti


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


async def _session_impl_branches(gw, action: str, character_id: int = 0, speed: int = 3,
                       enabled: bool = True, disable_native_ai: bool = False,
                       flag: str = "", seconds: int = 60) -> dict[str, Any]:
    if action == "experimental_flags":
        # READ the registry — what experimental flags exist + their validation status.
        return envelope("session", result={
            # rc2 B8: `default` is the boot value. The bridge has no readback,
            # so `live` is unknown; `last_written_here` is this server's own
            # write, if any (set_phase('expansion') arms off_goal_veto).
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
                     "All boot OFF, validation PENDING. set_phase('expansion') arms "
                     "off_goal_veto, which is " + OFFGOAL_VETO_SCOPE + "."),
        }, game=gw.game(), bridge_health=gw.health)
    if action == "set_experimental":
        if not flag:
            return envelope("session", ok=False, game=gw.game(),
                            blocked=why_blocked(
                                "unknown", severity="invalid",
                                summary=("set_experimental requires flag=<name>; valid: "
                                         + ", ".join(sorted(_EXPERIMENTAL_FLAGS))
                                         + ". Read them via session(action='experimental_flags').")),
                            bridge_health=gw.health)
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
        _roster_out = ((roster or {}).get("characters", roster)
                       if isinstance(roster, dict) else roster)
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
            _warn.append("seat_unknown: this session's own row was not found; `controlled` "
                         "is the bridge's resolution of character_id=0, not a confirmed claim")
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
                            blocked=why_blocked(
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
                            blocked=why_blocked(
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
                            blocked=why_blocked(
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
                            blocked=why_blocked(
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
        # speed guard (enforced; today doc-only): 4-5 refused while at_war/near-succession.
        if speed >= 4:
            wars = await gw.read("get_active_wars", {})
            if isinstance(wars, dict) and wars.get("wars"):
                return envelope("session", ok=False, game=gw.game(),
                                blocked=why_blocked("unknown", severity="invalid",
                                                    summary="speed 4-5 REFUSED while at war (the 'speed-4 lost the war invisibly' trap) — cap is 3"),
                                bridge_health=gw.health)
            speed = min(speed, 3)
        res = await gw.write("set_speed", {"speed": speed})
        return envelope("session", verdict="applied", result={"speed": speed, "raw": res},
                        game=gw.game(), bridge_health=gw.health)
    if action == "pause":
        res = await gw.write("pause_game", {})
        return envelope("session", verdict="applied", result={"paused": True}, game=gw.game(), bridge_health=gw.health)
    if action == "unpause":
        res = await gw.write("unpause_game", {})
        # The bridge clears the flag and queues the real PAUSE_SET for the next
        # tick, and the gateway's cached snapshot is not refreshed by the write:
        # ask the bridge until it reads unpaused (or 3 s pass), and report that.
        ti = await _settle_pause(gw, False, 3.0)
        g = _game_from_tick(gw, ti)
        return envelope("session", verdict="applied",
                        result={"paused": bool(g.get("paused")),
                                "settled": ti.get("paused") is False},
                        game=g, bridge_health=gw.health)
    if action == "wait":
        # Let time pass, server-side: unpause if paused, block, pause, report the
        # dates. Claude cannot sleep in Bash (see WAIT_CAP_S above).
        try:
            secs = int(seconds)
        except (TypeError, ValueError):
            secs = 60
        secs = max(1, min(secs, WAIT_CAP_S))
        before = await _settle_pause(gw, None)
        if before.get("paused") is not False:
            await gw.write("unpause_game", {})
            running = await _settle_pause(gw, False, 3.0)
            if running.get("paused") is not False:
                return envelope("session", ok=False, game=_game_from_tick(gw, running),
                                blocked=why_blocked(
                                    "unknown", severity="invalid",
                                    summary=("the game did not unpause within 3 s; nothing was "
                                             "waited. session(action='health'), then retry once.")),
                                bridge_health=gw.health)
        await _SLEEP(secs)
        await gw.write("pause_game", {})
        after = await _settle_pause(gw, True, 3.0)
        g = _game_from_tick(gw, after)
        return envelope("session", verdict="applied", result={
            "waited_s": secs, "cap_s": WAIT_CAP_S,
            "start_date": before.get("game_date"), "end_date": after.get("game_date"),
            "clock_ran": bool(before.get("game_date")) and before.get("game_date") != after.get("game_date"),
            "paused": bool(after.get("paused", True)),
            "note": ("the game is paused again; observe, decide, act, then wait again. "
                     f"For longer than {WAIT_CAP_S} s, call wait more than once."),
        }, game=g, bridge_health=gw.health)
    if action == "auto_events":
        res = await gw.write("set_auto_respond_events", {"enabled": enabled})
        return envelope("session", verdict="applied", result={"auto_events": enabled, "raw": res},
                        game=gw.game(), bridge_health=gw.health)
    return envelope("session", ok=False, game=gw.game(),
                    blocked=why_blocked("unknown",
                                        summary=f"unknown session action '{action}'; valid: control, release, whoami, speed, pause, unpause, wait, health, auto_events, experimental_flags, set_experimental"),
                    bridge_health=gw.health)
