// test/acceptance_world_reset.cpp — ck-hxh7 ACCEPTANCE.
//
// THE CLAIM UNDER TEST, in one sentence: when a save is loaded into a live CK3
// process, every world-scoped bridge cache is dropped — the character name
// cache, the sent-event instance-id set, the war-score cache, the war-monitor
// detection state, the ended-war ledger and the personality lock write set.
//
// THE BUG THIS EXISTS TO KEEP DEAD (proven live 2026-09-10): ids 31808 / 30853 /
// 29767 were queried on an 871 world (Lokpala / Putri / Mila); the same process
// then loaded a 1066 save where those ids are William the Conqueror, Harold
// Godwinson and Harald Hardrada, and the bridge served the 871 names
// byte-identically for the rest of the process. clear_name_cache() was defined,
// declared, and called from NOWHERE.
//
// NO GAME. This is a host binary. It links the REAL world_epoch.cpp, the REAL
// name_tables.cpp, the REAL war_history.cpp and the REAL
// personality_world_reset.cpp, and drives a reload through the documented
// world_epoch_observe() seam.
//
// WHERE THE TWO LANES MEET — read this before trusting a PASS.
// Three of the six caches (sent events, war score, war monitor) are file-static
// inside deeply engine-coupled translation units and cannot be linked on a host
// without a CK3 binary. Splitting them out purely to make them testable would
// separate the state from the only code that uses it, so they are NOT split.
// For those three this binary proves the RESET SEMANTICS against a faithful
// model registered exactly as the product registers it, and the companion
// structural lane (scripts/acceptance_world_reset.py) proves, with the real
// compiler on the real preprocessed translation unit, that the product wires
// the same function under the same name. The two halves together are the proof;
// neither half alone is. Every check below prints which lane it belongs to.

#include "bridge.h"
#include "world_epoch.h"
#include "name_tables.h"
#include "personality_locks.h"
#include "war_history.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Stub globals referenced by bridge.h / name_tables.cpp / personality_world_reset.cpp
// when compiled standalone (the test_session_registry / test_world_epoch pattern).
namespace bridge {
FILE*                  g_logfile = nullptr;
uintptr_t              g_base_addr = 0;
std::vector<MemRange>  g_valid_ranges;
std::shared_mutex      g_ranges_mutex;
}

using namespace bridge;

// ---------------------------------------------------------------------------
// Reporting: report every check, never abort on the first (acceptance_n6 style).
// ---------------------------------------------------------------------------
static int g_checks = 0;
static int g_fails  = 0;

static void check(const char* lane, bool ok, const char* what, const char* detail = "") {
    g_checks++;
    if (!ok) g_fails++;
    printf("  [%-10s] %-4s %s%s%s\n", lane, ok ? "ok" : "FAIL", what,
           detail[0] ? " — " : "", detail);
}

static void check_eq_str(const char* lane, const std::string& actual,
                         const char* expected, const char* what) {
    char detail[256];
    snprintf(detail, sizeof(detail), "expected \"%s\", got \"%s\"", expected, actual.c_str());
    check(lane, actual == expected, what, actual == expected ? "" : detail);
}

static void check_eq_sz(const char* lane, size_t actual, size_t expected, const char* what) {
    char detail[128];
    snprintf(detail, sizeof(detail), "expected %zu, got %zu", expected, actual);
    check(lane, actual == expected, what, actual == expected ? "" : detail);
}

// ---------------------------------------------------------------------------
// Two worlds, one process — the exact ids and names from the live evidence.
// ---------------------------------------------------------------------------
static const std::map<uint32_t, std::string> kWorld871 = {
    {31808, "Lokpala"}, {30853, "Putri"}, {29767, "Mila"},
};
static const std::map<uint32_t, std::string> kWorld1066 = {
    {31808, "William"}, {30853, "Harold"}, {29767, "Haraldr"}, {32095, "Alp_Arslan"},
};
static const std::map<uint32_t, std::string>* g_world = nullptr;

