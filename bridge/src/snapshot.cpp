// snapshot.cpp — Game state snapshot capture and query safety classification.
//
// See snapshot.h for full architectural documentation.

#include "bridge.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "snapshot.h"
#include "ck3_offsets.h"

#include <cstdio>
#include <cstring>
#include <setjmp.h>
#include <signal.h>
#include <string>
#include <unordered_map>

namespace bridge {

// Global snapshot store instance
SnapshotStore g_snapshot CK3_EARLY_INIT;

// Forward declarations for query handlers (defined in query_handlers.cpp)
bool do_query_character(const char* params_json, std::string* out);
bool do_query_realm(const char* params_json, std::string* out);
bool do_query_wars(const char* params_json, std::string* out);
bool do_query_council(const char* params_json, std::string* out);
bool do_query_factions(const char* params_json, std::string* out);
bool do_query_faction_threat(const char* params_json, std::string* out);
bool do_query_succession(const char* params_json, std::string* out);
bool do_query_claims(const char* params_json, std::string* out);
bool do_query_schemes(const char* params_json, std::string* out);
bool do_query_lifestyle(const char* params_json, std::string* out);
bool do_get_ai_personality(const char* params_json, std::string* out);
std::string enumerate_interactions();
std::string enumerate_decisions();

// SIGSEGV recovery support (defined in bridge_init.cpp)
extern thread_local sigjmp_buf g_recovery_point;
extern thread_local bool g_in_protected_region;

// =============================================================================
// Snapshot Capture
// =============================================================================

// Minimum tick before we attempt snapshot capture.
// Ticks 0 and 1 are too early — game state may not be fully initialized.
static constexpr uint64_t SNAPSHOT_MIN_TICK = 2;

// Helper: build a params JSON for a specific character_id
static std::string make_char_params(uint32_t char_id) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"character_id\":%u}", char_id);
    return std::string(buf);
}

// Helper: run a single do_query_* call inside SIGSEGV protection.
// Returns true if the query succeeded without crashing.
// On SIGSEGV, logs the failure and returns false (response left empty).
static bool protected_query(const char* query_name, uint32_t char_id,
                            bool (*query_fn)(const char*, std::string*),
                            const char* params, std::string* response) {
    response->clear();

    g_in_protected_region = true;
    int jmp_rc = sigsetjmp(g_recovery_point, 1);
    if (jmp_rc == 0) {
        bool ok = query_fn(params, response);
        g_in_protected_region = false;
        if (!ok) {
            if (g_logfile) {
                fprintf(g_logfile, "[CK3-Bridge] Snapshot: %s(char=%u) returned false\n",
                        query_name, char_id);
                fflush(g_logfile);
            }
            return false;
        }
        return true;
    } else {
        // SIGSEGV recovery
        g_in_protected_region = false;
        if (g_logfile) {
            fprintf(g_logfile, "[CK3-Bridge] Snapshot: SIGSEGV in %s(char=%u) — skipped\n",
                    query_name, char_id);
            fflush(g_logfile);
        }
        response->clear();
        return false;
    }
}

// Helper: run a global query (no char_id param) inside SIGSEGV protection.
// Returns the result string, or empty on failure.
static std::string protected_global_query(const char* query_name,
                                          std::string (*query_fn)()) {
    g_in_protected_region = true;
    int jmp_rc = sigsetjmp(g_recovery_point, 1);
    if (jmp_rc == 0) {
        std::string result = query_fn();
        g_in_protected_region = false;
        return result;
    } else {
        g_in_protected_region = false;
        if (g_logfile) {
            fprintf(g_logfile, "[CK3-Bridge] Snapshot: SIGSEGV in %s — skipped\n",
                    query_name);
            fflush(g_logfile);
        }
        return {};
    }
}

