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
        "pause",
        "set_speed",
        "set_controlled_character",
        "release_character",
        // Intent queue push — just appends to in-memory g_intent_queue, no game objects
        "queue_intent",
        // Personality writes via safe_read/process_vm patterns + personality lock map
        "set_ai_personality",
        // Atomic flag flips — safe from any thread
        "set_character_subsystems",
        "set_character_ai",
        "toggle_ai_subsystem",
        "toggle_all_ai",
        "enable_all_ai",
        // Personality read — checks lock map first (thread-safe), falls back to safe_read
        "get_ai_personality",
        // Roster/player queries — read atomics and controlled_chars vector (thread-safe)
        "get_player_character",
        "get_controlled_roster",
        // Intent-queuing actions — just enqueue to g_intent_queue (thread-safe), no engine calls
        "send_interaction",
        "declare_war",
        "declare_war_debug",
        "send_decision",
        // faction create/join — enqueues IntentType::FACTION + polls (like declare_war);
        // the engine call runs on the game tick (DRAIN_INTENTS), not inline. Needs UNPAUSED.
        "faction_action",
        // #132 start-a-scheme (murder/fabricate_hook/…) — enqueues IntentType::SCHEME
        // + polls; CreateActiveScheme runs on the game tick (DRAIN_INTENTS), not
        // inline. Needs UNPAUSED like other tier-3 writes.
        "scheme_action",
        "respond_event",
        // N7 (surface audit 2026-08-01): "fire_event" REMOVED from this list.
        // It was classified here since the original bridge import (8a26c96)
        // but a dispatch case / do_fire_event handler never existed in any
        // revision — a dead entry that made the classifier lie about the
        // surface. Callers now get the reader's inline unknown_action
        // fast-fail (is_registered_action). Use respond_event for event
        // interaction; re-add here ONLY together with a real dispatch case
        // in action_registry.inc.
        "set_auto_respond_events",
        // Deferred acceptance query — queues request + polls result, no direct game reads
        "query_interaction_acceptance",
        "query_interaction",
        // Deferred trigger evaluation query — queues request + polls result
        "query_trigger_result",
        // ck-7sg7n: Deferred named-trigger evaluation — same pattern as
        // query_trigger_result / query_valid_cbs. IPC-thread handler validates
        // inputs and writes to g_named_trigger_query; actual evaluator call
        // (FUN_07628500) happens on the game thread in process_named_trigger_query
        // from drain_and_execute_intents. Without this classification, the
        // request falls through to g_msg_queue, do_evaluate_named_trigger runs
        // on the game thread in on_tick's queued-action drain, and its 3s
        // polling loop blocks the same on_tick from ever reaching
        // drain_and_execute_intents — the slot never advances past state=1 and
        // the call times out. Same classifier-vs-drain deadlock that ck-ino2
        // fixed for probe_interaction_cascade / probe_gate_4_slots.
        "evaluate_named_trigger",
        // #64: annotated decision eval — deferred game-thread query (the trigger
        // evaluator must run where the watchdog is bound; the IPC thread blocks
        // SIGALRM). IPC handler submits + polls; process_decision_eval_query runs it
        // from drain_and_execute_intents. Needs the game UNPAUSED to drain.
        "list_valid_decisions",
        "list_decisions_annotated",
        // ck-ino2: Phase-3 probes 1 & 2 — deferred queries (queue request +
        // poll result). IPC-thread handler does only JSON parse + writes to
        // g_cascade_query / g_trigger_query globals; the actual game-state
        // reads happen on the game thread in drain_and_execute_intents.
        // Same pattern as query_trigger_result above.
        "probe_interaction_cascade",
        "probe_gate_4_slots",
        // ck-v1pi: Deferred CB validity query — same pattern as
        // query_trigger_result / query_interaction_acceptance. IPC-thread
        // handler only reads atomics + writes to g_cb_query; actual
        // CB_VALIDITY_CORE calls happen on the game thread in
        // drain_and_execute_intents. Previously classified as
        // is_game_state_query, which forced pause-gate — but the drain site
        // only fires while the game is ticking (unpaused), so pause-gate
        // deadlocked the query (classifier vs drain). Same bug that ck-ino2
        // fixed for probe_interaction_cascade / probe_gate_4_slots.
        "query_valid_cbs",
        // Same deferred-query family as query_valid_cbs (2026-06-09 live run):
        // unclassified, it fell through to the game-thread intent drain, where
        // its sleep-poll loop blocked the very thread that services g_cb_query
        // — every deferred strict query returned cb_query_timeout while the
        // service line logged "done" microseconds after the handler gave up.
        // IPC-thread handler does bilateral pre-reads via safe_read +
        // submit/poll on g_cb_query only.
        "query_cb_validity_strict",
        // Batched CB enumeration (_slot_drain_profile.md §3b): same g_cb_query
        // submit/poll discipline as query_cb_validity_strict, just looped over N
        // targets inside ONE coordinator drain window. IPC-thread-safe (only the
        // slot + safe_read probes), so it takes the coordinator inline in EITHER
        // pause state — the whole point is to share one pulse across the sweep.
        "query_cbs_for_targets",
        // Watchlist reads mutex-protected roster + atomic primary — no game objects
        "list_watchlist",
        // War score breakdown reads from hook-populated cache with read lock — no game objects
        "query_war_score_breakdown",
        // L6: unpaused-safe minimal active-wars brief. Uses safe_read
        // (process_vm_readv) throughout — no raw game-pointer deref that can
        // fault, best-effort torn reads are acceptable for an unpaused brief.
        // Must serve inline even while UNPAUSED so the agent sees it's at war
        // every turn in a human-vs-agent game (the paused-gated Tier-2 war
        // block never fires when the human keeps the game running).
        "get_active_wars",
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
        // ck-nocy: console passthrough — IPC-thread handler only enqueues a
        // CONSOLE_EXECUTE intent and polls g_intent_queue.completed_snapshot
        // (atomics + thread-safe queue + ResponseBuilder, no game-state reads).
        // Drain runs on the game thread in drain_and_execute_intents at the
        // tail of on_tick. Without this classification, the request is queued
        // for game-thread drain and the IPC thread blocks the game thread for
        // 5s polling for a result the game thread can't produce — same
        // classifier-vs-drain deadlock that ck-ino2 fixed for
        // probe_interaction_cascade / probe_gate_4_slots.
        "execute_console_command",
        // ck-7fyl: scripted-effect scaffold — IPC-thread handler validates
        // inputs and returns synchronously. No game-state reads (lookup_character
        // hits the immutable singleton DB), so safe under parallel AI eval.
        "execute_scripted_effect",
        // ck-bt9x: engine-effect (start_war) scaffold — same posture as
        // execute_scripted_effect: IPC-thread handler validates inputs and
        // returns synchronously, no game-state reads.
        "execute_engine_effect",
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
        "set_break_alliance_enabled",
        "set_vacant_fill_enabled",
        "set_offgoal_veto",
        "set_force_offensive_stance",
        "set_war_stance_record_walk",
        // safe_read pre-checks + bounded cb_query_*_sync submit/poll on
        // g_cb_query — the exact posture of query_cb_validity_strict above
        // (and of do_declare_war's own inline pre-validation, which already
        // runs these checks on the IPC thread). Queued, its slot poll
        // competed with the game thread that services g_cb_query.
        "pre_validate_war",
        // XTest/uinput input injection under its own mutex, no game-memory
        // access — sibling select_observe_default above.
        "inject_mouse_click",
        // ck-9fc3: records the declared save identity and decides the
        // personality-lock restore. Touches two std::strings, the lock tables
        // (own shared_mutex) and ck-hxh7's last fingerprint — no game-object
        // traversal, so it is safe from the IPC thread in any pause state.
        // It MUST serve while paused: a resume declares before unpausing.
        "declare_save_identity",
    };
    for (const char* s : safe) {
        if (strcmp(action_type, s) == 0) return true;
    }
    return false;
}

