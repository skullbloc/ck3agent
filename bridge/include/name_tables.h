#pragma once
#include <cstdint>
#include <string>

namespace bridge {

// Initialize hash→name lookup tables for interactions and decisions.
// Must be called during bridge_init before any observation is built.
void init_name_tables();

// Look up an interaction name by its MurmurHash3-32 (seed=0).
// Returns the name string or nullptr if not found.
const char* resolve_interaction_name(uint32_t hash);

// Look up a decision name by its MurmurHash3-32 (seed=0).
// Returns the name string or nullptr if not found.
const char* resolve_decision_name(uint32_t hash);

// =============================================================================
// Character Display Name Cache
// =============================================================================

// Thread-safe lookup of a resolved character display name.
// Returns the cached name, or empty string on cache miss (schedules resolution).
std::string get_cached_character_name(uint32_t char_id);

// Request that a character's name be resolved on the next on_tick.
// No-op if the name is already cached.
void request_name_resolution(uint32_t char_id);

// Called from on_tick (game thread) to resolve pending name requests.
// Calls CHAR_FIRST_NAME_RESOLVED for each pending ID and caches results.
void populate_name_cache();

// Clear all cached names. The cache is keyed by character id and character ids
// are REUSED across saves, so its lifetime is the loaded WORLD, not the process.
// This is registered with the world-epoch detector (see world_epoch.h) in
// name_tables.cpp and fires automatically when a save is loaded into a live
// process; do not rely on anything else calling it. (ck-hxh7)
void clear_name_cache();

// Test seam (ck-hxh7). Override the engine-backed name resolution step used by
// populate_name_cache(), so the cache's world-lifetime behaviour can be tested
// host-side with no game attached. Pass nullptr to restore the engine path.
// Returns true and fills `out` on success; false means "skip this id".
using character_name_resolver_fn = bool (*)(uint32_t char_id, std::string& out);
void set_character_name_resolver(character_name_resolver_fn fn);

} // namespace bridge
