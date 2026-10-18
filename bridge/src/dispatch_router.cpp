// dispatch_router.cpp — Main dispatch router, JSON helpers, character lookup,
// title/realm helpers, DB object helpers, AI control, controlled character management,
// bridge state recovery.
//
// Split from action_dispatch.cpp (mechanical refactor, no logic changes).

#include "log_dedupe.h"      // ck-os50
#include "instance_paths.h"  // ck-qn03
#include "action_dispatch_internal.h"
#include "claim_event_log.h"   //: ck-jij9
#include "primary_cause.h"   //: ck-6xi6 cause register
#include "skip_held_heir.h"
#include "game_date.h"
#include "snapshot.h"
#include "pipeline.h"
#include "episode_policy.h"
#include "budget_ledger.h"
#include "board.h"
#include <rapidjson/document.h>

#include <chrono>
#include <ctime>
#include <mutex>
#include <sys/stat.h>

namespace bridge {

// =============================================================================
// Function Pointers (initialized from resolved addresses)
// =============================================================================

fn_ai_char_toggle     g_ai_char_toggle = nullptr;
fn_char_is_alive      g_char_is_alive = nullptr;
fn_get_ai_controller  g_get_ai_ctrl = nullptr;
fn_get_council_config g_get_council_config = nullptr;
fn_get_council_instance g_get_council_instance = nullptr;

// Initialize function pointers from base address
void init_action_dispatch() {
    g_ai_char_toggle = reinterpret_cast<fn_ai_char_toggle>(
        g_base_addr + ck3::funcs::AI_CHAR_TOGGLE);
    g_char_is_alive = reinterpret_cast<fn_char_is_alive>(
        g_base_addr + ck3::funcs::CHAR_IS_ALIVE);
    g_get_ai_ctrl = reinterpret_cast<fn_get_ai_controller>(
        g_base_addr + ck3::funcs::CHAR_GET_AI_CONTROLLER);
    g_get_council_config = reinterpret_cast<fn_get_council_config>(
        g_base_addr + ck3::funcs::GET_COUNCIL_CONFIG);
    g_get_council_instance = reinterpret_cast<fn_get_council_instance>(
        g_base_addr + ck3::funcs::GET_COUNCIL_INSTANCE);
    // NOTE: g_compute_war_score removed — calling FUN_066ba0d0 from tick hook
    // context crashes. Using battle_score memory reads instead.
}

// =============================================================================
// Liege Resolution Helper
// =============================================================================
// The liege is stored as CCharacter* at SLandedCharacterData + 0x448 (LIEGE_PTR).
// Round 3 confirmed this with 8 accessor functions.
// Previous code incorrectly read core_data + 0x274 which is NOT the liege.
// Returns 0 if independent or unlanded.
// Resolve title_realm (SLandedCharacterData) for a character, with fallback probing.
// Primary path: char+0x1b8 (TITLE_REALM_DATA).
// Fallback: if null but core_data::IS_RULER is set, probe neighboring pointer slots
// for a valid title_realm struct (handles +-8 byte layout shifts across patches).
uintptr_t resolve_title_realm(uintptr_t char_addr) {
    // ck-86ue.1: safe_read returns 0 on unmapped addresses, so we don't need an
    // entry is_mapped(char_addr) check. Derived pointers ARE validated below
    // before any further dereference.
    uintptr_t title_realm = safe_read<uintptr_t>(char_addr + ck3::character::TITLE_REALM_DATA);
    if (title_realm && is_mapped(title_realm)) return title_realm;

    // Fallback: probe neighboring slots (cross-patch layout shift)
    // core_data::IS_RULER at 0x54C is unreliable — always probe if primary fails
    static const size_t probe_offsets[] = { 0x1c0, 0x1b0 };
    for (size_t off : probe_offsets) {
        uintptr_t candidate = safe_read<uintptr_t>(char_addr + off);
        if (!candidate || !is_mapped(candidate)) continue;
        // Validate: TIER at +0x1D4 should be 1-5 for a landed ruler
        int32_t tier = safe_read<int32_t>(candidate + ck3::title_realm::TIER);
        if (tier < 1 || tier > 5) continue;
        // Validate: TITLE_ARRAY_COUNT should be positive and reasonable
        int32_t tcnt = safe_read<int32_t>(candidate + ck3::title_realm::TITLE_ARRAY_COUNT);
        if (tcnt <= 0 || tcnt >= 500) continue;
        if (g_logfile) {
            fprintf(g_logfile,
                "[CK3-Bridge] resolve_title_realm: char %u RECOVERED at +0x%zx (tier=%d, titles=%d)\n",
                safe_read<uint32_t>(char_addr + ck3::character::ID) & 0xFFFFFF, off, tier, tcnt);
            fflush(g_logfile);
        }
        return candidate;
    }
    if (g_logfile) {
        // ck-os50: FIRST PER CHARACTER, then a rollup. This line was 19-85% of
        // every bridge log in the d39-d50 corpus (d43: 195,823 of 229,852
        // lines) and was eating the 64MB rotation headroom that sealed
        // evidence depends on. Keyed on the character so every distinct fact
        // still reaches the log exactly once.
        static bridge::FirstPerKeyLog gate;
        const uint32_t cid =
            safe_read<uint32_t>(char_addr + ck3::character::ID) & 0xFFFFFF;
        const auto d = gate.observe(cid);
        if (d.log_line) {
            fprintf(g_logfile,
                "[CK3-Bridge] resolve_title_realm: char %u IS_RULER=1 but"
                " no valid title_realm found (first for this char)\n", cid);
            fflush(g_logfile);
        }
        if (d.log_rollup) {
            fprintf(g_logfile,
                "[CK3-Bridge] resolve_title_realm: ROLLUP %zu occurrence(s) over "
                "%zu distinct char(s); %zu repeat(s) suppressed\n",
                d.total, d.distinct, d.suppressed);
            fflush(g_logfile);
        }
    }
    return 0;
}

// FIX #liege_id (DB round-trip gate): validate a resolved liege candidate id.
// A candidate is accepted ONLY if it (a) is outside {0,0xFFFFFF,self} AND (b) names a
// REAL character in the DB whose entry pointer EQUALS the pointer we read the id from.
// The old code's reject-set {0,0xFFFFFF,self} was the ONLY validation, so a small
// garbage/stale char-id (e.g. liege_id=3 seen live mid-recompute) leaked through verbatim.
// lookup_character masks to 24 bits and compares the lower 24 bits, so round-trip equality
// holds for the masked ids the resolver already produces. Core-dump validation
// (ck3_20260421_195552): zero false-rejects across all 3684 genuine lieges.
static inline uint32_t validate_liege_id(uint32_t cand, uint32_t self_id, uintptr_t src_ptr) {
    if (cand == 0 || cand == 0xFFFFFF || cand == self_id) return 0;
    void* dbc = lookup_character(cand);
    if (!dbc || is_sentinel(dbc)) return 0;                       // id not a real character
    if (reinterpret_cast<uintptr_t>(dbc) != src_ptr) return 0;    // ptr != DB entry => stale/garbage
    return cand;
}

uint32_t get_liege_id_from_addr(uintptr_t char_addr) {
    uintptr_t title_realm = resolve_title_realm(char_addr);
    if (!title_realm) return 0;

    uint32_t self_id = safe_read<uint32_t>(char_addr + ck3::character::ID) & 0xFFFFFF;

    // FIX (independence-resolver): PATH 1 (title_realm+0x448 LIEGE_PTR) is UNRELIABLE in
    // this build. For an INDEPENDENT ruler it is either NULL (Asian tier-5 emperors) or a
    // non-null *mapped sentinel/economy* object whose char-id field reads 0xFFFFFF (and is
    // occasionally junk e.g. 0x6) — it does NOT point to a real liege CCharacter. The old
    // code returned `liege_ptr+0x18 & 0xFFFFFF` for any safe_ptr, so it leaked the 0xFFFFFF
    // sentinel (and self/garbage) as a bogus "liege id". We now (FIX #liege_id) require the
    // resolved id to ROUND-TRIP through the character DB (validate_liege_id): the DB entry
    // pointer for that id must equal the pointer we read it from. A small garbage/stale id
    // (the liege_id=3 symptom) no longer passes; we fall through to PATH 2, else return 0.
    uintptr_t liege_ptr = safe_read<uintptr_t>(title_realm + ck3::title_realm::LIEGE_PTR);
    if (safe_ptr(liege_ptr)) {
        uint32_t lid = safe_read<uint32_t>(liege_ptr + ck3::character::ID) & 0xFFFFFF;
        uint32_t v = validate_liege_id(lid, self_id, liege_ptr);
        if (v) return v;
        // else: sentinel/self/garbage/stale 0x448 -> fall through to PATH 2
    }

    // PATH 2: Realm parent at title_realm+0x1C0 -> +0x28 (mirroring FUN_067539c0).
    // This is the AUTHORITATIVE liege signal. A real, different, non-sentinel, DB-roundtrip
    // holder id => vassal. Null/0/0xFFFFFF/self/stale => independent (return 0).
    uintptr_t realm_parent = safe_read<uintptr_t>(title_realm + ck3::title_realm::REALM_PARENT_OBJ);
    if (safe_ptr(realm_parent)) {
        uintptr_t holder_ptr = safe_read<uintptr_t>(realm_parent + ck3::realm_parent::HOLDER_CHAR_PTR);
        if (safe_ptr(holder_ptr)) {
            uint32_t holder_id = safe_read<uint32_t>(holder_ptr + ck3::character::ID) & 0xFFFFFF;
            uint32_t v = validate_liege_id(holder_id, self_id, holder_ptr);
            if (v) return v;
        }
    }

    return 0;
}

// =============================================================================
// Minimal JSON Helpers — implementations live in json_helpers.cpp so they can
// be unit-tested without linking the rest of the bridge. Declarations are in
// action_dispatch_internal.h.
// =============================================================================

// =============================================================================
// String Hash -- calls the game's own hash function (FUN_08213140)
// Signature: uint32_t hash(uint32_t seed, const char* data, uint32_t len)
// =============================================================================

uint32_t game_string_hash(const char* data, uint32_t len) {
    auto hash_fn = reinterpret_cast<uint32_t(*)(uint32_t, const char*, uint32_t)>(
        g_base_addr + ck3::funcs::STRING_HASH);
    return hash_fn(0, data, len);
}

// =============================================================================
// Character Lookup (ID -> pointer via character DB)
// =============================================================================

void* lookup_character(uint32_t char_id) {
    // ck-86ue.1: safe_read for the DB pointer (g_base_addr is always mapped),
    // then is_mapped() guards on each derived pointer before further reads.
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::CHARACTER_DB);
    if (!db) return nullptr;

    uint32_t masked_id = char_id & 0xFFFFFF;
    uint32_t capacity = safe_read<uint32_t>(db + ck3::hash_map::CAPACITY);
    if (masked_id >= capacity) return nullptr;

    uintptr_t table = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    if (!table || !is_mapped(table)) return nullptr;

    uintptr_t entry_ptr = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                               + static_cast<uintptr_t>(masked_id) * ck3::hash_map::ENTRY_STRIDE);
    if (!entry_ptr || !is_mapped(entry_ptr)) return nullptr;

    uint32_t stored_id = safe_read<uint32_t>(entry_ptr + ck3::character::ID);
    // Compare lower 24 bits (slot index) -- the upper 8 bits are a generation byte that
    // changes when a slot is recycled.  The bridge outputs masked (24-bit) IDs everywhere,
    // so callers (agent, internal helpers) will typically pass masked IDs.  A full 32-bit
    // comparison would always fail for those callers, blocking ALL non-self character
    // lookups.  The generation-byte safety net is not needed here: the bridge only reads
    // state while the game is paused, slot recycling during a single pause is not a
    // concern, and downstream is_alive checks catch dead characters.
    if ((stored_id & 0xFFFFFF) != masked_id) {
        return reinterpret_cast<void*>(safe_read_offset<uintptr_t>(ck3::globals::CHAR_DB_SENTINEL));
    }

    return reinterpret_cast<void*>(entry_ptr);
}

// Returns the explicitly controlled character.
// M1: when a ClientContext is installed, returns the current session's primary
// character; otherwise falls back to the global legacy mirror. Does NOT fall
// back to PLAYER_CHAR_ID — the LLM controls AI characters, not the player.
// Call set_controlled_character first, or pass explicit character_id/actor_id.
void* get_controlled_character() {
    uint32_t cid = 0;
    bridge::Session* s = bridge::current_session();
    if (s) {
        std::lock_guard<std::mutex> lk(s->mutex);
        cid = s->primary_character_id;
    }
    if (cid == 0 || cid == 0xFFFFFFFF) {
        cid = g_controlled_primary.load(std::memory_order_relaxed);
    }
    if (cid == 0 || cid == 0xFFFFFFFF) return nullptr;
    return lookup_character(cid);
}

// Returns the engine's player character (information only — not for LLM actions).
uint32_t get_player_character_id() {
    return safe_read<uint32_t>(g_base_addr + ck3::globals::PLAYER_CHAR_ID);
}

// PLAYER-PROTECTION INVARIANT (co-inhabitant design): the agent inhabits an
// AI-controlled character and must NEVER act AS the human player. The player is
// a valid TARGET (declare war on, scheme against, marry) but never a valid
// ACTOR. Mutating handlers call this after resolving their actor; if the actor
// IS the player it writes a structured rejection to *out and returns true (the
// handler then returns false without doing anything). Read/query handlers do
// NOT call this — the agent must be able to observe/identify the player.
// In observer mode (no human player) PLAYER_CHAR_ID is 0/0xFFFFFF → no-op.
bool reject_player_actor(uint32_t actor_id, const char* action, std::string* out) {
    uint32_t pid = get_player_character_id() & 0xFFFFFF;
    if (pid == 0 || pid == 0xFFFFFF) return false;   // observer / no human player
    if ((actor_id & 0xFFFFFF) != pid) return false;  // actor is not the player
    if (g_logfile) fprintf(g_logfile,
        "[CK3-Bridge][PLAYER-GUARD] refused '%s': actor %u is the human player — "
        "the agent controls an AI character, not the player\n",
        action ? action : "(action)", pid);
    if (out) {
        ResponseBuilder r;
        r.begin_object();
        r.key("success");             r.value_bool(false);
        r.key("action");              r.value_string(action ? action : "");
        r.key("error");               r.value_string("cannot_act_as_player");
        r.key("reason");              r.value_string(
            "The agent inhabits an AI character and must not act as the human player. "
            "The player is a valid TARGET, not a valid ACTOR.");
        r.key("player_character_id"); r.value_uint(pid);
        r.end_object();
        *out = r.take();
    }
    return true;
}

// Convenience overload: resolve a character* to its id and gate it.
bool reject_player_actor(void* actor, const char* action, std::string* out) {
    if (!actor) return false;
    return reject_player_actor(char_get_id(actor), action, out);
}

// Resolve character_id param: if > 0 lookup that character, otherwise use g_controlled_primary only.
// No PLAYER_CHAR_ID fallback.
void* resolve_character(const char* params_json) {
    int cid = json_get_int(params_json, "character_id");
    if (cid <= 0) return get_controlled_character();
    return lookup_character(static_cast<uint32_t>(cid));
}

// Resolve actor from "actor_id" param, fallback to g_controlled_primary only.
// No PLAYER_CHAR_ID fallback.
void* resolve_actor(const char* params_json) {
    int actor_id = json_get_int(params_json, "actor_id");
    if (actor_id > 0) return lookup_character(static_cast<uint32_t>(actor_id));
    return get_controlled_character();
}

bool is_sentinel(void* ptr) {
    return ptr == reinterpret_cast<void*>(
        read_offset<uintptr_t>(ck3::globals::CHAR_DB_SENTINEL));
}

uint32_t char_get_id(void* character) {
    return read<uint32_t>(reinterpret_cast<uintptr_t>(character) + ck3::character::ID);
}

