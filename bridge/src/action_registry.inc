// action_registry.inc — SINGLE SOURCE OF TRUTH for the bridge action surface.
// (D1-C / FIX 3.1, bridge-investigation-20260801)
//
// Every registered action is ONE line here:  BRIDGE_ACTION("name", handler)
// This file is #included twice by dispatch_router.cpp with different
// definitions of BRIDGE_ACTION:
//   1. inside dispatch_action()      — expands to the strcmp dispatch chain
//   2. inside is_registered_action() — expands to the name-membership test
// so the dispatch chain and the registry can NEVER drift. Before this table,
// unknown/unregistered names fell through to g_msg_queue and silently HUNG
// forever when the game was paused (the D1 query_titles class — bit at least
// 3 more times before: query_war_stance, query_dejure_progress,
// query_map_graph). The reader thread now fast-fails any name that is not in
// this table with an inline unknown_action reply (ipc_server.cpp,
// try_queue_fallthrough_rejection).
//
// Handlers must have the signature  bool(const char* params_json,
// std::string* response_out).  Handlers with a different shape (or bodies that
// used to live inline in the old dispatch chain) are adapted by the case_*
// wrappers directly above the #include site in dispatch_router.cpp.
//
// Aliases are separate lines sharing one handler (raid = raise_raiders, etc.).
//
// ADDING AN ACTION: add its line here (nothing else to update — dispatch and
// registration follow automatically) and classify it in snapshot.cpp
// (is_bridge_internal_query / is_game_state_query / is_game_thread_action) so
// its pause-state routing is explicit. An action left out of every classifier
// queues to g_msg_queue and, while paused, is refused with a typed
// `game_paused` error by the reader's fall-through guard (N1 belt-and-braces)
// instead of hanging.
//
// rc2 B2: DEBUG VERBS (raw memory reads, X/uinput input injection, the debug
// console) are wrapped in #ifdef CK3_BRIDGE_DEBUG_VERBS here AND their handler
// bodies are compiled out, so a default build neither dispatches, registers
// nor links them. dispatch_router.cpp:refuse_debug_verb answers them with
// error=unavailable_in_release. test_debug_verbs enumerates the set as the
// difference between this table with and without the define.

// --- AI control ---
BRIDGE_ACTION("toggle_all_ai", case_toggle_all_ai)
BRIDGE_ACTION("enable_all_ai", case_enable_all_ai)

BRIDGE_ACTION("send_interaction", do_send_interaction)

// --- Intent Queue (AI evaluation hook) ---
BRIDGE_ACTION("queue_intent", do_queue_intent)
BRIDGE_ACTION("query_intents", do_query_intents)
BRIDGE_ACTION("query_intent_results", do_query_intent_results)
// ck-jsis0: probe a single dispatch (success/rejected/crashed/pending/not_found).
BRIDGE_ACTION("query_dispatch_result", do_query_dispatch_result)
BRIDGE_ACTION("cancel_intent", do_cancel_intent)
BRIDGE_ACTION("clear_intents", do_clear_intents)

// send_decision FIRES a single named decision (it does NOT broad-eval the
// decision DB). FIXPLAN A7 / #64: the broad annotated decision EVAL path is
// disabled because the engine masks SIGALRM during certain trigger hangs, so
// NO signal watchdog (on any thread) can bound it — a campaign-fatal freeze
// (re_decision_eval_hang.md). The bounded, safe substitute is list-mode:
// list_valid_decisions / list_decisions_annotated, which eval ONLY a
// caller-supplied short list under a per-decision watchdog + kDecisionEvalSkip
// culprit backstop. Do NOT attempt a 4th watchdog variant on the broad eval —
// the masked SIGALRM defeats every signal-based bound.
BRIDGE_ACTION("send_decision", do_send_decision)

BRIDGE_ACTION("set_lifestyle_focus", do_set_lifestyle_focus)
BRIDGE_ACTION("set_education_focus", do_set_education_focus)
BRIDGE_ACTION("set_council_task", do_set_council_task)
BRIDGE_ACTION("construction_check", do_construction_check)
BRIDGE_ACTION("add_construction", do_add_construction)
BRIDGE_ACTION("construct_building", do_construct_building)

BRIDGE_ACTION("declare_war", do_declare_war)
BRIDGE_ACTION("declare_war_debug", do_declare_war_debug)
BRIDGE_ACTION("pre_validate_war", do_pre_validate_war)