static bool fake_resolver(uint32_t id, std::string& out) {
    if (!g_world) return false;
    auto it = g_world->find(id);
    if (it == g_world->end()) return false;
    out = it->second;
    return true;
}

// The real two-step flow: a reader-thread miss schedules resolution, the next
// game-thread populate_name_cache() fills it in.
static std::string resolve_through_cache(uint32_t id) {
    std::string first = get_cached_character_name(id);
    if (!first.empty()) return first;
    populate_name_cache();
    return get_cached_character_name(id);
}

// Fingerprints. GAME_DATE is in 1/24-day units.
static int32_t date_of(int year) { return 43800000 + year * 365 * WORLD_RAW_UNITS_PER_DAY; }

static WorldFingerprint fp_871() {
    WorldFingerprint f;
    f.loaded = true; f.game_state_ptr = 0x7f0000001000; f.start_date_year = 867;
    f.game_date = date_of(871);
    return f;
}
static WorldFingerprint fp_1066() {
    WorldFingerprint f;
    f.loaded = true; f.game_state_ptr = 0x7f0000009000; f.start_date_year = 1066;
    f.game_date = date_of(1066);
    return f;
}

// ---------------------------------------------------------------------------
// Faithful models of the three caches whose real definitions are file-static
// inside engine-coupled TUs. Registered EXACTLY as the product registers them;
// the structural lane proves the product's registration matches.
// ---------------------------------------------------------------------------
namespace model {

std::unordered_set<uint32_t> sent_event_instance_ids;
void clear_all_sent_events() { sent_event_instance_ids.clear(); }

std::map<uint32_t, int> war_score_cache;
std::map<uint32_t, int> war_score_reject;
void clear_war_score_cache() { war_score_cache.clear(); war_score_reject.clear(); }

std::unordered_set<uint32_t> known_war_ids;
int32_t last_active_count = -1;
bool    war_monitor_initialized = false;
void reset_war_monitor_state() {
    known_war_ids.clear();
    last_active_count = -1;
    war_monitor_initialized = false;
}

// war_history.cpp is engine-free, so THIS one drives the real ledger. The
// product registers the same call from war_history_hook.cpp (engine-facing).
void reset_war_history() { war_history().clear(); }

}  // namespace model

// A probe that counts fan-out, to prove invalidate_world_caches() calls
// everything registered and not just the first entry.
static int g_probe_calls = 0;
static void probe_reset() { g_probe_calls++; }

// ---------------------------------------------------------------------------
// 1. The registry and the epoch — the mechanism everything else hangs on.
// ---------------------------------------------------------------------------
static void lane_registry() {
    printf("\n1. REGISTRY AND EPOCH\n");
    const char* LANE = "mechanism";

    // ck-6kvq.5 hard constraint: 0 is that consumer's "no provider" sentinel, so
    // a live world must NEVER read as epoch 0 or resume loses its liveness check.
    check(LANE, world_epoch() >= 1, "world_epoch() is >= 1 before any reload (ck-6kvq.5: 0 is the 'unavailable' sentinel)");

    int before = world_reset_count();
    register_world_reset("acceptance_probe", &probe_reset);
    check(LANE, world_reset_count() == before + 1, "register_world_reset adds a reset");
    register_world_reset("acceptance_probe", &probe_reset);
    check(LANE, world_reset_count() == before + 1, "registration is idempotent per function");

    // Names, not just a count: the original bug was a cache ABSENT from the set,
    // which a count can never see.
    const char* names[64];
    int n = world_reset_names(names, 64);
    auto has = [&](const char* want) {
        for (int i = 0; i < n; i++) if (strcmp(names[i], want) == 0) return true;
        return false;
    };
    check(LANE, has("character_name_cache"),
          "character_name_cache is registered by the real name_tables.cpp static initializer");
    check(LANE, has("personality_locks"),
          "personality_locks is registered by the real personality_world_reset.cpp static initializer");

    uint64_t e0 = world_epoch();
    g_probe_calls = 0;
    invalidate_world_caches("acceptance: fan-out probe");
    check(LANE, g_probe_calls == 1, "invalidate_world_caches() fires every registered reset");
    check(LANE, world_epoch() == e0 + 1, "the epoch advances on an invalidation");
    check(LANE, world_epoch() >= 1, "world_epoch() is still >= 1 after an invalidation");
}