void* char_get_realm(void* character) {
    return read<void*>(reinterpret_cast<uintptr_t>(character) + ck3::character::REALM_DATA);
}

// =============================================================================
// Interaction/Decision Lookup
// =============================================================================

void* lookup_interaction(const char* interaction_type) {
    uintptr_t db = read_offset<uintptr_t>(ck3::globals::INTERACTION_DB);
    if (!db) return nullptr;

    uint32_t hash = game_string_hash(interaction_type, static_cast<uint32_t>(strlen(interaction_type)));
    uintptr_t table_base = read<uintptr_t>(db + ck3::interaction_ht::TABLE_OFFSET);
    uint32_t capacity_mask = read<uint32_t>(db + ck3::interaction_ht::CAPACITY_OFFSET);
    if (!table_base) return nullptr;

    int32_t idx = static_cast<int32_t>(capacity_mask & hash);
    uintptr_t entry = table_base + static_cast<uintptr_t>(idx) * ck3::interaction_ht::ENTRY_SIZE;

    if (read<uint8_t>(entry + 4) != 0) {
        uint8_t probe = 1;
        uintptr_t cur = entry;
        do {
            uint32_t entry_hash = read<uint32_t>(cur + 8);
            if (entry_hash == hash) {
                if (read<int8_t>(cur + 4) == -1) break;
                return read<void*>(cur + 0x10);
            }
            probe++;
            cur += ck3::interaction_ht::ENTRY_SIZE;
        } while (probe <= read<uint8_t>(cur + 0x1c - ck3::interaction_ht::ENTRY_SIZE));
    }

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] lookup_interaction('%s'): NOT FOUND (hash=0x%08x)\n",
            interaction_type, hash);
    return nullptr;
}

void* lookup_decision(const char* decision_type) {
    uintptr_t db = read_offset<uintptr_t>(ck3::globals::DECISION_DB);
    if (!db) return nullptr;

    uint32_t hash = game_string_hash(decision_type, static_cast<uint32_t>(strlen(decision_type)));
    uintptr_t table_base = read<uintptr_t>(db + ck3::decision_ht::TABLE_OFFSET);
    uint32_t capacity_mask = read<uint32_t>(db + ck3::decision_ht::CAPACITY_OFFSET);
    if (!table_base) return nullptr;

    int32_t idx = static_cast<int32_t>(capacity_mask & hash);
    uintptr_t entry = table_base + static_cast<uintptr_t>(idx) * ck3::decision_ht::ENTRY_SIZE;

    if (read<uint8_t>(entry + 4) != 0) {
        uint8_t probe = 1;
        uintptr_t cur = entry;
        do {
            if (read<uint32_t>(cur + 8) == hash) {
                if (read<int8_t>(cur + 4) == -1) break;
                return read<void*>(cur + 0x10);
            }
            probe++;
            cur += ck3::decision_ht::ENTRY_SIZE;
        } while (probe <= read<uint8_t>(cur + 0x1c - ck3::decision_ht::ENTRY_SIZE));
    }

    return nullptr;
}

// =============================================================================
// DB Object / String Helpers
// =============================================================================

const char* trait_category_name(uint8_t cat) {
    switch (cat) {
        case 0: return "personality";
        case 1: return "education";
        case 2: return "congenital";
        case 3: return "commander";
        case 4: return "commander";
        case 5: return "lifestyle";
        case 8: return "legend";
        default: return "other";
    }
}

bool read_db_obj_name(uintptr_t obj_ptr, const char*& name_out, size_t& len_out) {
    if (!obj_ptr) return false;
    uintptr_t name_addr = safe_read<uintptr_t>(obj_ptr + ck3::db_object::NAME_KEY);
    if (!name_addr || !is_mapped(name_addr)) return false;
    const char* name = reinterpret_cast<const char*>(name_addr);
    // Basic validation: first char must be printable ASCII
    uint8_t first_byte = safe_read<uint8_t>(name_addr);
    if (first_byte < 0x20 || first_byte > 0x7e) return false;
    len_out = safe_read<uint64_t>(obj_ptr + ck3::db_object::NAME_LENGTH);
    if (len_out == 0 || len_out > 256) return false;
    if (!is_mapped_range(name_addr, len_out)) return false;
    name_out = name;
    return true;
}

// Read std::__cxx11::basic_string from memory.
// Layout: [_M_p(8B)][_M_string_length(8B)][_M_local_buf(16B) or _M_allocated_capacity]
// _M_p always points to the string data (either local buf at str_addr+0x10 or heap).
// Routed through the canonical guard (bridge.h safe_read_game_string) after
// crash 20260625. This reader feeds the war-declaration hook (cb_name @
// war_declaration_hook.cpp:117), council/scheme/title name lookups and many
// per-tick query paths — all reading keys from game DB objects that churn under
// heavy war volume. The guard rejects bad lengths and bounds the copy via an
// EFAULT-safe process_vm_readv (never a direct std::string(ptr,size)).
std::string ad_read_std_string(uintptr_t str_addr) {
    return safe_read_game_string(str_addr);
}

uintptr_t ad_db_lookup(uintptr_t db_global, uint32_t id) {
    uintptr_t db = safe_read<uintptr_t>(g_base_addr + db_global);
    if (!db) return 0;
    uintptr_t table = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
    if (!table) return 0;
    uint32_t capacity = safe_read<uint32_t>(db + ck3::hash_map::CAPACITY);
    uint32_t masked = id & 0xFFFFFF;
    if (masked >= capacity) return 0;
    uintptr_t entry = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                           + static_cast<uintptr_t>(masked) * ck3::hash_map::ENTRY_STRIDE);
    if (!entry) return 0;
    if ((safe_read<uint32_t>(entry + ck3::hash_map::ENTRY_KEY_REF) & 0xFFFFFF) != masked) return 0;
    return entry;
}

// FIX #culture_name: emit culture_id + culture (display name, +0x260) + culture_key
// (scripted key, +0x1C8) for a culture obj. Pass the resolved CCulture* and masked id.
// Root cause of the prior bug: code read +0x10 (the uint32 culture ID) as a std::string,
// which always failed safe_ptr and emitted nothing. Safe for null/unmapped obj: emits id.
void emit_culture_fields(ResponseBuilder& r, uintptr_t culture_obj, uint32_t culture_id,
                         const char* id_key, const char* name_key, const char* script_key) {
    r.key(id_key); r.value_uint(culture_id & 0xFFFFFF);
    if (culture_obj && is_mapped(culture_obj)) {
        std::string disp = ad_read_std_string(culture_obj + culture::NAME);       // +0x260
        std::string key  = ad_read_std_string(culture_obj + culture::SCRIPT_KEY); // +0x1C8
        if (!disp.empty()) { r.key(name_key);   r.value_string(disp.c_str(), disp.size()); }
        if (!key.empty())  { r.key(script_key); r.value_string(key.c_str(),  key.size()); }
    }
}

void emit_culture_fields_by_id(ResponseBuilder& r, uint32_t culture_id,
                               const char* id_key, const char* name_key, const char* script_key) {
    uintptr_t obj = (culture_id != 0 && culture_id != 0xFFFFFFFF)
        ? ad_db_lookup(ck3::globals::CULTURE_INDEXED_DB, culture_id) : 0;
    emit_culture_fields(r, obj, culture_id, id_key, name_key, script_key);
}

// Read character name from memory.
std::string ad_read_char_name(uintptr_t c) {
    if (!safe_ptr(c)) return {};
    // Try the name cache first
    uint32_t char_id = safe_read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF;
    if (char_id != 0 && char_id != 0xFFFFFF) {
        std::string cached = get_cached_character_name(char_id);
        if (!cached.empty()) return cached;
    }
    // Fallback: return the raw loc key
    uintptr_t override_flag = safe_read<uintptr_t>(c + ck3::character::OVERRIDE_FLAG);
    uintptr_t name_addr = (override_flag != 0)
        ? (c + ck3::character::OVERRIDE_NAME)
        : (c + ck3::character::FIRST_NAME);
    return ad_read_std_string(name_addr);
}

void emit_char_names(ResponseBuilder& r, uintptr_t c) {
    // ck-86ue.1: early-return if c is not in mapped memory. All downstream
    // safe_read calls would return 0 on bad c (so no crash), but we'd emit
    // misleading "house_id=0" fields. Guard once at entry instead.
    if (!c || !is_mapped(c)) return;
    // Character name: stored directly on CCharacter at +0x20 (with override at +0x40)
    std::string name = ad_read_char_name(c);
    if (!name.empty()) {
        r.key("name"); r.value_string(name.c_str(), name.size());
    } else if (g_logfile) {
        uint32_t cid = safe_read<uint32_t>(c + ck3::character::ID) & 0xFFFFFF;
        fprintf(g_logfile, "[CK3-Bridge] emit_char_names: char %u name empty (override_flag=%p)\n",
                cid, (void*)safe_read<uintptr_t>(c + ck3::character::OVERRIDE_FLAG));
        fflush(g_logfile);
    }
    // House name
    uint32_t house_id = safe_read<uint32_t>(c + ck3::character::HOUSE_ID_DIRECT);
    if (house_id != 0 && house_id != 0xFFFFFFFF) {
        uintptr_t house_obj = ad_db_lookup(ck3::globals::HOUSE_DB, house_id);
        if (house_obj) {
            std::string hn = ad_read_std_string(house_obj + 0x10);
            if (!hn.empty()) { r.key("house_name"); r.value_string(hn.c_str(), hn.size()); }
        }
    }
    // Dynasty name
    // FIX I12 (#dynasty_fallback): family+0x10 is 0/0xFFFFFFFF for landed rulers, so
    // derive dynasty_id from the house (char+0x150 -> HOUSE_DB -> +0x24) as a fallback.
    uint32_t dynasty_id = 0;
    uintptr_t family = safe_read<uintptr_t>(c + ck3::character::FAMILY_DATA);
    if (family && is_mapped(family)) {
        uint32_t d = safe_read<uint32_t>(family + ck3::family::DYNASTY_ID);
        if (d != 0 && d != 0xFFFFFFFF) dynasty_id = d & 0xFFFFFF;
    }
    if (dynasty_id == 0 && house_id != 0 && house_id != 0xFFFFFFFF) {
        uintptr_t hobj = ad_db_lookup(ck3::globals::HOUSE_DB, house_id & 0xFFFFFF);
        if (hobj && is_mapped(hobj)) {
            uint32_t d = safe_read<uint32_t>(hobj + dynasty_house_obj::DYNASTY_ID);
            if (d != 0 && d != 0xFFFFFFFF) dynasty_id = d & 0xFFFFFF;
        }
    }
    if (dynasty_id != 0) {
        uintptr_t dyn_obj = ad_db_lookup(ck3::globals::DYNASTY_DB, dynasty_id);
        if (dyn_obj) {
            // FIX #is_kin side-fix D: dynasty NAME is at +0x80 (Dynasty_GetName), NOT
            // +0x10 (which is the dynasty ID). Prefer CUSTOM_NAME (+0x110) when set.
            std::string dn = ad_read_std_string(dyn_obj + dynasty_obj::CUSTOM_NAME);
            if (dn.empty()) dn = ad_read_std_string(dyn_obj + dynasty_obj::NAME);
            if (!dn.empty()) { r.key("dynasty_name"); r.value_string(dn.c_str(), dn.size()); }
        }
    }
    // Round 6: culture name — FIX #culture_name: was reading +0x10 (the uint32 culture ID)
    // as a std::string (always failed → empty). Route through emit_culture_fields which reads
    // NAME (+0x260) + SCRIPT_KEY (+0x1C8). Emits culture (display) + culture_key (scripted).
    // Note: emit_char_names already emitted culture_id elsewhere? It did not — but query_character
    // emits culture_id in its own block; here we use a distinct helper-emitted culture/culture_key
    // only (skip re-emitting culture_id to avoid a duplicate key in profiles that already emit it).
    {
        uint32_t culture_id = safe_read<uint32_t>(c + ck3::character::CULTURE_ID_DIRECT);
        if (culture_id != 0 && culture_id != 0xFFFFFFFF) {
            uintptr_t culture_obj = ad_db_lookup(ck3::globals::CULTURE_INDEXED_DB, culture_id);
            if (culture_obj && is_mapped(culture_obj)) {
                std::string cn = ad_read_std_string(culture_obj + culture::NAME);
                std::string ck = ad_read_std_string(culture_obj + culture::SCRIPT_KEY);
                if (!cn.empty()) { r.key("culture");     r.value_string(cn.c_str(), cn.size()); }
                if (!ck.empty()) { r.key("culture_key"); r.value_string(ck.c_str(), ck.size()); }
            }
        }
    }
    // Round 6: faith name
    uint32_t faith_id = safe_read<uint32_t>(c + ck3::character::FAITH_ID_DIRECT);
    if (faith_id != 0 && faith_id != 0xFFFFFFFF) {
        uintptr_t faith_obj = ad_db_lookup(ck3::globals::FAITH_INDEXED_DB, faith_id);
        if (faith_obj) {
            std::string fn = ad_read_std_string(faith_obj + 0x10);
            if (!fn.empty()) { r.key("faith"); r.value_string(fn.c_str(), fn.size()); }
        }
    }
}

// Helper: look up a title object by ID from the landed title DB
uintptr_t lookup_title_obj(uint32_t title_id) {
    // ck-86ue.1: safe_read for DB pointer, is_mapped() guards on table/entry.
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::TITLE_DB);
    if (!db) return 0;
    uint32_t masked = title_id & 0xFFFFFF;
    uint32_t capacity = safe_read<uint32_t>(db + ck3::title_db::ARRAY_COUNT);
    if (masked >= capacity) return 0;
    uintptr_t table = safe_read<uintptr_t>(db + ck3::title_db::ARRAY_PTR);
    if (!table || !is_mapped(table)) return 0;
    uintptr_t entry = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                       + static_cast<uintptr_t>(masked) * ck3::title_db::ELEMENT_STRIDE);
    if (!entry || !is_mapped(entry)) return 0;
    // Verify ID matches (masked comparison -- output serializes 24-bit IDs)
    if ((safe_read<uint32_t>(entry + ck3::title_obj::TITLE_ID) & 0xFFFFFF) != masked) return 0;
    return entry;
}

// Helper: get tier of a title object (1=barony..5=empire), returns 0 on failure
int get_title_tier(uintptr_t title_ptr) {
    if (!title_ptr) return 0;
    uintptr_t type_ptr = safe_read<uintptr_t>(title_ptr + ck3::title_obj::TITLE_TYPE_PTR);
    if (!type_ptr) return 0;
    return safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
}

// =============================================================================
// AI Control Actions
// =============================================================================

void disable_all_ai() {
    auto* flag = reinterpret_cast<uint8_t*>(g_base_addr + ck3::globals::AI_GLOBAL_TOGGLE);
    *flag = 0;
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Native AI disabled globally\n");
}

void enable_all_ai() {
    auto* flag = reinterpret_cast<uint8_t*>(g_base_addr + ck3::globals::AI_GLOBAL_TOGGLE);
    *flag = 1;
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Native AI re-enabled\n");
}

// =============================================================================
// Subsystem Toggles
// =============================================================================

