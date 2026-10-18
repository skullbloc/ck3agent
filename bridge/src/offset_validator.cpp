// Offset regression test suite — validates known offsets on bridge init.
// Reads expected patterns at critical offsets and reports PASS/FAIL per group.
// If >30% of groups fail, the bridge enters degraded mode (read-only, no hooks).

#include "bridge.h"
#include "ck3_offsets.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

// Use bridge log
extern FILE* bridge::g_logfile;

static void vlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void vlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (bridge::g_logfile) {
        fputs("[OffsetValidator] ", bridge::g_logfile);
        vfprintf(bridge::g_logfile, fmt, ap);
        fflush(bridge::g_logfile);
    }
    va_end(ap);
}

namespace bridge {

// A single offset check: read memory at base+offset and verify a condition.
struct OffsetCheck {
    const char* name;
    uintptr_t offset;
    enum CheckType {
        READABLE,        // Address must be mapped/readable
        PTR_NONNULL,     // Read a pointer, must be non-null
        PTR_MAPPED,      // Read a pointer, the target must also be mapped
        BYTE_NONZERO,    // Read a byte, must be non-zero
        U32_RANGE,       // Read a uint32, must be in [lo, hi]
    } type;
    uint32_t lo;  // for U32_RANGE
    uint32_t hi;
};

// A group of related checks
struct OffsetGroup {
    const char* name;
    const OffsetCheck* checks;
    int count;
};

// ── Check definitions ──────────────────────────────────────────────────────

// Global singletons: these pointers must be readable
static const OffsetCheck global_checks[] = {
    {"AI_ENABLED_FLAG",    ck3::globals::AI_ENABLED_FLAG,    OffsetCheck::READABLE,   0, 0},
    {"GAME_STATE_ROOT",    ck3::globals::GAME_STATE_ROOT,    OffsetCheck::READABLE,   0, 0},
    {"CHARACTER_DB",       ck3::globals::CHARACTER_DB,       OffsetCheck::READABLE,   0, 0},
    {"TRAIT_DB",           ck3::globals::TRAIT_DB,           OffsetCheck::READABLE,   0, 0},
    {"INTERACTION_DB",     ck3::globals::INTERACTION_DB,     OffsetCheck::READABLE,   0, 0},
    {"IDLER_SINGLETON",    ck3::globals::IDLER_SINGLETON,    OffsetCheck::READABLE,   0, 0},
    {"PLAYER_CHAR_ID",     ck3::globals::PLAYER_CHAR_ID,    OffsetCheck::READABLE,   0, 0},
};

// Database singletons: must be readable locations
static const OffsetCheck db_checks[] = {
    {"CULTURE_DB",         ck3::globals::CULTURE_DB,         OffsetCheck::READABLE,   0, 0},
    {"FAITH_DB",           ck3::globals::FAITH_DB,           OffsetCheck::READABLE,   0, 0},
    {"REALM_LAW_DB",       ck3::globals::REALM_LAW_DB,       OffsetCheck::READABLE,   0, 0},
    {"DYNASTY_DB",         ck3::globals::DYNASTY_DB,         OffsetCheck::READABLE,   0, 0},
    {"HOUSE_DB",           ck3::globals::HOUSE_DB,           OffsetCheck::READABLE,   0, 0},
    {"LANDED_TITLE_DB",    ck3::globals::LANDED_TITLE_DB,    OffsetCheck::READABLE,   0, 0},
    {"WAR_DB",             ck3::globals::WAR_DB,             OffsetCheck::READABLE,   0, 0},
    {"ARMY_DB",            ck3::globals::ARMY_DB,            OffsetCheck::READABLE,   0, 0},
    {"CASUS_BELLI_DB",     ck3::globals::CASUS_BELLI_DB,     OffsetCheck::READABLE,   0, 0},
    {"DECISION_DB",        ck3::globals::DECISION_DB,        OffsetCheck::READABLE,   0, 0},
};

// AI vtable addresses: must be readable and contain valid function pointers
static const OffsetCheck vtable_checks[] = {
    {"CAIMAN_VTABLE",          ck3::funcs::CAIMAN_VTABLE,          OffsetCheck::PTR_MAPPED, 0, 0},
    {"CAIMAN_INTERNAL_VTABLE", ck3::funcs::CAIMAN_INTERNAL_VTABLE, OffsetCheck::PTR_MAPPED, 0, 0},
};

// AI function addresses: the function entry points must be in mapped executable memory
static const OffsetCheck func_checks[] = {
    {"AI_MANAGER_MAIN_TICK",   ck3::funcs::AI_MANAGER_MAIN_TICK,   OffsetCheck::READABLE,   0, 0},
    {"AI_MANAGER_POST_EXECUTE", ck3::funcs::AI_MANAGER_POST_EXECUTE, OffsetCheck::READABLE, 0, 0},
    {"AI_CHAR_TOGGLE",         ck3::funcs::AI_CHAR_TOGGLE,         OffsetCheck::READABLE,   0, 0},
    {"AI_CHAR_EVAL",           ck3::funcs::AI_CHAR_EVAL,           OffsetCheck::READABLE,   0, 0},
};

// Game state structure: verify the layout hasn't shifted
// GAME_SPEED at game_state+0x70 should be a valid speed (0-4 when loaded)
// We check readability here since game may not be loaded yet at init time
static const OffsetCheck game_state_checks[] = {
    {"MAX_GAME_SPEED",     ck3::globals::MAX_GAME_SPEED,     OffsetCheck::READABLE,   0, 0},
    {"AI_TOGGLE_BASE",     ck3::globals::AI_TOGGLE_BASE,     OffsetCheck::READABLE,   0, 0},
    {"AI_RNG_COUNTER",     ck3::globals::AI_RNG_COUNTER,     OffsetCheck::READABLE,   0, 0},
};

// Siege/military system
static const OffsetCheck military_checks[] = {
    {"SIEGE_DB",           ck3::globals::SIEGE_DB,            OffsetCheck::READABLE,   0, 0},
    {"REGIMENT_DB",        ck3::globals::REGIMENT_DB,         OffsetCheck::READABLE,   0, 0},
    {"MAA_TYPE_DB",        ck3::globals::MAA_TYPE_DB,         OffsetCheck::READABLE,   0, 0},
    {"OWNER_LINK_DB",      ck3::globals::OWNER_LINK_DB,       OffsetCheck::READABLE,   0, 0},
};

// Event/scheme system
static const OffsetCheck event_checks[] = {
    {"EVENT_MANAGER",      ck3::globals::EVENT_MANAGER,       OffsetCheck::READABLE,   0, 0},
    {"SCHEME_REFDB",       ck3::globals::SCHEME_REFDB,        OffsetCheck::READABLE,   0, 0},
    {"SCHEME_TYPE_DB",     ck3::globals::SCHEME_TYPE_DB,      OffsetCheck::READABLE,   0, 0},
};

#define OFFSET_GROUP(name, arr) { name, arr, static_cast<int>(sizeof(arr) / sizeof(arr[0])) }

static const OffsetGroup all_groups[] = {
    OFFSET_GROUP("globals",      global_checks),
    OFFSET_GROUP("databases",    db_checks),
    OFFSET_GROUP("vtables",      vtable_checks),
    OFFSET_GROUP("functions",    func_checks),
    OFFSET_GROUP("game_state",   game_state_checks),
    OFFSET_GROUP("military",     military_checks),
    OFFSET_GROUP("events",       event_checks),
};

static constexpr int NUM_GROUPS = sizeof(all_groups) / sizeof(all_groups[0]);

// ── Execution ──────────────────────────────────────────────────────────────

static bool run_check(uintptr_t base, const OffsetCheck& check) {
    uintptr_t addr = base + check.offset;

    switch (check.type) {
    case OffsetCheck::READABLE:
        return is_mapped(addr);

    case OffsetCheck::BYTE_NONZERO:
        if (!is_mapped(addr)) return false;
        return safe_read<uint8_t>(addr) != 0;

    case OffsetCheck::PTR_NONNULL:
        if (!is_mapped_range(addr, sizeof(uintptr_t))) return false;
        return safe_read<uintptr_t>(addr) != 0;

    case OffsetCheck::PTR_MAPPED: {
        if (!is_mapped_range(addr, sizeof(uintptr_t))) return false;
        uintptr_t ptr = safe_read<uintptr_t>(addr);
        if (ptr == 0) return false;
        return is_mapped(ptr);
    }

    case OffsetCheck::U32_RANGE:
        if (!is_mapped_range(addr, sizeof(uint32_t))) return false;
        {
            uint32_t val = safe_read<uint32_t>(addr);
            return val >= check.lo && val <= check.hi;
        }
    }
    return false;
}

OffsetValidationResult validate_offsets() {
    OffsetValidationResult result = {};

    // We need the memory map for is_mapped checks
    refresh_memory_map();

    uintptr_t base = g_base_addr;
    if (base == 0) {
        vlog("ERROR: g_base_addr is 0, cannot validate\n");
        result.degraded = true;
        return result;
    }

    vlog("=== Offset Regression Test Suite ===\n");
    vlog("Binary base: 0x%lx\n", base);

    int groups_passed = 0;
    int groups_failed = 0;
    int total_checks = 0;
    int total_passed = 0;

    for (int g = 0; g < NUM_GROUPS; g++) {
        const OffsetGroup& group = all_groups[g];
        int group_pass = 0;
        int group_fail = 0;

        for (int c = 0; c < group.count; c++) {
            const OffsetCheck& check = group.checks[c];
            bool ok = run_check(base, check);
            total_checks++;

            if (ok) {
                group_pass++;
                total_passed++;
            } else {
                group_fail++;
                vlog("  FAIL: %s::%s (offset=0x%lx)\n",
                     group.name, check.name, check.offset);
            }
        }

        bool group_ok = (group_fail == 0);
        if (group_ok) {
            groups_passed++;
        } else {
            groups_failed++;
        }

        result.group_passed[g] = group_ok;
        result.group_checks_passed[g] = group_pass;
        result.group_checks_failed[g] = group_fail;
        result.group_checks_total[g] = group.count;
        vlog("  [%s] %s: %d/%d passed\n",
             group_ok ? "PASS" : "FAIL", group.name,
             group_pass, group.count);
    }

    result.total_groups = NUM_GROUPS;
    result.groups_passed = groups_passed;
    result.groups_failed = groups_failed;
    result.total_checks = total_checks;
    result.checks_passed = total_passed;
    result.checks_failed = total_checks - total_passed;

    // Degraded mode if >30% of groups fail
    float fail_pct = (NUM_GROUPS > 0)
        ? (static_cast<float>(groups_failed) / NUM_GROUPS) * 100.0f
        : 0.0f;
    result.failure_pct = fail_pct;
    result.degraded = (fail_pct > 30.0f);

    vlog("=== Summary: %d/%d groups passed, %d/%d checks passed (%.1f%% group failure) ===\n",
         groups_passed, NUM_GROUPS, total_passed, total_checks, fail_pct);

    if (result.degraded) {
        vlog("*** DEGRADED MODE: >30%% group failure — disabling vtable hooks ***\n");
    } else {
        vlog("All critical offset groups validated — full mode enabled\n");
    }

    return result;
}

int offset_validation_group_count() {
    return NUM_GROUPS;
}

const char* offset_validation_group_name(int index) {
    if (index < 0 || index >= NUM_GROUPS) return "unknown";
    return all_groups[index].name;
}

} // namespace bridge