// Path B' override (ck-8weg): vouch (cb, titles, target) for the next
// AI_CB_SELECTOR call on this thread; consumed once. rc2 B2: a DEBUG verb —
// it takes raw engine pointers from the client.
#ifdef CK3_BRIDGE_DEBUG_VERBS
BRIDGE_ACTION("set_cb_selector_override", do_set_cb_selector_override)
#endif

// Off-goal per-character routine-war veto (re_offgoal_veto_FINAL.md). Arms /
// disarms g_offgoal_veto_enabled at runtime. DEFAULT OFF — the hook is a pure
// pass-through until set here. While armed, a bridge-controlled ruler declares
// no routine offensive war (bypass==0); the agent's own steered wars (bypass==1)
// and every other realm are unaffected.
BRIDGE_ACTION("set_offgoal_veto", do_set_offgoal_veto)

// --- Console Command Passthrough ---
#ifdef CK3_BRIDGE_DEBUG_VERBS
BRIDGE_ACTION("execute_console_command", do_execute_console_command)
#endif

// ck-7fyl: invoke an arbitrary PDXScript scripted_effect by name on a
// character scope. SCAFFOLD — returns "not_implemented_pending_re" until
// CJominiScriptedEffectTemplateDatabase + CJominiScriptedEffect vtable
// are pinned. See re/findings/re_scripted_effect_dispatch.md.
BRIDGE_ACTION("execute_scripted_effect", do_execute_scripted_effect)

// ck-bt9x: invoke a registered ENGINE effect (CStartWarEffect, etc.) with
// explicit params, bypassing the user scripted_effect dispatcher. Sibling
// to execute_scripted_effect — currently only `start_war` is in scope.
// SCAFFOLD — returns "not_implemented_pending_re" until CStartWarEffect
// ctor + scope-evaluator construction are pinned. See
// re/findings/re_start_war_effect_dispatch.md.
BRIDGE_ACTION("execute_engine_effect", do_execute_engine_effect)

// break_alliance (re_break_alliance_FINAL.md, Path B): the literal
// CBreakAllowAllianceEffect break via the gated break_alliance_cmd. The real
// game-thread write is IMPLEMENTED (IntentType::BREAK_ALLIANCE) but stays
// DEFAULT-OFF + observer-save-GATED: it fires only when set_break_alliance_enabled
// is armed AND the caller passes confirm_observer_save=true. Otherwise the
// handler returns an honest gated refusal + the safe-now `conqueror`-variable
// scripted-effect lever.
BRIDGE_ACTION("break_alliance", do_break_alliance)

// Arm/disarm the DEFAULT-OFF real-break-alliance flag (re_break_alliance_FINAL.md
// §5). Bridge-internal RE lever — break_alliance fires the engine call only when
// this is armed AND the caller passes confirm_observer_save=true.
BRIDGE_ACTION("set_break_alliance_enabled", do_set_break_alliance_enabled)

// #79: arm/disarm the default-OFF vacant-seat fill path in do_appoint_councillor.
BRIDGE_ACTION("set_vacant_fill_enabled", do_set_vacant_fill_enabled)

// CAIWarCoordinator offensive-stance drive (Fix B1a, re_caiwarcoordinator_FINAL.md
// / re_steered_war_offensive_root.md). set_force_offensive_stance arms the
// DEFAULT-OFF flag; force_offensive_stance {war_id, attacker_id} enqueues a
// WAR_FORCE_OFFENSIVE_STANCE intent drained on the GAME thread, which drives the
// engine's OWN per-coordinator stance tick on our war's record (legitimate
// engine re-eval — the engine re-selects + writes rec+0x70 via its real
// selector). NEEDS-LIVE-VALIDATION (attempt-25); no-op while the flag is off.
BRIDGE_ACTION("set_force_offensive_stance", do_set_force_offensive_stance)
BRIDGE_ACTION("force_offensive_stance", do_force_offensive_stance)

// ck-eyfb: direct WAR_CREATE_CORE invocation, bypassing CStartWarEffect
// (whose scope-evaluator construction is open RE work — ck-bt9x). Mirrors
// WAR_DECLARE_SPECIAL_HANDLER (FUN_065db970): builds a minimal CB-params
// struct and calls WAR_CREATE_CORE directly. Designed for the LLM-as-player
// claim-war scenario (A.6 / ck-1qzw). Caller validates upstream gates.
BRIDGE_ACTION("call_war_create_core", do_call_war_create_core)
BRIDGE_ACTION("do_call_war_create_core", do_call_war_create_core)

