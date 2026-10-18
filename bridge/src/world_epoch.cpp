// world_epoch.cpp — detect a save/world reload inside a live CK3 process and
// drop every world-scoped bridge cache. See include/world_epoch.h for the
// failure this exists to prevent (ck-hxh7).
//
// WHY A POLLED FINGERPRINT AND NOT A STATE-MACHINE EDGE
// -----------------------------------------------------
// The obvious hook looked like the session state machine's DORMANT -> ACTIVE
// edge in bridge_init.cpp ("BUILD-B: First tick received"). It is NOT
// sufficient, and the reason is visible in the source: bridge_init.cpp declares
// SessionState::RELOADING, g_last_ai_manager ("detect save reloads (pointer
// changes)"), g_last_tick_id_at_active and RELOAD_DETECT_TICKS — and NONE of
// them is ever read or compared. g_last_ai_manager is written in exactly one
// place, bridge_shutdown(). The reload detector was designed and never built.
// DORMANT -> ACTIVE fires once, on the first tick after dlopen; a save loaded
// later in the same process never leaves ACTIVE, so that edge would never fire
// again and the caches would still be stale. Hence: no reliable in-process load
// SIGNAL exists in the bridge today, and the cheapest honest substitute is a
// per-tick discontinuity check on the world's own identity.
//
// THE FINGERPRINT (three guarded reads per tick)
//   1. *(GAME_STATE_ROOT) — the CGameState object pointer. A load destroys and
//      rebuilds the game state; the pointer normally changes. Not trusted
//      alone: a pool allocator can hand back the same address.
//   2. gs+0x0E START_DATE_YEAR (uint16, CONFIRMED) — the SAVE's campaign start
//      year. 867 vs 1066 differ outright. This is what makes the proven failure
//      case detectable even if (1) and (3) were both blind.
//   3. gs+0x08 GAME_DATE (int32, CONFIRMED, 24 units/day) — monotone within one
//      world. Any BACKWARD step, or a forward step larger than a game year
//      between consecutive ~daily ticks, requires a load.
//
// The signals are OR-ed. A false positive costs one extra name-resolution round
// trip; a false negative is the bug. Residual gap, stated honestly: loading a
// DIFFERENT save with the SAME campaign start year at NEARLY THE SAME date into
// a game state that landed on the SAME heap address would not be detected. That
// is a same-campaign branch reload, where ids overwhelmingly denote the same
// characters — the low-harm corner. Every cross-campaign reload, every
// rewind-to-earlier-save, and every long-jump reload is caught.

#include "world_epoch.h"
#include "bridge.h"
#include "ck3_offsets.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

namespace bridge {

namespace {

struct WorldResetEntry {
    const char*  name;
    WorldResetFn fn;
};

std::mutex& reset_mutex() {
    static std::mutex m;
    return m;
}

std::vector<WorldResetEntry>& reset_table() {
    static std::vector<WorldResetEntry> t;
    return t;
}

std::mutex             g_epoch_mutex;
uint64_t               g_epoch = 1;
bool                   g_have_prev = false;
WorldFingerprint       g_prev;

void wlog(const char* fmt, ...) {
    if (!g_logfile) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logfile, fmt, ap);
    va_end(ap);
    fflush(g_logfile);
}

}  // namespace

