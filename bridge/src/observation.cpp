// Observation serialization — reads game state and produces JSON
// for the external agent process.

#include "bridge.h"
#include "ck3_offsets.h"
#include "name_tables.h"
#include "action_dispatch_internal.h"  // resolve_title_realm()

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace bridge {

// Simple JSON string builder (no external dependency for MVP)
class JsonBuilder {
public:
    void begin_object() { buf_ += '{'; }
    void end_object() { trim_comma(); buf_ += "},"; }
    void begin_array() { buf_ += '['; }
    void end_array() { trim_comma(); buf_ += "],"; }

    void key(const char* k) {
        buf_ += '"';
        buf_ += k;
        buf_ += "\":";
    }

    // Append a single character to the JSON buffer, escaping as needed.
    // Uses unsigned char to correctly handle UTF-8 multi-byte sequences
    // (bytes >= 0x80) which must pass through unmodified.
    void append_json_char(unsigned char ch) {
        if (ch == '"') {
            buf_ += "\\\"";
        } else if (ch == '\\') {
            buf_ += "\\\\";
        } else if (ch == '\n') {
            buf_ += "\\n";
        } else if (ch == '\r') {
            buf_ += "\\r";
        } else if (ch == '\t') {
            buf_ += "\\t";
        } else if (ch < 0x20) {
            // Escape other control characters as \u00XX (JSON spec requirement)
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", ch);
            buf_ += esc;
        } else {
            // Bytes >= 0x20 pass through directly.  This includes:
            // - ASCII printable (0x20-0x7E)
            // - UTF-8 lead bytes (0xC0-0xFD) and continuation bytes (0x80-0xBF)
            // Raw UTF-8 bytes are valid in JSON strings per RFC 8259.
            buf_ += static_cast<char>(ch);
        }
    }

    void value_string(const char* v) {
        buf_ += '"';
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(v); *p; ++p) {
            append_json_char(*p);
        }
        buf_ += "\",";
    }

    void value_string(const char* v, size_t len) {
        buf_ += '"';
        const unsigned char* u = reinterpret_cast<const unsigned char*>(v);
        for (size_t i = 0; i < len; ++i) {
            append_json_char(u[i]);
        }
        buf_ += "\",";
    }

    void value_int(int64_t v) {
        buf_ += std::to_string(v);
        buf_ += ',';
    }

    void value_uint(uint64_t v) {
        buf_ += std::to_string(v);
        buf_ += ',';
    }

    void value_float(float v) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%.2f", v);
        buf_ += tmp;
        buf_ += ',';
    }

    void value_bool(bool v) {
        buf_ += v ? "true," : "false,";
    }

    void value_null() {
        buf_ += "null,";
    }

    void value_hex(uintptr_t v) {
        char hex[32];
        snprintf(hex, sizeof(hex), "\"0x%lx\"", v);
        buf_ += hex;
        buf_ += ',';
    }

    // Add a comma separator (for after end_object in arrays)
    void comma() { buf_ += ','; }

    const char* c_str() { trim_comma(); return buf_.c_str(); }
    size_t size() const { return buf_.size(); }
    void clear() { buf_.clear(); }

private:
    void trim_comma() {
        if (!buf_.empty() && buf_.back() == ',') {
            buf_.pop_back();
        }
    }
    std::string buf_;
};

// =============================================================================
// Character Lookup (duplicated from action_dispatch for self-containment)
// =============================================================================

static void* obs_lookup_character(uint32_t char_id) {
    uintptr_t db = read_offset<uintptr_t>(ck3::globals::CHARACTER_DB);
    if (!db) return nullptr;

    uint32_t masked_id = char_id & 0xFFFFFF;
    uint32_t capacity = read<uint32_t>(db + ck3::hash_map::CAPACITY);
    if (masked_id >= capacity) return nullptr;

    uintptr_t table = read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    if (!table) return nullptr;

    uintptr_t entry_ptr = read<uintptr_t>(
        table + ck3::hash_map::ENTRY_HEADER_SIZE
        + static_cast<uintptr_t>(masked_id) * ck3::hash_map::ENTRY_STRIDE);
    if (!entry_ptr) return nullptr;

    uint32_t stored_id = read<uint32_t>(entry_ptr + ck3::character::ID);
    // Masked 24-bit comparison — the bridge outputs masked IDs everywhere, so callers
    // pass masked IDs.  Full 32-bit comparison would always fail for those callers.
    if ((stored_id & 0xFFFFFF) != masked_id) return nullptr;

    return reinterpret_cast<void*>(entry_ptr);
}

static uint32_t char_id(void* character) {
    return read<uint32_t>(reinterpret_cast<uintptr_t>(character) + ck3::character::ID);
}

// =============================================================================
// Name Resolution (Round 5)
// =============================================================================

// Read std::__cxx11::basic_string from game memory. Routed through the canonical
// guard (bridge.h safe_read_game_string) after crash 20260625 — this is the
// war_monitor / title-name reader that runs every tick under heavy churn and is
// a prime corruptor candidate (war_name @1222, cb_name @1233 read from
// transient war/title objects being created/destroyed). The guard rejects bad
// lengths and bounds the copy.
static std::string read_std_string(uintptr_t str_addr) {
    return safe_read_game_string(str_addr);
}

// DB lookup helper: same pattern as title DB, used for house/dynasty DBs
static uintptr_t db_lookup(uintptr_t db_global, uint32_t id) {
    uintptr_t db = read_offset<uintptr_t>(db_global);
    if (!db) return 0;
    uintptr_t table = read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    if (!table) return 0;
    uint32_t capacity = read<uint32_t>(db + ck3::hash_map::CAPACITY);
    uint32_t masked = id & 0xFFFFFF;
    if (masked >= capacity) return 0;
    uintptr_t entry = read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                      + static_cast<uintptr_t>(masked) * ck3::hash_map::ENTRY_STRIDE);
    if (!entry) return 0;
    if ((read<uint32_t>(entry + ck3::hash_map::ENTRY_KEY_REF) & 0xFFFFFF) != masked) return 0;
    return entry;
}

// Resolve character name using the game's localization system.
// CK3 stores localization keys (e.g. "AmE_dE_e") at char+0x20/+0x40,
// NOT display strings. The game's CHAR_FIRST_NAME_RESOLVED function at
// 0x0675b4c0 handles override check AND loc key -> display name resolution
// (e.g. "AmE_dE_e" -> "Amédée").
// Signature: std::string* fn(std::string* output, CCharacter* char_ptr)
using fn_resolve_name = std::string*(*)(std::string*, void*);

static std::string resolve_character_name(uintptr_t char_ptr) {
    if (!char_ptr) return {};

    auto resolve_fn = reinterpret_cast<fn_resolve_name>(
        g_base_addr + ck3::funcs::CHAR_FIRST_NAME_RESOLVED);

    // SIGSEGV protection: character may be freed between lookup and call
    std::string result;
    watchdog_arm(2);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        return {};  // Return empty on crash — caller handles gracefully
    }
    try {
        resolve_fn(&result, reinterpret_cast<void*>(char_ptr));
    } catch (...) {
        result.clear();
    }
    g_in_protected_region = false;
    watchdog_disarm();
    return result;
}

static std::string resolve_house_name(uintptr_t char_ptr) {
    uint32_t house_id = read<uint32_t>(char_ptr + ck3::character::HOUSE_ID_DIRECT);
    if (house_id == 0 || house_id == 0xFFFFFFFF) return {};
    uintptr_t house_obj = db_lookup(ck3::globals::HOUSE_DB, house_id);
    if (!house_obj) return {};
    return read_std_string(house_obj + ck3::db_object::NAME_KEY);
}

// FIX I12 (#dynasty_fallback): derive dynasty_id from the character's house when the
// direct family+0x10 read is unavailable (0/0xFFFFFFFF, as for landed rulers). Pure read.
static uint32_t house_dynasty_fallback(uintptr_t char_ptr) {
    uint32_t house_id = read<uint32_t>(char_ptr + ck3::character::HOUSE_ID_DIRECT);
    if (house_id == 0 || house_id == 0xFFFFFFFF) return 0;
    uintptr_t house_obj = db_lookup(ck3::globals::HOUSE_DB, house_id & 0xFFFFFF);
    if (!house_obj) return 0;
    uint32_t d = read<uint32_t>(house_obj + dynasty_house_obj::DYNASTY_ID);
    if (d == 0 || d == 0xFFFFFFFF) return 0;
    return d & 0xFFFFFF;
}

static std::string resolve_dynasty_name(uintptr_t char_ptr) {
    uint32_t dynasty_id = 0;
    uintptr_t family = read<uintptr_t>(char_ptr + ck3::character::FAMILY_DATA);
    if (family) {
        uint32_t d = read<uint32_t>(family + ck3::family::DYNASTY_ID);
        if (d != 0 && d != 0xFFFFFFFF) dynasty_id = d & 0xFFFFFF;
    }
    if (dynasty_id == 0) dynasty_id = house_dynasty_fallback(char_ptr);
    if (dynasty_id == 0) return {};
    uintptr_t dynasty_obj = db_lookup(ck3::globals::DYNASTY_DB, dynasty_id);
    if (!dynasty_obj) return {};
    return read_std_string(dynasty_obj + ck3::db_object::NAME_KEY);
}

// Round 6: culture and faith name resolution
static std::string resolve_culture_name(uintptr_t char_ptr) {
    uint32_t culture_id = read<uint32_t>(char_ptr + ck3::character::CULTURE_ID_DIRECT);
    if (culture_id == 0 || culture_id == 0xFFFFFFFF) return {};
    uintptr_t culture_obj = db_lookup(ck3::globals::CULTURE_INDEXED_DB, culture_id);
    if (!culture_obj) return {};
    return read_std_string(culture_obj + ck3::db_object::NAME_KEY);
}

static std::string resolve_faith_name(uintptr_t char_ptr) {
    uint32_t faith_id = read<uint32_t>(char_ptr + ck3::character::FAITH_ID_DIRECT);
    if (faith_id == 0 || faith_id == 0xFFFFFFFF) return {};
    // #faith RE FIX (2026-06-19): FAITH_INDEXED_DB is not a hash-map (cap=0) + NAME_KEY
    // (0x10) is garbage for CFaith — faith name was empty for everyone. Use FAITH_DB
    // (0x0bedad78) + faith::FAITH_KEY (0x98).
    uintptr_t faith_obj = db_lookup(ck3::globals::FAITH_DB, faith_id);
    if (!faith_obj) return {};
    return read_std_string(faith_obj + faith::FAITH_KEY);
}

