// test/test_world_epoch.cpp — ck-hxh7 regression test.
//
// THE BUG: the bridge .so lives for the whole CK3 process, but the process can
// load a different save. Character ids are reused across saves. g_name_cache in
// name_tables.cpp was keyed by character id and clear_name_cache() had no caller
// anywhere in the tree, so every id touched before a reload was poisoned for the
// life of the process: ids 31808 / 30853 / 29767 kept returning the 871 world's
// Lokpala / Putri / Mila after a 1066 save was loaded, where those ids are
// William, Harold and Haraldr.
//
// This is a standalone host test — no game, no bridge socket. It compiles
// world_epoch.cpp and name_tables.cpp in isolation and supplies the few global
// symbols they reference. The engine's name resolver is replaced through the
// documented test seam set_character_name_resolver(), which lets the cache's
// real population path (populate_name_cache) run with no game attached.

#include "bridge.h"        // MemRange, and the globals the two sources reference
#include "world_epoch.h"
#include "name_tables.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <shared_mutex>
#include <string>
#include <vector>

// Stub globals referenced by bridge.h / name_tables.cpp when compiled standalone.
namespace bridge {
FILE*                  g_logfile = nullptr;
uintptr_t              g_base_addr = 0;
std::vector<MemRange>  g_valid_ranges;
std::shared_mutex      g_ranges_mutex;
}

using namespace bridge;

// ---------------------------------------------------------------------------
// Fake worlds — the exact ids and names from the live evidence.
// ---------------------------------------------------------------------------
static const std::map<uint32_t, std::string>* g_world = nullptr;

static const std::map<uint32_t, std::string> kWorld871 = {
    {31808, "Lokpala"},
    {30853, "Putri"},
    {29767, "Mila"},
};

static const std::map<uint32_t, std::string> kWorld1066 = {
    {31808, "William"},
    {30853, "Harold"},
    {29767, "Haraldr"},
    {32095, "Alp_Arslan"},
};

static bool fake_resolver(uint32_t id, std::string& out) {
    if (!g_world) return false;
    auto it = g_world->find(id);
    if (it == g_world->end()) return false;
    out = it->second;
    return true;
}

// Reader-thread query + game-thread resolution, i.e. the real two-step flow:
// a miss schedules resolution, the next populate_name_cache() fills it in.
static std::string resolve_through_cache(uint32_t id) {
    std::string first = get_cached_character_name(id);
    if (!first.empty()) return first;     // cache hit — no game-thread work needed
    populate_name_cache();                // game thread
    return get_cached_character_name(id);
}

static int g_probe_resets = 0;
static void probe_reset() { g_probe_resets++; }

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

#define CHECK_EQ_STR(actual, expected) do { \
    std::string a_ = (actual); \
    if (a_ != (expected)) { \
        fprintf(stderr, "FAIL %s:%d: expected \"%s\", got \"%s\"\n", \
                __FILE__, __LINE__, (expected), a_.c_str()); \
        return 1; \
    } \
} while (0)

// ---------------------------------------------------------------------------
// 1. The pure reload-decision function.
// ---------------------------------------------------------------------------
static int test_fingerprint_logic() {
    const char* why = nullptr;

    WorldFingerprint unloaded;                       // loaded == false
    WorldFingerprint w871;
    w871.loaded = true;
    w871.game_state_ptr = 0x7f0000001000;
    w871.start_date_year = 867;
    w871.game_date = 43800000 + 871 * 365 * 24;

    // Nothing loaded -> never a reload.
    CHECK(!world_fingerprint_indicates_reload(unloaded, unloaded, &why));
    CHECK(!world_fingerprint_indicates_reload(w871, unloaded, &why));

    // Unloaded -> loaded is a world coming into existence.
    CHECK(world_fingerprint_indicates_reload(unloaded, w871, &why));
    CHECK(strcmp(why, "game_state_appeared") == 0);

    // Normal daily advance within one world is NOT a reload.
    WorldFingerprint next = w871;
    next.game_date += WORLD_RAW_UNITS_PER_DAY;
    CHECK(!world_fingerprint_indicates_reload(w871, next, &why));

    // A whole game year minus a day still is not (threshold is inclusive-safe).
    WorldFingerprint yr = w871;
    yr.game_date += WORLD_MAX_FORWARD_JUMP;
    CHECK(!world_fingerprint_indicates_reload(w871, yr, &why));

    // The game state object being rebuilt is a reload.
    WorldFingerprint moved = w871;
    moved.game_state_ptr = 0x7f0000009000;
    CHECK(world_fingerprint_indicates_reload(w871, moved, &why));
    CHECK(strcmp(why, "game_state_ptr_changed") == 0);

    // THE PROVEN CASE: 871 world -> 1066 save. Caught even if the game state
    // object happened to land on the SAME heap address.
    WorldFingerprint w1066 = w871;
    w1066.start_date_year = 1066;
    w1066.game_date = 43800000 + 1066 * 365 * 24;
    CHECK(world_fingerprint_indicates_reload(w871, w1066, &why));
    CHECK(strcmp(why, "start_date_year_changed") == 0);

    // Rewinding to an earlier save of the same campaign.
    WorldFingerprint back = w871;
    back.game_date -= WORLD_RAW_UNITS_PER_DAY;
    CHECK(world_fingerprint_indicates_reload(w871, back, &why));
    CHECK(strcmp(why, "game_date_went_backwards") == 0);

    // Jumping far forward in the same campaign.
    WorldFingerprint fwd = w871;
    fwd.game_date += WORLD_MAX_FORWARD_JUMP + 1;
    CHECK(world_fingerprint_indicates_reload(w871, fwd, &why));
    CHECK(strcmp(why, "game_date_jumped_forward") == 0);

    printf("  ok: fingerprint logic\n");
    return 0;
}