const SubsystemToggle g_subsystem_toggles[] = {
    {"Decisions",              ck3::globals::AI_TOGGLE_BASE + 0x00},
    {"GrantRevokeTitles",      ck3::globals::AI_TOGGLE_BASE + 0x01},
    {"SetRealmCapital",        ck3::globals::AI_TOGGLE_BASE + 0x02},
    {"Lifestyles",             ck3::globals::AI_TOGGLE_BASE + 0x03},
    {"DynastyLegacies",        ck3::globals::AI_TOGGLE_BASE + 0x04},
    {"HousePower",             ck3::globals::AI_TOGGLE_BASE + 0x05},
    {"CultureTraditions",      ck3::globals::AI_TOGGLE_BASE + 0x06},
    {"DeclareWar",             ck3::globals::AI_TOGGLE_BASE + 0x07},
    {"Faction",                ck3::globals::AI_TOGGLE_BASE + 0x08},
    {"Marriages",              ck3::globals::AI_TOGGLE_BASE + 0x09},
    {"Council",                ck3::globals::AI_TOGGLE_BASE + 0x0A},
    {"CouncilTasks",           ck3::globals::AI_TOGGLE_BASE + 0x0B},
    {"SchemeCountermeasures",  ck3::globals::AI_TOGGLE_BASE + 0x0C},
    {"CallAllies",             ck3::globals::AI_TOGGLE_BASE + 0x0D},
    {"Development",            ck3::globals::AI_TOGGLE_BASE + 0x0E},
    {"ReligiousReformation",   ck3::globals::AI_TOGGLE_BASE + 0x0F},
    {"SuccessionElection",     ck3::globals::AI_TOGGLE_BASE + 0x10},
    {"SuccessionAppointment",  ck3::globals::AI_TOGGLE_BASE + 0x11},
    {"Goals",                  ck3::globals::AI_TOGGLE_BASE + 0x12},
    {"Interactions",           ck3::globals::AI_TOGGLE_BASE + 0x13},
    {"Laws",                   ck3::globals::AI_TOGGLE_BASE + 0x14},
    {"PrimaryTitle",           ck3::globals::AI_TOGGLE_BASE + 0x15},
    {"Capital",                ck3::globals::AI_TOGGLE_BASE + 0x16},
    {"ScriptedGui",            ck3::globals::AI_TOGGLE_BASE + 0x17},
    {"Artifacts",              ck3::globals::AI_TOGGLE_BASE + 0x18},
    {"CourtAmenities",         ck3::globals::AI_TOGGLE_BASE + 0x19},
    {"CourtTypes",             ck3::globals::AI_TOGGLE_BASE + 0x1A},
    {"CourtPositions",         ck3::globals::AI_TOGGLE_BASE + 0x1B},
    {"CourtLanguage",          ck3::globals::AI_TOGGLE_BASE + 0x1C},
    {"HostActivities",         ck3::globals::AI_TOGGLE_BASE + 0x1D},
    {"AttendOpenActivities",   ck3::globals::AI_TOGGLE_BASE + 0x1E},
    {"VassalStance",           ck3::globals::AI_TOGGLE_BASE + 0x1F},
    {"Accolades",              ck3::globals::AI_TOGGLE_BASE + 0x20},
    {"StationMenAtArms",       ck3::globals::AI_TOGGLE_BASE + 0x21},
    {"TaxCollector",           ck3::globals::AI_TOGGLE_BASE + 0x22},
    {"Legends",                ck3::globals::AI_TOGGLE_BASE + 0x23},
    {"Situations",             ck3::globals::AI_TOGGLE_BASE + 0x24},
    {"GreatProjects",          ck3::globals::AI_TOGGLE_BASE + 0x25},
};
const size_t g_subsystem_toggle_count = sizeof(g_subsystem_toggles) / sizeof(g_subsystem_toggles[0]);

// =============================================================================
// Controlled Character Management (Phase A)
// =============================================================================

bool do_set_controlled_character(const char* params_json, std::string* out) {
    int cid = json_get_int(params_json, "character_id");
    bool disable_ai = json_get_bool(params_json, "disable_native_ai", false);

    bridge::Session* session = bridge::current_session();

    if (session && session->is_observe()) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] set_controlled_character: rejected (observe mode)\n");
        if (out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_controlled_character");
            r.key("reason"); r.value_string("observe_mode");
            r.key("hint"); r.value_string("observe-mode sessions cannot claim characters");
            r.end_object();
            *out = r.take();
        }
        return false;
    }

    if (cid <= 0) {
        // Revert this session's primary to "none".  Legacy N=1 path also zeros the
        // global primary mirror when no session context is present.
        if (session) {
            {
                std::lock_guard<std::mutex> lk(session->mutex);
                session->primary_character_id = 0;
            }
            bridge::g_session_registry.rebuild_legacy_mirrors();
        } else {
            bridge::note_primary_change(0, bridge::kPrimaryCauseRelease);
        g_controlled_primary.store(0, std::memory_order_release);
        }
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] set_controlled_character: reverted to engine player\n");
        if (out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(true);
            r.key("action"); r.value_string("set_controlled_character");
            r.key("controlled_primary"); r.value_uint(0);
            r.key("note"); r.value_string("reverted to engine player character");
            r.end_object();
            *out = r.take();
        }
        return true;
    }

    uint32_t char_id = static_cast<uint32_t>(cid);
    // PLAYER-PROTECTION: the agent must never CONTROL the human player. Refuse
    // to set the controlled character to the player (the player has no AI brain
    // to steer; controlling them = hijacking the human). The player stays a
    // valid target for the agent's AI character, never the agent's identity.
    if (reject_player_actor(char_id, "set_controlled_character", out)) return false;
    void* ctrl = lookup_character(char_id);
    if (!ctrl || is_sentinel(ctrl)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] set_controlled_character: character %u not found\n", char_id);
        if (out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_controlled_character");
            r.key("reason"); r.value_string("character_not_found");
            r.key("character_id"); r.value_uint(char_id);
            r.end_object();
            *out = r.take();
        }
        return false;
    }

    if (!g_char_is_alive(ctrl)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] set_controlled_character: character %u is dead\n", char_id);
        if (out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_controlled_character");
            r.key("reason"); r.value_string("character_dead");
            r.key("character_id"); r.value_uint(char_id);
            r.end_object();
            *out = r.take();
        }
        return false;
    }

    // M1: claim ownership for this session.  Reject if another session already
    // owns the character.  (Legacy no-session path falls through to the global
    // roster-only behaviour.)
    if (session) {
        std::string claim_reason;
        // ck-swap / RULING THREE: `session(action=control)` calls the SWAP in one
        // IPC. A session that already steers somebody is SWITCHING, not adding —
        // which is what the tool's docstring has always promised and what the
        // handler never did. The add-not-switch defect is what accumulated a
        // seat's children for twenty years and saturated the global watchlist.
        //
        // swap_character is net zero, so this succeeds AT the cap where a claim
        // is refused; claim_seq never moves, so the succession classifier does
        // not read it as a seizure; and when the session holds nothing it
        // delegates to claim_character, so a first claim is still capped.
        uint32_t held_primary = 0;
        {
            std::lock_guard<std::mutex> lk(session->mutex);
            held_primary = session->primary_character_id;
        }
        // ck-6xi6: STAMP THE CAUSE BEFORE THE CLAIM, because the CLAIM is what
        // publishes the new primary — not the legacy-mirror store further down.
        // claim_character/swap_character rebuild the mirrors internally, so
        // g_controlled_primary is visible to the tick hook the moment either
        // returns, roughly fifty lines before the stamp used to run. That is why
        // ck-zgh4 read cause=unknown on the most ordinary control switch there is,
        // TWICE: my first fix moved the stamp ahead of do_set's own store, and
        // do_set's own store was never the publisher.
        //
        // Stamping this early is safe precisely BECAUSE the note is id-keyed: if
        // the claim is refused, the note names an id that never becomes primary,
        // and primary_change_cause_for only answers for an id it was actually
        // asked about. An early stamp can be unused; it cannot be wrong.
        bridge::note_primary_change(char_id, bridge::kPrimaryCauseControl);
        const bool ok = (held_primary != 0 && held_primary != char_id)
            ? bridge::g_session_registry.swap_character(
                  session, held_primary, char_id, &claim_reason)
            : bridge::g_session_registry.claim_character(
                  session, char_id, &claim_reason);
        if (!ok) {
            if (g_logfile) fprintf(g_logfile,
                "[CK3-Bridge] set_controlled_character: %u claim rejected (%s)\n",
                char_id, claim_reason.c_str());
            if (out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("set_controlled_character");
                r.key("reason"); r.value_string(claim_reason.c_str());
                r.key("character_id"); r.value_uint(char_id);
                r.key("session_id"); r.value_string(session->session_id.c_str());
                r.end_object();
                *out = r.take();
            }
            return false;
        }
    }

    // ck-sozz: the take's two remaining mutations — the legacy global-primary
    // write and the native-AI disable — USED TO RUN HERE, above the roster
    // check below.  An ownership_divergence / watchlist_full rejection then
    // returned success:false having already zeroed the engine's AI for the
    // character (and flipped g_agent_mode), with no undo: the same defect class
    // as the release-ordering bug this bead fixes, a mutation ahead of its own
    // validation.  They are packaged here and INVOKED AFTER the roster check,
    // which is the last thing that can refuse the take.
    bool ai_disabled_now = false;
    auto commit_take = [&]() {
        // ck-6kvq.11: on the SESSION path the global primary is chosen by the
        // registry (episode-manifest designation, else claim order) — see
        // SessionRegistry::rebuild_legacy_mirrors, which claim_character just
        // ran.  Storing char_id here would reinstate "last take wins", which is
        // exactly the N>1 nondeterminism that bead removes.  The legacy
        // no-session path has no registry behind it, so it still writes the
        // mirror directly.
        if (!session) {
            //: ck-6xi6: WHY the primary changed, recorded by the action that
            //: changed it, and stamped BEFORE the value is published.
            //:
            //: It was stamped after, and ck-zgh4 duly saw cause=unknown on the most
            //: ordinary control switch there is: the tick hook on another thread
            //: observed the new primary and read the cause register before this
            //: thread had written it, so the id did not match and the pairing check
            //: correctly refused to guess. The pairing check was right; the write
            //: order was wrong. Stamp first, publish second, and a reader that can
            //: see the new primary can already see why.
            //: The cause was stamped before the claim, which is where the primary
            //: actually becomes visible. This store only refreshes the legacy
            //: mirror; re-stamping here would be a second writer of the same fact.
            g_controlled_primary.store(char_id, std::memory_order_release);
        } else {
            //: ck-48z8: RECORD WHAT THIS CLAIM ASKED FOR, so the succession handoff
            //: can preserve it instead of inferring it from g_agent_mode, which is
            //: true whenever anyone holds anything at all.
            //:
            //: IN THE `session` BRANCH, and that placement is the whole point. My
            //: first version of this put the write inside `if (!session)` — the
            //: legacy no-session path — where `session->mutex` is a null
            //: dereference. It compiled, and the gate passed, because the host never
            //: exercises that path. The rebase conflict is what made me read the
            //: surrounding branch; nothing in my own lane did, because the row
            //: checked that the assignment EXISTS and not WHERE it is.
            std::lock_guard<std::mutex> lk(session->mutex);
            session->claim_disabled_native_ai = disable_ai;
        }
        // Disable native AI for this character if requested. When we actually
        // take over AI control, flip g_agent_mode so downstream gates
        // (disable_native_ai / observe-vs-control branching) see the
        // session has left passthrough.
        if (disable_ai && g_get_ai_ctrl && g_ai_char_toggle) {
            void* ai_ctrl = g_get_ai_ctrl(ctrl);
            if (ai_ctrl) {
                g_ai_char_toggle(ai_ctrl, 0);
                ai_disabled_now = true;
                g_agent_mode.store(true, std::memory_order_relaxed);
                if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] set_controlled_character: disabled native AI for %u (agent_mode=true)\n", char_id);
            }
        }
    };

#ifdef CK3_RELEASE_LEGACY_ORDER
    // ─── ck-sozz NEGATIVE CONTROL ────────────────────────────────────────────
    // Pre-fix position: mutate first, validate after.  Never in the library build.
    commit_take();
#endif

    // Roster maintenance.  On the SESSION path the roster is DERIVED from the
    // ownership table by rebuild_legacy_mirrors (which enforces the cap by
    // refusing the claim), so there is nothing to add here — only to verify.
    // On the legacy no-session path this is still the only writer.
    //
    // ck-6kvq.11 / design §1.5: a roster that disagrees with the ownership table
    // is an ERROR, not a log line.  The old code logged "watchlist full" and
    // answered success:true with the character missing from the roster, so the
    // caller believed it had control of a character no snapshot would ever
    // report.  Both paths below now fail loudly instead.
    size_t roster_size;
    bool roster_diverged = false;
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        bool present = std::find(g_controlled_chars.begin(), g_controlled_chars.end(),
                                 char_id) != g_controlled_chars.end();
        if (!present) {
            if (session) {
                // The registry accepted the claim but the derived roster does not
                // contain it — the two structures disagree.
                roster_diverged = true;
            } else if (g_controlled_chars.size() >= bridge::MAX_CONTROLLED_WATCHLIST) {
                roster_diverged = true;
            } else {
                g_controlled_chars.push_back(char_id);
            }
        }
        roster_size = g_controlled_chars.size();
    }

    if (roster_diverged) {
        std::string detail;
        bool registry_says = bridge::g_session_registry.ownership_divergence(&detail);
        const char* reason = (session || registry_says) ? "ownership_divergence"
                                                        : "watchlist_full";
        if (g_logfile) {
            fprintf(g_logfile,
                    "[CK3-Bridge] set_controlled_character: %s for %u — roster=%zu/%zu%s%s\n",
                    reason, char_id, roster_size, bridge::MAX_CONTROLLED_WATCHLIST,
                    detail.empty() ? "" : " detail=", detail.c_str());
            fflush(g_logfile);
        }
        // Undo the claim so the registry does not keep ownership of a character
        // the roster will never carry.  (Legacy path holds no claim to undo.)
        //: ck-qv51: EXPLICIT, and not a release. Called with no state this defaulted
        //: to Unknown — which ck-3ru8's census found reaching the handoff — and it
        //: also clobbered last_steered_character_id with a character this seat never
        //: steered, destroying a genuine earlier value. UndoneClaim says what this
        //: actually is, so the registry updates the primary and records nothing.
        if (session) bridge::g_session_registry.release_character(
                         session, char_id, bridge::ReleaseSubjectState::UndoneClaim);
        if (out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("set_controlled_character");
            r.key("reason"); r.value_string(reason);
            r.key("character_id"); r.value_uint(char_id);
            r.key("roster_size"); r.value_uint(static_cast<uint32_t>(roster_size));
            r.key("roster_cap");
            r.value_uint(static_cast<uint32_t>(bridge::MAX_CONTROLLED_WATCHLIST));
            if (!detail.empty()) { r.key("detail"); r.value_string(detail.c_str()); }
            r.end_object();
            *out = r.take();
        }
        return false;
    }

#ifndef CK3_RELEASE_LEGACY_ORDER
    // ── COMMIT: the roster check above was the last refusal point.
    commit_take();
#endif

    // ck-jij9: STAMPED. This was a raw fprintf with no monotonic stamp and no
    // tick, so a claim was locatable in the log only to the nearest 50-tick DIAG
    // marker — and ep-02's vacancies are 50 ticks, so the floor equalled the
    // phenomenon. The ledger's tick_executed stays the primary exact source; this
    // makes the log a full corroborator that can disagree with it.
    if (g_logfile) {
        char stamp[64];
        bridge::claim_event_stamp(stamp, sizeof stamp,
                                  bridge::g_tick_id.load(std::memory_order_relaxed));
        char mono[32];
        bridge::monotonic_stamp(mono, sizeof mono);
        fprintf(g_logfile,
                "%s[CK3-Bridge] CLAIM_SET_EVENT set_controlled_character: "
                "now controlling %u (roster size: %zu) %s\n",
                mono, char_id, roster_size, stamp);
        fflush(g_logfile);
    }

    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("set_controlled_character");
        r.key("controlled_primary"); r.value_uint(char_id);
        r.key("session_id"); r.value_string(session ? session->session_id.c_str() : "");
        r.key("ai_disabled"); r.value_bool(ai_disabled_now);
        r.key("agent_mode"); r.value_bool(g_agent_mode.load(std::memory_order_relaxed));
        r.key("roster_size"); r.value_uint(static_cast<uint32_t>(roster_size));
        // ck-xwlg P2: report the LIVE per-char AI gate (ai_ctrl+0x28) so every
        // take shows whether this char's AI tick — and hence war prosecution —
        // is actually running (catches a disable leaked by an earlier take).
        if (g_get_ai_ctrl) {
            void* ai_now = g_get_ai_ctrl(ctrl);
            if (ai_now) {
                r.key("ai_enabled");
                r.value_bool(safe_read<uint8_t>(
                    reinterpret_cast<uintptr_t>(ai_now) + ck3::ai_ctrl::AI_ENABLED) != 0);
            }
        }
        // Make the silent occupation-0 bleed observable: with native AI disabled,
        // CAIManager_MainTick skips this controller's per-char tick, so the war
        // coordinator never sieges an offensive war (occupation_score stays 0
        // forever). Release control or take control with disable_native_ai=false
        // while an offensive conquest war must be WON.
        // (re_control_suppresses_war.md fix #2.)
        if (ai_disabled_now)
            r.key("native_war_ai_note"),
              r.value_string("native AI disabled: offensive wars will NOT siege "
                             "(occupation stays 0) until control is released or "
                             "re-taken with disable_native_ai=false");
        r.end_object();
        *out = r.take();
    }
    return true;
}