// ---------------------------------------------------------------------------
// 2. The reported bug: the character name cache.
// ---------------------------------------------------------------------------
static void lane_name_cache() {
    printf("\n2. CHARACTER NAME CACHE — the reported bug, on real name_tables.cpp\n");
    const char* LANE = "real-code";

    set_character_name_resolver(&fake_resolver);
    world_epoch_reset_for_test();
    clear_name_cache();

    // --- the 871 world ---
    g_world = &kWorld871;
    world_epoch_observe(fp_871());
    check_eq_str(LANE, resolve_through_cache(31808), "Lokpala", "871 world: 31808 resolves");
    check_eq_str(LANE, resolve_through_cache(30853), "Putri",   "871 world: 30853 resolves");
    check_eq_str(LANE, resolve_through_cache(29767), "Mila",    "871 world: 29767 resolves");

    // --- NEGATIVE CONTROL: without the reload being observed, the bug is here ---
    // Swap the world WITHOUT telling the detector. This is the pre-fix bridge.
    g_world = &kWorld1066;
    check_eq_str(LANE, resolve_through_cache(31808), "Lokpala",
                 "negative control: with NO reload observed the stale 871 name is still served "
                 "(if this line ever reads \"William\", this test can no longer see the bug)");

    // --- the 1066 save is loaded into the same process ---
    bool detected = world_epoch_observe(fp_1066());
    check(LANE, detected, "loading the 1066 save is detected as a world reload");
    check_eq_str(LANE, resolve_through_cache(31808), "William",
                 "after the reload 31808 is William the Conqueror, not Lokpala");
    check_eq_str(LANE, resolve_through_cache(30853), "Harold",
                 "after the reload 30853 is Harold Godwinson, not Putri");
    check_eq_str(LANE, resolve_through_cache(29767), "Haraldr",
                 "after the reload 29767 is Harald Hardrada, not Mila");

    // An id never queried before the reload always resolved correctly — that is
    // what proved this was a cache hit and not a resolution failure. Keep it.
    check_eq_str(LANE, resolve_through_cache(32095), "Alp_Arslan",
                 "an id first queried after the reload resolves (it is a cache-hit bug, not resolution)");
}

// ---------------------------------------------------------------------------
// 3. The ended-war ledger (ck-sd4d) — real war_history.cpp.
// ---------------------------------------------------------------------------
static WarEndRecord make_war(uint32_t id, const char* cb, uint32_t atk, uint32_t def) {
    WarEndRecord r;
    r.war_id = id;
    r.cb_type = cb;
    r.attacker_id = atk;
    r.defender_id = def;
    r.outcome = 0;
    r.target_titles.push_back(108);   // both 1066 wars target k_england
    return r;
}

static void lane_war_ledger() {
    printf("\n3. ENDED-WAR LEDGER (ck-sd4d) — real war_history.cpp\n");
    const char* LANE = "real-code";

    register_world_reset("war_history_ledger", &model::reset_war_history);

    // Settle the detector ON world A first, so the only reload this lane
    // observes is the one it is asserting about.
    world_epoch_reset_for_test();
    world_epoch_observe(fp_871());
    war_history().clear();

    // World A records a termination for war id 7.
    war_history().record(make_war(7, "norwegian_invasion_cb", 29767, 30853));
    check_eq_sz(LANE, war_history().size(), 1, "a terminated war is recorded in world A");

    WarHistoryFilter f;
    f.war_id = 7;
    f.has_war_id = true;
    check_eq_sz(LANE, war_history().query(f).size(), 1, "world A: war id 7 is queryable");

    // A different save is loaded. War ids are reused.
    bool detected = world_epoch_observe(fp_1066());
    check(LANE, detected, "the reload is detected while a ledger row is live");
    check_eq_sz(LANE, war_history().size(), 0,
                "the ledger is EMPTY after the reload — a reused war id cannot match the previous world");
    check_eq_sz(LANE, war_history().query(f).size(), 0,
                "world B: war id 7 answers 0 rows, not world A's row");
}

