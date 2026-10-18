#pragma once
// world_epoch.h — world (loaded-save) lifetime tracking for the bridge.
//
// ck-hxh7. The bridge .so lives for the whole CK3 PROCESS, but the process can
// LOAD A DIFFERENT SAVE without restarting. That replaces the entire world, and
// game object ids (character / title / war / event ids) are REUSED across saves.
// Any bridge cache keyed by a game id therefore has a WORLD lifetime, not a
// process lifetime — and must be dropped when a new world is loaded.
//
// Proven failure (2026-09-10): ids 31808 / 30853 / 29767 were queried on an 871
// world (Lokpala / Putri / Mila); the same process then loaded a 1066 save where
// those ids are William the Conqueror / Harold Godwinson / Harald Hardrada, and
// the bridge kept serving the 871 names byte-identically for the rest of the
// process, because clear_name_cache() had no caller anywhere.
//
// Mechanism: every world-scoped cache registers a reset callback here once, and
// world_epoch_on_tick() (called from on_tick, game thread) samples a world
// fingerprint each tick and fires every registered reset when the fingerprint
// shows a discontinuity that cannot happen without a load.

#include <cstdint>

#include "init_order.h"

namespace bridge {

// ---------------------------------------------------------------------------
// World fingerprint
// ---------------------------------------------------------------------------
// Sampled from game memory each tick. Deliberately cheap: three guarded reads.
struct WorldFingerprint {
    bool      loaded          = false;  // GAME_STATE_ROOT was non-null
    uintptr_t game_state_ptr  = 0;      // *(GAME_STATE_ROOT) — the CGameState object
    uint16_t  start_date_year = 0;      // gs+0x0E — the SAVE's campaign start year
    int32_t   game_date       = 0;      // gs+0x08 — sub-day ticks (24/day) since epoch
};

// Raw units per game day (ck3::game_state::GAME_DATE is in 1/24 day).
constexpr int32_t WORLD_RAW_UNITS_PER_DAY = 24;

// A forward game-date jump larger than this between two consecutive bridge ticks
// cannot happen without a load. on_tick runs ~once per game DAY (see
// event_handler.cpp: "game runs ~1 tick per day"), i.e. a normal delta is ~24,
// so one game year is ~365x headroom. Deliberately biased LOW: a false positive
// costs one extra name-resolution round trip; a false negative is this bug.
constexpr int32_t WORLD_MAX_FORWARD_JUMP = 365 * WORLD_RAW_UNITS_PER_DAY;  // 8760

// PURE decision function — no memory access, host-testable.
// Returns true if `cur` cannot have followed `prev` within one continuous world.
// On true, *reason (if non-null) is set to a short static string.
bool world_fingerprint_indicates_reload(const WorldFingerprint& prev,
                                        const WorldFingerprint& cur,
                                        const char** reason);

// ---------------------------------------------------------------------------
// World-scoped cache registry
// ---------------------------------------------------------------------------
// Each world-scoped cache calls this ONCE (static initializer or init function)
// to hand over its clear function. Registration is idempotent per fn.
// Keeping this a registry rather than direct calls means world_epoch.cpp has no
// link dependency on any cache's translation unit — it compiles standalone.
using WorldResetFn = void (*)();
void register_world_reset(const char* name, WorldResetFn fn);

// Number of currently registered resets (diagnostics / tests).
int world_reset_count();

// Names of the currently registered resets, in registration order. Writes at
// most `max` pointers into `out` (each points at the caller-supplied literal
// passed to register_world_reset) and returns how many were written.
// Lets a test assert WHICH caches are wired, not merely how many — the original
// bug was a cache that was absent from the set, and a count alone cannot see
// that (ck-hxh7).
int world_reset_names(const char** out, int max);

// ck-22n3 — THE REGISTRATION HOLDER IS A CLASS TYPE, AT PRIORITY 101.
// Each world-scoped cache registers from a file-scope initializer. That used to
// be `static const bool x = []{ register_world_reset(...); return true; }();`,
// and an UNPRIORITIZED dynamic initializer runs AFTER dll_init (bridge_init's
// constructor attribute) — see include/init_order.h. `bool` cannot carry
// init_priority; a class type can, so the holder became one and the lambda body
// is unchanged. Registration is idempotent and touches only function-local
// statics inside world_epoch.cpp, so running earlier changes nothing but the
// moment. Usage, at file scope in the cache's own TU:
//
//     [[maybe_unused]] static const WorldResetRegistrar g_x CK3_EARLY_INIT([] {
//         register_world_reset("my_cache", &clear_my_cache);
//     });
struct WorldResetRegistrar {
    explicit WorldResetRegistrar(void (*register_now)()) { register_now(); }
};

// ---------------------------------------------------------------------------
// Epoch
// ---------------------------------------------------------------------------
// Monotonic counter. Starts at 1; incremented on every detected world load.
uint64_t world_epoch();

// Fire every registered reset and bump the epoch. `why` is logged.
// Safe to call from any thread; the resets themselves take their own locks.
void invalidate_world_caches(const char* why);

// Game thread, once per on_tick: sample the fingerprint and invalidate on a
// discontinuity. No-op while the game is not loaded.
void world_epoch_on_tick();

// Last fingerprint observed (diagnostics).
WorldFingerprint world_last_fingerprint();

// Test seam: feed a fingerprint directly instead of reading game memory.
// Returns true if a reload was detected (and caches were invalidated).
bool world_epoch_observe(const WorldFingerprint& fp);

// Test seam: reset the detector to its pristine state (no prior sample).
void world_epoch_reset_for_test();

} // namespace bridge