bool do_release_character(const char* params_json, std::string* out) {
    int cid = json_get_int(params_json, "character_id");
    if (cid <= 0) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] release_character: missing character_id\n");
        if (out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("release_character");
            r.key("reason"); r.value_string("missing_character_id");
            r.end_object();
            *out = r.take();
        }
        return false;
    }

    uint32_t char_id = static_cast<uint32_t>(cid);
    bridge::Session* session = bridge::current_session();

    // ck-sozz: THE HANDLER IS ATOMIC IN EFFECT.  Everything down to the COMMIT
    // marker below is pure inspection: it either refuses and leaves the bridge
    // bit-for-bit unchanged, or it falls through to a single commit point after
    // which the WHOLE cleanup runs.  A partial release must not be reachable.
    //
    // It used to be: the M1 ownership release (which erases the ownership entry
    // AND rebuilds the legacy mirrors from the reduced table) ran FIRST, above
    // ck-60bz's roster guard — a guard whose own contract is "validate roster
    // membership BEFORE mutating anything".  The guard then inspected the state
    // its own predecessor had just created: roster empty, primary 0, so EVERY
    // release of a session-owned character answered not_in_roster / "nothing
    // mutated" while the claim was in fact already gone, and the early return
    // skipped the personality-lock erase, the council-lock clear and the native
    // AI re-enable.  6/6 failures in the first live N=6 run (2026-09-10).

    // ── INSPECT 1: observe sessions hold no control state, so nothing to release.
    if (session && session->is_observe()) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] release_character: rejected (observe mode)\n");
        if (out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("release_character");
            r.key("reason"); r.value_string("observe_mode");
            r.end_object();
            *out = r.take();
        }
        return false;
    }

#ifndef CK3_RELEASE_LEGACY_ORDER
    // ── INSPECT 2: ownership, WITHOUT mutating it.  M1: a control session may
    // only release characters it owns.  owner_of() is the read-only form of
    // release_character()'s precondition (ownership_[char_id] == session), so
    // this rejects exactly the same set — but leaves the table intact.  The
    // legacy no-session path holds no claims and keeps the roster-only check.
    if (session && bridge::g_session_registry.owner_of(char_id) != session) {
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] release_character: %u not owned by session %s — typed reject, nothing mutated\n",
            char_id, session->session_id.c_str());
        if (out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("release_character");
            r.key("reason"); r.value_string("not_owned_by_session");
            r.key("character_id"); r.value_uint(char_id);
            r.key("session_id"); r.value_string(session->session_id.c_str());
            r.end_object();
            *out = r.take();
        }
        return false;
    }
#else
    // ─── ck-sozz NEGATIVE CONTROL ────────────────────────────────────────────
    // The pre-fix code, reproduced exactly: the M1 ownership release runs HERE,
    // above the roster guard, so it erases the ownership entry and rebuilds the
    // legacy mirrors before the guard has looked at them.  Build with
    // -DCK3_RELEASE_LEGACY_ORDER to restore the bug and watch a successful
    // ownership release come back as {"success":false,"reason":"not_in_roster"}.
    // Never defined by the library build.
    if (session) {
        if (!bridge::g_session_registry.release_character(session, char_id)) {
            if (g_logfile) fprintf(g_logfile,
                "[CK3-Bridge] release_character: %u not owned by session %s — typed reject\n",
                char_id, session->session_id.c_str());
            if (out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("release_character");
                r.key("reason"); r.value_string("not_owned_by_session");
                r.key("character_id"); r.value_uint(char_id);
                r.key("session_id"); r.value_string(session->session_id.c_str());
                r.end_object();
                *out = r.take();
            }
            return false;
        }
    }
#endif

    // ── INSPECT 3 — ck-60bz (N3 straggler, verify-round3 §3d): validate roster
    // membership BEFORE mutating anything. The old body cleared
    // personality/council locks, re-enabled native AI and answered success:true
    // for ANY character_id (release_character(999999999) -> silent-accept). A
    // character is releasable iff it is in the controlled roster OR is the
    // current primary (the primary can be roster-less when the watchlist was
    // full at control time — no longer reachable since ck-6kvq.11 made that
    // divergence an error, but kept as a tolerant release path for legacy
    // callers).  On the session path the roster is DERIVED from the ownership
    // table INSPECT 2 just vouched for, so a session-owned character passes here
    // by construction; a disagreement is a divergence and refusing it is safe
    // precisely because nothing has been mutated yet.
    uint32_t primary_now = g_controlled_primary.load(std::memory_order_relaxed);
    bool in_roster;
    size_t roster_size_now;
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        in_roster = std::find(g_controlled_chars.begin(), g_controlled_chars.end(),
                              char_id) != g_controlled_chars.end();
        roster_size_now = g_controlled_chars.size();
    }
    if (!in_roster && char_id != primary_now) {
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] release_character: %u NOT controlled (roster size %zu, primary %u) — typed reject, nothing mutated\n",
            char_id, roster_size_now, primary_now);
        if (out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("release_character");
            r.key("reason"); r.value_string("not_in_roster");
            r.key("character_id"); r.value_uint(char_id);
            r.key("controlled_primary"); r.value_uint(primary_now);
            r.key("roster_size"); r.value_uint(static_cast<uint32_t>(roster_size_now));
            r.key("hint"); r.value_string(
                "character is not controlled by the bridge — nothing to release; "
                "check get_controlled_roster for the current roster");
            r.end_object();
            *out = r.take();
        }
        return false;
    }

#ifndef CK3_RELEASE_LEGACY_ORDER
    // NOTE ON THE #ifndef THIS SITS INSIDE: the guard is deliberately absent from
    // the CK3_RELEASE_LEGACY_ORDER build. That build is ck-sozz's negative control
    // and exists to reproduce the old release-first ordering; a refusal here would
    // return before that ordering ran, so the control would stop reproducing the
    // very bug it is the control for. The shipped library always has the guard.
    //
    // ── INSPECT 4 — ck-cwal (progenitor ruling 2026-09-24): A CONTROL SESSION
    // MAY NOT RELEASE ITS LAST CHARACTER.
    //
    // The registered design is STEER mode: the agent tunes personality weights and
    // the native AI executes. A control seat holding nothing has nothing to steer,
    // and the tool surface gave no way back — so the ruling is to refuse the
    // release and say why, rather than allow a seat to strand itself. Switching to
    // another character stays allowed; this only refuses EMPTYING the set.
    //
    // NO CARVE-OUTS ARE NEEDED, and that is measured rather than assumed. The bead
    // named two exemptions — the second half of a control switch, and a handoff
    // corpse release — and NEITHER reaches this handler:
    //   * a control switch goes through SessionRegistry::swap_character, which is
    //     net-zero under one lock and never calls release_character;
    //   * the succession handoff's removal of the corpse is that same swap, inside
    //     check_controlled_handoff, not a release request;
    //   * do_set_controlled_character's roster-divergence ROLLBACK calls
    //     g_session_registry.release_character DIRECTLY, bypassing this function —
    //     which is correct: undoing a claim that just failed must be allowed to
    //     empty the set.
    // So every release that arrives here is one an agent asked for, and an
    // exemption for a case that cannot arise would be a contract the tree does not
    // keep. acceptance_cwal asserts those three bypasses so nobody adds one later.
    if (session && session->is_control()) {
        bool would_empty = false;
        {
            std::lock_guard<std::mutex> lk(session->mutex);
            would_empty = session->claimed_character_ids.size() == 1
                       && session->claimed_character_ids.count(char_id) == 1;
        }
        if (would_empty) {
            if (g_logfile) {
                fprintf(g_logfile,
                        "[CK3-Bridge] release_character: REFUSED last claim of "
                        "control session %s (R3/§2.7 amendment) tick=%llu char=%u\n",
                        session->session_id.c_str(),
                        (unsigned long long)bridge::g_tick_id.load(
                            std::memory_order_relaxed),
                        char_id);
                fflush(g_logfile);
            }
            if (out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string("release_character");
                r.key("reason"); r.value_string("last_claim_of_control_session");
                //: The explanation the ruling asks for, carried on the refusal so
                //: an agent reading only the reply learns the contract.
                r.key("hint"); r.value_string(
                    "you steer; the AI runs regardless. A control session's last "
                    "character cannot be released — switch to another character "
                    "instead.");
                r.end_object();
                *out = r.take();
            }
            return false;   //: REFUSED with nothing mutated.
        }
    }

    // ── COMMIT: the single mutation point.  Every check above has passed, so
    // from here the handler runs to completion — ownership release AND the full
    // cleanup half (personality lock, council lock, native AI, roster, primary,
    // agent_mode, mirrors).  release_character() is re-checked rather than
    // assumed: if another thread released the same claim between INSPECT 2 and
    // here, this is still a refusal with nothing mutated, because the cleanup
    // has not started.
    //: ck-7yrp: MEASURE the subject's state HERE, at the release, because this is
    //: the last moment it is true. Evaluated at handoff time instead, a character
    //: released alive and dying later reads as a corpse release and the recovery
    //: branch seizes it — ep-03, 3100 ticks after a deliberate drop.
    bridge::ReleaseSubjectState rel_state = bridge::ReleaseSubjectState::Unknown;
    {
        void* rc = lookup_character(char_id);
        if (rc && !is_sentinel(rc) && g_char_is_alive) {
            rel_state = g_char_is_alive(rc) ? bridge::ReleaseSubjectState::Alive
                                            : bridge::ReleaseSubjectState::Corpse;
        }
        //: A character that does not resolve stays Unknown rather than being
        //: called a corpse — "I cannot see it" is not "it is dead", and treating
        //: it as one would re-create the seizure through a different door.
    }
    if (session && !bridge::g_session_registry.release_character(
                       session, char_id, rel_state)) {
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] release_character: %u ownership vanished between check and commit "
            "(session %s) — typed reject, nothing mutated\n",
            char_id, session->session_id.c_str());
        if (out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string("release_character");
            r.key("reason"); r.value_string("not_owned_by_session");
            r.key("character_id"); r.value_uint(char_id);
            r.key("session_id"); r.value_string(session->session_id.c_str());
            r.end_object();
            *out = r.take();
        }
        return false;
    }
#endif

    // Clear personality lock
    {
        std::unique_lock<std::shared_mutex> lk(g_personality_lock_mutex, std::try_to_lock);
        if (lk.owns_lock()) {
            g_personality_locks.erase(char_id);
        } else {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] release_character: personality mutex busy, deferring lock clear for %u\n", char_id);
        }
    }

    // Clear council suppression lock (L20 stickiness) — let the native
    // per-ruler council-AI resume for this ruler once released.
    council_lock_remove(char_id);

    // Re-enable native AI by direct memory reads + write.
    void* ctrl = lookup_character(char_id);
    if (ctrl && !is_sentinel(ctrl)) {
        uintptr_t c = reinterpret_cast<uintptr_t>(ctrl);
        uintptr_t core_data = read<uintptr_t>(c + 0x1A8);
        if (core_data) {
            uintptr_t ai_ctrl = read<uintptr_t>(core_data + ck3::core_data::AI_CONTROLLER);
            if (ai_ctrl) {
                // R3 (ck-6kvq.10, design §2.7) — the half of R3 that SURVIVED
                // the 2026-09-12 G3' reversal: `ai_ctrl+0x28` is NEVER
                // re-enabled for a claimed character in an EXPERIMENT episode.
                // Handing a character back to the native AI mid-episode changes
                // the arm — the very thing R3 exists to prevent — and it is not
                // undoable after the fact, because the native AI will have
                // acted. A silent agent's character keeps its last steer and the
                // silence is measured (unavailability.h), never healed by giving
                // the character away. The claim itself is dropped above (the
                // agent asked for that); what stays is the block on the ENGINE's
                // AI. Free play is not an episode and keeps
                // multi-agent-control.md Q5.
                if (bridge::episode_pins_claims()) {
                    if (g_logfile) fprintf(g_logfile,
                        "[CK3-Bridge] release_character: NOT re-enabling native AI for %u "
                        "— experiment mode; ai_ctrl+0x28 stays disabled for the "
                        "life of the episode (R3, §2.7)\n", char_id);
                } else {
                    *reinterpret_cast<uint8_t*>(ai_ctrl + ck3::ai_ctrl::AI_ENABLED) = 1;
                    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] release_character: re-enabled native AI for %u (pure memory path)\n", char_id);
                }
            } else {
                if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] release_character: no AI controller for %u (ai_ctrl=null)\n", char_id);
            }
        } else {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] release_character: no core_data for %u\n", char_id);
        }
    } else {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] release_character: character %u not found or sentinel\n", char_id);
    }

    // Remove from roster
    size_t roster_size;
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        auto it = std::find(g_controlled_chars.begin(), g_controlled_chars.end(), char_id);
        if (it != g_controlled_chars.end()) {
            g_controlled_chars.erase(it);
        }
        roster_size = g_controlled_chars.size();
    }

    // Reset primary if this was the primary
    if (g_controlled_primary.load(std::memory_order_relaxed) == char_id) {
        bridge::note_primary_change(0, bridge::kPrimaryCauseRelease);
        g_controlled_primary.store(0, std::memory_order_release);
    }

    // When the last controlled character is released, drop agent_mode
    // back to passthrough — no character is under agent AI control.
    if (roster_size == 0) {
        g_agent_mode.store(false, std::memory_order_relaxed);
    }

    // M1: if a session released its last claim, rebuild legacy mirrors so the
    // global state reflects the remaining sessions.
    if (session) {
        bridge::g_session_registry.rebuild_legacy_mirrors();
    }

    // ck-jij9: STAMPED, for the same reason as the claim above.
    if (g_logfile) {
        char stamp[64];
        bridge::claim_event_stamp(stamp, sizeof stamp,
                                  bridge::g_tick_id.load(std::memory_order_relaxed));
        char mono[32];
        bridge::monotonic_stamp(mono, sizeof mono);
        fprintf(g_logfile,
                "%s[CK3-Bridge] CLAIM_SET_EVENT release_character: "
                "released %u (roster size: %zu) %s\n",
                mono, char_id, roster_size, stamp);
        fflush(g_logfile);
    }

    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("release_character");
        r.key("released_id"); r.value_uint(char_id);
        r.key("controlled_primary"); r.value_uint(g_controlled_primary.load(std::memory_order_relaxed));
        r.key("session_id"); r.value_string(session ? session->session_id.c_str() : "");
        r.key("agent_mode"); r.value_bool(g_agent_mode.load(std::memory_order_relaxed));
        r.key("roster_size"); r.value_uint(static_cast<uint32_t>(roster_size));
        r.end_object();
        *out = r.take();
    }
    return true;
}