// A8.2 (re_raise_armies.md): raise a CHOSEN AI's levies. RaiseAllArmiesCore
// is hard-wired to g_PlayerCharacterID, so the raise runs on the game thread
// via the guarded global-swap primitive (execute_raise_armies_intent),
// queued behind the intent/drain gate. NEEDS-LIVE-HARNESS-VALIDATION — see
// re/findings/re_raise_armies.md and harness scenario strategic_raise_levies.
BRIDGE_ACTION("raise_armies", do_raise_armies)

// #96 (re_raid_system.md): toggle a CHOSEN AI's raised armies into RAIDING
// mode (Norse/tribal loot engine — gold/prestige/piety, no casus belli).
// Hard-wired to g_PlayerCharacterID, so it runs on the game thread via the
// guarded global-swap primitive (execute_raise_raiders_intent), queued behind
// the intent/drain gate. Validator-gated: ineligible/non-Norse ruler no-ops.
// Raise armies first (raise_armies).
BRIDGE_ACTION("raise_raiders", do_raise_raiders)
BRIDGE_ACTION("raid", do_raise_raiders)

// REMOVED: move_army / order_siege / siege_holding / set_move_army_enabled
// dispatch cases deleted per design (agent does not drive army micro).

// #111 (re_create_title_action.md): LEGITIMATE de-jure create-title. Builds a
// CCreateTitleCommand for the AI ruler, runs its engine CanExecute gate (de-jure
// control + not-extant + gold cost), and on PASS dispatches it via
// GAME_COMMAND_DISPATCH(0xe). Game-thread only (execute_create_title_intent),
// under watchdog+sigsetjmp+fake-RNG. Pass actor_id + (title_id | title_key,
// e.g. "d_northumbria"). Unlocks the negotiate_the_danelaw 2nd-duchy gate.
BRIDGE_ACTION("create_title", do_create_title)

// ck-kkaxx / ck-zvu5l.3: parse outer `<effect> = { ... }` and route known
// effects (today: `start_war`) through existing direct-call handlers
// (call_war_create_core). Other effects return
// `not_implemented_pending_compiler_re` until the PDX-script compiler
// entry point lands. See re/findings/re_script_block_compiler_quest.md.
BRIDGE_ACTION("execute_script_block", do_execute_script_block)

// --- Pause / Speed ---
BRIDGE_ACTION("pause_game", case_pause_game)
BRIDGE_ACTION("unpause_game", case_unpause_game)
BRIDGE_ACTION("set_speed", case_set_speed)

// --- SDL Input Injection (ck-77pb) ---
#ifdef CK3_BRIDGE_DEBUG_VERBS
BRIDGE_ACTION("inject_mouse_click", do_inject_mouse_click)
BRIDGE_ACTION("select_observe_default", do_select_observe_default)
#endif

// ck-dq7ym.8: programmatic observe-mode entry via the live
// CJominiMultiplayerLobbyView singleton (captured by lobby_view_hook).
// ck3ctl tries this first; falls back to select_observe_default on miss.
BRIDGE_ACTION("enter_observe_mode", do_enter_observe_mode)

// --- AI Subsystem Control ---
BRIDGE_ACTION("toggle_ai_subsystem", case_toggle_ai_subsystem)
BRIDGE_ACTION("set_character_ai", case_set_character_ai)

// --- Per-Character Subsystem Control (Phase C) ---
BRIDGE_ACTION("set_character_subsystems", case_set_character_subsystems)
BRIDGE_ACTION("get_character_subsystems", case_get_character_subsystems)

// --- Decision-budget governor (ck-6kvq.13, R6) ---
BRIDGE_ACTION("report_usage", do_report_usage)
BRIDGE_ACTION("set_budget_config", do_set_budget_config)
BRIDGE_ACTION("get_budget_state", do_get_budget_state)

// --- The shared append-only board (ck-6kvq.12, R4) ---
// ARM-GATED AT RUNTIME. Being in this table is necessary but NOT sufficient:
// dispatch_action() and is_registered_action() both consult board().enabled()
// FIRST, so in every arm but B these three names answer `unknown_action` and
// are absent from the registry (decision G5, option A2 — the board tools are
// ABSENT in arm A, not present-and-refused). The table entry exists so that
// arm B needs no separate dispatch path, not because the verbs are always live.
BRIDGE_ACTION("board_post", do_board_post)
BRIDGE_ACTION("board_read", do_board_read)
BRIDGE_ACTION("board_admin", do_board_admin)