// ---------------------------------------------------------------------------
// Pure decision function
// ---------------------------------------------------------------------------
bool world_fingerprint_indicates_reload(const WorldFingerprint& prev,
                                        const WorldFingerprint& cur,
                                        const char** reason) {
    auto say = [&](const char* r) { if (reason) *reason = r; return true; };

    // Nothing loaded now — not a reload, just an unloaded/loading window.
    if (!cur.loaded) { if (reason) *reason = ""; return false; }

    // We were unloaded and are now loaded: a world came into existence. This is
    // a load by definition (it is also the first-ever sample, which is harmless
    // to treat as a load — the caches are empty).
    if (!prev.loaded) return say("game_state_appeared");

    if (cur.game_state_ptr != prev.game_state_ptr) return say("game_state_ptr_changed");
    if (cur.start_date_year != prev.start_date_year) return say("start_date_year_changed");

    if (cur.game_date < prev.game_date) return say("game_date_went_backwards");
    if (static_cast<int64_t>(cur.game_date) - static_cast<int64_t>(prev.game_date)
        > WORLD_MAX_FORWARD_JUMP) {
        return say("game_date_jumped_forward");
    }

    if (reason) *reason = "";
    return false;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
void register_world_reset(const char* name, WorldResetFn fn) {
    if (!fn) return;
    std::lock_guard<std::mutex> lock(reset_mutex());
    for (const auto& e : reset_table()) {
        if (e.fn == fn) return;  // idempotent
    }
    reset_table().push_back({name ? name : "(unnamed)", fn});
}

int world_reset_count() {
    std::lock_guard<std::mutex> lock(reset_mutex());
    return static_cast<int>(reset_table().size());
}

int world_reset_names(const char** out, int max) {
    if (!out || max <= 0) return 0;
    std::lock_guard<std::mutex> lock(reset_mutex());
    int n = 0;
    for (const auto& e : reset_table()) {
        if (n >= max) break;
        out[n++] = e.name;
    }
    return n;
}

uint64_t world_epoch() {
    std::lock_guard<std::mutex> lock(g_epoch_mutex);
    return g_epoch;
}

void invalidate_world_caches(const char* why) {
    // Snapshot the table under its own lock, then run the resets OUTSIDE it so a
    // reset is free to take whatever locks it likes (and even register more).
    std::vector<WorldResetEntry> entries;
    {
        std::lock_guard<std::mutex> lock(reset_mutex());
        entries = reset_table();
    }

    uint64_t epoch;
    {
        std::lock_guard<std::mutex> lock(g_epoch_mutex);
        epoch = ++g_epoch;
    }

    for (const auto& e : entries) {
        if (e.fn) e.fn();
    }

    wlog("[CK3-Bridge] WORLD-RELOAD: %s — epoch=%llu, cleared %zu world-scoped cache(s)\n",
         why ? why : "(unspecified)",
         static_cast<unsigned long long>(epoch), entries.size());
}

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------
bool world_epoch_observe(const WorldFingerprint& fp) {
    WorldFingerprint prev;
    bool have_prev;
    {
        std::lock_guard<std::mutex> lock(g_epoch_mutex);
        prev = g_prev;
        have_prev = g_have_prev;
    }

    WorldFingerprint prev_eff = have_prev ? prev : WorldFingerprint{};

    const char* reason = "";
    bool reload = world_fingerprint_indicates_reload(prev_eff, fp, &reason);

    // Record the new sample regardless (including the unloaded window, so the
    // next loaded sample trips "game_state_appeared").
    {
        std::lock_guard<std::mutex> lock(g_epoch_mutex);
        g_prev = fp;
        g_have_prev = true;
    }

    if (reload) {
        char why[192];
        snprintf(why, sizeof(why),
                 "%s (gs=%p start_year=%u date=%d <- gs=%p start_year=%u date=%d)",
                 reason,
                 reinterpret_cast<void*>(fp.game_state_ptr),
                 static_cast<unsigned>(fp.start_date_year), fp.game_date,
                 reinterpret_cast<void*>(prev_eff.game_state_ptr),
                 static_cast<unsigned>(prev_eff.start_date_year), prev_eff.game_date);
        invalidate_world_caches(why);
    }
    return reload;
}

WorldFingerprint world_last_fingerprint() {
    std::lock_guard<std::mutex> lock(g_epoch_mutex);
    return g_prev;
}

void world_epoch_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_epoch_mutex);
    g_prev = WorldFingerprint{};
    g_have_prev = false;
}

void world_epoch_on_tick() {
    // All reads go through safe_read (process_vm_readv) so this is independent
    // of refresh_memory_map() ordering inside on_tick.
    safe_read_clear();

    WorldFingerprint fp;
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (safe_read_had_fault()) return;  // cannot even read the root — observe nothing

    if (gs) {
        uint16_t start_year = safe_read<uint16_t>(gs + ck3::game_state::START_DATE_YEAR);
        int32_t  date       = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
        // A partial/faulted sample would look like a discontinuity and flap the
        // caches every tick. Drop it instead; the next tick resamples.
        if (safe_read_had_fault()) return;
        fp.loaded          = true;
        fp.game_state_ptr  = gs;
        fp.start_date_year = start_year;
        fp.game_date       = date;
    }
    world_epoch_observe(fp);
}

}  // namespace bridge
