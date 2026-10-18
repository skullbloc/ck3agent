// Per-character AI subsystem mask API.
//
// The 10 dispatch hooks (inline patches on parallel AI worker functions) have
// been removed. They were pure pass-throughs (try/catch wrappers) that added
// crash surface without providing functionality. The subsystem mask API is
// retained so the agent can record which subsystems it handles vs. delegates
// to native AI.

#include "bridge.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <mutex>

namespace bridge {

// =============================================================================
// Subsystem Bit Definitions — ALL 38 subsystems from R14 ground truth
// =============================================================================

static constexpr uint64_t SUBSYSTEM_BIT_DECISIONS            = (1ULL << 0);
static constexpr uint64_t SUBSYSTEM_BIT_GRANT_REVOKE_TITLES  = (1ULL << 1);
static constexpr uint64_t SUBSYSTEM_BIT_SET_REALM_CAPITAL    = (1ULL << 2);
static constexpr uint64_t SUBSYSTEM_BIT_LIFESTYLES           = (1ULL << 3);
static constexpr uint64_t SUBSYSTEM_BIT_DYNASTY_LEGACIES     = (1ULL << 4);
static constexpr uint64_t SUBSYSTEM_BIT_HOUSE_POWER          = (1ULL << 5);
static constexpr uint64_t SUBSYSTEM_BIT_CULTURE_TRADITIONS   = (1ULL << 6);
static constexpr uint64_t SUBSYSTEM_BIT_DECLARE_WAR          = (1ULL << 7);
static constexpr uint64_t SUBSYSTEM_BIT_FACTION              = (1ULL << 8);
static constexpr uint64_t SUBSYSTEM_BIT_MARRIAGES            = (1ULL << 9);
static constexpr uint64_t SUBSYSTEM_BIT_COUNCIL              = (1ULL << 10);
static constexpr uint64_t SUBSYSTEM_BIT_COUNCIL_TASKS        = (1ULL << 11);
static constexpr uint64_t SUBSYSTEM_BIT_SCHEME_COUNTER       = (1ULL << 12);
static constexpr uint64_t SUBSYSTEM_BIT_CALL_ALLIES          = (1ULL << 13);
static constexpr uint64_t SUBSYSTEM_BIT_DEVELOPMENT          = (1ULL << 14);
static constexpr uint64_t SUBSYSTEM_BIT_RELIGIOUS_REFORM     = (1ULL << 15);
static constexpr uint64_t SUBSYSTEM_BIT_SUCC_ELECTION        = (1ULL << 16);
static constexpr uint64_t SUBSYSTEM_BIT_SUCC_APPOINTMENT     = (1ULL << 17);
static constexpr uint64_t SUBSYSTEM_BIT_GOALS                = (1ULL << 18);
static constexpr uint64_t SUBSYSTEM_BIT_INTERACTIONS         = (1ULL << 19);
static constexpr uint64_t SUBSYSTEM_BIT_LAWS                 = (1ULL << 20);
static constexpr uint64_t SUBSYSTEM_BIT_PRIMARY_TITLE        = (1ULL << 21);
static constexpr uint64_t SUBSYSTEM_BIT_CAPITAL              = (1ULL << 22);
static constexpr uint64_t SUBSYSTEM_BIT_SCRIPTED_GUI         = (1ULL << 23);
static constexpr uint64_t SUBSYSTEM_BIT_ARTIFACTS            = (1ULL << 24);
static constexpr uint64_t SUBSYSTEM_BIT_COURT_AMENITIES      = (1ULL << 25);
static constexpr uint64_t SUBSYSTEM_BIT_COURT_TYPES          = (1ULL << 26);
static constexpr uint64_t SUBSYSTEM_BIT_COURT_POSITIONS      = (1ULL << 27);
static constexpr uint64_t SUBSYSTEM_BIT_COURT_LANGUAGE       = (1ULL << 28);
static constexpr uint64_t SUBSYSTEM_BIT_HOST_ACTIVITIES      = (1ULL << 29);
static constexpr uint64_t SUBSYSTEM_BIT_ATTEND_ACTIVITIES    = (1ULL << 30);
static constexpr uint64_t SUBSYSTEM_BIT_VASSAL_STANCE        = (1ULL << 31);
static constexpr uint64_t SUBSYSTEM_BIT_ACCOLADES            = (1ULL << 32);
static constexpr uint64_t SUBSYSTEM_BIT_STATION_MAA          = (1ULL << 33);
static constexpr uint64_t SUBSYSTEM_BIT_TAX_COLLECTOR        = (1ULL << 34);
static constexpr uint64_t SUBSYSTEM_BIT_LEGENDS              = (1ULL << 35);
static constexpr uint64_t SUBSYSTEM_BIT_SITUATIONS           = (1ULL << 36);
static constexpr uint64_t SUBSYSTEM_BIT_GREAT_PROJECTS       = (1ULL << 37);

struct SubsystemBitEntry {
    const char* name;
    uint64_t bit;
};

static const SubsystemBitEntry g_subsystem_bits[] = {
    {"Decisions",              SUBSYSTEM_BIT_DECISIONS},
    {"GrantRevokeTitles",      SUBSYSTEM_BIT_GRANT_REVOKE_TITLES},
    {"SetRealmCapital",        SUBSYSTEM_BIT_SET_REALM_CAPITAL},
    {"Lifestyles",             SUBSYSTEM_BIT_LIFESTYLES},
    {"DynastyLegacies",        SUBSYSTEM_BIT_DYNASTY_LEGACIES},
    {"HousePower",             SUBSYSTEM_BIT_HOUSE_POWER},
    {"CultureTraditions",      SUBSYSTEM_BIT_CULTURE_TRADITIONS},
    {"DeclareWar",             SUBSYSTEM_BIT_DECLARE_WAR},
    {"Faction",                SUBSYSTEM_BIT_FACTION},
    {"Marriages",              SUBSYSTEM_BIT_MARRIAGES},
    {"Council",                SUBSYSTEM_BIT_COUNCIL},
    {"CouncilTasks",           SUBSYSTEM_BIT_COUNCIL_TASKS},
    {"SchemeCountermeasures",  SUBSYSTEM_BIT_SCHEME_COUNTER},
    {"CallAllies",             SUBSYSTEM_BIT_CALL_ALLIES},
    {"Development",            SUBSYSTEM_BIT_DEVELOPMENT},
    {"ReligiousReformation",   SUBSYSTEM_BIT_RELIGIOUS_REFORM},
    {"SuccessionElection",     SUBSYSTEM_BIT_SUCC_ELECTION},
    {"SuccessionAppointment",  SUBSYSTEM_BIT_SUCC_APPOINTMENT},
    {"Goals",                  SUBSYSTEM_BIT_GOALS},
    {"Interactions",           SUBSYSTEM_BIT_INTERACTIONS},
    {"Laws",                   SUBSYSTEM_BIT_LAWS},
    {"PrimaryTitle",           SUBSYSTEM_BIT_PRIMARY_TITLE},
    {"Capital",                SUBSYSTEM_BIT_CAPITAL},
    {"ScriptedGui",            SUBSYSTEM_BIT_SCRIPTED_GUI},
    {"Artifacts",              SUBSYSTEM_BIT_ARTIFACTS},
    {"CourtAmenities",         SUBSYSTEM_BIT_COURT_AMENITIES},
    {"CourtTypes",             SUBSYSTEM_BIT_COURT_TYPES},
    {"CourtPositions",         SUBSYSTEM_BIT_COURT_POSITIONS},
    {"CourtLanguage",          SUBSYSTEM_BIT_COURT_LANGUAGE},
    {"HostActivities",         SUBSYSTEM_BIT_HOST_ACTIVITIES},
    {"AttendOpenActivities",   SUBSYSTEM_BIT_ATTEND_ACTIVITIES},
    {"VassalStance",           SUBSYSTEM_BIT_VASSAL_STANCE},
    {"Accolades",              SUBSYSTEM_BIT_ACCOLADES},
    {"StationMenAtArms",       SUBSYSTEM_BIT_STATION_MAA},
    {"TaxCollector",           SUBSYSTEM_BIT_TAX_COLLECTOR},
    {"Legends",                SUBSYSTEM_BIT_LEGENDS},
    {"Situations",             SUBSYSTEM_BIT_SITUATIONS},
    {"GreatProjects",          SUBSYSTEM_BIT_GREAT_PROJECTS},
};

static constexpr size_t NUM_SUBSYSTEM_BITS =
    sizeof(g_subsystem_bits) / sizeof(g_subsystem_bits[0]);

// =============================================================================
// Per-Character Subsystem Mask
// =============================================================================

static std::unordered_map<uint32_t, uint64_t> g_char_subsystem_mask CK3_EARLY_INIT;
static std::mutex g_mask_mutex;

// =============================================================================
// Public API: set/get character subsystem masks
// =============================================================================

// Declared in action_dispatch_internal.h (defined in dispatch_router.cpp);
// this file only includes bridge.h, so declare locally (same pattern as
// intent_dispatch.cpp's local lookup_character extern).
bool is_sentinel(void* ptr);

bool set_character_subsystems(uint32_t char_id, const char* params_json) {
    // N3 (surface-audit 2026-08-01): the mask map happily bookkept ANY
    // char_id — a bogus character_id returned success:true (silent-accept).
    // Validate existence first: lookup_character returns null (slot out of
    // range) or the DB SENTINEL (id miss) for characters that do not exist.
    // Returning false surfaces success:false at the dispatch layer.
    void* ch = lookup_character(char_id);
    if (!ch || is_sentinel(ch)) {
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] set_character_subsystems: character %u not found — rejected\n",
            char_id);
        return false;
    }

    uint64_t mask = 0xFFFFFFFFFFFFFFFFULL;
    {
        std::lock_guard<std::mutex> lock(g_mask_mutex);
        auto it = g_char_subsystem_mask.find(char_id);
        if (it != g_char_subsystem_mask.end()) {
            mask = it->second;
        }
    }

    bool any_set = false;

    for (size_t i = 0; i < NUM_SUBSYSTEM_BITS; i++) {
        const char* name = g_subsystem_bits[i].name;

        char pattern[128];
        snprintf(pattern, sizeof(pattern), "\"%s\"", name);
        const char* pos = strstr(params_json, pattern);
        if (!pos) continue;

        pos += strlen(pattern);
        while (*pos == ' ' || *pos == ':') pos++;

        if (strncmp(pos, "true", 4) == 0) {
            mask |= g_subsystem_bits[i].bit;
            any_set = true;
        } else if (strncmp(pos, "false", 5) == 0) {
            mask &= ~g_subsystem_bits[i].bit;
            any_set = true;
        }
    }

    if (!any_set) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] set_character_subsystems: no valid subsystem keys found\n");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_mask_mutex);
        g_char_subsystem_mask[char_id] = mask;
    }

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Character %u subsystem mask set to 0x%016llx\n",
            char_id, (unsigned long long)mask);
    return true;
}