// --- Bridge State Recovery ---
BRIDGE_ACTION("get_bridge_state", do_get_bridge_state)
BRIDGE_ACTION("who_holds_the_lock", do_who_holds_the_lock)
BRIDGE_ACTION("get_offset_validation", case_get_offset_validation)

// --- Controlled Character Selection (Phase A) ---
BRIDGE_ACTION("set_controlled_character", do_set_controlled_character)
BRIDGE_ACTION("release_character", do_release_character)
BRIDGE_ACTION("list_watchlist", do_list_watchlist)
BRIDGE_ACTION("get_player_character", do_get_player_character)
BRIDGE_ACTION("get_controlled_roster", do_get_controlled_roster)

// --- Query Handlers (return JSON data) ---
BRIDGE_ACTION("query_character", do_query_character)
BRIDGE_ACTION("query_opinion", do_query_opinion)
BRIDGE_ACTION("query_opinion_breakdown", do_query_opinion_breakdown)
BRIDGE_ACTION("query_imprisonment_reasons", do_query_imprisonment_reasons)
BRIDGE_ACTION("query_realm", do_query_realm)

// READ-ONLY de-jure-control meter (replaces the hang-prone create_title probe):
// {character_id, title_key="e_britannia"} -> {held, total, fraction}.
BRIDGE_ACTION("query_dejure_progress", do_query_dejure_progress)

BRIDGE_ACTION("query_succession", do_query_succession)
BRIDGE_ACTION("query_borders", do_query_borders)
BRIDGE_ACTION("query_claims", do_query_claims)
BRIDGE_ACTION("query_de_jure", do_query_de_jure)
BRIDGE_ACTION("query_county_details", do_query_county_details)
BRIDGE_ACTION("query_development_growth", do_query_development_growth)
BRIDGE_ACTION("query_county_neighbors", do_query_county_neighbors)
BRIDGE_ACTION("query_province_adjacency", do_query_province_adjacency)

// map_graph: baked-topology windowed county graph + live overlay (re_agent_map_*).
BRIDGE_ACTION("query_map_graph", do_query_map_graph)

BRIDGE_ACTION("query_wars", do_query_wars)
BRIDGE_ACTION("get_active_wars", do_get_active_wars)
BRIDGE_ACTION("query_war_db_scan", do_query_war_db_scan)
BRIDGE_ACTION("query_war_score_breakdown", do_query_war_score_breakdown)

// READ-ONLY per-war AI war-stance / siege-intent (re_query_war_stance.md).
// Pure memory reads; PAUSED-safe (no engine probe). The true-stance record
// walk is gated default-OFF behind g_war_stance_record_walk_enabled.
BRIDGE_ACTION("query_war_stance", do_query_war_stance)

// Toggle the (default-OFF) war-stance coordinator record walk at runtime.
// { "enabled": <bool> }. Pure flag store — no game-thread reentry.
BRIDGE_ACTION("set_war_stance_record_walk", case_set_war_stance_record_walk)

BRIDGE_ACTION("query_war_declarations", do_query_war_declarations)
// ck-sd4d: ENDED wars. WAR_END_CORE wipes the CWar in the same call that marks
// it ended, so no enumerator can ever surface one; this serves a hook-fed
// ledger instead. Identity is war_id, or cb_type + belligerents — a query that
// names only a target title is refused (two 1066 wars target k_england).
BRIDGE_ACTION("query_war_history", do_query_war_history)
BRIDGE_ACTION("query_scheme_inits", do_query_scheme_inits)
BRIDGE_ACTION("query_marriage_observations", do_query_marriage_observations)
BRIDGE_ACTION("query_council_tasks", do_query_council_tasks)
BRIDGE_ACTION("query_council", do_query_council)
BRIDGE_ACTION("query_character_memories", do_query_character_memories)
BRIDGE_ACTION("query_fabricate_targets", do_query_fabricate_targets)
BRIDGE_ACTION("query_factions", do_query_factions)
BRIDGE_ACTION("query_faction_threat", do_query_faction_threat)
BRIDGE_ACTION("query_vassal_loyalty", do_query_vassal_loyalty)
BRIDGE_ACTION("query_schemes", do_query_schemes)
BRIDGE_ACTION("query_incoming_schemes", do_query_incoming_schemes)
BRIDGE_ACTION("query_armies", do_query_armies)
BRIDGE_ACTION("query_lifestyle", do_query_lifestyle)
BRIDGE_ACTION("query_interaction", do_query_interaction_acceptance)
BRIDGE_ACTION("query_interaction_acceptance", do_query_interaction_acceptance)
BRIDGE_ACTION("query_trigger_result", do_query_trigger_result)
BRIDGE_ACTION("query_trigger_slots", do_query_trigger_slots)
BRIDGE_ACTION("evaluate_named_trigger", do_evaluate_named_trigger)
BRIDGE_ACTION("query_debug_flags", do_query_debug_flags)
BRIDGE_ACTION("query_relations", do_query_relations)
BRIDGE_ACTION("query_hooks", do_query_hooks)
BRIDGE_ACTION("query_available_cbs", do_query_available_cbs)
BRIDGE_ACTION("query_valid_cbs", do_query_valid_cbs)
BRIDGE_ACTION("query_cb_validity_strict", do_query_cb_validity_strict)