static void emit_name_fields(JsonBuilder& json, uintptr_t char_ptr) {
    std::string name = resolve_character_name(char_ptr);
    if (!name.empty()) {
        json.key("name"); json.value_string(name.c_str(), name.size());
    }
    std::string house = resolve_house_name(char_ptr);
    if (!house.empty()) {
        json.key("house_name"); json.value_string(house.c_str(), house.size());
    }
    std::string dynasty = resolve_dynasty_name(char_ptr);
    if (!dynasty.empty()) {
        json.key("dynasty_name"); json.value_string(dynasty.c_str(), dynasty.size());
    }
    // Round 6: culture and faith names
    std::string culture = resolve_culture_name(char_ptr);
    if (!culture.empty()) {
        json.key("culture"); json.value_string(culture.c_str(), culture.size());
    }
    std::string faith = resolve_faith_name(char_ptr);
    if (!faith.empty()) {
        json.key("faith"); json.value_string(faith.c_str(), faith.size());
    }
}

static bool char_alive(void* character) {
    return read<uint64_t>(
        reinterpret_cast<uintptr_t>(character) + ck3::character::DEATH_DATE) == 0;
}

static void* char_realm(void* character) {
    return read<void*>(
        reinterpret_cast<uintptr_t>(character) + ck3::character::REALM_DATA);
}

// Get liege character ID from a character pointer.
// Three resolution paths depending on character type:
//   1. Rulers: direct liege CCharacter* at title_realm+0x448
//   2. Rulers (fallback): realm parent at title_realm+0x1C0 → +0x28 (mirrors FUN_067539c0)
//   3. Courtiers: packed char ref at courtier_data+0xC8, resolved via character DB
// Returns 0 if independent, unlanded, or liege cannot be resolved.
static uint32_t get_liege_id(void* character) {
    if (!character) return 0;
    uintptr_t c = reinterpret_cast<uintptr_t>(character);

    uintptr_t title_realm = read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    if (title_realm && title_realm > 0x10000 && title_realm < 0x7FFFFFFFFFFF) {

        uint32_t self_id = read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF;

        // PATH 1: Direct liege pointer at title_realm+0x448.
        // FIX (independence-resolver): +0x448 is unreliable in this build — for independents
        // it is a non-null mapped sentinel whose id reads 0xFFFFFF. Reject {0, 0xFFFFFF, self}
        // and fall through to the authoritative PATH 2. (Matches dispatch_router.cpp.)
        uintptr_t liege_ptr = read<uintptr_t>(title_realm + ck3::title_realm::LIEGE_PTR);
        if (liege_ptr && liege_ptr > 0x10000 && liege_ptr < 0x7FFFFFFFFFFF) {
            uint32_t lid = read<uint32_t>(liege_ptr + ck3::character::ID);
            uint32_t masked = lid & 0xFFFFFF;
            if (masked > 0 && masked != 0xFFFFFF && masked != self_id) {
                return masked;
            }
        }

        // PATH 2: Realm parent at title_realm+0x1C0 → +0x28 (mirroring FUN_067539c0).
        // AUTHORITATIVE liege signal; 0xFFFFFF is the engine no-liege sentinel.
        uintptr_t realm_parent = read<uintptr_t>(title_realm + ck3::title_realm::REALM_PARENT_OBJ);
        if (realm_parent && realm_parent > 0x10000 && realm_parent < 0x7FFFFFFFFFFF) {
            uintptr_t holder_ptr = read<uintptr_t>(realm_parent + ck3::realm_parent::HOLDER_CHAR_PTR);
            if (holder_ptr && holder_ptr > 0x10000 && holder_ptr < 0x7FFFFFFFFFFF) {
                uint32_t holder_id = read<uint32_t>(holder_ptr + ck3::character::ID) & 0xFFFFFF;
                if (holder_id != self_id && holder_id != 0 && holder_id != 0xFFFFFF) {
                    return holder_id;
                }
            }
        }

        // Ruler with no liege from either path = independent.
        return 0;
    }

    // PATH 3: Courtier path — liege stored as packed char ref at courtier_data+0xC8
    uintptr_t courtier_data = read<uintptr_t>(c + ck3::character::COURTIER_DATA);
    if (courtier_data && courtier_data > 0x10000 && courtier_data < 0x7FFFFFFFFFFF) {
        uint32_t liege_ref = read<uint32_t>(courtier_data + ck3::courtier_data::LIEGE_CHAR_REF);
        if (liege_ref == 0 || liege_ref == 0xFFFFFFFF) return 0;
        uint32_t masked = liege_ref & 0xFFFFFF;
        if (masked == 0 || masked == 0xFFFFFF) return 0;

        // Validate the packed ref resolves to a real character
        void* liege_char = obs_lookup_character(liege_ref);
        if (liege_char) {
            return masked;
        }
    }

    return 0;
}


// =============================================================================
// Personality Accessors
// =============================================================================

using fn_get_personality = int(*)(void*);

struct PersonalityAxis {
    const char* name;
    uintptr_t func_offset;
};

static const PersonalityAxis g_personality_axes[] = {
    {"honor",        ck3::funcs::GET_AI_HONOR},
    {"greed",        ck3::funcs::GET_AI_GREED},
    {"rationality",  ck3::funcs::GET_AI_RATIONALITY},
    {"energy",       ck3::funcs::GET_AI_ENERGY},
    {"boldness",     ck3::funcs::GET_AI_BOLDNESS},
    {"zeal",         ck3::funcs::GET_AI_ZEAL},
    {"vengefulness", ck3::funcs::GET_AI_VENGEFULNESS},
    {"compassion",   ck3::funcs::GET_AI_COMPASSION},
    {"sociability",  ck3::funcs::GET_AI_SOCIABILITY},
};

static void build_personality(JsonBuilder& json, void* character) {
    json.key("personality");
    json.begin_object();
    // SIGSEGV protection: personality getters call engine functions on the character
    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        // Fill remaining axes with 0 on crash
        json.end_object();
        return;
    }
    try {
        for (const auto& axis : g_personality_axes) {
            auto getter = reinterpret_cast<fn_get_personality>(g_base_addr + axis.func_offset);
            int value = getter(character);
            json.key(axis.name);
            json.value_int(value);
        }
    } catch (...) {
        // Partial personality data is still useful
    }
    g_in_protected_region = false;
    watchdog_disarm();
    json.end_object();
}

// =============================================================================
// Ruler Stats
// =============================================================================

// Fixed-point monetary conversion: int64 / 100000.0 → float
static float fp_to_float(int64_t fp_val) {
    return static_cast<float>(static_cast<double>(fp_val) / 100000.0);
}

static void build_basic_stats(JsonBuilder& json, void* ruler) {
    uintptr_t r = reinterpret_cast<uintptr_t>(ruler);

    // Gold, prestige, piety are via realm_data sub-pointer (CONFIRMED Round 2)
    // Access: *(*(char+0x1a8) + offset), int64 fixed-point / 100000
    uintptr_t realm_ptr = read<uintptr_t>(r + ck3::character::REALM_DATA);
    if (realm_ptr) {
        json.key("gold");     json.value_float(fp_to_float(read<int64_t>(realm_ptr + ck3::character::REALM_GOLD)));
        json.key("piety");    json.value_float(fp_to_float(read<int64_t>(realm_ptr + ck3::character::REALM_PIETY)));
        json.key("prestige"); json.value_float(fp_to_float(read<int64_t>(realm_ptr + ck3::character::REALM_PRESTIGE)));

        // Round 7: Stress (core_data + 0x2F8, uint32)
        int32_t stress = read<int32_t>(realm_ptr + ck3::character_vitals::STRESS);
        json.key("stress"); json.value_int(stress);
        // Economy: monthly income display value (core_data+0x170), not accumulator (0x2E8)
        int64_t monthly_income_raw = read<int64_t>(realm_ptr + ck3::core_data::INCOME_DISPLAY);
        json.key("monthly_income"); json.value_float(static_cast<float>(static_cast<double>(monthly_income_raw) / 100000.0));

        // Income breakdown (core_data+0x2B0..0x2D0, all int64 fixed-point / 100000)
        json.key("income_breakdown");
        json.begin_object();
        json.key("total_monthly");     json.value_float(fp_to_float(read<int64_t>(realm_ptr + ck3::core_data::MONTHLY_INCOME)));
        json.key("gross_domain");      json.value_float(fp_to_float(read<int64_t>(realm_ptr + ck3::core_data::GROSS_DOMAIN_INCOME)));
        json.key("net_domain");        json.value_float(fp_to_float(read<int64_t>(realm_ptr + ck3::core_data::NET_DOMAIN_INCOME)));
        json.key("total_expenses");    json.value_float(fp_to_float(read<int64_t>(realm_ptr + ck3::core_data::TOTAL_EXPENSES)));
        json.key("net_gold");          json.value_float(fp_to_float(read<int64_t>(realm_ptr + ck3::core_data::NET_GOLD_INCOME)));
        json.end_object();

        int64_t piety_cap_raw = read<int64_t>(realm_ptr + ck3::core_data::PIETY_CAP);
        json.key("piety_cap"); json.value_float(static_cast<float>(static_cast<double>(piety_cap_raw) / 100000.0));

        int64_t prestige_cap_raw = read<int64_t>(realm_ptr + ck3::core_data::PRESTIGE_CAP);
        json.key("prestige_cap"); json.value_float(static_cast<float>(static_cast<double>(prestige_cap_raw) / 100000.0));
    } else {
        json.key("gold");     json.value_float(0.0f);
        json.key("piety");    json.value_float(0.0f);
        json.key("prestige"); json.value_float(0.0f);
        json.key("stress");   json.value_int(0);
        json.key("monthly_income"); json.value_float(0.0f);
        json.key("piety_cap");      json.value_float(0.0f);
        json.key("prestige_cap");   json.value_float(0.0f);
    }

    // Round 7: Dread (title_realm + 0x350, int64 fixed-point, rulers only)
    uintptr_t title_realm_ptr = resolve_title_realm(r);
    if (title_realm_ptr) {
        int64_t dread_raw = read<int64_t>(title_realm_ptr + ck3::character_vitals::DREAD);
        float dread = static_cast<float>(static_cast<double>(dread_raw) / 100000.0);
        json.key("dread"); json.value_float(dread);
        // Barter goods (title_realm + 0x490, int64 fixed-point)
        int64_t barter_raw = read<int64_t>(title_realm_ptr + ck3::title_realm::BARTER_GOODS);
        json.key("barter_goods"); json.value_float(static_cast<float>(static_cast<double>(barter_raw) / 100000.0));
    } else {
        json.key("dread"); json.value_float(0.0f);
        json.key("barter_goods"); json.value_float(0.0f);
    }

    // Legitimacy (diplomatic sub-struct + 0x28, int64 fixed-point)
    uintptr_t diplo_ptr = read<uintptr_t>(r + ck3::character::DIPLOMATIC_DATA);
    if (diplo_ptr) {
        int64_t legit_raw = read<int64_t>(diplo_ptr + ck3::diplomatic_data::LEGITIMACY);
        json.key("legitimacy"); json.value_float(static_cast<float>(static_cast<double>(legit_raw) / 100000.0));
    } else {
        json.key("legitimacy"); json.value_float(0.0f);
    }

    // Age: call GET_AGE(char*) -> int16 (years)
    auto fn_age = reinterpret_cast<int16_t(*)(void*)>(g_base_addr + ck3::funcs::GET_AGE);
    int16_t age = fn_age(ruler);
    json.key("age"); json.value_int(age);

    // Birth date index (raw int16 at char+0x6C)
    int16_t birth_date = read<int16_t>(r + ck3::character::BIRTH_DATE_SHORT);
    json.key("birth_date"); json.value_int(birth_date);

    // Imprisonment status.
    // L7 fix: ck3::funcs::IS_IMPRISONED (0x0664adf0) is NOT an imprisonment
    // check — it is maybe_realm_predicate_inverted (independence/liege-
    // membership predicate, true for ~25% of landed chars / all independent
    // rulers). Mirror build_char_profile: default false until the real
    // ImprisonerLink / CIsImprisonedTrigger field is pinned. Never emit true.
    {
        json.key("is_imprisoned"); json.value_bool(false);
    }
}