bool do_list_watchlist(const char* /*params_json*/, std::string* out) {
    std::vector<uint32_t> roster;
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        roster = g_controlled_chars;
    }
    uint32_t primary = g_controlled_primary.load(std::memory_order_relaxed);

    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("action"); r.value_string("list_watchlist");
        r.key("controlled_primary"); r.value_uint(primary);
        r.key("roster_size"); r.value_uint(static_cast<uint32_t>(roster.size()));
        r.key("characters"); r.begin_array();
        for (uint32_t cid : roster) {
            r.value_uint(cid);
        }
        r.end_array();
        r.end_object();
        *out = r.take();
    }
    return true;
}

// =============================================================================
// Bridge State Recovery Query
// =============================================================================

// ck-ngoh1: Multi-polecat dispatch needs a way to ask "who currently owns the
// bridge socket?" before slinging work. Returns the peer's pid/uid (from
// SO_PEERCRED at accept time), the fd number, how long the slot has been
// held, and a live kill(pid, 0) probe so the caller can distinguish a healthy
// owner from a stale lock. Classified as bridge-internal so it serves even
// when the game is DORMANT/paused — that's exactly when the caller needs it.
bool do_who_holds_the_lock(const char* /* params_json */, std::string* out) {
    ClientLockInfo info{};
    get_client_lock_info(&info);
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("who_holds_the_lock");
    r.key("success"); r.value_bool(true);
    r.key("connected"); r.value_bool(info.connected);
    r.key("fd"); r.value_int(info.fd);
    r.key("peer_pid"); r.value_int(info.peer_pid);
    r.key("peer_uid"); r.value_int(info.peer_uid);
    r.key("peer_gid"); r.value_int(info.peer_gid);
    r.key("connection_age_seconds");
        r.value_uint(static_cast<uint32_t>(info.connection_age_seconds));
    r.key("peer_alive"); r.value_bool(info.peer_alive);
    r.end_object();
    if (out) *out = r.take();
    return true;
}

bool do_get_bridge_state(const char* /* params_json */, std::string* out) {
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("get_bridge_state");
    r.key("success"); r.value_bool(true);

    // Controlled character IDs -- snapshot under lock
    std::vector<uint32_t> chars_snapshot;
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        chars_snapshot = g_controlled_chars;
    }
    r.key("controlled_primary"); r.value_uint(g_controlled_primary.load(std::memory_order_relaxed));
    // Player-protection: surface the human player's character so the agent can
    // IDENTIFY them (and treat them as a target) but knows it must never act AS
    // them. 0xFFFFFF = observer / no human player. The agent's controlled_primary
    // is guaranteed never to equal this (set_controlled_character refuses it).
    r.key("player_character_id"); r.value_uint(get_player_character_id() & 0xFFFFFF);
    r.key("controlled_chars"); r.begin_array();
    for (uint32_t cid : chars_snapshot) {
        r.value_uint(cid);
    }
    r.end_array();

    // Subsystem mask per character
    r.key("subsystem_masks"); r.begin_object();
    for (uint32_t cid : chars_snapshot) {
        std::string sub_json;
        get_character_subsystems(cid, &sub_json);
        char key_buf[16];
        snprintf(key_buf, sizeof(key_buf), "%u", cid);
        // FIX I11: splice the pre-formed subsystem object verbatim instead of
        // double-encoding it as a quoted string. get_character_subsystems always
        // returns a complete {...} object (known-valid invariant holds here).
        r.key(key_buf); r.value_raw(sub_json);
    }
    r.end_object();

    // Current tick_id
    r.key("tick_id"); r.value_uint(g_tick_id.load(std::memory_order_relaxed));

    // Session / bridge state
    const char* state_str = session_state_string();
    r.key("bridge_state"); r.value_string(state_str);
    r.key("session_state"); r.value_string(state_str);

    // Intent queue
    r.key("pending_intents"); r.value_uint(static_cast<uint32_t>(g_intent_queue.pending_count()));

    // H3 (re_warplanner_lifecycle.md §1.4): CB-slot busy-return counter. Tells us
    // whether the H1 bounded-drain hardening reduced slot collisions in the live
    // run. Instrumentation only — no behavioural effect.
    r.key("cb_query_busy_count");
    r.value_uint(static_cast<uint32_t>(g_cb_query_busy_count.load(std::memory_order_relaxed)));

    // ck-p2sb: the deferred coordinator's two typed refusals. Reported HERE,
    // beside cb_query_busy_count, because that is the counter this is modelled
    // on and a reader looking for "did the bridge refuse anything" should find
    // them together rather than in two surfaces. 0 is a real statement —
    // the bridge looked and neither fired; the keys being ABSENT means an older
    // bridge that could not say.
    r.key("busy_deferred_count");
    r.value_uint(static_cast<uint32_t>(g_busy_deferred_count.load(std::memory_order_relaxed)));
    r.key("drain_unavailable_count");
    r.value_uint(static_cast<uint32_t>(g_drain_unavailable_count.load(std::memory_order_relaxed)));

    // FIX I05: agent-control mode (matches set_controlled_character / release_character)
    r.key("agent_mode"); r.value_bool(g_agent_mode.load(std::memory_order_relaxed));

    // ck-6kvq.11 (R2): the session registry itself. SessionRegistry::sessions_json
    // has existed since M1 with NO caller — the ownership table was unreadable
    // from every client, so "get_controlled_roster agrees with the registry" was
    // a claim nothing could check, and the N>1 defects stayed invisible. Each
    // entry carries claim_seq, which is what elects the global primary, so the
    // choice is auditable rather than inferred. Read-only; observe sessions see
    // it too, which is the point — the meter lane verifies ownership without
    // owning anything.
    r.key("sessions"); r.value_raw(bridge::g_session_registry.sessions_json());
    // ck-7rk6: CUMULATIVE since process start, GLOBAL (one number, not per
    // session), and the two outcomes SEPARATE — settled with ck-9rto before
    // either side cut. Cumulative because a bridge-side per-interval delta drops
    // firings whenever a sample is missed, and a missed sample is exactly the
    // condition under which a collision is most likely; their block already
    // carries samples_truncated so a reader diffing across a gap can see it.
    // Separate because a redirect means the seat SURVIVED and an exhaustion
    // means it ENDED — a sum would report a survivor where there was a casualty.
    // ALWAYS EMITTED, zero included: an absent field is indistinguishable from
    // an older binary that never had one.
    r.key("skip_held_heir");
    r.value_uint(static_cast<uint32_t>(
        bridge::g_skip_held_heir_fired.load(std::memory_order_relaxed)));
    r.key("skip_held_heir_exhausted");
    r.value_uint(static_cast<uint32_t>(
        bridge::g_skip_held_heir_exhausted.load(std::memory_order_relaxed)));

    // Design §1.5: a watchlist/ownership divergence is an ERROR, not a log line.
    // It was only visible on the set_controlled_character reply; surfacing the
    // latch here means any client — including the observer — can see it.
    {
        std::string divergence_detail;
        // ck-bzcw / ck-9rto piece 8: ROSTER OCCUPANCY, as the pair.
        //
        // A roster can sit AT the cap and refuse nothing if no seat happens to
        // try, so a refusal count alone cannot distinguish "the surface was
        // open" from "nobody tested it". Occupancy shows saturation either way.
        //
        // THE CAP IS A VALUE, not just the count: MAX_CONTROLLED_WATCHLIST is a
        // compile-time constant a future deploy could change, and a bundle
        // recording only the count would be uninterpretable against a different
        // build. Same rule as ceiling_bridge_resolved recording the number
        // beside the env name rather than assuming it.
        //
        // The count comes from controlled_claim_count(), which is the SAME
        // function claim_character reaches through to enforce the cap — so this
        // number is the number that refuses, by construction and not by two
        // implementations happening to agree.
        r.key("controlled_watchlist"); r.begin_object();
        r.key("count");
        r.value_uint(static_cast<uint32_t>(
            bridge::g_session_registry.controlled_claim_count()));
        r.key("cap");
        r.value_uint(static_cast<uint32_t>(bridge::MAX_CONTROLLED_WATCHLIST));
        r.end_object();

        bool diverged = bridge::g_session_registry.ownership_divergence(&divergence_detail);
        r.key("ownership_divergence"); r.value_bool(diverged);
        if (diverged) {
            r.key("ownership_divergence_detail");
            r.value_string(divergence_detail.c_str());
        }
    }

    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// Player / Roster Queries
// =============================================================================

bool do_get_player_character(const char* /*params_json*/, std::string* out) {
    uint32_t player_id = read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
    if (!out) return true;
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("get_player_character");
    r.key("player_character_id"); r.value_uint(player_id & 0xFFFFFF);
    r.end_object();
    *out = r.take();
    return true;
}

bool do_get_controlled_roster(const char* /*params_json*/, std::string* out) {
    if (!out) return true;
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("get_controlled_roster");

    uint32_t primary = g_controlled_primary.load(std::memory_order_relaxed);
    r.key("primary_character_id"); r.value_uint(primary & 0xFFFFFF);

    r.key("roster"); r.begin_array();
    {
        std::lock_guard<std::mutex> lock(g_controlled_mutex);
        for (uint32_t cid : g_controlled_chars) {
            r.value_uint(cid & 0xFFFFFF);
        }
    }
    r.end_array();

    uint32_t player_id = read_offset<uint32_t>(ck3::globals::PLAYER_CHAR_ID);
    r.key("player_character_id"); r.value_uint(player_id & 0xFFFFFF);

    r.end_object();
    *out = r.take();
    return true;
}

// =============================================================================
// Safety gate for UNVERIFIED game-function Execute writes (2026-06-18).
// These call native Execute/effect/command-dispatch paths whose execution
// safety has NOT been live-validated. An unverified Execute can corrupt game
// state and crash a LATER tick — outside any per-handler sigsetjmp net, so the
// crash-net cannot contain it. To protect autonomous runs, they are DISABLED by
// default and return a clean rejection. Enable ONE AT A TIME for controlled
// validation by listing the action name in <state dir>/ck3-enabled-writes (the file is
// re-read on every call, so no relaunch needed). Once a write is proven safe,
// either keep it in that file or remove it from this list.
// =============================================================================
// <state dir>/ck3-enabled-writes: /tmp in legacy layout, else the private dir.
// Opened through the verified state-dir fd (O_NOFOLLOW), never by path.
static constexpr const char* kEnabledWritesName = "ck3-enabled-writes";

static bool experimental_write_enabled(const char* action) {
    static const char* kExperimental[] = {
        // hire_mercenary REMOVED 2026-06-18 — VALIDATED working (live: company 17
        // hired for ruler 13043, gold charged 1051.73→820.2; non-queue hire effect
        // applies). set_title_law REMOVED 2026-06-19 — goes through the real
        // add_title_law engine path (FUN_061d14f0, non-queue); success mirrors the
        // engine's own SResult verdict (honest rejects return the real "Couldn't add
        // law" tooltip — no false-success risk), so it applies when the engine allows.
        // Both now permanently enabled.
        // maa_recruit REMOVED 2026-06-19 — VALIDATED working via the full
        // CCreateMAARegimentCommand ctor→Execute→base_dtor path. re_create_maa_regiment_command.md.
        //
        // 2026-06-19 (workflow ck3-remaining-writes-resolution): fixes applied + live-validated.
        //  - appoint_councillor ENABLED — SWAP_COUNCIL_RAW_LEAF corrected 0x068b4d10→0x067b4d10
        //    (was an IMAGE VA; +0x100000 bug) + 4-arg sig (council,owner,char_a,char_b). Live:
        //    ruler 11515 swapped seats 7315<->7316, applied=1, no crash. Occupied-swap only
        //    (vacant gated as vacant_fill_unsupported). re_resolution_appoint.md.
        //  - set_doctrine ENABLED — faith_lookup fixed + arg order corrected to [doctrine,faith]
        //    (was reversed → always "Invalid doctrine"). Live: faith 23 (Catholicism) added
        //    tenet_communion/doctrine_pluralism_fundamentalist/etc, engine "Added ... doctrine".
        //    ADD-only (no same-group removal). re_resolution_faith.md.
        //  - maa_station/unstation ENABLED — leaf offsets corrected -0x100000 (0x065981e0/
        //    0x06599050; were image VAs → crash) + full-ref reconstruction (leaf validates
        //    regobj+0x10==cmd+0x20 on the FULL ref; roster passes masked) + location_id is a
        //    1-based PROVINCE index (cmd+0x24, via FUN_0632a6a0). Live (handler-side
        //    regiment+0x118 readback): unstation cleared origin→sentinel; station(provIdx=1)
        //    set origin→province#1; both CHANGED, no crash. re_resolution_maa_station.md.
        //    NOTE: agent needs province-index discovery (a query exposing realm province
        //    indices) for optimal stationing; unstation needs only regiment_id.
        "faction_action",
        // #132 scheme_action — NEW engine-write territory (CreateActiveScheme forward,
        // touches the scheme manager + InitializeScheme + agent staffing). Self-gated
        // by the engine's own scheme-start predicates, wrapped in the standard fake-RNG
        // + watchdog + sigsetjmp crash-net, but NOT yet live-validated. Disabled by
        // default; enable for controlled validation by adding "scheme_action" to
        // /tmp/ck3-enabled-writes (re-read every call — no relaunch). Once a live murder
        // is confirmed crash-free and query(schemes) shows it owned, remove from here.
        "scheme_action",
        // break_alliance — the literal CBreakAllowAllianceEffect break. OFFSETS
        // PINNED (alliance::BREAK_ALLIANCE_*) + chain HIGH-confidence, but the
        // handler is observer-save-GATED: it makes NO engine call yet (drain path
        // + direct-cmd arg ABI unverified — re_break_alliance_impl.md §6). Listed
        // here so that, once the observer-save smoke confirms the arg unwrap and
        // the game-thread drain path lands, the real break is double-gated behind
        // /tmp/ck3-enabled-writes for controlled validation rather than reachable
        // by default. The safe-now ally-call-refusal lever is the `conqueror`-var
        // scripted-effect via execute_scripted_effect.
        "break_alliance",
    };
    bool is_exp = false;
    for (const char* e : kExperimental) if (strcmp(action, e) == 0) { is_exp = true; break; }
    if (!is_exp) return true;  // not gated
    FILE* f = bridge::instance_state_fopen(kEnabledWritesName, "r");
    if (!f) return false;
    // rc2 B1: an exact line match (surrounding whitespace ignored), not a
    // substring: "scheme_action_x" must not enable "scheme_action".
    char line[256];
    bool on = false;
    const size_t alen = strlen(action);
    while (!on && fgets(line, sizeof line, f)) {
        const char* b = line;
        while (*b == ' ' || *b == '\t') ++b;
        size_t len = strlen(b);
        while (len && (b[len - 1] == '\n' || b[len - 1] == '\r' ||
                       b[len - 1] == ' ' || b[len - 1] == '\t')) --len;
        on = (len == alen && memcmp(b, action, alen) == 0);
    }
    fclose(f);
    return on;
}

// =============================================================================
// Action-Log JSONL Tap (overlay-v2 §2.2)
//
// Append-only feed of state-mutating bridge RPCs:
// {ts, game_date, session_id, kind, verb, args, result}. kind is "action" or
// "management" (clock, per-character AI, steering flags; see action_log_kind).
// Lives at <telemetry>/action_log.jsonl (per instance since ck-qn03). Reads
// and diagnostic management RPCs are excluded. The overlay renderer
// humanizes these verbs into the kill-feed; the run monitor consumes them as
// clip-candidate signals.
// =============================================================================

static std::mutex g_action_log_mutex;

static std::string format_action_log_timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    auto tt = system_clock::to_time_t(now);
    struct tm utc;
    gmtime_r(&tt, &utc);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec,
             static_cast<int>(ms.count()));
    return std::string(buf);
}

static std::string format_action_log_game_date() {
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (!gs) return "unknown";
    int32_t raw_date = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
    if (raw_date == 0) return "unknown";
    char buf[16];
    ck3::game_date::format_raw(raw_date, buf, sizeof(buf));  // ck-05mg
    return std::string(buf);
}