// READ-ONLY: enumerate ALL win-line CBs for an (attacker, target) pair with
// validity/scope/grants/requirements_unmet/notes. UNPAUSED required (engine
// CB_VALIDITY_CHECK runs on the game thread, like valid_cbs_strict).
BRIDGE_ACTION("query_cbs_for_target", do_query_cbs_for_target)
// Batched: N targets resolved in ONE deferred drain window (one coordinator
// pulse), same single-flight g_cb_query discipline (_slot_drain_profile.md §3b).
BRIDGE_ACTION("query_cbs_for_targets", do_query_cbs_for_targets)

BRIDGE_ACTION("query_alliances", do_query_alliances)

// #122 alliance hygiene: surface wars the ruler was dragged into as a
// called ally (distant ally-offensive wars tying up its levies).
BRIDGE_ACTION("query_call_to_arms", do_query_call_to_arms)

BRIDGE_ACTION("query_diplomatic_range", do_query_diplomatic_range)
BRIDGE_ACTION("query_cb_catalog", do_query_cb_catalog)
BRIDGE_ACTION("query_building_types", do_query_building_types)

// --- Phase-3 probes (ck-e58l.8) ---
BRIDGE_ACTION("probe_interaction_cascade", do_probe_interaction_cascade)
BRIDGE_ACTION("probe_gate_4_slots", do_probe_gate_4_slots)
BRIDGE_ACTION("list_pending_interactions", do_list_pending_interactions)
BRIDGE_ACTION("tick_diff", do_tick_diff)
BRIDGE_ACTION("probe_war_orphans", do_probe_war_orphans)

BRIDGE_ACTION("list_interactions", case_list_interactions)
// #64: list_decisions always serves the LIGHT name+hash list (see
// case_list_decisions in dispatch_router.cpp for the full rationale).
BRIDGE_ACTION("list_decisions", case_list_decisions)

// #64 LIST-MODE annotated decisions (2026-06-19, production): the full-DB engine
// eval is unsafe (hangs the game thread on certain decisions — the engine masks
// SIGALRM so no watchdog can bound it; re_decision_eval_hang.md). The safe, useful
// form is BOUNDED: the agent passes a "decisions":[names/hashes] list and we eval
// ONLY those (a handful) — fast, and the agent only asks about decisions it's
// actually considering. do_list_* serve the light name+hash list when no list is
// given (so a stray call can never freeze), and the bounded deferred eval when a
// list is present (per-decision watchdog + the kDecisionEvalSkip culprit backstop).
BRIDGE_ACTION("list_valid_decisions", do_list_valid_decisions)
BRIDGE_ACTION("list_decisions_annotated", do_list_decisions_annotated)

// --- New observation tools ---
BRIDGE_ACTION("get_tick_info", do_get_tick_info)

