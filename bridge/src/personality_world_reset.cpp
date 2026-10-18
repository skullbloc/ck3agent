// personality_world_reset.cpp — the world-reset half of the personality lock
// table (ck-hxh7 / ck-9fc3).
//
// Deliberately a separate translation unit from personality.cpp. personality.cpp
// is thoroughly engine-coupled (character lookup, offset headers, raw memory
// writes) and cannot be compiled on a host; this file touches nothing but two
// std::unordered_maps and a mutex, so bridge/test/acceptance_world_reset links
// the REAL table and the REAL reset function with no CK3 binary anywhere. The
// table definitions live here for the same reason.
//
// See include/personality_locks.h for why this is a quarantine and not a clear.

#include "personality_locks.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "world_epoch.h"

#include <cstdio>

namespace bridge {

// Declared in bridge.h; redeclared here so this TU stays engine-free. Null in
// the host acceptance, which is why every use is guarded.
extern FILE* g_logfile;

std::shared_mutex g_personality_lock_mutex;
std::unordered_map<uint32_t, PersonalityLock> g_personality_locks CK3_EARLY_INIT;
std::unordered_map<uint32_t, PersonalityLock> g_quarantined_personality_locks CK3_EARLY_INIT;

namespace {
uint64_t g_quarantine_epoch = 0;   // 0 = nothing has ever been quarantined
}

void quarantine_personality_locks_on_world_reset() {
    size_t moved = 0;
    uint64_t epoch = world_epoch();
    {
        std::unique_lock<std::shared_mutex> lock(g_personality_lock_mutex);
        if (g_personality_locks.empty()) return;

        // Merge rather than overwrite: two reloads in a row must not silently
        // drop the first world's entries.
        for (auto& [cid, pl] : g_personality_locks) {
            g_quarantined_personality_locks[cid] = pl;
        }
        moved = g_personality_locks.size();
        g_personality_locks.clear();
        g_quarantine_epoch = epoch;
    }

    if (g_logfile) {
        fprintf(g_logfile,
                "[CK3-Bridge] WORLD-RELOAD: quarantined %zu personality lock(s) at epoch=%llu "
                "— the live write set is now EMPTY; quarantined locks are NEVER auto-re-applied "
                "(ck-9fc3 owns the save-identity decision)\n",
                moved, static_cast<unsigned long long>(epoch));
        fflush(g_logfile);
    }
}

size_t restore_quarantined_personality_locks() {
    size_t moved = 0;
    {
        std::unique_lock<std::shared_mutex> lock(g_personality_lock_mutex);
        for (auto& [cid, pl] : g_quarantined_personality_locks) {
            // An explicit set against the world now loaded is newer evidence
            // than anything carried over — it does not get overwritten.
            if (g_personality_locks.find(cid) != g_personality_locks.end()) continue;
            g_personality_locks[cid] = pl;
            moved++;
        }
        g_quarantined_personality_locks.clear();
        g_quarantine_epoch = 0;
    }

    if (g_logfile) {
        fprintf(g_logfile,
                "[CK3-Bridge] SAVE-IDENTITY: restored %zu personality lock(s) into the live "
                "write set — the declared save identity matched the world the locks were set in "
                "(ck-9fc3)\n",
                moved);
        fflush(g_logfile);
    }
    return moved;
}

size_t personality_lock_count() {
    std::shared_lock<std::shared_mutex> lock(g_personality_lock_mutex);
    return g_personality_locks.size();
}

size_t personality_quarantine_count() {
    std::shared_lock<std::shared_mutex> lock(g_personality_lock_mutex);
    return g_quarantined_personality_locks.size();
}

uint64_t personality_quarantine_epoch() {
    std::shared_lock<std::shared_mutex> lock(g_personality_lock_mutex);
    return g_quarantine_epoch;
}

// Register once, at .so load. The registry is what makes this fire; there is no
// other reload signal in the bridge (see world_epoch.cpp).
[[maybe_unused]] static const WorldResetRegistrar g_personality_locks_world_scoped CK3_EARLY_INIT([] {
    register_world_reset("personality_locks", &quarantine_personality_locks_on_world_reset);
});

} // namespace bridge