bool is_bridge_management_verb(const char* action_type) {
    static const char* mgmt[] = {
        // pause/speed
        "pause_game", "unpause_game", "set_speed", "pause", "set_pause",
        // AI subsystem toggles (bridge-local bookkeeping)
        "toggle_all_ai", "enable_all_ai", "toggle_ai_subsystem",
        "set_character_ai", "set_character_subsystems",
        // bridge automation / observer
        "set_auto_respond_events", "set_controlled_character", "release_character",
        "select_observe_default", "enter_observe_mode", "inject_mouse_click",
        // steering overrides (bridge-local flags consumed by hooks)
        "set_cb_selector_override", "set_offgoal_veto",
        "set_break_alliance_enabled", "set_vacant_fill_enabled",
        "set_force_offensive_stance", "set_war_stance_record_walk",
        // diagnostics / meta
        "who_holds_the_lock", "get_bridge_state", "get_offset_validation", "get_tick_info",
        // misc reads mis-routed as actions
        "evaluate_named_trigger", "tick_diff",
        "pending_events", "notifications", "query_pending_events", "event_probabilities",
        "memory_probe", "resolve_global",
        // validation helpers (reads, not writes)
        "pre_validate_war", "construction_check",
        // ck-6kvq.13 (R6): the budget governor's own surface. Bridge-local
        // bookkeeping, and the way a SPENT session reports the usage that will
        // eventually let it out — so it must never be gated by the budget.
        "report_usage", "set_budget_config", "get_budget_state",
        // ck-6kvq.5 / ck-9fc3: the save-identity declaration. Bridge-local
        // bookkeeping — it writes nothing to the world, it states WHICH FILE is
        // loaded. It belongs here for a second reason: as a TICK_WRITE the flush
        // loop would BUFFER it until the next flush, and the whole point of
        // declaring on resume is that the steering decision lands BEFORE the
        // next tick applies (or fails to apply) anything.
        "declare_save_identity",
    };
    for (const char* m : mgmt) {
        if (strcmp(action_type, m) == 0) return true;
    }
    return false;
}

// =============================================================================
// ck-6kvq.13 / R6 / decision G7 — THE DECISION-BUDGET GOVERNOR, BRIDGE SIDE
// =============================================================================
// Design of record: docs/design/event-pipeline-conductor.md §5. The supervisor
// tails its agent's claude-CLI transcript and reports RAW TOKEN COUNTS; the
// BRIDGE owns the ledger, the game year and the refusal. An agent cannot be
// trusted to stop itself, and the only thing that can actually stop it acting
// is the thing that decides whether its calls are accepted.
//
// The year key is unforgeable on purpose: a supervisor reports tokens, never a
// date. The year comes from the engine's own calendar below.

namespace {
// The env-configured ceiling is PINNED — see budget_config_is_pinned().
bool g_budget_pinned = false;
}  // namespace

void budget_observe_engine_clock() {
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (!gs) return;
    int32_t raw_date = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
    if (raw_date == 0) return;        // unreadable: never move the year
    char buf[16];
    ck3::game_date::format_raw(raw_date, buf, sizeof(buf));   // ck-05mg calendar
    budget_ledger().observe_clock(ck3::game_date::raw_to_year(raw_date), buf);
}

bool budget_config_is_pinned() { return g_budget_pinned; }

void budget_init_from_env() {
    // The ceiling is a PER-EPISODE CONFIG VALUE (§5.3: 1.5 x the median
    // per-agent per-game-year spend across the A and B pilots, written into the
    // pre-registration at freeze). It arrives as an environment variable set by
    // whoever launches the game, which is exactly the place an agent cannot
    // reach. Unset => no cap: record everything, refuse nothing.
    const char* raw = getenv("CK3_BUDGET_PER_AGENT_PER_YEAR");
    int64_t ceiling = 0;
    if (raw && raw[0]) {
        char* end = nullptr;
        long long v = strtoll(raw, &end, 10);
        if (end && *end == '\0' && v > 0) {
            ceiling = static_cast<int64_t>(v);
            g_budget_pinned = true;
        } else if (g_logfile) {
            fprintf(g_logfile, "[CK3-Bridge][budget] ignoring unparseable "
                    "CK3_BUDGET_PER_AGENT_PER_YEAR=%s\n", raw);
        }
    }
    // Record-without-refusing, for a pilot episode that must measure the median
    // the ceiling will later be derived FROM.
    const char* enf = getenv("CK3_BUDGET_ENFORCE");
    const bool enforce = !(enf && (strcmp(enf, "0") == 0 || strcmp(enf, "off") == 0 ||
                                   strcmp(enf, "false") == 0));
    budget_ledger().configure(ceiling, enforce);
    if (g_logfile) {
        fprintf(g_logfile,
                "[CK3-Bridge][budget] per_agent_per_year=%lld enforce=%s pinned=%s\n",
                static_cast<long long>(ceiling), enforce ? "yes" : "no",
                g_budget_pinned ? "yes" : "no");
        fflush(g_logfile);
    }
}

void budget_emit_state(ResponseBuilder& r) {
    BudgetLedger& L = budget_ledger();
    r.key("budget"); r.begin_object();
    r.key("enabled");            r.value_bool(L.enabled());
    r.key("per_agent_per_year"); r.value_int(L.ceiling());
    r.key("armed");              r.value_bool(L.armed());
    r.key("pinned");             r.value_bool(g_budget_pinned);
    r.key("game_year");          r.value_int(L.game_year());
    r.key("resets_at");          r.value_string(L.resets_at().c_str());
    r.key("refusals");           r.value_int(L.refusal_count());
    r.key("clock_regressions");  r.value_int(L.clock_regressions());
    std::vector<BudgetRow> rows = L.rows();
    int32_t year = L.game_year();
    r.key("exhausted_sessions"); r.begin_array();
    for (const BudgetRow& row : rows) {
        if (row.game_year == year && row.exhausted) r.value_string(row.session_id.c_str());
    }
    r.end_array();
    r.end_object();
}

// --- handlers ---------------------------------------------------------------

// report_usage — the supervisor's one call. ADDITIVE ONLY: usage can never be
// reported downward (budget_ledger clamps every per-field delta at zero), and a
// session may only report for ITSELF when a session context exists, so one
// agent cannot spend another's allowance.
bool do_report_usage(const char* params_json, std::string* out) {
    Session* session = current_session();
    std::string sid = session ? session->session_id : std::string();
    const char* explicit_sid = json_get_string(params_json, "session_id");
    if (sid.empty()) sid = explicit_sid ? explicit_sid : "anon";

    budget_observe_engine_clock();

    BudgetUsage u;
    u.output_tokens         = json_get_int(params_json, "output_tokens");
    u.uncached_input_tokens = json_get_int(params_json, "uncached_input_tokens");
    u.cache_read_tokens     = json_get_int(params_json, "cache_read_tokens");
    u.cache_write_tokens    = json_get_int(params_json, "cache_write_tokens");
    const int64_t rounds    = json_get_int(params_json, "rounds");
    const char* msg_id      = json_get_string(params_json, "message_id");

    BudgetVerdict v = budget_ledger().report(
        sid, msg_id ? msg_id : "", u, rounds);

    if (v.crossed && g_logfile) {
        fprintf(g_logfile,
                "[CK3-Bridge][budget] session %s spent its %lld-token decision "
                "budget for game year %d at %s; mutations refused until %s\n",
                sid.c_str(), static_cast<long long>(v.ceiling),
                static_cast<int>(v.game_year), v.game_date.c_str(),
                v.resets_at.c_str());
        fflush(g_logfile);
    }

    ResponseBuilder r; r.begin_object();
    r.key("action");     r.value_string("report_usage");
    r.key("success");    r.value_bool(true);
    r.key("session_id"); r.value_string(sid.c_str());
    r.key("game_year");  r.value_int(v.game_year);
    r.key("game_date");  r.value_string(v.game_date.c_str());
    r.key("spend");      r.value_int(v.spend);
    r.key("ceiling");    r.value_int(v.ceiling);
    r.key("exhausted");  r.value_bool(!v.allowed);
    r.key("crossed");    r.value_bool(v.crossed);
    r.key("resets_at");  r.value_string(v.resets_at.c_str());
    r.key("budget_state"); r.value_string(v.allowed ? "ok" : "exhausted");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// set_budget_config — arming for a host test or a dev run. REFUSED when the
// ceiling came from the environment, because a ceiling an agent's own traffic
// can move is not a ceiling.
bool do_set_budget_config(const char* params_json, std::string* out) {
    ResponseBuilder r; r.begin_object();
    r.key("action"); r.value_string("set_budget_config");
    if (budget_config_is_pinned()) {
        r.key("success"); r.value_bool(false);
        r.key("reason");  r.value_string("budget_config_pinned");
        r.key("hint");    r.value_string(
            "The per-episode ceiling was set by CK3_BUDGET_PER_AGENT_PER_YEAR at "
            "launch and cannot be changed from inside the session.");
        r.key("per_agent_per_year"); r.value_int(budget_ledger().ceiling());
        r.end_object();
        if (out) *out = r.take();
        return false;
    }
    const int64_t ceiling = json_get_int(params_json, "per_agent_per_year");
    const bool enforce = json_get_bool(params_json, "enforce", true);
    budget_ledger().configure(ceiling, enforce);
    if (g_logfile) {
        fprintf(g_logfile, "[CK3-Bridge][budget] configured per_agent_per_year=%lld "
                "enforce=%s\n", static_cast<long long>(ceiling), enforce ? "yes" : "no");
    }
    r.key("success"); r.value_bool(true);
    r.key("per_agent_per_year"); r.value_int(budget_ledger().ceiling());
    r.key("enabled"); r.value_bool(budget_ledger().enabled());
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// get_budget_state — the bundle payload, read-only, safe in every state.
bool do_get_budget_state(const char* /* params_json */, std::string* out) {
    budget_observe_engine_clock();
    ResponseBuilder r; r.begin_object();
    r.key("action");  r.value_string("get_budget_state");
    r.key("success"); r.value_bool(true);
    r.key("budget");  r.value_raw(budget_ledger().export_json());
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// =============================================================================
// ck-6kvq.12 / R4 — THE SHARED APPEND-ONLY BOARD, BRIDGE SIDE
// =============================================================================
// Design of record: docs/design/event-pipeline-conductor.md §3 (decisions G4,
// G5). The board itself is PURE (board.{h,cpp}); these three handlers are the
// only place it meets the game, and the only thing they add is the part that
// cannot be pure: the ATTRIBUTION, minted here from the session registry and
// the clock owner, and never read from the caller's frame.
//
// THE ARM GATE lives further down, in dispatch_action() and
// is_registered_action(), because "arm A has no board" has to be true of the
// REGISTRY and not merely of the handler — an agent that can see `board_post`
// refuse is in a third condition, which is exactly what G5's option A2 rejects.

namespace {

// The unforgeable half of a post (§3.1). session_id and character_id come from
// the ownership table via the installed ClientContext; game_date and tick come
// from the clock owner; flush_id comes from the conductor that is applying this
// call. Nothing here is reachable from the wire.
BoardAttribution mint_board_attribution() {
    BoardAttribution who;
    if (Session* s = current_session()) {
        who.session_id = s->session_id;
        std::lock_guard<std::mutex> lk(s->mutex);
        who.character_id = s->primary_character_id;
    }
    // NO FALLBACK TO g_controlled_primary, deliberately, and unlike the
    // pipeline's ExecutionStamp. That global is the GLOBAL primary — somebody
    // else's character in a six-session episode — and writing it here would
    // attribute a post to a character its author does not own, which is the one
    // thing this whole file exists to prevent. A session that has claimed
    // nothing speaks for nobody, and `character_id: 0` says exactly that.
    who.game_date = format_action_log_game_date();   // ck3::game_date, ck-05mg
    who.tick      = g_tick_id.load(std::memory_order_relaxed);
    who.flush_id  = pipeline_current_flush_id();
    return who;
}

}  // namespace

// board_post {text} -> {success, post_id, seq, game_date, tick, flush_id}
// The frozen shape, per R4 "design frozen before episode one" as corrected by
// the 2026-09-12 vocabulary ruling (window_id -> flush_id).
bool do_board_post(const char* params_json, std::string* out) {
    const BoardAttribution who = mint_board_attribution();
    const BoardPostResult res = board().post(params_json, who);

    ResponseBuilder r; r.begin_object();
    r.key("action");  r.value_string("board_post");
    r.key("success"); r.value_bool(res.ok);
    if (!res.ok) {
        r.key("reason"); r.value_string(res.reason.c_str());
        if (!res.quota.empty()) {
            r.key("quota");    r.value_string(res.quota.c_str());
            r.key("limit");    r.value_int(res.limit);
            r.key("observed"); r.value_int(res.observed);
        }
        r.key("hint"); r.value_string(
            "The board refuses rather than truncating or dropping: it is "
            "append-only and every accepted post is in the episode record.");
        r.end_object();
        if (out) *out = r.take();
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge][board] post refused: %s%s%s (session=%s flush=%llu)\n",
            res.reason.c_str(), res.quota.empty() ? "" : " quota=",
            res.quota.c_str(), who.session_id.c_str(),
            (unsigned long long)who.flush_id);
        return false;
    }

    r.key("post_id");      r.value_string(res.post.post_id.c_str());
    r.key("seq");          r.value_uint(res.post.seq);
    r.key("session_id");   r.value_string(res.post.who.session_id.c_str());
    r.key("character_id"); r.value_uint(res.post.who.character_id);
    r.key("game_date");    r.value_string(res.post.who.game_date.c_str());
    r.key("tick");         r.value_uint(res.post.who.tick);
    r.key("flush_id");     r.value_uint(res.post.who.flush_id);
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// board_read {since_seq} -> {posts:[{seq, session_id, character_id, game_date,
//                                    tick, flush_id, text}], next_seq}
// READ-ONLY (session.cpp's kReadonly), so an observe-mode session — the meter
// lane, the harness draining board.jsonl — reads the board freely and can never
// post to it.
bool do_board_read(const char* params_json, std::string* out) {
    int64_t since = json_get_int(params_json, "since_seq");
    if (since < 0) since = 0;
    int64_t limit = json_get_int(params_json, "limit");
    if (limit < 0) limit = 0;

    // Splice the board's own page body (posts/next_seq/total/truncated) rather
    // than re-rendering it here, so there is exactly ONE renderer of a post and
    // the wire shape and board.jsonl cannot drift apart.
    const std::string page = board().read_json(static_cast<uint64_t>(since),
                                               static_cast<size_t>(limit));

    ResponseBuilder r; r.begin_object();
    r.key("action");  r.value_string("board_read");
    r.key("success"); r.value_bool(true);
    std::string body = r.take();          // "{...true" — take() trims the comma
    if (page.size() >= 2) {
        body += ",";
        body += page.substr(1, page.size() - 2);   // merge the page's members
    }
    body += "}";
    if (out) *out = body;
    return true;
}

// board_admin {op, host_token} — the HOST-ONLY lifecycle verb (§3.2: "created
// and sealed by the episode harness"). It is host-only in the one way an agent
// cannot route around: the token is read from the GAME PROCESS'S ENVIRONMENT,
// which the harness set when it launched CK3 and which no agent can read. With
// no CK3_BOARD_ADMIN_TOKEN configured the verb is not registered at all.
bool do_board_admin(const char* params_json, std::string* out) {
    const char* want = getenv("CK3_BOARD_ADMIN_TOKEN");
    const char* got  = json_get_string(params_json, "host_token");
    ResponseBuilder r; r.begin_object();
    r.key("action"); r.value_string("board_admin");
    if (!want || !*want || !got || strcmp(want, got) != 0) {
        r.key("success"); r.value_bool(false);
        r.key("reason");  r.value_string("board_admin_forbidden");
        r.key("hint");    r.value_string(
            "board_admin requires the host token the harness set in the game "
            "process's environment; it is not an agent-reachable verb.");
        r.end_object();
        if (out) *out = r.take();
        return false;
    }

    const char* op = json_get_string(params_json, "op");
    if (op && strcmp(op, "seal") == 0) {
        board().seal();
    } else if (op && strcmp(op, "begin") == 0) {
        const char* eid = json_get_string(params_json, "episode_id");
        board().begin_episode(eid ? eid : "");
        // ck-81su: and rotate the carrier with it. An explicit host `begin` is
        // the ONE thing allowed to discard a board — a relaunch is not, which is
        // the whole bead. The carrier is per-episode-id, so a new episode gets a
        // new file and cannot read the previous one even by accident.
        board_carrier_begin(eid ? eid : "");
    } else {
        r.key("success"); r.value_bool(false);
        r.key("reason");  r.value_string("board_admin_unknown_op");
        r.end_object();
        if (out) *out = r.take();
        return false;
    }

    r.key("success"); r.value_bool(true);
    r.key("op");      r.value_string(op);
    r.key("board");   r.value_raw(board().stats_json());
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// rc2 B8: the clock, per-character AI and steering-flag verbs are logged with
// kind "management"; other management verbs and reads are not logged.
static const char* action_log_kind(const char* action_type) {
    if (!action_type || !action_type[0]) return nullptr;
    static const char* const kLoggedMgmt[] = {
        "pause_game", "unpause_game", "pause", "set_pause", "set_speed",
        "set_character_ai", "set_character_subsystems",
        "set_offgoal_veto", "set_force_offensive_stance",
    };
    for (const char* m : kLoggedMgmt)
        if (strcmp(action_type, m) == 0) return "management";
    // Exclude pure game-state queries.
    if (is_game_state_query(action_type)) return nullptr;
    // Exclude read families by prefix.
    if (strncmp(action_type, "query_", 6) == 0) return nullptr;
    if (strncmp(action_type, "list_", 5) == 0) return nullptr;
    if (strncmp(action_type, "get_", 4) == 0) return nullptr;
    if (strncmp(action_type, "probe_", 6) == 0) return nullptr;
    // Exclude the remaining bridge-management and read helpers.
    if (is_bridge_management_verb(action_type)) return nullptr;
    return "action";
}

static void emit_action_log(const char* verb, const char* kind, const char* args_json,
                            const char* result_json) {
    if (!verb || !verb[0]) return;
    Session* session = current_session();

    // Build the JSONL line. args_json and result_json are already JSON objects
    // when populated; splice them raw. Fall back to {} / null on empty/malformed.
    std::string line;
    line.reserve(128 + (args_json ? strlen(args_json) : 2) + (result_json ? strlen(result_json) : 4));
    line += "{\"ts\":\"";
    line += format_action_log_timestamp();
    line += "\",\"game_date\":\"";
    line += format_action_log_game_date();
    auto append_escaped = [&line](const char* str) {
        for (const char* p = str; *p; ++p) {
            if (*p == '"' || *p == '\\') line += '\\';
            if (static_cast<unsigned char>(*p) < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned char>(*p));
                line += esc;
            } else {
                line += *p;
            }
        }
    };
    line += "\",\"session_id\":\"";
    append_escaped(session ? session->session_id.c_str() : "");
    line += "\",\"kind\":\"";
    line += kind;
    line += "\",\"verb\":\"";
    append_escaped(verb);
    line += "\",\"args\":";
    line += (args_json && args_json[0] == '{') ? args_json : "{}";
    line += ",\"result\":";
    line += (result_json && result_json[0] == '{') ? result_json : "null";
    line += "}";

    if (!bridge::instance_private_dir_ok()) return;   // rc2 B1: no writes in an untrusted dir
    std::lock_guard<std::mutex> lock(g_action_log_mutex);
    // ck-qn03: the telemetry sink follows the instance, so two episodes on one
    // host do not interleave their action logs into one file.
    ::mkdir(bridge::instance_telemetry_dir(), bridge::instance_dir_mode());
    std::string log_path = std::string(bridge::instance_telemetry_dir()) + "/action_log.jsonl";
    FILE* f = bridge::instance_open_private_log(log_path.c_str(), false);
    if (f) {
        ::fputs(line.c_str(), f);
        ::fputc('\n', f);
        ::fclose(f);
    }
}

// =============================================================================
// Inline-case adapters for the action registry (D1-C / FIX 3.1)
// =============================================================================
// Bodies that used to live inline in the hand-written dispatch chain, lifted
// into named functions with the uniform bool(const char*, std::string*)
// handler signature so the single source-of-truth table (action_registry.inc)
// can reference them. Behavior is unchanged: handlers that write no response
// still rely on dispatch_action_with_response to synthesize
// {"success":<bool>,"action":...}.

// ck-60bz (N3 straggler, verify-round3 §3d): json_get_bool's silent
// default-to-false meant enabled:"banana" (or a missing key) DISABLED all AI
// with a synthesized success:true. Strict-parse the boolean and typed-reject
// anything else — same contract as set_cb_selector_override's
// invalid_enabled_type reject (ai_cb_selector_hook.cpp:380).
static bool case_toggle_all_ai(const char* params_json, std::string* response_out) {
    bool enabled = false;
    if (!json_get_bool_strict(params_json, "enabled", &enabled)) {
        const char* reason = json_has_key(params_json, "enabled")
            ? "invalid_enabled_type" : "missing_enabled";
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] toggle_all_ai REJECTED (%s) — no AI state changed\n", reason);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action");  r.value_string("toggle_all_ai");
            r.key("reason");  r.value_string(reason);
            r.key("hint");    r.value_string(
                "enabled must be a JSON boolean (true/false), not a string or number");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }
    if (enabled) enable_all_ai(); else disable_all_ai();
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success");        r.value_bool(true);
        r.key("action");         r.value_string("toggle_all_ai");
        r.key("all_ai_enabled"); r.value_bool(enabled);
        r.end_object();
        *response_out = r.take();
    }
    return true;
}

