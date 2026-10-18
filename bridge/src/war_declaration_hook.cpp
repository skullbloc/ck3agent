// war_declaration_hook.cpp — Detect new wars by monitoring WAR_DB count changes
// each tick. When the active war count increases, scan the DB for unknown war IDs
// and log all fields of the newly created CWar objects.
//
// Architecture: Instead of hooking WAR_CREATE_CORE (which may not fire for all
// war creation paths), we poll the global WAR_DB every tick. This is foolproof —
// it doesn't matter HOW the game creates wars, we detect them by count change
// and read the result.
//
// This is a pure observer — it does not modify any game state.

#include "action_dispatch_internal.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "name_tables.h"
#include "world_epoch.h"   // ck-hxh7

#include <shared_mutex>
#include <deque>
#include <unordered_set>
#include <chrono>
#include <string>

namespace bridge {

// =============================================================================
// War Declaration Record
// =============================================================================

struct WarDeclarationRecord {
    uint64_t tick;                // game tick when war was detected
    uint64_t timestamp_ms;       // wall-clock time (epoch ms)
    uint32_t war_id;             // CWar::war_id
    uint32_t attacker_id;        // primary attacker char ref
    uint32_t defender_id;        // primary defender char ref
    char     cb_type_name[128];  // CB type name string
    uint32_t cb_type_index;      // CB type DB index
    uint32_t claimant_id;        // claimant char ref (0xFFFFFFFF if none)
    uint32_t target_titles[8];   // up to 8 target title IDs
    int      target_title_count; // actual count
    char     war_name[256];      // generated war name
    char     attacker_name[128]; // attacker character name
    char     defender_name[128]; // defender character name
    // Participant data
    uint32_t atk_participants[16]; // attacker-side participant char IDs
    int      atk_participant_count;
    uint32_t def_participants[16]; // defender-side participant char IDs
    int      def_participant_count;
};

static std::shared_mutex g_war_decl_mutex;
static std::deque<WarDeclarationRecord> g_war_declarations CK3_EARLY_INIT;
static constexpr size_t MAX_WAR_DECLARATIONS = 64;

// Tracking state for tick-based detection
static std::unordered_set<uint32_t> g_known_war_ids CK3_EARLY_INIT;
static int32_t g_last_active_count = -1;
static bool g_war_monitor_initialized = false;

// ck-hxh7: this detection state is keyed by WAR ID (reused across saves) and was
// never reset. Two failures followed a save load into a live process:
//   (a) a reused war id was already in g_known_war_ids, so a genuinely NEW war
//       declaration in the new world was silently dropped;
//   (b) g_war_monitor_initialized stayed true, so the seeding branch that
//       snapshots the baseline never re-ran and g_last_active_count carried the
//       previous world's count into the delta.
// Resetting restores exactly the first-call behaviour (re-seed, report nothing
// pre-existing), which is the correct response to a load. Registered with the
// world-epoch detector; the resets run on the game thread, same as check_war_db_tick.
static void reset_war_monitor_state() {
    g_known_war_ids.clear();
    g_last_active_count = -1;
    g_war_monitor_initialized = false;
}

[[maybe_unused]] static const WorldResetRegistrar g_war_monitor_world_scoped CK3_EARLY_INIT([] {
    register_world_reset("war_monitor_detection_state", &reset_war_monitor_state);
});

// =============================================================================
// Strip CK3 loc markup from a war name.
//   - "#tag text#!"  → "text"  (formatting scopes; opener is '#' + token + ' ',
//                                closer is "#!")
//   - control byte 0x15 ... 0x15 → dropped (inline format token wrappers)
// Leaves already-resolved text and plain spaces intact. Pure byte-level pass,
// no allocation beyond the output string — crash-safe on the game thread.
// =============================================================================

static std::string strip_loc_markup(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(in[i]);
        if (ch == 0x15) {
            // Skip to the matching 0x15 terminator (or end of string).
            size_t j = i + 1;
            while (j < in.size() && static_cast<unsigned char>(in[j]) != 0x15) ++j;
            i = (j < in.size()) ? j : in.size() - 1;
            continue;
        }
        if (ch == '#') {
            // "#!" is a reset/closer — drop the two bytes.
            if (i + 1 < in.size() && in[i + 1] == '!') {
                ++i;
                continue;
            }
            // "#tag " is a scope opener — drop "#tag" and the single trailing
            // space delimiter, keeping the enclosed text.
            size_t j = i + 1;
            while (j < in.size() && in[j] != ' ' && in[j] != '#') ++j;
            if (j < in.size() && in[j] == ' ') {
                i = j;  // skip the delimiter space too
                continue;
            }
            // Bare '#' with no token delimiter — keep it literally.
            out.push_back('#');
            continue;
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

// =============================================================================
// Read war fields from a CWar pointer into a WarDeclarationRecord
// =============================================================================

static void read_war_fields(uintptr_t war_ptr, WarDeclarationRecord& rec) {
    // War ID
    rec.war_id = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);

    // Primary attacker/defender
    rec.attacker_id = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_ATK_REF) & 0xFFFFFF;
    rec.defender_id = safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF;

    // CB type name and index
    uintptr_t cb_type_ref = safe_read<uintptr_t>(war_ptr + ck3::war_obj::CB_TYPE_REF);
    rec.cb_type_index = 0;
    if (cb_type_ref > 0x10000 && is_mapped(cb_type_ref)) {
        std::string cb_name = ad_read_std_string(cb_type_ref + ck3::db_object::NAME_KEY);
        if (!cb_name.empty()) {
            strncpy(rec.cb_type_name, cb_name.c_str(), sizeof(rec.cb_type_name) - 1);
        }
        rec.cb_type_index = safe_read<uint32_t>(cb_type_ref + ck3::cb_type::DB_INDEX);
    }

    // Claimant
    rec.claimant_id = safe_read<uint32_t>(war_ptr + ck3::war_obj::CLAIMANT_REF);
    if (rec.claimant_id != 0xFFFFFFFF) {
        rec.claimant_id &= 0xFFFFFF;
    }

    // Target titles
    uintptr_t cb_titles_ptr = safe_read<uintptr_t>(war_ptr + ck3::war_obj::CB_TITLE_ARRAY);
    int32_t cb_title_count = safe_read<int32_t>(war_ptr + ck3::war_obj::CB_TITLE_COUNT);
    if (cb_title_count < 0 || cb_title_count > 8) cb_title_count = std::min(cb_title_count, 8);
    rec.target_title_count = cb_title_count;
    if (cb_titles_ptr && cb_title_count > 0) {
        for (int t = 0; t < cb_title_count; t++) {
            uint64_t title_ref = safe_read<uint64_t>(cb_titles_ptr + t * 8);
            rec.target_titles[t] = static_cast<uint32_t>(title_ref & 0xFFFFFF);
        }
    }

    // War name — engine string carries loc markup ("#high …#!"); strip it.
    std::string war_name = strip_loc_markup(
        ad_read_std_string(war_ptr + ck3::war_obj::NAME_DATA_PTR));
    if (!war_name.empty()) {
        strncpy(rec.war_name, war_name.c_str(), sizeof(rec.war_name) - 1);
    }

    // Character names. ad_read_char_name is cache-first and otherwise falls
    // back to the raw loc key. We run on the game thread (check_war_db_tick →
    // on_tick), so SCHEDULE resolution for both principals — but do NOT drain
    // the cache synchronously here.
    //
    // crash2 20260626: synchronously draining populate_name_cache() for both
    // belligerents of EVERY war declaration, under heavy concurrent-war churn,
    // repeatedly fed freshly-dead/freed belligerent objects to the engine's
    // name resolver → heap free-list corruption. The single on_tick drain
    // (bridge_init.cpp) now handles all pending resolutions one tick later;
    // the resolver input is hardened in populate_name_cache. The cost is that
    // a war record captured on the same tick as its declaration falls back to
    // the raw loc key (or numeric id) and picks up the resolved display name
    // on the next read once the on_tick drain has run.
    request_name_resolution(rec.attacker_id);
    request_name_resolution(rec.defender_id);

    // Character names
    uintptr_t atk_char = ad_db_lookup(ck3::globals::CHARACTER_DB, rec.attacker_id);
    if (atk_char) {
        std::string name = ad_read_char_name(atk_char);
        strncpy(rec.attacker_name, name.c_str(), sizeof(rec.attacker_name) - 1);
    }

    uintptr_t def_char = ad_db_lookup(ck3::globals::CHARACTER_DB, rec.defender_id);
    if (def_char) {
        std::string name = ad_read_char_name(def_char);
        strncpy(rec.defender_name, name.c_str(), sizeof(rec.defender_name) - 1);
    }

    // Attacker-side participants
    uintptr_t atk_part_arr = safe_read<uintptr_t>(war_ptr + ck3::war_obj::ATK_PARTICIPANT);
    int32_t atk_part_count = safe_read<int32_t>(war_ptr + ck3::war_obj::ATK_PART_COUNT);
    if (atk_part_count < 0 || atk_part_count > 16) atk_part_count = std::min(atk_part_count, 16);
    rec.atk_participant_count = atk_part_count;
    if (atk_part_arr && atk_part_count > 0) {
        for (int i = 0; i < atk_part_count; i++) {
            uintptr_t part_ptr = safe_read<uintptr_t>(atk_part_arr + i * 8);
            if (part_ptr && safe_ptr(part_ptr)) {
                rec.atk_participants[i] = safe_read<uint32_t>(part_ptr + ck3::war_participant::CHAR_ID) & 0xFFFFFF;
            }
        }
    }

    // Defender-side participants
    uintptr_t def_part_arr = safe_read<uintptr_t>(war_ptr + ck3::war_obj::DEF_PARTICIPANT);
    int32_t def_part_count = safe_read<int32_t>(war_ptr + ck3::war_obj::DEF_PART_COUNT);
    if (def_part_count < 0 || def_part_count > 16) def_part_count = std::min(def_part_count, 16);
    rec.def_participant_count = def_part_count;
    if (def_part_arr && def_part_count > 0) {
        for (int i = 0; i < def_part_count; i++) {
            uintptr_t part_ptr = safe_read<uintptr_t>(def_part_arr + i * 8);
            if (part_ptr && safe_ptr(part_ptr)) {
                rec.def_participants[i] = safe_read<uint32_t>(part_ptr + ck3::war_participant::CHAR_ID) & 0xFFFFFF;
            }
        }
    }
}

// =============================================================================
// Scan WAR_DB and return active war count + collect war IDs
// =============================================================================

struct WarDbScanResult {
    int32_t active_count;
    // New wars found (war_id → war_ptr)
    struct NewWar {
        uint32_t war_id;
        uintptr_t war_ptr;
    };
    NewWar new_wars[32];
    int new_war_count;
};

static bool scan_war_db(WarDbScanResult& result) {
    result.active_count = 0;
    result.new_war_count = 0;

    uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
    if (!war_db) return false;

    uintptr_t table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
    uint32_t capacity = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
    if (!table || capacity == 0 || capacity > 100000) return false;

    for (uint32_t idx = 0; idx < capacity && idx < 10000; idx++) {
        uintptr_t entry = table + ck3::hash_map::ENTRY_HEADER_SIZE
                          + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
        uintptr_t war_ptr = safe_read<uintptr_t>(entry);
        if (!war_ptr || !safe_ptr(war_ptr)) continue;

        uint32_t stored_ref = safe_read<uint32_t>(war_ptr + ck3::war_obj::WAR_ID);
        if ((stored_ref & 0xFFFFFF) != idx) continue;

        uint8_t is_ended = safe_read<uint8_t>(war_ptr + ck3::war_obj::IS_ENDED);
        if (is_ended != 0) continue;

        result.active_count++;

        // Check if this is a war we haven't seen before
        if (g_known_war_ids.find(stored_ref) == g_known_war_ids.end()) {
            if (result.new_war_count < 32) {
                result.new_wars[result.new_war_count].war_id = stored_ref;
                result.new_wars[result.new_war_count].war_ptr = war_ptr;
                result.new_war_count++;
            }
        }
    }
    return true;
}

// =============================================================================
// Tick check — called from on_tick every tick
// =============================================================================

void check_war_db_tick() {
    WarDbScanResult scan{};
    if (!scan_war_db(scan)) return;

    uint64_t tick = g_tick_id.load(std::memory_order_relaxed);

    // First call: populate known war set, don't report existing wars
    if (!g_war_monitor_initialized) {
        g_war_monitor_initialized = true;
        g_last_active_count = scan.active_count;
        for (int i = 0; i < scan.new_war_count; i++) {
            g_known_war_ids.insert(scan.new_wars[i].war_id);
        }
        ADLOG("war_monitor: initialized with %d active wars at tick=%lu\n",
              scan.active_count, tick);
        return;
    }

    // No new wars? Update count and return.
    if (scan.new_war_count == 0) {
        g_last_active_count = scan.active_count;
        return;
    }

    // New wars detected — read and log them
    ADLOG("war_monitor: count change %d → %d (+%d new) at tick=%lu\n",
          g_last_active_count, scan.active_count, scan.new_war_count, tick);

    for (int i = 0; i < scan.new_war_count; i++) {
        uint32_t war_id = scan.new_wars[i].war_id;
        uintptr_t war_ptr = scan.new_wars[i].war_ptr;

        // Add to known set
        g_known_war_ids.insert(war_id);

        // Build record
        WarDeclarationRecord rec{};
        rec.tick = tick;
        rec.timestamp_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        read_war_fields(war_ptr, rec);

        // Log
        ADLOG("WAR DETECTED: war_id=%u attacker=%u(%s) defender=%u(%s) "
              "cb=%s(idx=%u) claimant=%u titles=%d name=\"%s\" "
              "atk_participants=%d def_participants=%d tick=%lu\n",
              rec.war_id, rec.attacker_id, rec.attacker_name,
              rec.defender_id, rec.defender_name,
              rec.cb_type_name, rec.cb_type_index,
              rec.claimant_id, rec.target_title_count,
              rec.war_name,
              rec.atk_participant_count, rec.def_participant_count,
              rec.tick);

        // Store in ring buffer
        {
            std::unique_lock<std::shared_mutex> lock(g_war_decl_mutex);
            g_war_declarations.push_back(rec);
            while (g_war_declarations.size() > MAX_WAR_DECLARATIONS) {
                g_war_declarations.pop_front();
            }
        }
    }

    g_last_active_count = scan.active_count;
}

// =============================================================================
// Initialization (called during first-tick deferred init)
// =============================================================================

void init_war_declaration_hook() {
    // No inline hook needed — we monitor WAR_DB count changes in on_tick.
    // Just log that the monitor is ready. Actual initialization happens
    // on the first call to check_war_db_tick().
    ADLOG("war_monitor: WAR_DB count-change monitor ready "
          "(WAR_DB=0x%lx, base=0x%lx)\n",
          (unsigned long)ck3::globals::WAR_DB,
          (unsigned long)g_base_addr);
}

// =============================================================================
// Query API (called from IPC thread)
// =============================================================================

bool do_query_war_declarations(const char* params_json, std::string* out) {
    if (!out) return false;

    int limit = json_get_int(params_json, "limit");
    if (limit <= 0 || limit > 64) limit = 64;

    uint64_t since_tick = 0;
    if (json_has_key(params_json, "since_tick")) {
        since_tick = static_cast<uint64_t>(json_get_int(params_json, "since_tick"));
    }

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_war_declarations");
    r.key("success"); r.value_bool(true);
    r.key("current_tick"); r.value_uint(g_tick_id.load(std::memory_order_relaxed));
    r.key("monitor_active"); r.value_bool(g_war_monitor_initialized);
    r.key("active_war_count"); r.value_int(g_last_active_count);
    r.key("known_wars"); r.value_uint(static_cast<uint64_t>(g_known_war_ids.size()));

    r.key("declarations"); r.begin_array();
    {
        std::shared_lock<std::shared_mutex> lock(g_war_decl_mutex);
        int emitted = 0;
        // Iterate in reverse (newest first)
        for (auto it = g_war_declarations.rbegin();
             it != g_war_declarations.rend() && emitted < limit; ++it) {
            if (it->tick < since_tick) continue;

            r.begin_object();
            r.key("tick"); r.value_uint(it->tick);
            r.key("timestamp_ms"); r.value_uint(it->timestamp_ms);
            r.key("war_id"); r.value_uint(it->war_id);
            r.key("attacker_id"); r.value_uint(it->attacker_id);
            r.key("defender_id"); r.value_uint(it->defender_id);
            r.key("cb_type"); r.value_string(it->cb_type_name);
            r.key("cb_type_index"); r.value_uint(it->cb_type_index);
            r.key("claimant_id"); r.value_uint(
                it->claimant_id == 0xFFFFFF ? 0xFFFFFFFF : it->claimant_id);

            r.key("target_titles"); r.begin_array();
            for (int t = 0; t < it->target_title_count; t++) {
                r.value_uint(it->target_titles[t]);
            }
            r.end_array();

            r.key("war_name"); r.value_string(it->war_name);
            r.key("attacker_name"); r.value_string(it->attacker_name);
            r.key("defender_name"); r.value_string(it->defender_name);

            // Participant arrays
            r.key("atk_participants"); r.begin_array();
            for (int p = 0; p < it->atk_participant_count; p++) {
                r.value_uint(it->atk_participants[p]);
            }
            r.end_array();

            r.key("def_participants"); r.begin_array();
            for (int p = 0; p < it->def_participant_count; p++) {
                r.value_uint(it->def_participants[p]);
            }
            r.end_array();

            r.end_object();
            emitted++;
        }
    }
    r.end_array();

    r.key("total_recorded"); r.value_uint(
        static_cast<uint64_t>(g_war_declarations.size()));
    r.end_object();

    *out = r.take();
    return true;
}

} // namespace bridge