static void build_skills(JsonBuilder& json, void* ruler) {
    uintptr_t r = reinterpret_cast<uintptr_t>(ruler);
    json.key("skills");
    json.begin_object();
    // CONFIRMED Round 2: skills are int32 at 0xBC, stride 4 (NOT int16 at 0xC0)
    json.key("diplomacy");    json.value_int(read<int32_t>(r + ck3::character::SKILL_DIPLOMACY));
    json.key("martial");      json.value_int(read<int32_t>(r + ck3::character::SKILL_MARTIAL));
    json.key("stewardship");  json.value_int(read<int32_t>(r + ck3::character::SKILL_STEWARDSHIP));
    json.key("intrigue");     json.value_int(read<int32_t>(r + ck3::character::SKILL_INTRIGUE));
    json.key("learning");     json.value_int(read<int32_t>(r + ck3::character::SKILL_LEARNING));
    json.key("prowess");      json.value_int(read<int32_t>(r + ck3::character::SKILL_PROWESS));
    json.end_object();
}

// =============================================================================
// Traits (Round 3: sorted trait ID array on CCharacter, resolved via trait DB)
// =============================================================================

// trait_category_name() defined in dispatch_router.cpp, declared in action_dispatch_internal.h

static void build_traits(JsonBuilder& json, void* ruler) {
    // CONFIRMED Round 3: traits stored as sorted uint32[] at char+0xF0, count at char+0xFC
    // Trait DB singleton at TRAIT_DB resolves trait_id -> CTrait*
    uintptr_t r = reinterpret_cast<uintptr_t>(ruler);
    uintptr_t trait_array_ptr = read<uintptr_t>(r + ck3::character::TRAIT_ARRAY);
    int32_t trait_count = read<int32_t>(r + ck3::character::TRAIT_COUNT);

    // Get trait database singleton for resolving trait IDs to CTrait* entries
    uintptr_t tdb = read_offset<uintptr_t>(ck3::globals::TRAIT_DB);

    json.key("traits");
    json.begin_array();

    // Function pointer for trait descriptor resolution (initialized once, outside loop)
    using fn_get_desc = uintptr_t(*)(uintptr_t);
    auto get_desc = reinterpret_cast<fn_get_desc>(g_base_addr + ck3::funcs::TRAIT_GET_DESCRIPTOR);

    if (trait_array_ptr && trait_count > 0 && trait_count < 200 && tdb) {
        int32_t max_trait_id = read<int32_t>(tdb + ck3::trait_db::ARRAY_COUNT);
        uintptr_t trait_table = read<uintptr_t>(tdb + ck3::trait_db::ARRAY_PTR);

        for (int32_t i = 0; i < trait_count; i++) {
            uint32_t trait_id = read<uint32_t>(trait_array_ptr + i * 4);

            json.begin_object();
            json.key("id"); json.value_uint(trait_id);

            // Resolve trait_id to CTrait* via indexed array lookup
            if (trait_table && trait_id < static_cast<uint32_t>(max_trait_id)) {
                uintptr_t trait_entry = read<uintptr_t>(trait_table + trait_id * 8);
                if (trait_entry) {
                    uint8_t category = read<uint8_t>(trait_entry + ck3::trait_entry::CATEGORY);
                    uint32_t flags = read<uint32_t>(trait_entry + ck3::trait_entry::FLAGS);
                    uint32_t name_hash = read<uint32_t>(trait_entry + ck3::trait_entry::NAME_HASH);

                    json.key("category"); json.value_string(trait_category_name(category));
                    json.key("flags"); json.value_uint(flags);
                    json.key("name_hash"); json.value_hex(name_hash);

                    // Resolve trait name via descriptor function
                    uintptr_t desc = get_desc(trait_entry);
                    if (desc) {
                        const char* name = reinterpret_cast<const char*>(read<uintptr_t>(desc + ck3::trait_descriptor::NAME_PTR));
                        size_t len = read<size_t>(desc + ck3::trait_descriptor::NAME_LENGTH);
                        if (name && len > 0 && len < 256) {
                            json.key("name"); json.value_string(name, len);
                        }
                    }
                } else {
                    json.key("category"); json.value_string("unknown");
                }
            } else {
                json.key("category"); json.value_string("unknown");
            }

            json.end_object();
                }
    }

    json.end_array();

    // Emit a flat string array of trait names for easy consumption by the agent
    json.key("trait_names");
    json.begin_array();
    if (trait_array_ptr && trait_count > 0 && trait_count < 200 && tdb) {
        int32_t max_trait_id2 = read<int32_t>(tdb + ck3::trait_db::ARRAY_COUNT);
        uintptr_t trait_table2 = read<uintptr_t>(tdb + ck3::trait_db::ARRAY_PTR);
        for (int32_t i = 0; i < trait_count; i++) {
            uint32_t trait_id = read<uint32_t>(trait_array_ptr + i * 4);
            if (trait_table2 && trait_id < static_cast<uint32_t>(max_trait_id2)) {
                uintptr_t trait_entry = read<uintptr_t>(trait_table2 + trait_id * 8);
                if (trait_entry) {
                    uintptr_t desc = get_desc(trait_entry);
                    if (desc) {
                        const char* name = reinterpret_cast<const char*>(read<uintptr_t>(desc + ck3::trait_descriptor::NAME_PTR));
                        size_t len = read<size_t>(desc + ck3::trait_descriptor::NAME_LENGTH);
                        if (name && len > 0 && len < 256) {
                            json.value_string(name, len);
                        }
                    }
                }
            }
        }
    }
    json.end_array();
}

// =============================================================================
// Family (Round 3: parents, dynasty, spouse, children, heirs)
// =============================================================================

