// snapshot.h — Thread-safe game state snapshot system.
//
// PROBLEM: The IPC reader thread serves queries by reading game memory directly.
// When the game is unpaused, CAIManager::MainTick spawns parallel worker threads
// that mutate game objects (character stats, realm data, war state, etc.).
// Reading those objects from the IPC thread during parallel execution is a data
// race that causes crashes (torn reads, dangling pointers, inconsistent state).
//
// SOLUTION: Capture a snapshot of all query-relevant game state during the
// SERIAL section of on_tick(), BEFORE calling orig(ai_manager) which launches
// parallel workers. The IPC thread then serves queries from the snapshot,
// never touching live game memory for object graph traversal.
//
// TIMELINE:
//   on_tick(ai_manager) {
//     capture_snapshot();           // Serial: single-threaded, safe to read all state
//     drain_and_execute_intents();  // Serial: execute queued actions
//     orig(ai_manager);            // Parallel: workers mutate game objects (UNSAFE to read)
//   }
//
// QUERY CLASSIFICATION:
//
// TIER 1 — Always safe (no snapshot needed):
//   Reads from atomics, bridge-internal state, or immutable globals.
//   These can be served from any thread at any time.
//   - get_bridge_state     (reads bridge atomics: session_state, tick_id, etc.)
//   - get_tick_info         (reads g_tick_id atomic + date from global)
//   - query_intents         (reads bridge-internal IntentQueue, mutex-protected)
//   - pause/unpause/speed   (writes to atomic game state byte, no object traversal)
//   - set_controlled_character / release_character (bridge-internal bookkeeping)
//
// TIER 2 — Snapshot-required (object graph traversal):
//   These chase pointer chains through CCharacter, realm data, title objects,
//   war objects, etc. Unsafe during parallel AI execution.
//   - query_character       (char -> realm_data -> title_realm -> family -> traits)
//   - query_realm           (char -> title_realm -> vassal list -> each vassal char)
//   - query_wars            (war DB -> each war -> participants -> char lookups)
//   - query_council         (char -> core_data -> council instance -> position array)
//   - query_factions        (char -> faction array -> each faction -> members)
//   - query_succession      (char -> core_data -> heir list)
//   - query_schemes         (char -> scheme array)
//   - query_armies          (char -> army array)
//   - query_claims          (char -> core_data -> claims array -> title DB lookup)
//   - query_relations       (char -> relation array -> char lookups)
//   - query_opinion         (CALLS game function get_opinion — must be game thread)
//   - query_lifestyle       (char -> lifestyle data)
//   - query_borders         (county DB iteration)
//   - query_county_details  (county/barony DB traversal)
//   - query_de_jure         (title DB traversal)
//   - query_title_hierarchy (title DB traversal)
//   - query_available_cbs   (iterates CB definitions)
//   - query_interaction_acceptance  (may call game functions)
//   - list_interactions     (iterates interaction DB)
//   - list_decisions        (iterates decision DB)
//   - get_ai_personality    (CALLS game getter functions)
//   - get_ai_evaluations    (reads AI controller state)
//   - get_character_subsystems (reads AI controller subsystem masks)
//
// TIER 3 — Must execute on game thread (mutating actions):
//   These call game engine functions that modify state. They MUST run on the
//   game thread during the serial section (drain_and_execute_intents).
//   Already correctly queued via the message queue -> on_tick drain loop.
//   - send_interaction, declare_war, send_decision
//   - set_ai_personality, set_character_subsystems
//   - toggle_ai_subsystem, set_character_ai
//   - queue_intent (bridge-internal, but execution is game-thread)
//   - respond_event
//   (fire_event was listed here + in is_bridge_internal_query from the initial
//    import but NEVER had a dispatch case in any revision — dead classifier
//    entry removed per N7, surface audit 2026-08-01.)
//
// SNAPSHOT STRATEGY:
//
// Pre-formatted JSON strings, NOT raw bytes.
//
// Rationale:
//   1. Queries already produce JSON. Storing pre-formatted JSON means the IPC
//      thread just does a string copy to respond — zero parsing, zero traversal.
//   2. Raw byte snapshots would require re-implementing all pointer validation
//      and offset calculations in a second code path. The observation/query code
//      already handles all the edge cases (NULL pointers, sentinel values, dead
//      characters, etc.). Running it once on the game thread captures all that
//      logic correctly.
//   3. The snapshot is ~50-200KB of JSON per controlled character — trivial
//      memory cost.
//   4. Snapshot capture adds ~1-2ms to the serial section (acceptable: the game
//      tick is 30-50ms, and we're only in the serial section for ~5ms already).
//
// SNAPSHOT SCOPE:
//
// The snapshot captures state for the controlled character(s) and their
// immediate neighborhood (vassals, liege, war participants, council, etc.).
// Ad-hoc queries for arbitrary character IDs will still need live reads,
// but those are rare and can use the pause-before-query fallback.
//
// IMPLEMENTATION PHASES:
//
// Phase 1 (this file): Architecture and data structures.
//   - SnapshotStore: thread-safe container for pre-built JSON responses.
//   - capture_snapshot(): called from on_tick serial section.
//   - try_serve_from_snapshot(): called from IPC inline query path.
//
// Phase 2: Integrate with ipc_server.cpp inline query path.
//   - Modify try_inline_safe_query() to check snapshot first.
//   - If snapshot has a valid response, return it immediately.
//   - If not (cache miss), fall through to live read (safe only when paused).
//
// Phase 3: Pause-gate for unpaused cache misses.
//   - If game is unpaused and snapshot misses, pause briefly, serve, unpause.
//   - Or reject with {"error":"snapshot_miss","hint":"pause game first"}.
//
// ALTERNATIVE CONSIDERED: Pause-before-query
//
// The simplest possible fix: when a query arrives and the game is unpaused,
// pause the game, serve the query from live memory (now safe), then unpause.
// Pros: No new data structures, no snapshot staleness, exact results.
// Cons: Adds ~50-100ms latency per query (pause propagation), visible game
//       stuttering if queries are frequent (10+ per second during agent turns).
// Verdict: Good enough for Phase 0 / fallback, but snapshot is better for
//          production throughput.

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace bridge {

// =============================================================================
// Snapshot Store
// =============================================================================

// Thread-safe store for pre-captured query responses.
// Written by game thread (on_tick serial section), read by IPC thread.
//
// The key is a composite of (query_type, character_id) — e.g., "query_character:42".
// The value is the complete JSON response string, ready to send over IPC.
//
// Uses a double-buffer pattern: the game thread writes to the "back" buffer,
// then atomically swaps it to become the "front" buffer. The IPC thread only
// ever reads from the front buffer. No lock contention between reader and writer.
class SnapshotStore {
public:
    // Called from game thread (serial section of on_tick).
    // Builds responses for all anticipated queries and stores them.
    void begin_capture() {
        // Write to whichever buffer is NOT currently being read
        int back = 1 - front_.load(std::memory_order_acquire);
        buffers_[back].clear();
        write_idx_ = back;
    }

    // Store a pre-built response for a query key.
    // Called during capture (game thread only).
    void store(const std::string& key, std::string value) {
        buffers_[write_idx_][key] = std::move(value);
    }

    // Finish capture and make the new snapshot visible to readers.
    // Called from game thread after all stores are complete.
    void end_capture() {
        capture_tick_ = tick_id_;
        front_.store(write_idx_, std::memory_order_release);
    }

    // Set the tick ID for the current capture.
    void set_tick(uint64_t tick) { tick_id_ = tick; }

    // Try to serve a query from the snapshot.
    // Returns true if found (and fills response_out). Thread-safe for readers.
    bool try_serve(const std::string& key, std::string* response_out) const {
        int idx = front_.load(std::memory_order_acquire);
        auto it = buffers_[idx].find(key);
        if (it == buffers_[idx].end()) return false;
        if (response_out) *response_out = it->second;
        return true;
    }

    // Get the tick ID of the current snapshot.
    uint64_t snapshot_tick() const { return capture_tick_; }

    // Check if a snapshot is available (at least one capture completed).
    bool has_snapshot() const { return capture_tick_ > 0; }

private:
    // Double buffer: one is "front" (readable), one is "back" (writable).
    // std::unordered_map is used for simplicity; could switch to a flat
    // array with enum keys for better cache performance if needed.
    mutable std::unordered_map<std::string, std::string> buffers_[2];
    std::atomic<int> front_{0};
    int write_idx_ = 1;
    uint64_t tick_id_ = 0;
    uint64_t capture_tick_ = 0;
};

// Global snapshot store, defined in bridge_init.cpp or snapshot.cpp
extern SnapshotStore g_snapshot;

// =============================================================================
// Snapshot Capture API
// =============================================================================

// Capture a complete state snapshot for all controlled characters.
// Called from on_tick() in the serial section, BEFORE orig(ai_manager).
//
// Captures:
//   - query_character for each controlled character
//   - query_realm for each controlled character
//   - query_wars for each controlled character
//   - query_council for each controlled character
//   - query_factions for each controlled character
//   - query_succession for each controlled character
//   - query_claims for each controlled character
//   - query_schemes for each controlled character
//   - query_lifestyle for each controlled character
//   - get_ai_personality for each controlled character
//   - list_interactions (global, not per-character)
//   - list_decisions (global, not per-character)
//
// NOT captured (too expensive or not needed every tick):
//   - query_borders (full map scan, expensive, rarely queried)
//   - query_county_details (per-county, query on demand)
//   - query_armies (changes rapidly, query on demand)
//   - query_opinion (per-pair, too many combinations)
//   - query_relations (expensive, query on demand)
//   - query_available_cbs (per-target, too many combinations)
//   - get_ai_evaluations (expensive, debugging tool)
//   - query_title_hierarchy / query_de_jure (large, static-ish)
void capture_snapshot();

// =============================================================================
// Query Safety Classification (revised)
// =============================================================================

// Returns true if a query can ALWAYS be served without game thread access.
// These read only bridge-internal state (atomics, mutexed containers).
bool is_bridge_internal_query(const char* action_type);

// Returns true if a query traverses the game object graph and therefore
// needs either a snapshot or game-thread execution.
bool is_game_state_query(const char* action_type);

// Returns true if this is a mutating action that must execute on the game thread.
bool is_game_thread_action(const char* action_type);

// =============================================================================
// Snapshot Key Construction
// =============================================================================

// Build a snapshot lookup key from action type and character ID.
// Format: "action_type" or "action_type:char_id"
inline std::string snapshot_key(const char* action_type, uint32_t char_id = 0) {
    std::string k(action_type);
    if (char_id != 0) {
        k += ':';
        k += std::to_string(char_id);
    }
    return k;
}

} // namespace bridge