void capture_snapshot() {
    uint64_t tick = g_tick_id.load(std::memory_order_relaxed);

    // Guard 1: Skip early ticks — game state not fully initialized yet.
    if (tick < SNAPSHOT_MIN_TICK) {
        if (g_logfile) {
            fprintf(g_logfile, "[CK3-Bridge] Snapshot: skipping tick %lu (too early, min=%lu)\n",
                    tick, SNAPSHOT_MIN_TICK);
            fflush(g_logfile);
        }
        return;
    }

    // Gather the set of controlled characters
    std::vector<uint32_t> chars_to_snapshot;
    {
        std::lock_guard<std::mutex> lock(g_controlled_mutex);
        chars_to_snapshot = g_controlled_chars;
    }

    // If no explicitly controlled characters, use controlled_primary.
    // Fall back to PLAYER_CHAR_ID for bootstrap (before LLM connects and sets controlled char).
    if (chars_to_snapshot.empty()) {
        uint32_t primary = g_controlled_primary.load(std::memory_order_relaxed);
        if (primary == 0) {
            primary = read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
        }
        if (primary != 0 && primary != 0xFFFFFFFF) {
            chars_to_snapshot.push_back(primary);
        }
    }

    // Guard 2: No controlled character available — nothing to snapshot.
    if (chars_to_snapshot.empty()) {
        if (g_logfile && tick % 50 == 0) {
            fprintf(g_logfile, "[CK3-Bridge] Snapshot: skipping tick %lu (no controlled character)\n",
                    tick);
            fflush(g_logfile);
        }
        return;
    }

    g_snapshot.set_tick(tick);
    g_snapshot.begin_capture();

    int queries_ok = 0;
    int queries_fail = 0;

    // --- Skip-on-repeated-failure tracking ---
    // If a query SIGSEGV'd 3 times in a row, skip it for the next 50 ticks.
    // Key: query name string. Value: {consecutive_fail_count, skip_until_tick}.
    struct FailTracker {
        int consecutive_fails = 0;
        uint64_t skip_until_tick = 0;
    };
    static std::unordered_map<std::string, FailTracker> s_fail_trackers;
    static constexpr int FAIL_THRESHOLD = 3;
    static constexpr uint64_t SKIP_TICKS = 50;

    // Per-character snapshots
    for (uint32_t cid : chars_to_snapshot) {
        std::string params = make_char_params(cid);
        std::string response;

        // Macro-like helper to reduce repetition, with skip-on-repeated-failure
        #define SNAP_QUERY(name, fn) do { \
            std::string _fkey = std::string(#name) + ":" + std::to_string(cid); \
            auto& _ft = s_fail_trackers[_fkey]; \
            if (_ft.consecutive_fails >= FAIL_THRESHOLD && tick < _ft.skip_until_tick) { \
                queries_fail++; \
                break; /* skip this query — still in cooldown */ \
            } \
            if (protected_query(#name, cid, fn, params.c_str(), &response)) { \
                _ft.consecutive_fails = 0; /* reset on success */ \
                if (!response.empty()) { \
                    g_snapshot.store(snapshot_key(#name, cid), std::move(response)); \
                    queries_ok++; \
                } \
            } else { \
                _ft.consecutive_fails++; \
                if (_ft.consecutive_fails >= FAIL_THRESHOLD) { \
                    _ft.skip_until_tick = tick + SKIP_TICKS; \
                    if (g_logfile) { \
                        fprintf(g_logfile, "[CK3-Bridge] Snapshot: %s(char=%u) failed %d times — " \
                                "skipping for %lu ticks\n", #name, cid, \
                                _ft.consecutive_fails, SKIP_TICKS); \
                        fflush(g_logfile); \
                    } \
                } \
                queries_fail++; \
            } \
        } while(0)

        SNAP_QUERY(query_character, do_query_character);
        SNAP_QUERY(query_realm, do_query_realm);
        SNAP_QUERY(query_wars, do_query_wars);
        SNAP_QUERY(query_council, do_query_council);
        SNAP_QUERY(query_factions, do_query_factions);
        SNAP_QUERY(query_faction_threat, do_query_faction_threat);
        SNAP_QUERY(query_succession, do_query_succession);
        SNAP_QUERY(query_claims, do_query_claims);
        SNAP_QUERY(query_schemes, do_query_schemes);
        SNAP_QUERY(query_lifestyle, do_query_lifestyle);
        SNAP_QUERY(get_ai_personality, do_get_ai_personality);

        #undef SNAP_QUERY
    }

    // Global snapshots (not per-character)
    {
        std::string interactions = protected_global_query("list_interactions", enumerate_interactions);
        if (!interactions.empty()) {
            g_snapshot.store("list_interactions", std::move(interactions));
            queries_ok++;
        } else {
            queries_fail++;
        }
    }
    {
        std::string decisions = protected_global_query("list_decisions", enumerate_decisions);
        if (!decisions.empty()) {
            g_snapshot.store("list_decisions", std::move(decisions));
            queries_ok++;
        } else {
            queries_fail++;
        }
    }

    g_snapshot.end_capture();

    // Log capture timing periodically (or always if there were failures)
    static uint64_t last_log_tick = 0;
    if (queries_fail > 0 || tick - last_log_tick >= 50) {
        last_log_tick = tick;
        if (g_logfile) {
            fprintf(g_logfile, "[CK3-Bridge] Snapshot captured at tick %lu "
                    "(%zu chars, %d ok, %d failed)\n",
                    tick, chars_to_snapshot.size(), queries_ok, queries_fail);
            fflush(g_logfile);
        }
    }
}

// =============================================================================
// Query Safety Classification
// =============================================================================

bool is_bridge_internal_query(const char* action_type) {
    // These queries read ONLY from bridge-internal state (atomics, mutex-protected
    // containers) and never touch game object pointers. Safe from any thread.
    //
    // L13 D1 (witness W2/W6): the deferred-slot submitters, get_active_wars,
    // pre_validate_war, get_ai_personality, query_council_tasks and
    // query_debug_flags read game objects and moved to is_game_state_query;
    // set_ai_personality writes engine memory and moved to
    // is_game_thread_action.
    //
    // THE ENUMERATED EXCEPTIONS: handlers served here that still read (or poke a
    // flag in) game memory, each through safe_read, with the reason it stays on
    // this thread. ONE declaration: row A0 of bridge/scripts/inline_safety_static.py
    // reads these lines (`ipc-exception <name>: <reason>`) and reds any other
    // IPC-thread handler that touches game memory, and any line whose handler no
    // longer does.
    //   ipc-exception get_tick_info: the clock's observation must answer when the drain is not running
    //   ipc-exception pause_game: the player's clock verb: must answer when the drain is not running
    //   ipc-exception unpause_game: the player's clock verb: must answer when the drain is not running
    //   ipc-exception set_pause: the player's clock verb: must answer when the drain is not running
    //   ipc-exception set_speed: the player's clock verb: must answer when the drain is not running
    //   ipc-exception send_interaction: an intent enqueuer: resolves actor/target, then polls the drain's verdict
    //   ipc-exception declare_war: an intent enqueuer that polls (its pre-validation runs on the game thread)
    //   ipc-exception send_decision: an intent enqueuer that polls
    //   ipc-exception queue_intent: an intent enqueuer
    //   ipc-exception faction_action: an intent enqueuer that polls
    //   ipc-exception scheme_action: an intent enqueuer that polls
    //   ipc-exception query_interaction_check: an intent enqueuer that polls
    //   ipc-exception set_controlled_character: a management verb (liveness read, AI toggle); operator/setup
    //   ipc-exception release_character: a management verb (AI toggle); operator/setup
    //   ipc-exception set_character_ai: a management verb (AI toggle); operator/setup
    //   ipc-exception enter_observe_mode: the operator's observer bring-up
    //   ipc-exception get_offset_validation: static offset probes
    //   ipc-exception query_cb_catalog: the CB database, immutable after startup
    //   ipc-exception get_bridge_state: scalar global reads (the player id)
    //   ipc-exception report_usage: scalar global read (the game date) for the budget ledger
    //   ipc-exception get_budget_state: scalar global read (the game date) for the budget ledger
    //   ipc-exception board_post: scalar global read (the game date) for the post's stamp
    //   ipc-exception toggle_all_ai: single-byte AI flag stores
    //   ipc-exception toggle_ai_subsystem: single-byte AI flag stores
    //   ipc-exception get_player_character: scalar global read (the player id)
    //   ipc-exception get_controlled_roster: scalar reads (ids and liveness) of the controlled characters
    static const char* safe[] = {
        "get_bridge_state",
        // ck-ngoh1: who_holds_the_lock reads IPC-server static state (mutex-
        // protected fd + atomic-equivalent pid/uid/connect_ts) and probes
        // peer liveness via kill(pid, 0). Never touches game objects, must
        // serve in any state (DORMANT, paused, busy) so dispatchers can
        // diagnose "why is the bridge refusing my connection".
        "who_holds_the_lock",
        "get_offset_validation",
        "get_tick_info",
        // ck-6kvq.13 (R6): the decision-budget governor. Pure bridge state
        // (a mutex-protected ledger), never a game object — safe from any
        // thread, in any pause state, which is what ANY polarity means.
        "report_usage",
        "set_budget_config",
        "get_budget_state",
        // ck-6kvq.12 (R4): the shared board. Pure bridge state — a
        // mutex-protected vector, never a game object — so it is safe from any
        // thread in any pause state, which is what ANY polarity means. Note
        // that `board_post` still classifies as TICK_WRITE in
        // classify_polarity(), because that function asks "is this a mutation?"
        // BEFORE it asks "is it bridge-internal?": a post is buffered and
        // executed in a flush like any other write (§3.2), and carries the same
        // flush_id as the actions around it. What being listed here buys is the
        // routing: it is served INLINE, never handed to the game-thread drain.
        "board_post",
        "board_read",
        "board_admin",
        "query_intents",
        "query_intent_results",
        "cancel_intent",
        "clear_intents",
        "pause_game",
        "unpause_game",
        "set_pause",
        "set_speed",
        "set_controlled_character",
        "release_character",
        // Intent queue push — just appends to in-memory g_intent_queue, no game objects
        "queue_intent",
        // Atomic flag flips — safe from any thread
        "set_character_ai",
        "toggle_ai_subsystem",
        "toggle_all_ai",
        // Roster/player queries — read atomics and controlled_chars vector (thread-safe)
        "get_player_character",
        "get_controlled_roster",
        // Intent-queuing actions: enqueue to g_intent_queue and poll the drain's verdict
        // (what they read on this thread is in their ipc-exception lines above).
        "send_interaction",
        "declare_war",
        "send_decision",
        // faction create/join — enqueues IntentType::FACTION + polls (like declare_war);
        // the engine call runs in the game-thread drain (DRAIN_INTENTS), not inline.
        "faction_action",
        // #132 start-a-scheme (murder/fabricate_hook/…) — enqueues IntentType::SCHEME
        // + polls; CreateActiveScheme runs in the game-thread drain
        // (DRAIN_INTENTS), not inline.
        "scheme_action",
        // N7 (surface audit 2026-08-01): "fire_event" REMOVED from this list.
        // It was classified here since the original bridge import (8a26c96)
        // but a dispatch case / do_fire_event handler never existed in any
        // revision — a dead entry that made the classifier lie about the
        // surface. Callers now get the reader's inline unknown_action
        // fast-fail (is_registered_action). Re-add here ONLY together with a
        // real dispatch case in action_registry.inc.
        // F16: the send pipeline's own checks for an interaction (imprison_targets):
        // the handler enqueues a "check" INTERACTION intent and polls; the checks
        // run on the game tick (ai_eval_hook.cpp execute_interaction_check_intent),
        // nothing is sent.
        "query_interaction_check",
        // CB catalog reads static DB singleton (immutable after startup) + caches result
        "query_cb_catalog",
        // Scheme initiation reads from hook-populated ring buffer with read lock — no game objects
        "query_scheme_inits",
        // ck-sd4d: ended-war ledger. Same class as query_scheme_inits — a
        // mutex-protected bridge-local vector filled by the WAR_END_CORE hook
        // on the game thread. No game objects are touched on the read side, so
        // it must serve in EITHER pause state: the meter lane reads it from
        // inside an owned pause window, and an operator reads it unpaused.
        "query_war_history",
        // Path B' (ck-8weg) override armer — writes only to a mutex-protected
        // bridge-local slot, never touches game objects. The hook on the game
        // thread consumes the slot, gated by g_in_bridge_war_intent.
        "set_cb_selector_override",
        // ck-li56t / ck-dq7ym.4: bridge-internal auto-observe for the
        // multiplayer-lobby character-select screen. Touches libX11/libXtst
        // via dlopen, /dev/uinput as a fallback, and an atomic read of
        // session_state — no game-memory access, so safe to dispatch inline
        // even while DORMANT (the state we need to escape from).
        "select_observe_default",
        // ck-dq7ym.8: reads the captured LobbyView singleton (atomic load)
        // and invokes game_unpause. Same posture as select_observe_default —
        // no game-memory traversal, safe to dispatch inline from DORMANT.
        "enter_observe_mode",
        // ── N1 batch (surface audit 2026-08-01): formerly UNCLASSIFIED names ──
        // These fell through to g_msg_queue and silently hung every paused
        // caller (no drain while paused, no rejection). Each is IPC-thread-safe
        // by the same argument as an already-classified sibling:
        //
        // Pure intent-ring read (mutex-protected completed ring) — siblings
        // query_intents / query_intent_results above.
        "query_dispatch_result",
        // Hook-populated ring-buffer read under its own read lock — sibling
        // query_scheme_inits above.
        "query_marriage_observations",
        // Pure atomic flag flips (default-OFF RE levers) — sibling
        // set_cb_selector_override above. All verified single
        // std::atomic<bool>::store handlers; no game-object access.
        "set_offgoal_veto",
        // F16: the liege's standing answer to a faction demand — a map under its
        // own mutex, read by the AI_PICK_EVENT_OPTION hook; no game-object access.
        "set_faction_demand_response",
        "set_war_stance_record_walk",
        // XTest/uinput input injection under its own mutex, no game-memory
        // access — sibling select_observe_default above.
        "inject_mouse_click",
        // ck-9fc3: records the declared save identity and decides the
        // personality-lock restore. Touches two std::strings, the lock tables
        // (own shared_mutex) and ck-hxh7's last fingerprint — no game-object
        // traversal, so it is safe from the IPC thread in any pause state.
        // A resume declares before the clock runs.
        "declare_save_identity",
        // Recorded-data monitor reads (pure vector/atomic/safe_read, own
        // mutexes — no game-object traversal), so bridge state. (audit B2,
        // 20260612; L13 D1 moved them here from is_game_state_query, where
        // they had been put only to be served while paused.)
        "query_war_declarations",
    };
    for (const char* s : safe) {
        if (strcmp(action_type, s) == 0) return true;
    }
    return false;
}

bool is_game_state_query(const char* action_type) {
    // These queries traverse the game object graph (CCharacter, realm data,
    // title objects, war objects, etc.). L13 D1: they run on the GAME thread —
    // the IPC thread hands each to the session-pass drain and waits
    // (ipc_server.cpp try_inline_safe_query, Tier 2) — in either clock state.
    // Read from the IPC thread they would race the engine's commands and the
    // daily tick's parallel AI workers. The per-entry notes below record why
    // each member was classified; "inline when paused" in them is the routing
    // L13 replaced with this one.
    static const char* queries[] = {
        "query_character",
        "query_realm",
        "query_wars",
        "query_war_db_scan",
        // Engine-singleton war-stance read (do_query_war_stance, query_handlers.cpp).
        // Pure read: walks the 7 SAIWarStance singleton slots + (when armed) a bounded
        // war-record vector, all via safe_read/is_mapped, NO engine probe (doc-comment
        // "plain read"). Was unclassified → fell through to g_msg_queue →
        // never drained while paused → MCP timeout → _WAR_STANCE_UNSERVED caches the
        // gateway permanently into synthesis-only mode (the Layer-1 engine read was
        // NEVER served). Classified here (same fix as query_dejure_progress).
        "query_war_stance",
        "query_council",
        "query_factions",
        "query_faction_threat",
        // Per-vassal loyalty snapshot: traverses the realm vassal array + faction
        // member arrays AND calls GET_OPINION_OF per vassal (engine fn). Same
        // class as query_realm / query_opinion — must run paused / game-thread.
        "query_vassal_loyalty",
        "query_succession",
        "query_claims",
        "query_schemes",
        "query_incoming_schemes",
        // l3-schemes: the scheme type database (static script data, pure reads).
        "query_scheme_types",
        "query_armies",
        "query_lifestyle",
        "query_opinion",
        "query_county_details",
        "query_de_jure",
        // de-jure-control meter — pure safe_read (no engine call, no game-thread
        // sub-query). Was unclassified → queued to g_msg_queue → HUNG when paused
        // (P0 in the LiveVerify health matrix, query_dejure_progress). Classifying
        // it here makes it a game-thread read like every other graph walk.
        "query_dejure_progress",
        "query_title_hierarchy",
        "query_relations",
        "query_available_cbs",
        "list_interactions",
        "list_decisions",
        "get_ai_evaluations",
        // v0.4.0 new query endpoints
        "query_title_claimants",
        // playbook tools (history/legibility/plotting) — pure game-graph reads.
        "query_title_history",
        "query_title_succession",
        "query_hireable_mercenaries",
        "query_maa_types",            // lane-query: MAA_TYPE_DB walk, pure reads
        "query_valid_war_targets",
        "query_cb_options",
        "query_imprisonment_reasons",
        "search_characters",
        "query_marriage_candidates",  // ck-leg: read-only DB scan
        "memory_probe",
        "query_character_memories",   // read-only: enumerate a character's memories
        "resolve_global",
        // Event query endpoints (read event queue from game memory)
        // Audit ck-39gy: additional handlers with unsafe game-state reads
        "query_province_adjacency",
        // Baked-topology windowed county graph + live overlay (map_graph.cpp). Pure
        // reads: mmap'd artifact (bounded to header counts) + safe_read game-object
        // overlay. Same class as the other graph-walkers. Without this it was
        // unclassified and queued (and timed out when paused). (cycle-3 crash-fix)
        "query_map_graph",
        "query_development_growth",
        "query_hooks",
        "query_alliances",
        "query_trait_db",
        "query_partition_preview",
        // Per-title succession preview (partition map). Pure safe_read graph walk
        // (titles + cached per-title heir arrays at title+0x290) — no engine call.
        // Same class as query_partition_preview/query_succession_simulation.
        // Without this it fell through to the queue in BOTH states → timed out
        // during the agent's paused read-phase.
        "query_title_succession_map",
        "query_levy_readiness",
        "query_trait_compatibility",
        "query_election_state",
        "query_building_types",
        "query_diplomatic_range",
        "query_dynasty",
        // ck-cjev: additional game-state query handlers missing from classification
        "query_opinion_breakdown",
        "query_county_neighbors",
        "query_realm_laws",
        "query_law_groups",
        "query_available_law_changes",
        "query_innovation_progress",
        "query_fascination_progress",
        "query_research_speed",
        "query_succession_simulation",
        // Meta-query dispatcher — routes to game-state query handlers
        "query",
        // Phase-3 Wave-2 probes (ck-e58l.8) — game-state reads / deferred queries.
        "list_pending_interactions",
        "tick_diff",
        // ── N1 batch (surface audit 2026-08-01): formerly UNCLASSIFIED reads ──
        // All three are pure game-graph traversals that were in NO classifier,
        // so a paused call (the agent's default observe state) sat in
        // g_msg_queue and hung silently. Same fix as query_dejure_progress /
        // query_map_graph above.
        //
        // Walks the ruler's war/ally call arrays + war objects — same class as
        // query_alliances / query_relations. (#122 alliance-hygiene read.)
        "query_call_to_arms",
        // char -> education focus + guardian chain reads — same class as
        // search_characters / query_succession.
        "query_heir_education",
        // Walks a live interaction object's trigger-slot tree (interaction DB
        // + per-slot children) — same class as list_interactions /
        // query_cb_options.
        "query_trigger_slots",
    };
    // L13 D1 (witness W2/W6): game-object readers that were served from the IPC
    // thread. The deferred-slot submitters service their own slot on the game
    // thread (service_slots_if_game_thread); get_active_wars and
    // get_ai_personality walk wars / characters; pre_validate_war reads the
    // pair and runs the CB check; query_council_tasks resolves a character and
    // walks its council; query_debug_flags reads the engine's debug globals.
    static const char* l13_moved[] = {
        "get_ai_personality", "get_active_wars", "pre_validate_war",
        "query_council_tasks", "query_debug_flags",
        // FF2 (witness R4): resolves the war in WAR_DB and reads the live war's
        // scores (resolve_war_by_id, IS_ENDED, compute_*_score).
        "query_war_score_breakdown",
        "query_interaction_acceptance", "query_interaction", "query_trigger_result",
        "list_valid_decisions", "list_decisions_annotated", "query_joinable_factions",
        "probe_interaction_cascade", "probe_gate_4_slots", "query_valid_cbs",
        "query_cb_validity_strict", "query_cbs_for_targets", "query_cbs_for_target",
    };
    for (const char* q : l13_moved) {
        if (strcmp(action_type, q) == 0) return true;
    }
    // L13 D1: query_conquest_targets walks the graph AND takes the engine's
    // strict CB verdict per county. On the game thread the strict evaluation
    // runs DIRECTLY (cb_query_strict_full_sync services its slot inline there),
    // so there is no slot submit+poll on the thread that would service it.
    if (strcmp(action_type, "query_conquest_targets") == 0) return true;
    for (const char* q : queries) {
        if (strcmp(action_type, q) == 0) return true;
    }
    return false;
}

bool is_game_thread_action(const char* action_type) {
    // Actions that call engine functions directly and must run on the game thread.
    // These are NOT safe for inline execution from the reader thread: the reader
    // queues them to g_msg_queue and the game-thread drain runs them on its next
    // pass, paused or running (L13 D1).
    //
    // L14: every name here is audited by section G of
    // bridge/scripts/inline_safety_static.py (THE RULE in its docstring: the
    // drain's sigsetjmp is a crash net, never the handler's answer). The
    // DECLARED EXCEPTIONS, one row of one handler each, ONE declaration that row
    // G reads (`game-thread-ok <name>: <row>: <reason>`); a line whose row no
    // longer reds is itself a FAIL. None stands since L16 (the one there was,
    // respond_event's, went with the handler).
    static const char* actions[] = {
        // Calls LAW_LOOKUP_BY_NAME + ADD_REALM_LAW engine functions
        "change_realm_law",
        // Calls SET_FOCUS_APPLY (state mutation) — must run on the game thread.
        // (ck-leg, 20260612)
        "set_lifestyle_focus",
        // Calls CHANGE_COUNCIL_TASK (builds + dispatches CChangeCouncilTaskCommand);
        // game-thread mutation. (20260612)
        "set_council_task",
        // A player's build order: CAddConstructionCommand's validity, then its
        // execute (start the construction, pay the price) and the construction
        // speed refresh, under the fake random scope. Game-thread mutation.
        "construct_building",
        // The game-thread READS (is_game_thread_read says why they are here).
        "construction_check",
        "query_fabricate_targets",
        "query_create_title_cost",
        // playbook write tools — call engine functions directly (CAPITAL_RELOCATE,
        // ASSIGN_COUNCILLOR effect, CREATE_MAA_REGIMENT) or write game memory; must run
        // on the game thread (queued, drained there).
        "move_capital",
        "set_obligation_level",
        "appoint_councillor",
        "maa_recruit",
        "maa_station",
        "maa_unstation",
        "hire_mercenary",
        "set_doctrine",
        // Calls SET_FAITH (SetFaithOnCharacter) — converts the actor's own faith;
        // fires on_faith_conversion + recalc, a game-thread mutation.
        // (convert-own-faith lever, 20260624)
        "set_character_faith",
        // Calls SET_CULTURE (SetCultureOnCharacter) — converts the actor's own
        // culture; fires culture/heritage/innovation recalc, a game-thread
        // mutation. THE convert-own-
        // culture lever — unblocks e_britannia (culture-gated can_create).
        // (ck-#130, 20260624)
        "set_character_culture",
        "set_title_law",
        // ── N1 batch (surface audit 2026-08-01): formerly UNCLASSIFIED writes ──
        // Each of these queued to g_msg_queue unclassified; classified here as
        // game-thread mutations.
        //
        // Calls SET_EDUCATION_FOCUS engine mutation — the direct sibling of
        // set_lifestyle_focus above; its omission here was a plain oversight.
        "set_education_focus",
        // War/title mutation levers (WAR_CREATE_CORE, CCreateTitleCommand
        // dispatch). All engine mutations that run in the game-thread drain.
        // create_title is the NSE crown-creation lever.
        "call_war_create_core",
        "create_title",
        // L13 D1 (witness W6): the personality-byte store into engine memory
        // (personality.cpp) runs in the game-thread drain. Its reply reaches the
        // requester by request_id from the drain, as every queued action's does.
        "set_ai_personality",
    };
    for (const char* a : actions) {
        if (strcmp(action_type, a) == 0) return true;
    }
    return false;
}

bool is_game_thread_read(const char* action_type) {
    // Reads that run on the game thread, and so are listed in is_game_thread_action
    // too (that list is the routing: queued to g_msg_queue, run by the game-thread
    // drain). They change nothing, so the pipeline's classify_polarity calls them
    // tick_read and the drain stores no write verdict for them
    // (drain_records_write_verdict): a read in the completed ring would list as a
    // finished write in inspect(pending) and take a slot from the writes looked up
    // there by poll_token (lane l6, witness #3).
    static const char* reads[] = {
        // The construction validity and price (CAddConstructionCommand's), under
        // the fake random scope, which needs the engine's RandomScopeManager slot
        // stable: on the IPC thread while paused another thread's engine scope was
        // popped under the bridge's arm and the restore tripped the scope latch
        // (lane l6, witness #2). One call checks a list of buildings (`buildings`).
        "construction_check",
        // The council task's target triggers, under the fake random scope. Served
        // inline while paused it tripped the scope latch in live867
        // l6fin4-latch-3 (the call right after a pause, the first after a
        // building enumeration).
        "query_fabricate_targets",
        // The create-title IsValid (its scripted triggers) and price, under the fake
        // random scope, as the mint runs them (L14 S3, surface-eval finding 14).
        "query_create_title_cost",
    };
    if (!action_type) return false;
    for (const char* a : reads) {
        if (strcmp(action_type, a) == 0) return is_game_thread_action(action_type);
    }
    return false;
}

bool drain_records_write_verdict(const char* action_type) {
    if (!action_type) return false;
    return is_game_thread_action(action_type) && !is_game_thread_read(action_type);
}

} // namespace bridge
