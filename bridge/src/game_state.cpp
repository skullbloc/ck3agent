// Game state reading — extracts character data from CK3 process memory
// using the known struct offsets from reverse engineering.

#include "bridge.h"
#include "ck3_offsets.h"

#include <cstdio>
#include <cstring>

namespace bridge {

// Cached pointer to the CAIManager instance (set on first tick)
static void* g_ai_manager = nullptr;

void set_ai_manager(void* mgr) {
    g_ai_manager = mgr;
}

void* get_ai_manager() {
    return g_ai_manager;
}

// get_game_state / get_interaction_db removed — were unused. Recoverable from git history.

// dispatch_router.cpp (action_dispatch_internal.h): the one character id and
// realm-data read.
uint32_t char_get_id(void* character);
void* char_get_realm(void* character);

// Every read below is safe_read: enumerate_ai_characters serves Tier-2 queries
// (query_realm's realm county count, get_ai_evaluations) inline on the IPC
// reader thread, where a raw dereference of a freed entry is a fault on that
// thread (bridge/scripts/inline_safety_static.py audits this).

// Check if a character is alive: death_date ptr at +0x1c8 == null. A death
// date that could not be read is NOT alive: safe_read answers 0 on a fault,
// which is the alive value, so the fault count decides (counted, not cleared:
// the caller's own fault flag survives).
static bool char_is_alive(void* character) {
    const int faults = safe_read_fault_count();
    const uint64_t death = safe_read<uint64_t>(reinterpret_cast<uintptr_t>(character) + ck3::character::DEATH_DATE);
    return safe_read_fault_count() == faults && death == 0;
}

// Get the AI controller for a character (via realm data chain)
static void* char_get_ai_ctrl(void* character) {
    void* realm = char_get_realm(character);
    if (!realm) return nullptr;
    return safe_read<void*>(reinterpret_cast<uintptr_t>(realm) + ck3::character::REALM_AI_CTRL);
}

// Check if a character's AI is enabled
static bool char_ai_enabled(void* ai_ctrl) {
    if (!ai_ctrl) return false;
    return safe_read<uint8_t>(reinterpret_cast<uintptr_t>(ai_ctrl) + ck3::ai_ctrl::AI_ENABLED) != 0;
}

void enumerate_ai_characters(std::function<bool(const CharacterInfo&)> callback) {
    // Use the global CHARACTER_DB hash table (confirmed working via lookup_character)
    // Layout: standard ck3::hash_map header (TABLE_PTR/CAPACITY/ENTRY_STRIDE).
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::CHARACTER_DB);
    if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] enumerate_ai_chars: CHARACTER_DB=%p\n", (void*)db); fflush(g_logfile); }
    if (!db) return;

    uintptr_t table = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    uint32_t capacity = safe_read<uint32_t>(db + ck3::hash_map::CAPACITY);
    if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge]   table=%p capacity=%u\n", (void*)table, capacity); fflush(g_logfile); }
    if (!table || capacity == 0 || capacity > 500000) return;

    uintptr_t sentinel = safe_read_offset<uintptr_t>(ck3::globals::CHAR_DB_SENTINEL);
    int iterated = 0, alive_ai = 0;
    for (uint32_t i = 0; i < capacity; i++) {
        uintptr_t entry_ptr = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                              + static_cast<uintptr_t>(i) * ck3::hash_map::ENTRY_STRIDE);
        if (!entry_ptr) continue;

        // Validate pointer is in reasonable heap range
        if (entry_ptr < 0x10000 || entry_ptr > 0x7FFFFFFFFFFF) continue;

        if (entry_ptr == sentinel) continue;

        void* character = reinterpret_cast<void*>(entry_ptr);

        CharacterInfo info;
        info.raw_ptr = character;
        info.id = char_get_id(character);
        info.alive = char_is_alive(character);
        // Engine's canonical IsRuler: char+0x1B8 (TITLE_REALM_DATA) != 0
        // (core_data::IS_RULER at 0x54C is unreliable)
        {
            uintptr_t title_realm = safe_read<uintptr_t>(
                reinterpret_cast<uintptr_t>(character) + ck3::character::TITLE_REALM_DATA);
            info.has_realm = (title_realm != 0);
        }
        info.ai_controller = char_get_ai_ctrl(character);
        info.ai_controlled = char_ai_enabled(info.ai_controller);

        iterated++;

        // Only report alive AI-controlled characters
        if (info.alive && info.ai_controlled) {
            alive_ai++;
            if (!callback(info)) break;
        }
    }

    if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge]   enumerate done: capacity=%u iterated=%d alive_ai=%d\n", capacity, iterated, alive_ai); fflush(g_logfile); }
}

// Generate RNG using the same hash chain as the native AI
uint32_t generate_rng() {
    if (!g_ai_manager || !is_mapped(reinterpret_cast<uintptr_t>(g_ai_manager))) return 0;
    uintptr_t mgr = reinterpret_cast<uintptr_t>(g_ai_manager);

    const int faults = safe_read_fault_count();
    uint32_t seed = safe_read<uint32_t>(mgr + ck3::ai_mgr::RNG_SEED);
    uint32_t secondary = safe_read<uint32_t>(mgr + ck3::ai_mgr::RNG_SECONDARY);
    // A seed that could not be read is not stored back as 1 (counted, not
    // cleared: the caller's fault check sees it).
    if (safe_read_fault_count() != faults) return 0;

    uint32_t val = seed * 0xb529a27d + secondary;
    val = (val >> 8 ^ val) + 0x68e31da4;
    val = (val * 0x100 ^ val) * 0x1b56c4e9;
    val = (val >> 8 ^ val) * 0x92d68ca2;
    uint32_t result = (val & 0x7ffffffe) ^ (val >> 8);

    // Increment the seed counter (same as native AI)
    *reinterpret_cast<uint32_t*>(mgr + ck3::ai_mgr::RNG_SEED) = seed + 1;

    return result;
}

} // namespace bridge