// ---------------------------------------------------------------------------
// 4. Personality locks (ck-9fc3) — real personality_world_reset.cpp.
//    The ONLY member of this family that WRITES into game memory.
// ---------------------------------------------------------------------------
static PersonalityLock lock_with(int8_t v) {
    PersonalityLock pl{};
    for (int i = 0; i < 9; i++) { pl.axes[i] = v; pl.locked[i] = true; }
    return pl;
}

static void lane_personality() {
    printf("\n4. PERSONALITY LOCKS (ck-9fc3) — real personality_world_reset.cpp; this one WRITES\n");
    const char* LANE = "real-code";

    world_epoch_reset_for_test();
    world_epoch_observe(fp_871());          // settle the detector ON world A
    {
        std::unique_lock<std::shared_mutex> lk(g_personality_lock_mutex);
        g_personality_locks.clear();
        g_quarantined_personality_locks.clear();
    }

    // The agent steers two characters in world A.
    {
        std::unique_lock<std::shared_mutex> lk(g_personality_lock_mutex);
        g_personality_locks[31808] = lock_with(90);
        g_personality_locks[30853] = lock_with(20);
    }
    check_eq_sz(LANE, personality_lock_count(), 2, "world A: two characters are steered");

    world_epoch_observe(fp_1066());

    // THE P1: apply_personality_locks() iterates exactly g_personality_locks,
    // three times per tick, poking int8 axes at a raw pointer. If the set is
    // empty it writes nothing, which is the whole hazard.
    check_eq_sz(LANE, personality_lock_count(), 0,
                "after the reload the LIVE write set is EMPTY — apply_personality_locks() writes nothing "
                "into the new world's characters");
    check_eq_sz(LANE, personality_quarantine_count(), 2,
                "both entries are QUARANTINED, not destroyed — resume (R7) loses nothing");
    check(LANE, personality_quarantine_epoch() == world_epoch(),
          "the quarantine is stamped with the epoch of the reload that quarantined it");
    check(LANE, personality_quarantine_epoch() > 0, "the quarantine epoch is set");

    // The axes survive intact, so ck-9fc3's save-identity work has something to
    // restore from.
    {
        std::shared_lock<std::shared_mutex> lk(g_personality_lock_mutex);
        auto it = g_quarantined_personality_locks.find(31808);
        bool ok = it != g_quarantined_personality_locks.end() && it->second.axes[0] == 90
                  && it->second.locked[0];
        check(LANE, ok, "quarantined axes are preserved byte-for-byte");
    }

    // Never auto-re-applied: that decision belongs to ck-9fc3's save identity.
    {
        std::shared_lock<std::shared_mutex> lk(g_personality_lock_mutex);
        check(LANE, g_personality_locks.find(31808) == g_personality_locks.end(),
              "a quarantined lock is NOT auto-re-applied to the new world");
    }

    // A second reload must not drop the first world's quarantine.
    {
        std::unique_lock<std::shared_mutex> lk(g_personality_lock_mutex);
        g_personality_locks[40001] = lock_with(50);
    }
    WorldFingerprint third = fp_1066();
    third.game_state_ptr = 0x7f000000f000;
    third.start_date_year = 1178;
    third.game_date = date_of(1178);
    world_epoch_observe(third);
    check_eq_sz(LANE, personality_lock_count(), 0, "second reload: the live write set is empty again");
    check_eq_sz(LANE, personality_quarantine_count(), 3,
                "a second reload MERGES into the quarantine instead of dropping the first world's entries");
}