// ---------------------------------------------------------------------------
// 2. The registry fires every registered reset and bumps the epoch.
// ---------------------------------------------------------------------------
static int test_registry() {
    int before_count = world_reset_count();
    register_world_reset("probe", &probe_reset);
    CHECK(world_reset_count() == before_count + 1);

    // Registration is idempotent — a double-registered cache is not cleared twice.
    register_world_reset("probe", &probe_reset);
    CHECK(world_reset_count() == before_count + 1);

    uint64_t e0 = world_epoch();
    int r0 = g_probe_resets;
    invalidate_world_caches("unit-test");
    CHECK(world_epoch() == e0 + 1);
    CHECK(g_probe_resets == r0 + 1);

    printf("  ok: reset registry (%d registered)\n", world_reset_count());
    return 0;
}

// ---------------------------------------------------------------------------
// 3. THE REGRESSION: a name cached in world A must not survive into world B.
// ---------------------------------------------------------------------------
static int test_name_cache_survives_no_reload_but_not_a_reload() {
    set_character_name_resolver(&fake_resolver);
    world_epoch_reset_for_test();

    // --- World A: the 871 save. ---
    g_world = &kWorld871;

    WorldFingerprint a;
    a.loaded = true;
    a.game_state_ptr = 0x7f0000001000;
    a.start_date_year = 867;
    a.game_date = 43800000 + 871 * 365 * 24;
    world_epoch_observe(a);   // first sample: world appeared, caches empty

    CHECK_EQ_STR(resolve_through_cache(31808), "Lokpala");
    CHECK_EQ_STR(resolve_through_cache(30853), "Putri");
    CHECK_EQ_STR(resolve_through_cache(29767), "Mila");

    // A normal tick must NOT drop the cache — otherwise we would be "passing"
    // this test by never caching anything at all.
    WorldFingerprint a2 = a;
    a2.game_state_ptr = a.game_state_ptr;
    a2.game_date += WORLD_RAW_UNITS_PER_DAY;
    CHECK(!world_epoch_observe(a2));
    g_world = nullptr;                                    // resolver would now fail
    CHECK_EQ_STR(get_cached_character_name(31808), "Lokpala");   // served from cache

    // --- The reload: the same process loads the 1066 save. ---
    // Same game-state address on purpose, so the ONLY signal is the start year —
    // this is the worst realistic case, and it must still be caught.
    g_world = &kWorld1066;
    WorldFingerprint b;
    b.loaded = true;
    b.game_state_ptr = a.game_state_ptr;
    b.start_date_year = 1066;
    b.game_date = 43800000 + 1066 * 365 * 24;

    uint64_t epoch_before = world_epoch();
    CHECK(world_epoch_observe(b));
    CHECK(world_epoch() > epoch_before);

    // THE ASSERTION. Before the fix these three returned Lokpala / Putri / Mila.
    CHECK_EQ_STR(resolve_through_cache(31808), "William");
    CHECK_EQ_STR(resolve_through_cache(30853), "Harold");
    CHECK_EQ_STR(resolve_through_cache(29767), "Haraldr");

    // An id never queried before the reload resolves correctly either (it always
    // did — that asymmetry is what proved this was a cache-hit bug).
    CHECK_EQ_STR(resolve_through_cache(32095), "Alp_Arslan");

    g_world = nullptr;
    set_character_name_resolver(nullptr);
    printf("  ok: name cache re-resolves after a world reload\n");
    return 0;
}

// ---------------------------------------------------------------------------
// 4. The name cache is actually registered — i.e. the wiring exists, not just
//    the ability to wire it. This is the assertion that would have caught the
//    original bug, where clear_name_cache() existed but nothing called it.
// ---------------------------------------------------------------------------
static int test_name_cache_is_registered() {
    set_character_name_resolver(&fake_resolver);
    g_world = &kWorld871;
    world_epoch_reset_for_test();
    invalidate_world_caches("clean slate for registration check");

    CHECK_EQ_STR(resolve_through_cache(31808), "Lokpala");

    // Go through the generic invalidation entry point only — no direct
    // clear_name_cache() call. If name_tables.cpp had not registered itself,
    // this would still return "Lokpala".
    g_world = &kWorld1066;
    invalidate_world_caches("registration check");
    CHECK_EQ_STR(resolve_through_cache(31808), "William");

    g_world = nullptr;
    set_character_name_resolver(nullptr);
    printf("  ok: name cache is registered with the world-reset registry\n");
    return 0;
}

int main() {
    printf("test_world_epoch (ck-hxh7)\n");
    if (test_fingerprint_logic()) return 1;
    if (test_registry()) return 1;
    if (test_name_cache_survives_no_reload_but_not_a_reload()) return 1;
    if (test_name_cache_is_registered()) return 1;
    printf("ALL PASS\n");
    return 0;
}