static void build_family(JsonBuilder& json, void* ruler) {
    uintptr_t r = reinterpret_cast<uintptr_t>(ruler);

    json.key("family");
    json.begin_object();

    // Family sub-struct at char+0x1a0 (CONFIRMED R3, HIGH confidence)
    uintptr_t family_ptr = read<uintptr_t>(r + ck3::character::FAMILY_DATA);
    if (family_ptr) {
        uint32_t father = read<uint32_t>(family_ptr + ck3::family::FATHER_ID);
        uint32_t mother = read<uint32_t>(family_ptr + ck3::family::MOTHER_ID);
        uint32_t real_father = read<uint32_t>(family_ptr + ck3::family::REAL_FATHER_ID);
        uint32_t dynasty = read<uint32_t>(family_ptr + ck3::family::DYNASTY_ID);
        uint32_t house = read<uint32_t>(family_ptr + ck3::family::HOUSE_ID);

        json.key("father_id");
        if (father != 0xFFFFFFFF) json.value_uint(father & 0xFFFFFF); else json.value_null();
        json.key("mother_id");
        if (mother != 0xFFFFFFFF) json.value_uint(mother & 0xFFFFFF); else json.value_null();
        json.key("real_father_id");
        if (real_father != 0xFFFFFFFF) json.value_uint(real_father & 0xFFFFFF); else json.value_null();
        // FIX I12 (#dynasty_fallback): family+0x10 is 0/0xFFFFFFFF for landed rulers;
        // derive dynasty_id from the house (char+0x150 -> HOUSE_DB -> +0x24).
        if (dynasty == 0 || dynasty == 0xFFFFFFFF) {
            uint32_t fb = house_dynasty_fallback(r);
            if (fb != 0) dynasty = fb;
        }
        json.key("dynasty_id");
        if (dynasty != 0 && dynasty != 0xFFFFFFFF) json.value_uint(dynasty & 0xFFFFFF); else json.value_null();
        json.key("house_id");
        if (house != 0xFFFFFFFF) json.value_uint(house); else json.value_null();
    } else {
        json.key("father_id"); json.value_null();
        json.key("mother_id"); json.value_null();
        json.key("real_father_id"); json.value_null();
        json.key("dynasty_id"); json.value_null();
        json.key("house_id"); json.value_null();
    }

    // Spouses via family sub-struct + SPOUSE_ARRAY (canonical PDX array path)
    // CK3 supports multiple spouses/concubines — iterate entire array
    {
        uint32_t first_alive_spouse = 0;
        json.key("spouses");
        json.begin_array();
        if (family_ptr) {
            uintptr_t spouse_arr = family_ptr + ck3::family::SPOUSE_ARRAY;
            int32_t spouse_count = read<int32_t>(spouse_arr + pdx_array::COUNT);
            uintptr_t spouse_data = read<uintptr_t>(spouse_arr + pdx_array::DATA_PTR);
            if (spouse_data && spouse_count > 0 && spouse_count <= 10) {
                for (int i = 0; i < spouse_count; i++) {
                    uint32_t spouse_ref = read<uint32_t>(spouse_data + i * sizeof(uint32_t));
                    uint32_t spouse_id = spouse_ref & 0xFFFFFF;
                    if (spouse_id == 0 || spouse_id == 0xFFFFFF) continue;

                    // Check if spouse is still alive (dead spouses persist in the array)
                    void* spouse_char = obs_lookup_character(spouse_id);
                    uintptr_t sc = reinterpret_cast<uintptr_t>(spouse_char);
                    bool alive = spouse_char && read<uint64_t>(sc + ck3::character::DEATH_DATE) == 0;

                    json.begin_object();
                    json.key("id"); json.value_uint(spouse_id);
                    json.key("alive"); json.value_bool(alive);
                    json.end_object();

                    if (alive && first_alive_spouse == 0) {
                        first_alive_spouse = spouse_id;
                    }
                }
            }
        }
        json.end_array();

        // Backward compatibility: first alive spouse (or null)
        json.key("spouse_id");
        if (first_alive_spouse != 0) {
            json.value_uint(first_alive_spouse);
        } else {
            json.value_null();
        }
    }

    // Children via family sub-struct + CHILDREN_ARRAY (canonical PDX array path)
    json.key("children");
    json.begin_array();
    if (family_ptr) {
        uintptr_t child_arr = family_ptr + ck3::family::CHILDREN_ARRAY;
        int32_t child_count = read<int32_t>(child_arr + pdx_array::COUNT);
        uintptr_t child_data = read<uintptr_t>(child_arr + pdx_array::DATA_PTR);
        if (child_count > 0 && child_data && child_count < 50) {
            for (int i = 0; i < child_count; i++) {
                uint32_t cid = read<uint32_t>(child_data + i * sizeof(uint32_t)) & 0xFFFFFF;
                if (cid != 0 && cid != 0xFFFFFF) {
                    json.value_uint(cid);
                }
            }
        }
    }
    json.end_array();

    // Succession: title IDs this character is heir TO (not heir character IDs)
    void* realm = char_realm(ruler);
    if (realm) {
        uintptr_t rd = reinterpret_cast<uintptr_t>(realm);
        uintptr_t heir_ptr = read<uintptr_t>(rd + ck3::character::REALM_HEIR_LIST);
        int32_t heir_count = read<int32_t>(rd + ck3::character::REALM_HEIR_COUNT);
        json.key("heir_to_titles");
        json.begin_array();
        if (heir_ptr && heir_count > 0 && heir_count < 50) {
            for (int32_t i = 0; i < heir_count; i++) {
                uint32_t title_id = read<uint32_t>(heir_ptr + i * 4);
                json.value_uint(title_id & 0xFFFFFF);
            }
        }
        json.end_array();
    } else {
        json.key("heir_to_titles"); json.begin_array(); json.end_array();
    }

    json.end_object();
}

// Forward declarations for title DB functions (defined below)
static uintptr_t title_db_lookup(uint32_t title_id);
static int32_t title_get_tier(uintptr_t title_ptr);

// =============================================================================
// Claims (R25 FIX: CPdxArray<CClaim, int> at core_data+0x1E8, NOT char+0x170)
// Each CClaim = 0x10 bytes: +0x00=vtable(8B), +0x08=title_id(u32), +0x0C=pressed(byte), +0x0D=implicit(byte)
// =============================================================================

static void build_claims(JsonBuilder& json, void* ruler) {
    uintptr_t r = reinterpret_cast<uintptr_t>(ruler);

    // R25 FIX: Claims are at core_data+0x1E8, not char+0x170
    uintptr_t core_data = read<uintptr_t>(r + ck3::character::REALM_DATA);  // char+0x1A8
    uintptr_t claim_array = 0;
    int32_t claim_count = 0;
    if (core_data) {
        uintptr_t claims_pdx = core_data + ck3::core_data::CLAIMS_ARRAY;  // core_data+0x1E8
        claim_array = read<uintptr_t>(claims_pdx + 0x00);  // data pointer
        claim_count = read<int32_t>(claims_pdx + 0x0C);     // count
    }

    // Sanity cap
    if (claim_count < 0 || claim_count > 500) claim_count = 0;

    json.key("claims");
    json.begin_array();

    if (claim_array && claim_count > 0) {
        for (int32_t i = 0; i < claim_count; i++) {
            uintptr_t entry = claim_array + i * ck3::claim_entry::ENTRY_SIZE;  // 0x10 stride

            // R25 FIX: +0x00 is vtable, title_id is at +0x08 (uint32)
            uint32_t title_id = read<uint32_t>(entry + ck3::claim_entry::TITLE_ID);
            uint8_t pressed = read<uint8_t>(entry + ck3::claim_entry::PRESSED);
            uint8_t implicit_flag = read<uint8_t>(entry + ck3::claim_entry::IMPLICIT);

            json.begin_object();
            json.key("title_id"); json.value_uint(title_id);

            // Resolve title name via landed title database
            uintptr_t tobj = title_db_lookup(title_id);
            if (tobj) {
                auto display_name = read_std_string(tobj + ck3::title_obj::TITLE_DISPLAY_NAME);
                if (!display_name.empty()) {
                    json.key("title_name"); json.value_string(display_name.c_str(), display_name.size());
                }
                int32_t tier = title_get_tier(tobj);
                if (tier > 0) {
                    json.key("tier"); json.value_int(tier);
                }
            }

            json.key("pressed"); json.value_bool(pressed != 0);
            json.key("implicit"); json.value_bool(implicit_flag != 0);

            // Claim type: implicit(0xD)!=0 → implicit, pressed(0xC)!=0 → pressed, both 0 → unpressed
            if (implicit_flag) {
                json.key("type"); json.value_string("implicit");
            } else {
                json.key("type"); json.value_string(pressed ? "pressed" : "unpressed");
            }

            json.end_object();
        }
    }

    json.end_array();
}

// =============================================================================
// Relationships
// =============================================================================

static void build_relationships(JsonBuilder& json, void* ruler) {
    // Liege resolution uses two paths:
    //   Rulers: CCharacter* at ruler_data(+0x1B8)+0x448
    //   Courtiers: packed char ref at courtier_data(+0x1B0)+0xC8, resolved via char DB
    uint32_t liege = get_liege_id(ruler);
    if (liege > 0) {
        json.key("liege_id"); json.value_uint(liege);
    } else {
        json.key("liege_id"); json.value_int(0);  // 0 = independent/unlanded
    }
}

// =============================================================================
// Title DB Lookup
// =============================================================================

// Resolve title_id → CLandedTitle* via the landed title database
static uintptr_t title_db_lookup(uint32_t title_id) {
    uintptr_t db = read_offset<uintptr_t>(ck3::globals::LANDED_TITLE_DB);
    if (!db) return 0;
    uintptr_t arr = read<uintptr_t>(db + ck3::title_db::ARRAY_PTR);
    if (!arr) return 0;
    uintptr_t elem = arr + ck3::title_db::ELEMENT_PTR_OFFSET
                   + static_cast<uintptr_t>(title_id & 0xFFFFFF) * ck3::title_db::ELEMENT_STRIDE;
    uintptr_t title_ptr = read<uintptr_t>(elem);
    if (!title_ptr) return 0;
    if ((read<uint32_t>(title_ptr + ck3::title_obj::TITLE_ID) & 0xFFFFFF) != (title_id & 0xFFFFFF)) return 0;
    return title_ptr;
}

// Get tier for a resolved title (1=barony,2=county,3=duchy,4=kingdom,5=empire)
static int32_t title_get_tier(uintptr_t title_ptr) {
    if (!title_ptr) return 0;
    uintptr_t type_ptr = read<uintptr_t>(title_ptr + ck3::title_obj::TITLE_TYPE_PTR);
    if (!type_ptr) return 0;
    return read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
}

// =============================================================================
// Domain & Realm
// =============================================================================