// ---------------------------------------------------------------------------
// 5. Sent events / war score / war monitor — reset semantics on models.
//    (The structural lane proves the product registers the same functions.)
// ---------------------------------------------------------------------------
static void lane_modelled() {
    printf("\n5. SENT EVENTS / WAR SCORE / WAR MONITOR — reset semantics (models; see the\n"
           "   structural lane in scripts/acceptance_world_reset.py for the product wiring)\n");
    const char* LANE = "modelled";

    register_world_reset("sent_event_instance_ids", &model::clear_all_sent_events);
    register_world_reset("war_score_cache",         &model::clear_war_score_cache);
    register_world_reset("war_monitor_detection_state", &model::reset_war_monitor_state);

    world_epoch_reset_for_test();
    world_epoch_observe(fp_871());          // settle the detector ON world A

    model::sent_event_instance_ids = {1001, 1002, 1003};
    model::war_score_cache[7] = 42;
    model::war_score_reject[7] = 3;
    model::known_war_ids = {7, 9};
    model::last_active_count = 2;
    model::war_monitor_initialized = true;

    bool detected = world_epoch_observe(fp_1066());
    check(LANE, detected, "the reload is detected while all three carry the previous world's state");

    check_eq_sz(LANE, model::sent_event_instance_ids.size(), 0,
                "sent-event instance ids cleared — a reused instance id in the new save is no longer "
                "permanently suppressed");
    check_eq_sz(LANE, model::war_score_cache.size(), 0, "war-score cache cleared");
    check_eq_sz(LANE, model::war_score_reject.size(), 0, "war-score reject reasons cleared");
    check_eq_sz(LANE, model::known_war_ids.size(), 0,
                "war-monitor known war ids cleared — a genuinely new declaration is no longer dropped");
    check(LANE, model::last_active_count == -1 && !model::war_monitor_initialized,
          "the war monitor is back to first-call state, so it RE-SEEDS its baseline");
}

// ---------------------------------------------------------------------------
// 6. The false-positive floor. A fix that fires every tick would destroy live
//    steering continuously — worse than the bug.
// ---------------------------------------------------------------------------
static void lane_no_false_positives() {
    printf("\n6. FALSE-POSITIVE FLOOR — one continuous world must never trip a reset\n");
    const char* LANE = "mechanism";

    world_epoch_reset_for_test();
    WorldFingerprint f = fp_1066();
    world_epoch_observe(f);                 // first sample: a world appearing

    uint64_t e = world_epoch();
    g_probe_calls = 0;
    bool any = false;
    for (int day = 0; day < 365 * 3; day++) {   // three game years of daily ticks
        f.game_date += WORLD_RAW_UNITS_PER_DAY;
        if (world_epoch_observe(f)) any = true;
    }
    check(LANE, !any, "three game-years of normal daily ticks trip no reload");
    check(LANE, world_epoch() == e, "the epoch does not move inside one continuous world");
    check(LANE, g_probe_calls == 0, "no registered reset fires inside one continuous world");

    // An engine hiccup that makes the world unreadable must not count as a load
    // by itself (the sample is dropped, not treated as a discontinuity).
    WorldFingerprint unloaded;
    check(LANE, !world_epoch_observe(unloaded), "an unloaded sample is not itself a reload");
}

int main() {
    printf("acceptance_world_reset (ck-hxh7): a save reload drops every world-scoped bridge cache\n");
    printf("no CK3, no bridge socket — host only\n");

    lane_registry();
    lane_name_cache();
    lane_war_ledger();
    lane_personality();
    lane_modelled();
    lane_no_false_positives();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails) {
        printf("acceptance_world_reset: FAIL\n");
        return 1;
    }
    printf("acceptance_world_reset: PASS\n");
    return 0;
}