bool is_game_state_query(const char* action_type) {
    // These queries traverse the game object graph (CCharacter, realm data,
    // title objects, war objects, etc.). During parallel AI execution, reading
    // these objects from the IPC thread is a data race. Must use snapshot or
    // pause-gate.
    static const char* queries[] = {
        "query_character",
        "query_realm",
        "query_wars",
        "query_war_db_scan",
        // Engine-singleton war-stance read (do_query_war_stance, query_handlers.cpp).
        // Pure read: walks the 7 SAIWarStance singleton slots + (when armed) a bounded
        // war-record vector, all via safe_read/is_mapped, NO engine probe (doc-comment
        // "plain read ⇒ PAUSED-safe"). Was unclassified → fell through to g_msg_queue →
        // never drained while paused → MCP timeout → _WAR_STANCE_UNSERVED caches the
        // gateway permanently into synthesis-only mode (the Layer-1 engine read was
        // NEVER served). Classifying it here (same fix as query_dejure_progress)
        // serves it inline when paused (the agent's observe state) and rejects when
        // unpaused (matches the game-graph data-race contract).
        "query_war_stance",
        // Recorded-data monitor reads (pure vector/atomic/safe_read, own
        // mutexes — no game-thread traversal). Registered inline so they
        // resolve while PAUSED (the agent's default observe state) instead of
        // queuing as Tier-3 actions and timing out. (audit B2, 20260612)
        "query_war_declarations",
        "query_council_tasks",
        "query_debug_flags",
        "query_council",
        // Read-only fabricate-eligibility discovery: builds a temp eval scope per
        // candidate county + runs the task's validity triggers; no mutation. Agent
        // calls it while PAUSED (observe state) — safe inline, like construction_check.
        "query_fabricate_targets",
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
        "query_armies",
        "query_lifestyle",
        "query_opinion",
        "query_borders",
        "query_county_details",
        "query_de_jure",
        // de-jure-control meter — pure safe_read (no engine call, no game-thread
        // sub-query). Was unclassified → queued to g_msg_queue → HUNG when paused
        // (P0 in the LiveVerify health matrix, query_dejure_progress). Classifying
        // it Tier-2 serves it inline when paused (correct) and REJECTs when
        // unpaused (matches every other game-graph read's data-race contract).
        "query_dejure_progress",
        "query_title_hierarchy",
        "query_relations",
        "query_available_cbs",
        "list_interactions",
        "list_decisions",
        // NOTE: list_valid_decisions / list_decisions_annotated are NOT here —
        // they are DEFERRED game-thread queries (the annotated trigger-eval must run
        // on the game thread where the watchdog fires), classified in
        // is_bridge_internal_query like evaluate_named_trigger. They need the game
        // UNPAUSED to drain.
        "get_ai_evaluations",
        "get_character_subsystems",
        // v0.4.0 new query endpoints
        "query_title_claimants",
        // playbook tools (history/legibility/plotting) — pure game-graph reads,
        // served inline when paused like the other queries.
        "query_title_history",
        "query_title_succession",
        "query_joinable_factions",
        "query_hireable_mercenaries",
        "query_valid_war_targets",
        "query_cb_options",
        "query_imprisonment_reasons",
        "search_characters",
        "query_marriage_candidates",  // ck-leg: read-only DB scan, safe inline when paused
        "construction_check",         // read-only can-build check (no mutation), inline when paused
        "memory_probe",
        "query_character_memories",   // read-only: enumerate a character's memories (paused)
        "resolve_global",
        // Event query endpoints (read event queue from game memory)
        "pending_events",
        "notifications",
        "query_pending_events",
        // Audit ck-39gy: additional handlers with unsafe game-state reads
        "query_province_adjacency",
        // Baked-topology windowed county graph + live overlay (map_graph.cpp). Pure
        // reads: mmap'd artifact (bounded to header counts) + safe_read game-object
        // overlay. Served inline when PAUSED (game thread parked → race-free); falls
        // through to the game-thread g_msg_queue drain when unpaused, same class as the
        // other graph-walkers. Without this it was unclassified → never served inline →
        // only ran on the game thread (and timed out when paused). (cycle-3 crash-fix)
        "query_map_graph",
        "query_development_growth",
        "query_hooks",
        "query_alliances",
        "query_trait_db",
        "query_partition_preview",
        // Per-title succession preview (partition map). Pure safe_read graph walk
        // (titles + cached per-title heir arrays at title+0x290) — no engine call.
        // Same class as query_partition_preview/query_succession_simulation: served
        // inline while PAUSED (game thread parked → race-free), falls through to the
        // game-thread queue while unpaused. Without this it fell through to the queue
        // in BOTH states → timed out during the agent's paused read-phase.
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
        // NOTE ck-v1pi: query_valid_cbs moved to is_bridge_internal_query —
        // it is a deferred query (IPC-thread handler only touches g_cb_query
        // state, drain runs on game thread in drain_and_execute_intents).
        // Classifying it here forced pause-gate, which deadlocks the drain
        // (on_tick only fires while unpaused). Same fix as ck-ino2.
        "query_realm_laws",
        "query_law_groups",
        "query_available_law_changes",
        "query_innovation_progress",
        "query_fascination_progress",
        "query_research_speed",
        "query_event_probabilities",
        "event_probabilities",
        "query_succession_simulation",
        // Meta-query dispatcher — routes to game-state query handlers
        "query",
        // Phase-3 Wave-2 probes (ck-e58l.8) — game-state reads / deferred queries.
        // NOTE: probe_interaction_cascade and probe_gate_4_slots moved to
        // is_bridge_internal_query per ck-ino2 — they are deferred-query probes
        // (no IPC-thread game reads) and classifying them here caused a
        // classifier vs game-thread-drain deadlock.
        "list_pending_interactions",
        "tick_diff",
        "probe_war_orphans",
        // ── N1 batch (surface audit 2026-08-01): formerly UNCLASSIFIED reads ──
        // All three are pure game-graph traversals that were in NO classifier,
        // so a paused call (the agent's default observe state) sat in
        // g_msg_queue and hung silently. Same fix as query_dejure_progress /
        // query_map_graph above: serve inline while PAUSED (game thread parked
        // → race-free), typed game_unpaused reject while unpaused.
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
    for (const char* q : queries) {
        if (strcmp(action_type, q) == 0) return true;
    }
    return false;
}

bool is_game_thread_action(const char* action_type) {
    // Actions that call engine functions directly and must run on the game thread.
    // These are NOT safe for inline execution from the reader thread.
    //
    // CONSUMER (ck-09q4u.9): ipc_server.cpp::try_game_thread_paused_rejection
    // uses this classifier to short-circuit Tier-3 actions when the game is
    // paused — on_tick won't drain g_msg_queue while paused, so without this
    // gate the agent times out instead of getting a clear "game_paused" reason.
    // New entries here automatically inherit that precondition behavior.
    static const char* actions[] = {
        // Calls LAW_LOOKUP_BY_NAME + ADD_REALM_LAW engine functions
        "change_realm_law",
        // Calls SET_FOCUS_APPLY (state mutation) — must run on the game thread;
        // call while UNPAUSED so on_tick drains it. (ck-leg, 20260612)
        "set_lifestyle_focus",
        // Calls CHANGE_COUNCIL_TASK (builds + dispatches CChangeCouncilTaskCommand);
        // game-thread mutation, call while UNPAUSED so on_tick drains it. (20260612)
        "set_council_task",
        // Builds + dispatches CAddConstructionCommand (mode 0xe); game-thread
        // mutation, call while UNPAUSED so on_tick drains it. (20260612)
        "add_construction",
        // Non-gated CAddBuildingEffect builder (CONSTRUCTION_START); game-thread
        // mutation. (20260612)
        "construct_building",
        // playbook write tools — call engine functions directly (CAPITAL_RELOCATE,
        // ASSIGN_COUNCILLOR effect, CREATE_MAA_REGIMENT) or write game memory; must run
        // on the game thread (queued, drained on_tick), call while UNPAUSED.
        "move_capital",
        "set_obligation_level",
        "appoint_councillor",
        "maa_recruit",
        "maa_station",
        "maa_unstation",
        "hire_mercenary",
        "set_doctrine",
        // Calls SET_FAITH (SetFaithOnCharacter) — converts the actor's own faith;
        // fires on_faith_conversion + recalc, a game-thread mutation. Call while
        // UNPAUSED so on_tick drains it. (convert-own-faith lever, 20260624)
        "set_character_faith",
        // Calls SET_CULTURE (SetCultureOnCharacter) — converts the actor's own
        // culture; fires culture/heritage/innovation recalc, a game-thread
        // mutation. Call while UNPAUSED so on_tick drains it. THE convert-own-
        // culture lever — unblocks e_britannia (culture-gated can_create).
        // (ck-#130, 20260624)
        "set_character_culture",
        "set_title_law",
        // ── N1 batch (surface audit 2026-08-01): formerly UNCLASSIFIED writes ──
        // Each of these queued to g_msg_queue with NO paused rejection, so a
        // paused caller hung silently for 20s+ — and PAUSED is the agent's
        // default observe state. Classifying them here keeps their unpaused
        // queue-and-drain routing byte-identical and adds the typed
        // `game_paused` fast-reject (ck-09q4u.9) while paused.
        //
        // Calls SET_EDUCATION_FOCUS engine mutation — the direct sibling of
        // set_lifestyle_focus above; its omission here was a plain oversight.
        "set_education_focus",
        // War/title/army mutation levers (WAR_CREATE_CORE, CCreateTitleCommand
        // dispatch, guarded g_PlayerCharacterID-swap raises, WAR_FORCE_
        // OFFENSIVE_STANCE intent). All engine mutations that drain on_tick;
        // call while UNPAUSED. create_title is the NSE crown-creation lever —
        // a silent paused hang here stalls the live agent's war path.
        "call_war_create_core",
        "do_call_war_create_core",   // suite alias, same handler
        "create_title",
        "raise_armies",
        "raise_raiders",
        "raid",                      // alias of raise_raiders
        "force_offensive_stance",
        // Routes parsed `start_war = { ... }` blocks into the same
        // call_war_create_core machinery — same posture.
        "execute_script_block",
        // Gated CBreakAllowAllianceEffect write (IntentType::BREAK_ALLIANCE,
        // default-OFF + observer-save-gated). The armed path is a game-thread
        // relation mutation; the gated refusal is harmless to reject paused.
        "break_alliance",
    };
    for (const char* a : actions) {
        if (strcmp(action_type, a) == 0) return true;
    }
    return false;
}

} // namespace bridge