static bool case_enable_all_ai(const char* /*params_json*/, std::string* /*response_out*/) {
    enable_all_ai();
    return true;
}

static bool case_pause_game(const char* params_json, std::string* /*response_out*/) {
    return do_pause_game(params_json);
}

static bool case_unpause_game(const char* params_json, std::string* /*response_out*/) {
    return do_unpause_game(params_json);
}

static bool case_set_speed(const char* params_json, std::string* /*response_out*/) {
    return do_set_speed(params_json);
}

static bool case_toggle_ai_subsystem(const char* params_json, std::string* /*response_out*/) {
    return do_toggle_ai_subsystem(params_json);
}

static bool case_set_character_ai(const char* params_json, std::string* response_out) {
    bool ok = do_set_character_ai(params_json);
    if (response_out) {
        bool enabled = json_get_bool(params_json, "enabled", true);
        int cid = json_get_int(params_json, "character_id");
        ResponseBuilder rb;
        rb.begin_object();
        rb.key("success"); rb.value_bool(ok);
        rb.key("action"); rb.value_string("set_character_ai");
        rb.key("character_id"); rb.value_int(cid);
        rb.key("ai_enabled"); rb.value_bool(ok ? enabled : true);
        rb.end_object();
        *response_out = rb.take();
    }
    return ok;
}

static bool case_set_character_subsystems(const char* params_json, std::string* response_out) {
    int cid = json_get_int(params_json, "character_id");
    // Resolve char_id=0 to controlled character (no PLAYER_CHAR_ID fallback)
    if (cid <= 0) {
        uint32_t resolved = g_controlled_primary.load(std::memory_order_relaxed);
        if (resolved == 0 || resolved == 0xFFFFFFFF) {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] set_character_subsystems: no controlled character\n");
            if (response_out) *response_out = R"({"success":false,"action":"set_character_subsystems","reason":"no_controlled_character"})";
            return false;
        }
        cid = static_cast<int>(resolved);
    }
    if (reject_player_actor(static_cast<uint32_t>(cid), "set_character_subsystems", response_out))
        return true;  // response written; action refused
    // Pass full params_json -- handler searches for subsystem name keys directly
    bool ok = set_character_subsystems(static_cast<uint32_t>(cid), params_json);
    // Bug B fix: construct response JSON
    if (response_out) {
        ResponseBuilder rb;
        rb.begin_object();
        rb.key("success"); rb.value_bool(ok);
        rb.key("action"); rb.value_string("set_character_subsystems");
        rb.key("character_id"); rb.value_uint(static_cast<uint32_t>(cid));
        // HONESTY (#125): the mask is bookkeeping only — no dispatch hook
        // consumes it, so toggling a subsystem off does NOT gate engine
        // behaviour. Most importantly CallAllies=false does NOT prevent the
        // engine's call-to-arms auto-join. Surface this so the agent stops
        // relying on a dead lever and uses alliance hygiene instead.
        rb.key("engine_enforced"); rb.value_bool(false);
        if (strstr(params_json, "\"CallAllies\"")) {
            rb.key("warning"); rb.value_string(
                "CallAllies toggle is bookkeeping only and does NOT suppress "
                "the engine's ally call-to-arms auto-join. To stay out of ally "
                "offensive wars: never form/renew liability alliances (#122), "
                "monitor query(domain=call_to_arms), and bias defensive when an "
                "offensive call lands. See re_alliance_hygiene_125.md.");
        }
        rb.end_object();
        *response_out = rb.take();
    }
    return ok;
}

static bool case_get_character_subsystems(const char* params_json, std::string* response_out) {
    int cid = json_get_int(params_json, "character_id");
    // Resolve char_id=0 to controlled character (no PLAYER_CHAR_ID fallback)
    if (cid <= 0) {
        uint32_t resolved = g_controlled_primary.load(std::memory_order_relaxed);
        if (resolved == 0 || resolved == 0xFFFFFFFF) {
            if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] get_character_subsystems: no controlled character\n");
            if (response_out) *response_out = R"({"success":false,"action":"get_character_subsystems","reason":"no_controlled_character"})";
            return false;
        }
        cid = static_cast<int>(resolved);
    }
    return get_character_subsystems(static_cast<uint32_t>(cid), response_out);
}

static bool case_get_offset_validation(const char* /*params_json*/, std::string* response_out) {
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("get_offset_validation");
    r.key("success"); r.value_bool(true);
    auto result = validate_offsets();
    r.key("total_groups"); r.value_int(result.total_groups);
    r.key("groups_passed"); r.value_int(result.groups_passed);
    r.key("groups_failed"); r.value_int(result.groups_failed);
    r.key("total_checks"); r.value_int(result.total_checks);
    r.key("checks_passed"); r.value_int(result.checks_passed);
    r.key("checks_failed"); r.value_int(result.checks_failed);
    r.key("failure_pct"); r.value_double(static_cast<double>(result.failure_pct));
    r.key("degraded"); r.value_bool(result.degraded);
    r.key("groups"); r.begin_array();
    for (int i = 0; i < result.total_groups; i++) {
        r.begin_object();
        r.key("name"); r.value_string(offset_validation_group_name(i));
        r.key("passed"); r.value_int(result.group_checks_passed[i]);
        r.key("failed"); r.value_int(result.group_checks_failed[i]);
        r.key("total"); r.value_int(result.group_checks_total[i]);
        r.end_object();
    }
    r.end_array();
    r.end_object();
    if (response_out) *response_out = r.take();
    return true;
}

// Toggle the (default-OFF) war-stance coordinator record walk at runtime.
// { "enabled": <bool> }. Pure flag store — no game-thread reentry.
static bool case_set_war_stance_record_walk(const char* params_json, std::string* response_out) {
    // json_get_int cannot read a JSON boolean: {"enabled": false} parsed as an
    // int yielded a non-zero result, so a disarm silently ARMED the flag while
    // reporting success (found 2026-08-22 while chasing ck-26fm — the flag is
    // the leading suspect for a deterministic game-thread hang, and it could
    // not be turned off). `true` happened to work, which hid this. Accept the
    // JSON literals explicitly before falling back to the integer form.
    bool enabled;
    if (!json_get_bool_strict(params_json, "enabled", &enabled)) {
        enabled = json_get_int(params_json, "enabled") != 0;  // legacy 1/0 form
    }
    g_war_stance_record_walk_enabled.store(enabled, std::memory_order_relaxed);
    if (response_out) {
        *response_out = std::string(
            "{\"success\":true,\"action\":\"set_war_stance_record_walk\","
            "\"enabled\":") + (enabled ? "true" : "false") + "}";
    }
    return true;
}

static bool case_list_interactions(const char* /*params_json*/, std::string* response_out) {
    if (response_out) {
        static std::string int_result;
        int_result = enumerate_interactions();
        *response_out = int_result.c_str();
    }
    return true;
}

// #64: this route always serves the LIGHT name+hash list (enumerate_decisions) —
// no engine eval, always safe/fast. The ANNOTATED engine eval lives on the separate
// list_decisions_annotated / list_valid_decisions routes (bounded list-mode,
// deferred to the game thread). It is NOT disabled — but the FULL-DB eval remains
// unsafe because the engine masks SIGALRM during certain trigger hangs, so no
// watchdog (on any thread) can bound it (re_decision_eval_hang.md). The annotated
// routes mitigate via bounded list-mode + culprit skip-list + self-heal, not a
// thread move. See re_decision_eval_64_NEXTBATCH.md.
static bool case_list_decisions(const char* /*params_json*/, std::string* response_out) {
    if (response_out) {
        static std::string dec_result;
        dec_result = enumerate_decisions();
        *response_out = dec_result.c_str();
    }
    return true;
}

static bool case_set_auto_respond_events(const char* params_json, std::string* response_out) {
    bool enabled = json_get_bool(params_json, "enabled", true);
    g_auto_respond_events.store(enabled, std::memory_order_relaxed);
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Auto-respond events: %s\n", enabled ? "enabled" : "disabled");
    if (response_out) {
        char buf[128];
        snprintf(buf, sizeof(buf), R"({"success":true,"auto_respond_events":%s})",
                 enabled ? "true" : "false");
        *response_out = buf;
    }
    return true;
}

// Shared by the "pause" and "set_pause" registry entries (the two dispatch
// cases were byte-identical). Differs from pause_game/unpause_game only in
// param shape ({"paused":bool} vs none).
static bool case_set_pause(const char* params_json, std::string* /*response_out*/) {
    bool paused = json_get_bool(params_json, "paused", true);
    if (paused) game_pause(); else game_unpause();
    return true;
}

static bool refuse_debug_verb(const char* verb, std::string* response_out);

// --- Query dispatch (new tool schema names): the `query` meta-router ---
static bool case_query_meta(const char* params_json, std::string* response_out) {
    const char* query_type = json_get_string(params_json, "type");
    if (!query_type) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query: missing 'type'\n");
        return false;
    }

    // Route to existing query handlers
    if (strcmp(query_type, "controlled_character") == 0 || strcmp(query_type, "my_character") == 0 || strcmp(query_type, "character") == 0) {
        return do_query_character(params_json, response_out);
    }
    if (strcmp(query_type, "controlled_realm") == 0 || strcmp(query_type, "my_realm") == 0 || strcmp(query_type, "realm") == 0) {
        return do_query_realm(params_json, response_out);
    }
    if (strcmp(query_type, "controlled_military") == 0 || strcmp(query_type, "my_military") == 0 || strcmp(query_type, "armies") == 0) {
        return do_query_armies(params_json, response_out);
    }
    if (strcmp(query_type, "controlled_economy") == 0 || strcmp(query_type, "my_economy") == 0) {
        return do_query_county_details(params_json, response_out);
    }
    if (strcmp(query_type, "available_decisions") == 0) {
        if (response_out) {
            static std::string dec_result;
            dec_result = enumerate_decisions();
            *response_out = dec_result.c_str();
        }
        return true;
    }
    if (strcmp(query_type, "available_interactions") == 0) {
        if (response_out) {
            static std::string int_result;
            int_result = enumerate_interactions();
            *response_out = int_result.c_str();
        }
        return true;
    }
    if (strcmp(query_type, "notifications") == 0 || strcmp(query_type, "pending_events") == 0) {
        return do_query_pending_events(params_json, response_out);
    }
    if (strcmp(query_type, "event_probabilities") == 0) {
        return do_query_event_probabilities(params_json, response_out);
    }
    if (strcmp(query_type, "war_details") == 0) {
        if (response_out) *response_out = "{\"note\":\"war detail offsets not yet mapped\"}";
        return true;
    }
    if (strcmp(query_type, "scheme_details") == 0) {
        if (response_out) *response_out = "{\"note\":\"scheme detail offsets not yet mapped\"}";
        return true;
    }
    if (strcmp(query_type, "succession") == 0) {
        return do_query_succession(params_json, response_out);
    }
    // rc2 B2: debug query types. Refused (and their handlers not linked)
    // unless built with CK3_BRIDGE_DEBUG_VERBS.
    if (refuse_debug_verb(query_type, response_out)) return false;