bool get_character_subsystems(uint32_t char_id, std::string* response_out) {
    uint64_t mask = 0xFFFFFFFFFFFFFFFFULL;
    {
        std::lock_guard<std::mutex> lock(g_mask_mutex);
        auto it = g_char_subsystem_mask.find(char_id);
        if (it != g_char_subsystem_mask.end()) {
            mask = it->second;
        }
    }

    if (response_out) {
        bool in_map = (mask != 0xFFFFFFFFFFFFFFFFULL || g_char_subsystem_mask.count(char_id));
        std::string json = "{\"success\":true,\"character_id\":";
        json += std::to_string(char_id);
        json += ",\"in_map\":";
        json += in_map ? "true" : "false";
        json += ",\"mask\":";
        json += std::to_string(mask);
        // HONESTY (#125): the subsystem mask is BOOKKEEPING ONLY. The dispatch
        // hooks that once consumed it were removed (see top-of-file comment), so
        // toggling a subsystem OFF does NOT gate any engine behaviour. In
        // particular CallAllies=false does NOT suppress the engine's
        // call-to-arms auto-join — the agent kept relying on this dead lever.
        json += ",\"engine_enforced\":false";
        json += ",\"note\":\"mask is bookkeeping only; toggles do NOT gate engine "
                "behaviour (e.g. CallAllies=false does NOT block ally call-to-arms "
                "auto-join). Use alliance hygiene + query(domain=call_to_arms).\"";
        json += ",\"subsystems\":{";

        bool first = true;
        for (size_t i = 0; i < NUM_SUBSYSTEM_BITS; i++) {
            if (!first) json += ",";
            first = false;
            json += "\"";
            json += g_subsystem_bits[i].name;
            json += "\":";
            json += (mask & g_subsystem_bits[i].bit) ? "true" : "false";
        }

        json += "}}";
        *response_out = json;
    }

    return true;
}

// =============================================================================
// Init / Shutdown — no-ops (dispatch hooks removed)
// =============================================================================

void init_subsystem_hooks() {
    // No-op: dispatch hooks removed (were pure pass-throughs).
}

void shutdown_subsystem_hooks() {
    // No hooks to remove. Clear per-character masks.
    std::lock_guard<std::mutex> lock(g_mask_mutex);
    g_char_subsystem_mask.clear();
}

} // namespace bridge