static void build_realm_data(JsonBuilder& json, void* ruler) {
    uintptr_t r = reinterpret_cast<uintptr_t>(ruler);
    void* realm = char_realm(ruler);
    // Engine's canonical IsRuler: char+0x1B8 (TITLE_REALM_DATA) != 0
    // (core_data::IS_RULER at 0x54C is unreliable — Ghidra found no accesses)
    uintptr_t raw_tr = read<uintptr_t>(r + ck3::character::TITLE_REALM_DATA);
    bool is_ruler = (raw_tr != 0);
    json.key("is_ruler"); json.value_bool(is_ruler);

    if (!is_ruler) {
        json.key("domain_size");        json.value_int(0);
        json.key("domain_limit");       json.value_int(0);
        json.key("realm_size");         json.value_int(0);
        json.key("vassal_count");       json.value_int(0);
        json.key("vassal_limit");       json.value_int(0);
        json.key("military_current");      json.value_int(0);
        json.key("military_max");          json.value_int(0);
        json.key("levy_own_domain_max");   json.value_int(0);
        json.key("military_without_hires"); json.value_int(0);
        json.key("military_hireable");     json.value_int(0);
        json.key("is_at_war");          json.value_bool(false);
        json.key("realm_tier");         json.value_int(0);
        return;
    }

    // Title realm sub-struct — use resolve_title_realm() with fallback probing
    // (raw char+0x1b8 can be null for some characters whose data shifted to +0x1c0)
    uintptr_t tr = resolve_title_realm(r);

    // Domain size: count held titles with tier <= 2 (counties/baronies).
    // Held title IDs at title_realm+0x1E0, count at +0x1EC.
    // NOTE: labeled VASSAL_LIST/VASSAL_COUNT but per Round 4 these are held title IDs.
    int32_t domain_size = 0;
    int32_t county_count = 0;
    // Realm size: use title_realm+0x1EC (TITLE_ARRAY_COUNT) which is the held title count.
    // TITLE_COUNT at 0x74 returns 0 for AI characters so we use TITLE_ARRAY_COUNT instead.
    int32_t realm_size = 0;
    if (tr) {
        int32_t engine_realm_size = read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
        if (engine_realm_size > 0 && engine_realm_size < 50000) {
            realm_size = engine_realm_size;
        }

        // Iterate held title array for domain_size (directly held baronies/counties)
        uintptr_t title_ids = read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
        int32_t title_count = read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);

        // Debug: log title array state for first call
        static bool logged_title_debug = false;
        if (!logged_title_debug && tr) {
            logged_title_debug = true;
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] realm_size debug: tr=%p, engine_realm_size=%d, "
                    "title_arr_ptr=%p, title_arr_count=%d\n",
                    (void*)tr, engine_realm_size, (void*)title_ids, title_count);
            // Log first few title IDs and their tier resolution
            if (title_ids && title_count > 0) {
                int log_count = title_count < 8 ? title_count : 8;
                for (int di = 0; di < log_count; di++) {
                    uint32_t dtid = read<uint32_t>(title_ids + di * 4);
                    uintptr_t dtptr = title_db_lookup(dtid);
                    int32_t dtier = title_get_tier(dtptr);
                    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge]   title[%d]: id=0x%08x (masked=0x%06x), "
                            "obj=%p, tier=%d\n",
                            di, dtid, dtid & 0xFFFFFF, (void*)dtptr, dtier);
                }
            }
        }

        if (title_ids && title_count > 0 && title_count < 5000) {
            for (int32_t i = 0; i < title_count && i < 500; i++) {
                uint32_t tid = read<uint32_t>(title_ids + i * 4);
                uintptr_t tptr = title_db_lookup(tid);
                int32_t tier = title_get_tier(tptr);
                if (tier == 2) {
                    county_count++;
                }
                if (tier == 2) {
                    domain_size++;  // domain = counties only (tier 2)
                }
            }
        }
    }
    json.key("domain_size"); json.value_int(domain_size);

    // Domain limit at title_realm+0x3D0 (clamp min 1)
    int32_t domain_limit = 1;
    if (tr) {
        int32_t raw_limit = read<int32_t>(tr + 0x3D0);
        if (raw_limit > 0) domain_limit = raw_limit;
    }
    json.key("domain_limit"); json.value_int(domain_limit);

    json.key("realm_size"); json.value_int(realm_size);

    // Vassal count: use VASSAL_COUNT_CACHED at +0x3D8 (excludes barons, matches game UI)
    // The raw DIRECT_VASSAL_ARRAY count at +0xB4 includes barons — not what the player sees
    if (tr) {
        int32_t vassal_count = read<int32_t>(tr + ck3::title_realm::VASSAL_COUNT_CACHED);
        if (vassal_count < 0 || vassal_count > 5000) vassal_count = 0; // sanity clamp
        json.key("vassal_count"); json.value_int(vassal_count);
    } else {
        json.key("vassal_count"); json.value_int(0);
    }

    // Vassal limit: cached at title_realm+0x3D4
    if (tr) {
        int32_t vassal_limit = read<int32_t>(tr + ck3::title_realm::VASSAL_LIMIT_CACHED);
        if (vassal_limit < 0 || vassal_limit > 1000) vassal_limit = 0; // sanity clamp
        json.key("vassal_limit"); json.value_int(vassal_limit);
    } else {
        json.key("vassal_limit"); json.value_int(0);
    }

    // CONFIRMED Round 2+3: military via title_realm sub-pointer at char+0x1b8
    if (tr) {
        json.key("military_current");      json.value_int(read<int32_t>(tr + ck3::title_realm::MIL_CURRENT));
        json.key("military_max");          json.value_int(read<int32_t>(tr + ck3::title_realm::MIL_MAX));
        json.key("levy_own_domain_max");   json.value_int(read<int32_t>(tr + ck3::title_realm::MIL_OWN_DOMAIN_MAX));
        json.key("military_without_hires"); json.value_int(read<int32_t>(tr + ck3::title_realm::MIL_WITHOUT_HIRES));
        json.key("military_hireable");     json.value_int(read<int32_t>(tr + ck3::title_realm::MIL_HIREABLE));
    } else {
        json.key("military_current");      json.value_int(0);
        json.key("military_max");          json.value_int(0);
        json.key("levy_own_domain_max");   json.value_int(0);
        json.key("military_without_hires"); json.value_int(0);
        json.key("military_hireable");     json.value_int(0);
    }

    // War count via title_realm WAR_REF_ARRAY (not deprecated diplomatic_data path)
    int32_t war_count = 0;
    if (tr) {
        war_count = read<int32_t>(tr + ck3::title_realm::WAR_REF_ARRAY + 0x0C);
        if (war_count < 0 || war_count > 1000) war_count = 0; // sanity clamp
    }
    json.key("is_at_war"); json.value_bool(war_count > 0);

    // Round 3: realm tier at title_realm+0x1D4 (CONFIRMED, 3 accessors)
    if (tr) {
        json.key("realm_tier"); json.value_int(read<int32_t>(tr + ck3::title_realm::REALM_TIER));
    } else {
        json.key("realm_tier"); json.value_int(0);
    }

    // Primary title = first element of held title array (sorted highest tier first)
    // Pattern copied from action_dispatch.cpp query_realm
    if (tr) {
        int32_t title_arr_count = read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
        uintptr_t title_arr_ptr = read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
        if (title_arr_ptr && title_arr_count > 0) {
            uint32_t primary_tid = read<uint32_t>(title_arr_ptr);
            json.key("primary_title_id"); json.value_int(primary_tid & 0xFFFFFF);
            uintptr_t tobj = title_db_lookup(primary_tid);
            if (tobj) {
                auto display_name = read_std_string(tobj + ck3::title_obj::TITLE_DISPLAY_NAME);
                if (!display_name.empty()) {
                    json.key("primary_title_name"); json.value_string(display_name.c_str(), display_name.size());
                }
                uintptr_t type_ptr = read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
                if (type_ptr) {
                    auto title_key = read_std_string(type_ptr + ck3::db_object::NAME_KEY);
                    if (!title_key.empty()) {
                        json.key("primary_title_key"); json.value_string(title_key.c_str(), title_key.size());
                    }
                }
            }
        }
    }
}

// =============================================================================
// Domain Detail (county-level breakdown with development and buildings)
// =============================================================================

static void build_domain(JsonBuilder& json, void* ruler) {
    uintptr_t r = reinterpret_cast<uintptr_t>(ruler);
    uintptr_t tr = resolve_title_realm(r);

    json.key("domain");
    json.begin_array();

    if (!tr) {
        json.end_array();
            return;
    }

    uintptr_t title_ids = read<uintptr_t>(tr + ck3::title_realm::VASSAL_LIST);
    int32_t title_count = read<int32_t>(tr + ck3::title_realm::VASSAL_COUNT);

    if (!title_ids || title_count <= 0 || title_count > 5000) {
        json.end_array();
            return;
    }

    int32_t county_emitted = 0;
    for (int32_t i = 0; i < title_count && county_emitted < 32; i++) {
        uint32_t tid = read<uint32_t>(title_ids + i * 4);
        uintptr_t tptr = title_db_lookup(tid);
        if (!tptr) continue;

        int32_t tier = title_get_tier(tptr);
        if (tier != 2) continue; // only counties

        json.begin_object();
        json.key("title_id"); json.value_uint(tid);
        json.key("tier"); json.value_int(tier);

        // R19: Navigate county_title → first barony → province → county_data
        uintptr_t county_ptr = 0;
        {
            int32_t sub_count = read<int32_t>(tptr + ck3::title_obj::SUB_TITLE_COUNT);
            if (sub_count > 0) {
                uintptr_t sub_arr = read<uintptr_t>(tptr + ck3::title_obj::SUB_TITLE_ARRAY);
                if (sub_arr) {
                    uint32_t barony_id = read<uint32_t>(sub_arr) & 0xFFFFFF;
                    uintptr_t barony = title_db_lookup(barony_id);
                    if (barony) {
                        uintptr_t province = read<uintptr_t>(barony + ck3::title_obj::PROVINCE_PTR);
                        if (province) {
                            county_ptr = read<uintptr_t>(province + ck3::province::COUNTY_DATA_PTR);
                        }
                    }
                }
            }
        }
        if (county_ptr) {
            int32_t dev = read<int32_t>(county_ptr + ck3::county::DEVELOPMENT);
            json.key("development"); json.value_int(dev);

            uintptr_t prov_arr = read<uintptr_t>(county_ptr + ck3::county::PROVINCE_ARRAY);
            int32_t prov_count = read<int32_t>(county_ptr + ck3::county::PROVINCE_COUNT);
            if (prov_count < 0 || prov_count > 20) prov_count = 0;

            json.key("provinces");
            json.begin_array();
            if (prov_arr && prov_count > 0) {
                for (int32_t p = 0; p < prov_count; p++) {
                    uintptr_t prov = read<uintptr_t>(prov_arr + p * 8);
                    if (!prov) continue;
                    json.begin_object();
                    // Building slot total
                    uintptr_t slot_data = prov + ck3::province::BUILDING_SLOT_DATA;
                    int32_t total_slots = read<int32_t>(slot_data + 0x24);
                    json.key("building_slots"); json.value_int(total_slots);
                    json.end_object();
                                }
            }
            json.end_array();
                } else {
            json.key("development"); json.value_int(0);
            json.key("provinces"); json.begin_array(); json.end_array();
        }

        // De jure parent
        uint32_t dejure = read<uint32_t>(tptr + ck3::title_obj::DE_JURE_PARENT);
        if (dejure != 0 && dejure != 0xFFFFFFFF) {
            json.key("de_jure_duchy"); json.value_uint(dejure);
        } else {
            json.key("de_jure_duchy"); json.value_null();
        }

        json.end_object();
            county_emitted++;
    }

    json.end_array();
}

// =============================================================================
// Gold Budget Tiers
// =============================================================================