// ck-9fc3 — the harness declares WHICH SAVE is loaded, using ck-6kvq.2's
// run_summary.json identity ("episode_id" + save.sha256). This is the ONLY
// trigger that can restore agent steering quarantined by a world reload, and
// the only caller of the on-disk lock carrier. Reading a character id to guess
// the world is explicitly what it exists to replace. See
// bridge/include/world_identity.h.
BRIDGE_ACTION("declare_save_identity", do_declare_save_identity)
BRIDGE_ACTION("get_ai_personality", do_get_ai_personality)
BRIDGE_ACTION("set_ai_personality", do_set_ai_personality)
BRIDGE_ACTION("get_ai_evaluations", do_get_ai_evaluations)
BRIDGE_ACTION("search_characters", do_search_characters)
BRIDGE_ACTION("query_heir_education", do_query_heir_education)
BRIDGE_ACTION("query_marriage_candidates", do_query_marriage_candidates)
BRIDGE_ACTION("query_title_hierarchy", do_query_title_hierarchy)
BRIDGE_ACTION("query_title_claimants", do_query_title_claimants)
BRIDGE_ACTION("query_title_history", do_query_title_history)
BRIDGE_ACTION("query_title_succession", do_query_title_succession)
BRIDGE_ACTION("query_title_succession_map", do_query_title_succession_map)
BRIDGE_ACTION("query_joinable_factions", do_query_joinable_factions)
BRIDGE_ACTION("faction_action", do_dispatch_faction_intent)
BRIDGE_ACTION("scheme_action", do_dispatch_scheme_intent)
BRIDGE_ACTION("move_capital", do_move_capital)
BRIDGE_ACTION("set_obligation_level", do_set_obligation_level)
BRIDGE_ACTION("appoint_councillor", do_appoint_councillor)
BRIDGE_ACTION("maa_recruit", do_maa_recruit)
BRIDGE_ACTION("maa_station", do_maa_station)
BRIDGE_ACTION("maa_unstation", do_maa_unstation)
BRIDGE_ACTION("hire_mercenary", do_hire_mercenary)
BRIDGE_ACTION("set_doctrine", do_set_doctrine)
BRIDGE_ACTION("set_character_faith", do_set_character_faith)
BRIDGE_ACTION("set_character_culture", do_set_character_culture)
BRIDGE_ACTION("set_title_law", do_set_title_law)
BRIDGE_ACTION("query_hireable_mercenaries", do_query_hireable_mercenaries)
BRIDGE_ACTION("query_valid_war_targets", do_query_valid_war_targets)

// #110: de-jure-duchy-completion conquest targeting (2nd-duchy win-line).
BRIDGE_ACTION("query_conquest_targets", do_query_conquest_targets)

BRIDGE_ACTION("query_cb_options", do_query_cb_options)
BRIDGE_ACTION("query_election_state", do_query_election_state)
BRIDGE_ACTION("query_dynasty", do_query_dynasty)
BRIDGE_ACTION("query_levy_readiness", do_query_levy_readiness)
BRIDGE_ACTION("query_partition_preview", do_query_partition_preview)
BRIDGE_ACTION("query_succession_simulation", do_query_succession_simulation)
BRIDGE_ACTION("query_trait_db", do_query_trait_db)
BRIDGE_ACTION("query_trait_compatibility", do_query_trait_compatibility)

// --- Law Queries & Actions ---
BRIDGE_ACTION("query_realm_laws", do_query_realm_laws)
BRIDGE_ACTION("query_law_groups", do_query_law_groups)
BRIDGE_ACTION("query_available_law_changes", do_query_available_law_changes)
BRIDGE_ACTION("change_realm_law", do_change_realm_law)
BRIDGE_ACTION("query_innovation_progress", do_query_innovation_progress)
BRIDGE_ACTION("query_fascination_progress", do_query_fascination_progress)
BRIDGE_ACTION("query_research_speed", do_query_research_speed)

// --- RE Validation Endpoints ---
#ifdef CK3_BRIDGE_DEBUG_VERBS
BRIDGE_ACTION("memory_probe", do_query_memory_probe)
BRIDGE_ACTION("resolve_global", do_query_resolve_global)
#endif

// --- Event Queries ---
BRIDGE_ACTION("pending_events", do_query_pending_events)
BRIDGE_ACTION("notifications", do_query_pending_events)
BRIDGE_ACTION("query_pending_events", do_query_pending_events)
BRIDGE_ACTION("query_event_probabilities", do_query_event_probabilities)
BRIDGE_ACTION("event_probabilities", do_query_event_probabilities)

// --- Event Response ---
BRIDGE_ACTION("respond_event", do_respond_event)

// --- Auto-respond events toggle ---
BRIDGE_ACTION("set_auto_respond_events", case_set_auto_respond_events)

// --- Pause/Speed via event_handler (new protocol names) ---
BRIDGE_ACTION("pause", case_set_pause)
BRIDGE_ACTION("set_pause", case_set_pause)

// --- Query dispatch (new tool schema names) ---
BRIDGE_ACTION("query", case_query_meta)