#ifdef CK3_BRIDGE_DEBUG_VERBS
    if (strcmp(query_type, "memory_probe") == 0) {
        return do_query_memory_probe(params_json, response_out);
    }
#endif
    if (strcmp(query_type, "character_memories") == 0
            || strcmp(query_type, "memories") == 0) {
        return do_query_character_memories(params_json, response_out);
    }
#ifdef CK3_BRIDGE_DEBUG_VERBS
    if (strcmp(query_type, "resolve_global") == 0) {
        return do_query_resolve_global(params_json, response_out);
    }
#endif

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] query: unknown type '%s'\n", query_type);
    return false;
}

// M1: helper used by the action registry macro.  If no ClientContext is
// installed, writes a typed failure to response_out and returns true.  Otherwise
// returns false and the caller proceeds to the real handler.
static bool fail_if_no_context(const char* action_name, std::string* response_out) {
    std::string reason;
    if (!bridge::require_dispatch_context(action_name, &reason)) {
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge][M1] '%s' dispatched with no ClientContext\n", action_name);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action");  r.value_string(action_name);
            r.key("reason");  r.value_string(reason.c_str());
            r.key("hint");    r.value_string(
                "No ClientContext installed; this is an internal bridge error.");
            r.end_object();
            *response_out = r.take();
        }
        return true;
    }
    return false;
}

// rc2 B2: raw memory reads (memory_probe, resolve_global), X/uinput input
// injection (inject_mouse_click, select_observe_default), the debug console
// (execute_console_command) and raw client pointers handed to the engine
// (set_cb_selector_override) are COMPILED OUT unless built with
// CK3_BRIDGE_DEBUG_VERBS (CMake option, OFF): their action_registry.inc lines
// and handler bodies are #ifdef'd, so a default build does not link them.
// This list only words the refusal; test_debug_verbs checks it equals the set
// the define adds to action_registry.inc.
static const char* const kDebugVerbs[] = {
    "memory_probe", "resolve_global", "inject_mouse_click",
    "select_observe_default", "execute_console_command",
    "set_cb_selector_override",
};

bool debug_verb_compiled_out(const char* verb) {
#ifdef CK3_BRIDGE_DEBUG_VERBS
    (void)verb;
    return false;
#else
    if (!verb) return false;
    for (const char* d : kDebugVerbs)
        if (strcmp(verb, d) == 0) return true;
    return false;
#endif
}

static bool refuse_debug_verb(const char* verb, std::string* response_out) {
#ifdef CK3_BRIDGE_DEBUG_VERBS
    (void)verb; (void)response_out;
    return false;
#else
    for (const char* d : kDebugVerbs) {
        if (strcmp(verb, d) != 0) continue;
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] %s refused: unavailable_in_release\n", verb);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action");  r.value_string(verb);
            r.key("error");   r.value_string("unavailable_in_release");
            r.end_object();
            *response_out = r.take();
        }
        return true;
    }
    return false;
#endif
}

// =============================================================================
// Action Dispatch (JSON command -> engine function call)
// =============================================================================

bool dispatch_action(const char* action_type, const char* params_json,
                     std::string* response_out) {
    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Action: %s params: %s\n", action_type, params_json);
    if (refuse_debug_verb(action_type, response_out)) return false;

    // M1: per-session ownership + mode choke point.  Read-only actions are
    // allowed for everyone.  Mutations are refused in observe mode.  Mutations
    // targeting an explicit actor/character must be owned by the current
    // session (or the session primary when actor_id is 0).  Character-claim
    // actions manage ownership themselves and bypass the actor check.
    bridge::Session* session = bridge::current_session();

    // ck-6kvq.9 / decision G1: IN PIPELINE MODE THE BRIDGE OWNS THE CLOCK.
    // pause_game / unpause_game / set_speed from a session are typed refusals;
    // the flush loop is the only thing allowed to move the clock, and it calls
    // do_pause_game / do_unpause_game / do_set_speed directly rather than
    // through this choke point. Unchanged in legacy mode (the default), where
    // this is one relaxed atomic load that returns false.
    if (session && bridge::pipeline_owns_clock() && bridge::is_clock_verb(action_type)) {
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge][pipeline] clock verb %s refused: clock_owned_by_pipeline\n",
            action_type);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string(action_type);
            r.key("reason"); r.value_string("clock_owned_by_pipeline");
            r.key("hint"); r.value_string(
                "The pipeline flush loop owns the clock in pipeline mode (G1). "
                "Submit your work and collect it with `check`; the bridge will "
                "pause only for as long as the paused reads in a flush take.");
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }

    // ck-6kvq.13 / decision G7: THE DECISION BUDGET IS ENFORCED HERE.
    // MUTATIONS ONLY. A session that has spent its per-game-year allowance
    // keeps every read, every bridge-management verb and `report_usage` itself
    // (§5.3) — it can observe and log, it simply cannot spend the world's
    // state until its game year rolls. In the shipped default (no ceiling
    // configured) `armed()` is one relaxed atomic load returning false.
    if (session && bridge::budget_ledger().armed()) {
        bridge::budget_observe_engine_clock();
        const bool gated = bridge::budget_gate_applies(
            action_type, bridge::action_is_mutation(action_type),
            is_bridge_management_verb(action_type));
        bridge::BudgetVerdict v = bridge::budget_ledger().evaluate(
            session->session_id, action_type, gated);
        if (!v.allowed) {
            if (g_logfile) fprintf(g_logfile,
                "[CK3-Bridge][budget] %s refused: budget_exhausted "
                "(session=%s game_date=%s spend=%lld ceiling=%lld)\n",
                action_type, v.session_id.c_str(), v.game_date.c_str(),
                static_cast<long long>(v.spend), static_cast<long long>(v.ceiling));
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success");    r.value_bool(false);
                r.key("action");     r.value_string(action_type);
                r.key("reason");     r.value_string("budget_exhausted");
                r.key("session_id"); r.value_string(v.session_id.c_str());
                r.key("game_date");  r.value_string(v.game_date.c_str());
                r.key("game_year");  r.value_int(v.game_year);
                r.key("spend");      r.value_int(v.spend);
                r.key("ceiling");    r.value_int(v.ceiling);
                r.key("resets_at");  r.value_string(v.resets_at.c_str());
                r.key("hint");       r.value_string(
                    "This session has spent its decision budget for this game year "
                    "(output + uncached input tokens). Reads, board_read and "
                    "report_usage continue; mutations resume when the game year "
                    "rolls over.");
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }
    }

    // ck-6kvq.5 / ck-9fc3: `declare_save_identity` is HARNESS PROVENANCE, not a
    // game mutation. It writes nothing to the world; it states WHICH SAVE FILE
    // is loaded and lets the bridge decide what to do with steering it is
    // already holding — and every effect it can have is conservative (discard,
    // never grant). The process that knows the answer is the episode harness,
    // which attaches in OBSERVE mode: that is the whole shape of the E0 arm, and
    // world_identity.h says so in as many words ("an observer-mode harness
    // declares before any control session attaches"). Gating it on control would
    // mean the identity could only ever be vouched for by a session that is
    // itself part of the experimental condition.
    const bool is_provenance_declaration =
        (strcmp(action_type, "declare_save_identity") == 0);

    // ck-1ek9: THE NULL-MODE CLOCK PIN, and the same argument one field over.
    // In `null` the bridge mints no control session at all (pipeline_mode.h),
    // so `set_speed` — a mutation, and therefore observe-refused — would be
    // unsettable BY ANYONE, and `pin_game_speed` refuses an episode it cannot
    // pin. The game speed is a declared CONSTANT OF the episode, not a decision
    // inside it: it changes how fast the world is simulated, never what anyone
    // in it decides. Exactly the `declare_save_identity` reasoning above.
    //
    // Scoped by MODE, not by client name, so it cannot leak into an arm:
    // `mode_admits_observer_clock_verbs` is true for `null` alone. In pipeline
    // mode the clock-ownership refusal below still applies first.
    const bool is_null_mode_clock_pin =
        bridge::is_clock_verb(action_type) &&
        bridge::mode_admits_observer_clock_verbs(bridge::pipeline_mode());

    if (session && bridge::action_is_mutation(action_type) &&
        !is_provenance_declaration && !is_null_mode_clock_pin) {
        if (session->is_observe()) {
            if (g_logfile) fprintf(g_logfile,
                "[CK3-Bridge] Action %s rejected: observe mode cannot mutate\n", action_type);
            if (response_out) {
                ResponseBuilder r; r.begin_object();
                r.key("success"); r.value_bool(false);
                r.key("action"); r.value_string(action_type);
                r.key("reason"); r.value_string("observe_mode");
                r.key("hint"); r.value_string("observe-mode sessions are read-only");
                r.end_object();
                *response_out = r.take();
            }
            return false;
        }

        if (strcmp(action_type, "set_controlled_character") != 0 &&
            strcmp(action_type, "release_character") != 0) {
            int actor_id = json_get_int(params_json, "actor_id");
            if (actor_id <= 0) actor_id = json_get_int(params_json, "character_id");
            if (actor_id > 0) {
                std::string reason;
                if (!bridge::validate_actor_ownership(static_cast<uint32_t>(actor_id),
                                                      action_type, &reason)) {
                    if (g_logfile) fprintf(g_logfile,
                        "[CK3-Bridge] Action %s rejected: actor %d (%s)\n",
                        action_type, actor_id, reason.c_str());
                    if (response_out) {
                        ResponseBuilder r; r.begin_object();
                        r.key("success"); r.value_bool(false);
                        r.key("action"); r.value_string(action_type);
                        r.key("reason"); r.value_string(reason.c_str());
                        r.key("actor_id"); r.value_uint(static_cast<uint32_t>(actor_id));
                        r.key("session_id"); r.value_string(session->session_id.c_str());
                        r.end_object();
                        *response_out = r.take();
                    }
                    return false;
                }
            }
        }
    }

    // Safety gate: unverified Execute-path writes are disabled unless explicitly
    // enabled in <state dir>/ck3-enabled-writes (see experimental_write_enabled above).
    if (!experimental_write_enabled(action_type)) {
        if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] experimental write GATED (disabled): %s\n", action_type);
        if (response_out) {
            ResponseBuilder r; r.begin_object();
            r.key("success"); r.value_bool(false);
            r.key("action"); r.value_string(action_type);
            r.key("reason"); r.value_string("experimental_write_disabled");
            r.key("hint"); r.value_string(
                "This write path is unverified and gated for safety (it could crash "
                "the game). Rely on the native AI instead (e.g. it seats councillors "
                "on its own with the Council subsystem ON). For controlled validation, "
                "add its action name, one per line, to the file in enabled_writes_file.");
            r.key("enabled_writes_file"); r.value_string(
                (std::string(bridge::instance_state_dir()) + "/" + kEnabledWritesName).c_str());
            r.end_object();
            *response_out = r.take();
        }
        return false;
    }


    // --- ck-6kvq.12 / R4 / G5: THE ARM GATE --------------------------------
    // Arm A registers NO board verb (option A2, ruled by the progenitor
    // 2026-09-10). The gate is HERE, above the registry chain, and mirrored
    // exactly in is_registered_action() below, so in every arm but B the two
    // board verbs are indistinguishable from a typo: `unknown_action`, absent
    // from the registry, and fast-failed by the reader thread before they ever
    // reach a handler. That is what "no channel" has to mean — a board_post
    // that exists and refuses would put arm A in a third condition, which is
    // the confound A2 exists to avoid.
    if (board_verb_registration(action_type) == 0) {
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge][board] %s in an arm with no board — unknown_action\n",
            action_type);
        if (response_out) {
            *response_out = "{\"success\":false,\"error\":\"unknown_action\",\"action\":\"";
            *response_out += action_type;
            *response_out += "\"}";
        }
        return false;
    }

    // --- Registered actions: single source-of-truth table (D1-C / FIX 3.1) ---
    // action_registry.inc expands here into the strcmp dispatch chain. The SAME
    // table generates is_registered_action() below, so dispatch and registration
    // can never drift — drift is how the query_titles class of paused-caller
    // hangs was born (unregistered name -> g_msg_queue -> no drain while paused
    // -> silent 20s+ stall; bit query_war_stance, query_dejure_progress and
    // query_map_graph before that). Add new actions to action_registry.inc, not
    // here.
#define BRIDGE_ACTION(NAME, HANDLER) \
    if (strcmp(action_type, NAME) == 0) { \
        if (fail_if_no_context(NAME, response_out)) return false; \
        return HANDLER(params_json, response_out); \
    }
#include "action_registry.inc"
#undef BRIDGE_ACTION

    if (g_logfile) fprintf(g_logfile, "[CK3-Bridge] Unknown action type: %s\n", action_type);
    if (response_out) {
        *response_out = "{\"success\":false,\"error\":\"unknown_action\",\"action\":\"";
        *response_out += action_type;
        *response_out += "\"}";
    }
    return false;
}

// D1-C / FIX 3.1 (bridge-investigation-20260801): registry membership test,
// generated from the SAME action_registry.inc table as the dispatch chain in
// dispatch_action() above — the two cannot drift. Consumed by the IPC reader
// thread (ipc_server.cpp::try_queue_fallthrough_rejection) to fast-fail
// unregistered action names with an inline unknown_action reply. Before this,
// an unknown name fell through to g_msg_queue and, while the game was paused,
// hung the caller forever with no reply (D1, the query_titles hang class).
// NOTE: this is NOT a pause classifier — it says nothing about routing, only
// whether a dispatch case exists (do NOT gate on the snapshot.cpp classifier
// lists here: legit Tier-3 / unclassified actions must still queue).
bool is_registered_action(const char* action_type) {
    if (!action_type || !action_type[0]) return false;
    // ck-6kvq.12 / G5: the arm gate, mirrored from dispatch_action(). This is
    // the half that matters most — the reader thread asks THIS function whether
    // a name exists, so an arm without a board fast-fails `board_post` with
    // `unknown_action` on the reader thread, exactly as it does for a typo, and
    // nothing downstream ever sees the verb.
    if (const int reg = board_verb_registration(action_type); reg >= 0)
        return reg == 1;
#define BRIDGE_ACTION(NAME, HANDLER) \
    if (strcmp(action_type, NAME) == 0) return true;
#include "action_registry.inc"
#undef BRIDGE_ACTION
    return false;
}

// Wrapper that auto-generates response for handlers that don't set one
bool dispatch_action_with_response(const char* action_type, const char* params_json,
                                    std::string* response_out) {
    std::string resp;
    bool ok = dispatch_action(action_type, params_json, response_out ? response_out : &resp);

    // If handler succeeded but didn't populate response, generate one
    if (response_out && response_out->empty()) {
        if (ok) {
            *response_out = "{\"success\":true,\"action\":\"";
            *response_out += action_type;
            *response_out += "\"}";
        } else {
            *response_out = "{\"success\":false,\"action\":\"";
            *response_out += action_type;
            *response_out += "\"}";
        }
    }

    // overlay-v2 §2.2: tap executed state-mutating action verbs into the JSONL feed.
    if (const char* kind = action_log_kind(action_type)) {
        const char* result = response_out ? response_out->c_str() : resp.c_str();
        emit_action_log(action_type, kind, params_json, result);
    }

    return ok;
}

} // namespace bridge