static void build_gold_budget(JsonBuilder& json, void* ruler) {
    void* realm = char_realm(ruler);
    json.key("gold_budget");
    json.begin_object();

    if (realm) {
        uintptr_t rd = reinterpret_cast<uintptr_t>(realm);
        // CONFIRMED: core_data income fields (MEDIUM confidence, 3-4 accessors each)
        json.key("monthly_income");
        json.value_float(fp_to_float(read<int64_t>(rd + ck3::core_data::MONTHLY_INCOME)));
        json.key("income_accumulated");
        json.value_float(fp_to_float(read<int64_t>(rd + ck3::core_data::INCOME_ACCUMULATOR)));
    } else {
        json.key("monthly_income");     json.value_float(0.0f);
        json.key("income_accumulated"); json.value_float(0.0f);
    }

    json.end_object();
}

// =============================================================================
// Wars
// =============================================================================

static void build_wars(JsonBuilder& json, void* ruler) {
    uintptr_t r = reinterpret_cast<uintptr_t>(ruler);
    uint32_t my_id = read<uint32_t>(r + ck3::character::ID) & 0xFFFFFF;
    json.key("wars");
    json.begin_array();

    // R8V-4 fix: war list via title_realm sub-struct + 0x318 (WAR_REF_ARRAY)
    uintptr_t title_realm = resolve_title_realm(r);
    if (title_realm) {
        uintptr_t war_arr = safe_read<uintptr_t>(title_realm + ck3::title_realm::WAR_REF_ARRAY);
        int32_t war_count = safe_read<int32_t>(title_realm + ck3::title_realm::WAR_REF_ARRAY + 0x0C);

        // Dedup tracking
        uint32_t seen_ids[256];
        int seen_count = 0;

        if (war_arr && war_count > 0 && war_count <= 100) {
            for (int32_t i = 0; i < war_count; i++) {
                // War refs are uint32 (4-byte stride), resolved via WAR_DB
                uint32_t war_ref = safe_read<uint32_t>(war_arr + i * 4);
                uint32_t war_idx = war_ref & 0xFFFFFF;

                uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
                if (!war_db) continue;
                uint32_t war_cap = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
                if (war_idx >= war_cap) continue;
                uintptr_t war_table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
                if (!war_table) continue;
                uintptr_t war_ptr = safe_read<uintptr_t>(war_table + ck3::hash_map::ENTRY_HEADER_SIZE
                                                        + static_cast<uintptr_t>(war_idx) * ck3::hash_map::ENTRY_STRIDE);
                if (!war_ptr) continue;
                // Verify war ref (masked 24-bit comparison)
                uint32_t stored_id = safe_read<uint32_t>(war_ptr + 0x08);
                if ((stored_id & 0xFFFFFF) != war_idx) continue;
                // Skip ended wars
                uint8_t is_ended = safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED);
                if (is_ended != 0) continue;
                // Dedup
                bool dup = false;
                for (int s = 0; s < seen_count; s++) { if (seen_ids[s] == war_ref) { dup = true; break; } }
                if (dup) continue;
                if (seen_count < 256) seen_ids[seen_count++] = war_ref;

                json.begin_object();

                // War ID (HIGH confidence, verified offset)
                json.key("war_id");      json.value_int(safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID));

                // War name (SSO std::string at +0x2D8)
                std::string war_name = read_std_string(war_ptr + ck3::war_obj::NAME_DATA_PTR);
                if (!war_name.empty()) {
                    json.key("name"); json.value_string(war_name.c_str(), war_name.size());
                }

                // Active flag (IS_ENDED: 0=active, non-zero=ended)
                json.key("active"); json.value_bool(true);  // already filtered ended wars above

                // CB type name (if available)
                uintptr_t cb_type_ref = safe_read<uintptr_t>(war_ptr + ck3::war_obj::CB_TYPE_REF);
                if (cb_type_ref) {
                    std::string cb_name = read_std_string(cb_type_ref + ck3::db_object::NAME_KEY);
                    if (!cb_name.empty()) {
                        json.key("cb_type"); json.value_string(cb_name.c_str(), cb_name.size());
                    }
                }

                // Primary attacker/defender via direct refs (R8V4 confirmed)
                uint32_t primary_atk = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_ATK_REF) & 0xFFFFFF;
                uint32_t primary_def = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF;
                json.key("attacker_id"); json.value_int(primary_atk);
                json.key("defender_id"); json.value_int(primary_def);

                // Full participant lists (container stores pointers, 8-byte stride)
                bool scope_is_attacker = false;
                bool scope_is_defender = false;
                {
                    uintptr_t atk_container = war_ptr + ck3::war_obj::ATK_PARTICIPANT;
                    uintptr_t atk_arr = safe_read<uintptr_t>(atk_container);
                    int32_t atk_count = safe_read<int32_t>(atk_container + 0x0C);
                    json.key("attackers"); json.begin_array();
                    if (atk_arr && atk_count > 0 && atk_count <= 100) {
                        for (int32_t j = 0; j < atk_count && j < 20; j++) {
                            uintptr_t part_ptr = safe_read<uintptr_t>(atk_arr + j * 8);
                            if (!part_ptr) continue;
                            uint32_t pid = safe_read<uint32_t>(part_ptr + ck3::war_participant::CHAR_ID) & 0xFFFFFF;
                            json.value_int(pid);
                            if (pid == my_id) scope_is_attacker = true;
                        }
                    }
                    json.end_array();

                    uintptr_t def_container = war_ptr + ck3::war_obj::DEF_PARTICIPANT;
                    uintptr_t def_arr = safe_read<uintptr_t>(def_container);
                    int32_t def_count = safe_read<int32_t>(def_container + 0x0C);
                    json.key("defenders"); json.begin_array();
                    if (def_arr && def_count > 0 && def_count <= 100) {
                        for (int32_t j = 0; j < def_count && j < 20; j++) {
                            uintptr_t part_ptr = safe_read<uintptr_t>(def_arr + j * 8);
                            if (!part_ptr) continue;
                            uint32_t pid = safe_read<uint32_t>(part_ptr + ck3::war_participant::CHAR_ID) & 0xFFFFFF;
                            json.value_int(pid);
                            if (pid == my_id) scope_is_defender = true;
                        }
                    }
                    json.end_array();
                }

                // Battle score from raw memory reads (safe from tick hook context).
                int bc = 0;
                double bs = compute_battle_score(war_ptr, &bc);
                json.key("battle_score"); json.value_int(static_cast<int>(bs));
                json.key("battle_count"); json.value_int(bc);

                // Ticking war score from raw memory reads (R24). NOTE: this is the
                // RAW UNCAPPED accumulator — it is NOT the score that gates
                // enforce_demands and can climb past 100. Treat it as momentum only.
                int ts = compute_ticking_score(war_ptr);
                json.key("ticking_score"); json.value_int(ts);

                // A8: surface the REAL engine war score (clamped sum of
                // imprisonment+battle+occupation+ticking+landless, ±100) when the
                // WAR_SCORE_UPDATE hook has populated the cache for this war. This is
                // the value that actually gates enforce_demands — the agent was being
                // misled by the uncapped ticking_score and chasing wars that could
                // never resolve (no occupation → real score never crosses threshold).
                // Attacker-relative advantage (positive = attacker ahead); bs/ts
                // are computed attacker-minus-defender (see bridge.h). Used for
                // the clamped fallback and favored_side.
                int attacker_advantage = static_cast<int>(bs) + ts;

                uint32_t this_war_id = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);
                WarScoreBreakdown wbd{};
                WarScoreRejectReason wrej = WarScoreRejectReason::None;
                if (query_war_score_breakdown_fresh(this_war_id, &wbd, &wrej)) {
                    json.key("war_score"); json.value_int(wbd.total);
                    json.key("war_score_source"); json.value_string("computed", 8);
                    json.key("score_breakdown"); json.begin_object();
                    json.key("imprisonment"); json.value_int((wbd.imprisonment_atk + wbd.imprisonment_def) / 1000);
                    json.key("battle");       json.value_int((wbd.battle_atk + wbd.battle_def) / 1000);
                    json.key("occupation");   json.value_int((wbd.occupation_atk + wbd.occupation_def) / 1000);
                    json.key("ticking");      json.value_int((wbd.ticking_atk + wbd.ticking_def) / 1000);
                    json.key("landless");     json.value_int((wbd.landless_atk + wbd.landless_def) / 1000);
                    json.end_object();
                } else {
                    // Cache miss, or a STALE entry (hook pre-validation has been
                    // rejecting refreshes — see ck-xwlg; the fossil is typically
                    // the all-zero day-one breakdown). Either way, clamp the
                    // live battle+ticking estimate instead of a misleading 0.
                    WarScoreBreakdown stale_probe{};
                    bool had_stale = query_war_score_breakdown(this_war_id, &stale_probe);
                    int fallback = attacker_advantage;
                    if (fallback > 100) fallback = 100;
                    if (fallback < -100) fallback = -100;
                    json.key("war_score"); json.value_int(fallback);
                    if (had_stale) {
                        json.key("war_score_source"); json.value_string("computed_stale", 14);
                        const char* rn = war_score_reject_name(wrej);
                        json.key("score_hook_reject");
                        json.value_string(rn, static_cast<int>(strlen(rn)));
                    } else {
                        json.key("war_score_source"); json.value_string("computed_fallback", 17);
                    }
                }

                // Who is winning, and which side the scope ruler is on (TASK C).
                const char* favored_side = (attacker_advantage > 0) ? "attacker"
                                         : (attacker_advantage < 0) ? "defender" : "even";
                json.key("favored_side"); json.value_string(favored_side);

                const char* scope_side = scope_is_attacker ? "attacker"
                                       : scope_is_defender ? "defender" : "neutral";
                json.key("my_side"); json.value_string(scope_side);

                json.end_object();
                        }
        }
    }

    json.end_array();
}

// =============================================================================
// Truces
// =============================================================================

static void build_truces(JsonBuilder& json, void* ruler) {
    uintptr_t r = reinterpret_cast<uintptr_t>(ruler);
    uint32_t my_id = read<uint32_t>(r + ck3::character::ID);
    json.key("truces");
    json.begin_array();

    // Round 10: truce list via diplomatic_data sub-struct at char+0x1c0
    // Each entry is a CTruceContainer* with two slots (one per participant).
    // We find which slot belongs to our character and read the OTHER party's ID.
    uintptr_t diplo = read<uintptr_t>(r + ck3::character::DIPLOMATIC_DATA);
    if (diplo) {
        uintptr_t truce_arr = read<uintptr_t>(diplo + ck3::diplomatic_data::TRUCE_LIST_PTR);
        int32_t truce_count = read<int32_t>(diplo + ck3::diplomatic_data::TRUCE_LIST_COUNT);

        if (truce_arr && truce_count > 0 && truce_count <= 200) {
            for (int32_t i = 0; i < truce_count; i++) {
                uintptr_t tc = read<uintptr_t>(truce_arr + i * 8);
                if (!tc) continue;

                uint32_t cid1 = read<uint32_t>(tc + ck3::truce_container::CHAR_ID_1);
                uint32_t cid2 = read<uint32_t>(tc + ck3::truce_container::CHAR_ID_2);

                // Determine which slot is ours and which is the other party
                uint32_t target_id;
                size_t my_slot_expiry;
                size_t my_slot_reason;
                if (cid1 == my_id) {
                    target_id = cid2;
                    my_slot_expiry = ck3::truce_container::SLOT1_EXPIRY;
                    my_slot_reason = ck3::truce_container::SLOT1_REASON_ENUM;
                } else if (cid2 == my_id) {
                    target_id = cid1;
                    my_slot_expiry = ck3::truce_container::SLOT2_EXPIRY;
                    my_slot_reason = ck3::truce_container::SLOT2_REASON_ENUM;
                } else {
                    continue; // Container doesn't involve us (shouldn't happen)
                }

                int32_t expiry = read<int32_t>(tc + my_slot_expiry);
                int32_t reason = read<int32_t>(tc + my_slot_reason);

                // reason==3 means invalid/expired — skip
                if (reason == 3) continue;

                json.begin_object();
                json.key("target_id"); json.value_int(target_id);
                json.key("end_date"); json.value_int(expiry);
                // 0=attacker, 1=defender, 2=white_peace
                const char* reason_str = (reason == 0) ? "attacker" :
                                         (reason == 1) ? "defender" :
                                         (reason == 2) ? "white_peace" : "unknown";
                json.key("reason"); json.value_string(reason_str);
                json.end_object();
            }
        }
    }

    json.end_array();
}

// =============================================================================
// Council
// =============================================================================

// Helper: find the council_type_obj by probing pointers on the council instance.
// Validates by checking RULER_CHAR_ID at +0x3A0 matches and COUNCILLOR_COUNT is sane.
static uintptr_t obs_find_council_type_obj(uintptr_t council_instance, uint32_t ruler_id) {
    if (!safe_ptr(council_instance)) return 0;

    // Check if the instance itself IS the council_type_obj
    {
        uint32_t ruler_at = safe_read<uint32_t>(council_instance + ck3::council_type_obj::RULER_CHAR_ID);
        int32_t count_at = safe_read<int32_t>(council_instance + ck3::council_type_obj::COUNCILLOR_COUNT);
        if ((ruler_at & 0xFFFFFF) == ruler_id && count_at >= 0 && count_at <= 20) {
            return council_instance;
        }
    }

    // Probe pointer fields for a council_type_obj pointer
    static const size_t probe_offsets[] = { 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30 };
    for (size_t off : probe_offsets) {
        uintptr_t candidate = safe_read<uintptr_t>(council_instance + off);
        if (!safe_ptr(candidate) || candidate == council_instance) continue;
        uint32_t ruler_at = safe_read<uint32_t>(candidate + ck3::council_type_obj::RULER_CHAR_ID);
        int32_t count_at = safe_read<int32_t>(candidate + ck3::council_type_obj::COUNCILLOR_COUNT);
        if ((ruler_at & 0xFFFFFF) == ruler_id && count_at >= 0 && count_at <= 20) {
            return candidate;
        }
    }

    return 0;
}

// Helper: scan COUNCIL_TYPE_DB for an entry whose RULER_CHAR_ID (+0x3A0) matches ruler_id.
// Bypasses the broken instance+0x04 path (which returns 0xFFFFFFFF = lazy init).
static uintptr_t scan_council_type_db_for_ruler(uint32_t ruler_id) {
    uintptr_t db = read_offset<uintptr_t>(ck3::globals::COUNCIL_TYPE_DB);
    if (!db) return 0;
    uintptr_t table = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    if (!safe_ptr(table)) return 0;
    uint32_t capacity = safe_read<uint32_t>(db + ck3::hash_map::CAPACITY);
    if (capacity == 0 || capacity > 100000) return 0;

    for (uint32_t i = 0; i < capacity; i++) {
        uintptr_t entry = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                               + static_cast<uintptr_t>(i) * ck3::hash_map::ENTRY_STRIDE);
        if (!safe_ptr(entry)) continue;
        uint32_t entry_ruler = safe_read<uint32_t>(entry + ck3::council_type_obj::RULER_CHAR_ID);
        if ((entry_ruler & 0xFFFFFF) != ruler_id) continue;
        int32_t count = safe_read<int32_t>(entry + ck3::council_type_obj::COUNCILLOR_COUNT);
        if (count >= 0 && count <= 20) {
            return entry;
        }
    }
    return 0;
}

// Helper: emit a single councillor entry into the JsonBuilder. Returns true if emitted.
static bool obs_emit_councillor(JsonBuilder& json, uint32_t char_id, const char* position_name) {
    void* holder = obs_lookup_character(char_id);
    if (!holder) return false;
    uintptr_t h = reinterpret_cast<uintptr_t>(holder);

    json.begin_object();
    json.key("position"); json.value_string(position_name);
    json.key("holder_id"); json.value_uint(char_id & 0xFFFFFF);
    emit_name_fields(json, h);

    json.key("skills");
    json.begin_object();
    json.key("diplomacy");
    json.value_int(safe_read<int32_t>(h + ck3::character::SKILL_DIPLOMACY));
    json.key("martial");
    json.value_int(safe_read<int32_t>(h + ck3::character::SKILL_MARTIAL));
    json.key("stewardship");
    json.value_int(safe_read<int32_t>(h + ck3::character::SKILL_STEWARDSHIP));
    json.key("intrigue");
    json.value_int(safe_read<int32_t>(h + ck3::character::SKILL_INTRIGUE));
    json.key("learning");
    json.value_int(safe_read<int32_t>(h + ck3::character::SKILL_LEARNING));
    json.key("prowess");
    json.value_int(safe_read<int32_t>(h + ck3::character::SKILL_PROWESS));
    json.end_object();

    json.end_object();
    return true;
}

static void build_council(JsonBuilder& json, void* ruler) {
    json.key("council");
    json.begin_array();

    int emitted = 0;
    uintptr_t r_addr = reinterpret_cast<uintptr_t>(ruler);
    uint32_t ruler_id = safe_read<uint32_t>(r_addr + ck3::character::ID) & 0xFFFFFF;

    // =========================================================================
    // PRIMARY PATH (v3): Find council_type_obj, read COUNCILLOR_IDS directly
    // =========================================================================
    uintptr_t council_type_obj_addr = 0;

    // Step 1: Get council instance via function call
    uintptr_t ci = 0;
    if (g_get_council_instance) {
        void* instance = g_get_council_instance(ruler);
        ci = reinterpret_cast<uintptr_t>(instance);
    }

    // Step 1b: If function returned null, try direct memory read from alive_data+0x4E8
    // CONFIRMED via Ghidra: GetCouncilInstance uses *(char+0x1A8)+0x4E8, NOT char+0x1B8.
    if (!safe_ptr(ci)) {
        uintptr_t alive_data = safe_read<uintptr_t>(r_addr + ck3::character::REALM_DATA);
        if (safe_ptr(alive_data)) {
            ci = safe_read<uintptr_t>(alive_data + ck3::core_data::COUNCIL_INSTANCE_PTR);
        }
    }

    // Step 2: Find the council_type_obj from the council instance
    if (safe_ptr(ci)) {
        // First try: DB lookup
        uint32_t council_type_id = safe_read<uint32_t>(ci + 0x04);
        if (council_type_id != 0 && council_type_id != 0xFFFFFFFF) {
            uintptr_t db_result = ad_db_lookup(ck3::globals::COUNCIL_TYPE_DB, council_type_id);
            if (safe_ptr(db_result)) {
                uint32_t db_ruler = safe_read<uint32_t>(db_result + ck3::council_type_obj::RULER_CHAR_ID);
                int32_t db_count = safe_read<int32_t>(db_result + ck3::council_type_obj::COUNCILLOR_COUNT);
                if ((db_ruler & 0xFFFFFF) == ruler_id && db_count >= 0 && db_count <= 20) {
                    council_type_obj_addr = db_result;
                }
            }
        }

        // Second try: probe instance for council_type_obj pointer
        if (!safe_ptr(council_type_obj_addr)) {
            council_type_obj_addr = obs_find_council_type_obj(ci, ruler_id);
        }
    }

    // DB SCAN: If instance+0x04 was invalid (0xFFFFFFFF) and probe failed,
    // scan COUNCIL_TYPE_DB for an entry whose RULER_CHAR_ID matches our ruler.
    // This bypasses the broken instance->type_id->DB lookup entirely.
    if (!safe_ptr(council_type_obj_addr)) {
        council_type_obj_addr = scan_council_type_db_for_ruler(ruler_id);
    }

    // Step 3: Read councillors from council_type_obj
    if (safe_ptr(council_type_obj_addr)) {
        // 3a: Try position tracker for names + char IDs
        uintptr_t tracker = safe_read<uintptr_t>(council_type_obj_addr + ck3::council_type_obj::POSITION_TRACKER);
        if (safe_ptr(tracker)) {
            uintptr_t assign_arr = safe_read<uintptr_t>(tracker + ck3::pos_tracker::ASSIGN_ARRAY);
            int32_t assign_count = safe_read<int32_t>(tracker + ck3::pos_tracker::ASSIGN_COUNT);
            if (safe_ptr(assign_arr) && assign_count > 0 && assign_count <= 20) {
                for (int32_t i = 0; i < assign_count; i++) {
                    uint32_t assign_id = safe_read<uint32_t>(assign_arr + i * sizeof(uint32_t));
                    if (assign_id == 0 || assign_id == 0xFFFFFFFF) continue;
                    uintptr_t ae = ad_db_lookup(ck3::globals::COUNCIL_POSITION_DB, assign_id);
                    if (!ae) continue;
                    uint32_t cid_raw = safe_read<uint32_t>(ae + ck3::council_assign::ASSIGNED_CHAR_ID);
                    if (cid_raw == 0 || cid_raw == 0xFFFFFFFF) continue;

                    std::string pname = "unknown";
                    uintptr_t intermediate = safe_read<uintptr_t>(ae + ck3::council_assign::INTERMEDIATE_PTR);
                    if (safe_ptr(intermediate)) {
                        uintptr_t pto = safe_read<uintptr_t>(intermediate + 0x30);
                        if (safe_ptr(pto)) {
                            std::string raw = ad_read_std_string(pto + ck3::council_pos_type::NAME_KEY);
                            if (!raw.empty()) pname = raw;
                        }
                    }
                    if (obs_emit_councillor(json, cid_raw & 0xFFFFFF, pname.c_str()))
                        emitted++;
                }
            }
        }

        // 3b: Fall back to COUNCILLOR_IDS direct array if position tracker yielded nothing
        if (emitted == 0) {
            uintptr_t cid_arr = safe_read<uintptr_t>(council_type_obj_addr + ck3::council_type_obj::COUNCILLOR_IDS);
            int32_t cid_count = safe_read<int32_t>(council_type_obj_addr + ck3::council_type_obj::COUNCILLOR_COUNT);
            if (safe_ptr(cid_arr) && cid_count > 0 && cid_count <= 20) {
                for (int32_t i = 0; i < cid_count; i++) {
                    uint32_t cid_raw = safe_read<uint32_t>(cid_arr + i * sizeof(uint32_t));
                    if (cid_raw == 0 || cid_raw == 0xFFFFFFFF) continue;
                    if (obs_emit_councillor(json, cid_raw & 0xFFFFFF, "unknown"))
                        emitted++;
                }
            }
        }
    }

    // =========================================================================
    // SECONDARY: GET_COUNCIL_CONFIG sorted array (config+0x40/+0x4C)
    // =========================================================================
    if (emitted == 0 && g_get_council_config) {
        void* config = g_get_council_config(ruler);
        if (config) {
            uintptr_t cfg = reinterpret_cast<uintptr_t>(config);
            uintptr_t id_arr = safe_read<uintptr_t>(cfg + ck3::council_member::COUNCILLOR_ARRAY);
            int32_t count = safe_read<int32_t>(cfg + ck3::council_member::COUNCILLOR_COUNT);
            if (safe_ptr(id_arr) && count > 0 && count <= 20) {
                for (int32_t i = 0; i < count; i++) {
                    uint32_t cid_raw = safe_read<uint32_t>(id_arr + i * sizeof(uint32_t));
                    if (cid_raw == 0 || cid_raw == 0xFFFFFFFF) continue;
                    if (obs_emit_councillor(json, cid_raw & 0xFFFFFF, "unknown"))
                        emitted++;
                }
            }
        }
    }

    // =========================================================================
    // TERTIARY: Direct alive_data+0x4F0 read (GetCouncilConfig path)
    // CONFIRMED via Ghidra: GetCouncilConfig returns *(char+0x1A8)+0x4F0
    // (alive_data, NOT landed_data at char+0x1B8). Previous "fix" to use 0x1B8
    // was WRONG — reverted to correct 0x1A8.
    // =========================================================================
    if (emitted == 0) {
        uintptr_t alive_data = safe_read<uintptr_t>(r_addr + ck3::character::REALM_DATA);
        if (safe_ptr(alive_data)) {
            uintptr_t cfg = alive_data + ck3::core_data::COUNCIL_OWNER_INLINE;
            uintptr_t id_arr = safe_read<uintptr_t>(cfg + ck3::council_member::COUNCILLOR_ARRAY);
            int32_t count = safe_read<int32_t>(cfg + ck3::council_member::COUNCILLOR_COUNT);
            if (safe_ptr(id_arr) && count > 0 && count <= 20) {
                for (int32_t i = 0; i < count; i++) {
                    uint32_t cid_raw = safe_read<uint32_t>(id_arr + i * sizeof(uint32_t));
                    if (cid_raw == 0 || cid_raw == 0xFFFFFFFF) continue;
                    if (obs_emit_councillor(json, cid_raw & 0xFFFFFF, "unknown"))
                        emitted++;
                }
            }
        }
    }

    json.end_array();
}

// =============================================================================
// Schemes
// =============================================================================

static void build_schemes(JsonBuilder& json, void* ruler) {
    uintptr_t r = reinterpret_cast<uintptr_t>(ruler);
    json.key("schemes");
    json.begin_array();

    // Access scheme refs via core_data sub-struct (char+0x1A8)
    uintptr_t core_data = read<uintptr_t>(r + 0x1A8);
    if (!core_data) { json.end_array(); return; }

    uintptr_t scheme_ref_ptr = read<uintptr_t>(core_data + ck3::core_data::SCHEME_REF_PTR);
    int32_t scheme_ref_count = read<int32_t>(core_data + ck3::core_data::SCHEME_REF_COUNT);

    if (!scheme_ref_ptr || scheme_ref_count <= 0 || scheme_ref_count > 200) {
        json.end_array();
        return;
    }

    uintptr_t scheme_refdb = read_offset<uintptr_t>(ck3::globals::SCHEME_REFDB);
    if (!scheme_refdb) { json.end_array(); return; }

    uintptr_t scheme_table = read<uintptr_t>(scheme_refdb + ck3::hash_map::TABLE_PTR);
    if (!scheme_table) { json.end_array(); return; }

    for (int32_t i = 0; i < scheme_ref_count; i++) {
        uint32_t ref = read<uint32_t>(scheme_ref_ptr + i * 4);
        uint32_t idx = ref & 0xFFFFFF;

        uintptr_t scheme_ptr = read<uintptr_t>(scheme_table + ck3::hash_map::ENTRY_HEADER_SIZE
                                               + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE);
        if (!scheme_ptr) continue;

        // Verify scheme ref (masked 24-bit comparison)
        uint32_t stored_id = read<uint32_t>(scheme_ptr + ck3::active_scheme::SCHEME_ID);
        if ((stored_id & 0xFFFFFF) != idx) continue;

        // Check validity via STATUS field (R10-8: 0=active, 1=invalidated)
        int32_t status = read<int32_t>(scheme_ptr + ck3::active_scheme::STATUS);
        if (status == 1) continue;  // skip invalidated schemes

        json.begin_object();

        // Scheme ID
        json.key("scheme_id"); json.value_uint(ref & 0xFFFFFF);

        // Scheme type: dereference CSchemeType* at +0x18, read name via db_object::NAME_KEY (R10-8)
        uintptr_t type_ptr = read<uintptr_t>(scheme_ptr + ck3::active_scheme::SCHEME_TYPE_PTR);
        bool type_name_found = false;
        if (type_ptr) {
            std::string type_name = read_std_string(type_ptr + ck3::db_object::NAME_KEY);
            if (!type_name.empty()) {
                json.key("type"); json.value_string(type_name.c_str(), type_name.size());
                type_name_found = true;
            }
        }
        if (!type_name_found) {
            uint32_t subtype_id = read<uint32_t>(scheme_ptr + ck3::active_scheme::SCHEME_SUBTYPE_ID);
            json.key("type"); json.value_string("unknown");
            json.key("subtype_id"); json.value_uint(subtype_id);
        }

        // Owner char ID
        uint32_t owner_id = read<uint32_t>(scheme_ptr + ck3::active_scheme::OWNER_ID);
        json.key("owner_id"); json.value_uint(owner_id & 0xFFFFFF);

        // Target: read target_type and target_ref (R10-8)
        int32_t target_type = read<int32_t>(scheme_ptr + ck3::active_scheme::TARGET_TYPE);
        uint32_t target_ref_raw = read<uint32_t>(scheme_ptr + ck3::active_scheme::TARGET_REF);

        if (target_type == 0 && target_ref_raw != 0 && target_ref_raw != 0xFFFFFFFF) {
            // Character target
            void* target_char = obs_lookup_character(target_ref_raw);
            if (target_char) {
                uint32_t target_id = target_ref_raw & 0xFFFFFF;
                json.key("target_id"); json.value_uint(target_id);
                std::string tname = resolve_character_name(reinterpret_cast<uintptr_t>(target_char));
                if (!tname.empty()) {
                    json.key("target_name"); json.value_string(tname.c_str(), tname.size());
                }
            } else {
                json.key("target_id"); json.value_uint(target_ref_raw & 0xFFFFFF);
            }
        } else if (target_type == 4 || target_ref_raw == 0 || target_ref_raw == 0xFFFFFFFF) {
            json.key("target_id"); json.value_null();
        } else {
            // Non-character target (title/culture/faith)
            json.key("target_type"); json.value_int(target_type);
            json.key("target_ref"); json.value_uint(target_ref_raw & 0xFFFFFF);
        }

        // Success chance (int32)
        int32_t success_chance = read<int32_t>(scheme_ptr + ck3::active_scheme::SUCCESS_CHANCE);
        json.key("success_chance"); json.value_int(success_chance);

        // Progress (int32, R10-8: at +0x70)
        int32_t progress = read<int32_t>(scheme_ptr + ck3::active_scheme::PROGRESS);
        json.key("progress"); json.value_int(progress);

        // Max progress (int32, R10-8: at +0x1E8)
        int32_t max_progress = read<int32_t>(scheme_ptr + ck3::active_scheme::MAX_PROGRESS);
        json.key("max_progress"); json.value_int(max_progress);

        // Frozen flag (R10-8: at +0x108)
        uint8_t is_frozen = read<uint8_t>(scheme_ptr + ck3::active_scheme::IS_FROZEN);
        json.key("is_frozen"); json.value_bool(is_frozen != 0);

        // Agent count
        int32_t agents = read<int32_t>(scheme_ptr + ck3::active_scheme::AGENT_SLOTS_COUNT);
        json.key("agents"); json.value_int(agents);

        json.end_object();
    }

    json.end_array();
}

} // namespace bridge
