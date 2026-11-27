// factions.cpp — faction management from both seats.
//
// Everything the bridge does with political factions lives here:
//
//   query_joinable_factions   the VASSAL's options against its liege, each with the
//                             engine's own verdict and reason text: which faction
//                             types it may create, which existing factions it may
//                             join, the ones it is in (may it leave), and for a
//                             faction it leads, whether it may press its demand now.
//                             A deferred read: the verdicts are the engine's trigger
//                             evaluators, which run on the game thread
//                             (process_faction_query, from drain_and_execute_intents).
//   faction_action            create | join | leave | press_demand, executed on the
//                             game thread (execute_faction_intent, IntentType::FACTION),
//                             each refused with the engine's reason text and read back.
//   set_faction_demand_response
//                             the LIEGE's standing answer to a faction's ultimatum
//                             (accept | refuse | ai). The AI picks event options in
//                             AI_PICK_EVENT_OPTION; the hook below returns the option
//                             named FACTION_DEMAND_ACCEPT or FACTION_DEMAND_REFUSE for
//                             a controlled liege that set one, and records every answer
//                             it gave in a ring that query_factions reports.
//
// RE: re/findings/re_factions.md and re_faction_join_create.md.
// Every engine call below runs under the fake-RNG scope (trigger evaluation and
// leader election draw from it), the watchdog and the sigsetjmp crash-net.

#include "bridge.h"
#include "init_order.h"
#include "intent_queue.h"
#include "ck3_offsets.h"
#include "action_dispatch_internal.h"
#include "scope_slot_policy.h"
#include "game_date.h"
#include "engine_text.h"
#include "faction_war.h"
#include "faction_demand.h"
#ifdef _WIN32
#include "windows/ck3_offsets_win.h"   // CEvent's key (struct_layout::CEVENT_NAME_KEY)
#endif

// faction_war.h is engine-free, so it carries its own copies of the two WAR_DB
// constants; they must stay the offset header's.
static_assert(bridge::kFactionWarNone == ck3::war_db::WAR_ID_INVALID,
              "faction_war.h kFactionWarNone must equal war_db::WAR_ID_INVALID");
static_assert(bridge::kFactionWarSlotMask == ck3::war_db::WAR_ID_IDX_MASK,
              "faction_war.h kFactionWarSlotMask must equal war_db::WAR_ID_IDX_MASK");
// faction_demand.h is engine-free too: its name-walk offsets must stay the offset header's.
static_assert(bridge::faction_demand::kEntryKey == ck3::event_sys::OPTION_ENTRY_KEY,
              "faction_demand.h kEntryKey must equal event_sys::OPTION_ENTRY_KEY");
static_assert(bridge::faction_demand::kChildren == ck3::event_sys::OPTION_NAME_CHILDREN,
              "faction_demand.h kChildren must equal event_sys::OPTION_NAME_CHILDREN");
static_assert(bridge::faction_demand::kChildCount == ck3::event_sys::OPTION_NAME_CHILD_COUNT,
              "faction_demand.h kChildCount must equal event_sys::OPTION_NAME_CHILD_COUNT");

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <setjmp.h>

#define FACLOG(fmt, ...) do { \
    if (bridge::g_logfile) { fprintf(bridge::g_logfile, "[CK3-Bridge][faction] " fmt, ##__VA_ARGS__); fflush(bridge::g_logfile); } \
} while(0)

namespace bridge {

// ============================================================================
// Engine entry points (raw offsets in ck3_offsets.h; signatures from the decompile)
// ============================================================================
namespace {

using fn_can_create   = uint8_t (*)(void* actor, void* liege, void* type, const uint8_t* flags, std::string* reason);
using fn_can_join     = uint8_t (*)(void* actor, void* faction, const uint8_t* flags, std::string* reason);
using fn_can_leave    = uint8_t (*)(void* faction, void* actor, std::string* reason);
using fn_can_fire     = uint8_t (*)(void* faction, std::string* reason);
using fn_get_mgr      = void* (*)(void* game_state);
using fn_get_or_create= void* (*)(void* mgr, void* type, void* actor, void* liege);
using fn_join         = void  (*)(void* faction, uint32_t char_ref, int title, void* claimant_data, char skip_validate);
// Returns a bool in AL; the upper bytes are left over from the array end pointer
// (decompile of FUN_05f8c4d0), so reading the whole register as a pointer made
// every character a member (a faction test run).
using fn_is_member    = uint8_t (*)(void* faction, void* character);
using fn_remove       = void  (*)(void* mgr, void* faction, uint32_t char_ref, uint64_t reason_value, uint8_t* destroyed);
using fn_press_demand = void  (*)(void* faction);

// The check flags the player's commands pass: CCreateFactionCommand's CanExecute
// (FUN_05f99d40) and the join_faction effect (FUN_05065360) set all five.
const uint8_t kJoinCheckFlags[8] = {1, 1, 1, 1, 1, 0, 0, 0};

// The engine's reason text is rendered tooltip text; engine_text.h strips the
// markup and keeps the words.
std::string clean_reason(const std::string& in) { return clean_engine_text(in); }

// One guarded engine call: watchdog + sigsetjmp. Returns false when the call
// faulted or timed out (the out-params are then not meaningful).
template <typename F>
bool guarded(unsigned secs, F&& f) {
    watchdog_arm(secs);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        FACLOG("engine call faulted (sig=%d)\n", sig);
        return false;
    }
    f();
    g_in_protected_region = false;
    watchdog_disarm();
    return true;
}

std::string type_key(uintptr_t type_ptr) {
    if (!type_ptr || !is_mapped(type_ptr)) return std::string();
    return ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
}

uintptr_t faction_type_by_name(const std::string& key) {
    uintptr_t inst = safe_read<uintptr_t>(g_base_addr + ck3::globals::FACTION_TYPE_DB_V2);
    if (!inst) return 0;
    uintptr_t arr = safe_read<uintptr_t>(inst + ck3::game_db::ENTRIES_ARRAY);
    uint32_t count = safe_read<uint32_t>(inst + ck3::game_db::LIVE_COUNT);
    if (!arr || count == 0 || count > 256) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t t = safe_read<uintptr_t>(arr + static_cast<uintptr_t>(i) * 8);
        if (t && type_key(t) == key) return t;
    }
    return 0;
}

std::vector<uintptr_t> all_faction_types() {
    std::vector<uintptr_t> out;
    uintptr_t inst = safe_read<uintptr_t>(g_base_addr + ck3::globals::FACTION_TYPE_DB_V2);
    if (!inst) return out;
    uintptr_t arr = safe_read<uintptr_t>(inst + ck3::game_db::ENTRIES_ARRAY);
    uint32_t count = safe_read<uint32_t>(inst + ck3::game_db::LIVE_COUNT);
    if (!arr || count == 0 || count > 256) return out;
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t t = safe_read<uintptr_t>(arr + static_cast<uintptr_t>(i) * 8);
        if (t && is_mapped(t)) out.push_back(t);
    }
    return out;
}

bool type_is_creatable(uintptr_t t) {
    return safe_read<uint8_t>(t + ck3::faction_v2::type_scripts::CREATABLE_FLAG) != 0;
}
bool type_is_county(uintptr_t t) {
    return safe_read<uint8_t>(t + ck3::faction_v2::type_scripts::IS_COUNTY_FACTION) != 0;
}

uintptr_t faction_by_id(uint32_t id) {
    if (!id) return 0;
    uintptr_t f = ad_db_lookup(ck3::globals::FACTION_DB, id);
    if (!f || !is_mapped(f)) return 0;
    if (f == g_base_addr + ck3::faction_write::NULL_FACTION_SENTINEL) return 0;
    return f;
}

// Factions aimed at `ruler` (his title_realm faction list).
std::vector<uint32_t> factions_targeting(uintptr_t ruler) {
    std::vector<uint32_t> out;
    uintptr_t td = safe_read<uintptr_t>(ruler + ck3::character::TITLE_REALM_DATA);
    if (!td || !is_mapped(td)) return out;
    uintptr_t list = safe_read<uintptr_t>(td + ck3::title_realm::FACTION_LIST);
    int32_t n = safe_read<int32_t>(td + ck3::title_realm::FACTION_COUNT);
    if (!list || !is_mapped(list) || n <= 0 || n >= 200) return out;
    for (int32_t i = 0; i < n; i++) {
        uint32_t id = safe_read<uint32_t>(list + static_cast<uintptr_t>(i) * 4);
        if (id && id != 0xFFFFFFFF) out.push_back(id & 0xFFFFFF);
    }
    return out;
}

int32_t game_date_raw() {
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    return gs ? safe_read<int32_t>(gs + ck3::game_state::GAME_DATE) : 0;
}

void emit_date(ResponseBuilder& r, const char* key, int32_t raw) {
    char buf[16];
    ck3::game_date::format_raw(raw, buf, sizeof buf);
    r.key(key); r.value_string(buf);
}

uint32_t leader_ref(uintptr_t f) {
    uint32_t l = safe_read<uint32_t>(f + ck3::faction_v2::LEADER_CHAR_REF);
    return (l == 0xFFFFFFFF) ? 0 : (l & 0xFFFFFF);
}

double fp(int64_t v) { return static_cast<double>(v) / 100000.0; }

// The faction's demand path, read from its own fields: discontent against the
// engine's threshold, the demand state (classify_faction_demand, the one
// definition), and the war the faction declares if refused.
void emit_demand_state(ResponseBuilder& r, uintptr_t f, uintptr_t type) {
    int64_t thr = safe_read_offset<int64_t>(ck3::globals::FACTION_DISCONTENT_THRESHOLD);
    r.key("discontent"); r.value_double(fp(safe_read<int64_t>(f + ck3::faction_v2::DISCONTENT)));
    r.key("discontent_threshold"); r.value_double(thr > 0 ? fp(thr) : 100.0);
    emit_faction_demand(r, classify_faction_demand(f));
    emit_faction_war(r, f);
    std::string cb = type ? ad_read_std_string(type + ck3::faction_v2::type_scripts::WAR_CB_KEY) : std::string();
    if (!cb.empty()) { r.key("war_if_refused"); r.value_string(cb.c_str(), cb.size()); }
}

void emit_char_ref(ResponseBuilder& r, const char* id_key, const char* name_key, uint32_t id) {
    r.key(id_key); r.value_uint(id);
    void* c = id ? lookup_character(id) : nullptr;
    if (c && !is_sentinel(c)) {
        std::string n = ad_read_char_name(reinterpret_cast<uintptr_t>(c));
        if (!n.empty()) { r.key(name_key); r.value_string(n.c_str(), n.size()); }
    }
}

// ----------------------------------------------------------------------------
// Engine predicates (game thread only). Each returns 1 allowed, 0 refused, -1 the
// call faulted; `reason` gets the engine's text on a refusal.
// ----------------------------------------------------------------------------
// The player's create check (CCreateFactionCommand's CanExecute): the faction
// type's can_create trigger AND whether the actor may be in a faction of that type
// against the liege at all (FACTION_CAN_CREATE alone answers only the first: one
// faction test run created faction 41 for a vassal already in another faction, and the
// join inside it left him out).
int can_create(uintptr_t type, void* actor, void* liege, std::string* reason) {
    auto fn = reinterpret_cast<fn_can_create>(game_fn(ck3::faction_write::FACTION_CAN_CREATE_AS_COMMAND));
    uint8_t ok = 0; std::string r;
    if (!guarded(3, [&] { ok = fn(actor, liege, reinterpret_cast<void*>(type), kJoinCheckFlags, &r); })) return -1;
    if (reason) *reason = clean_reason(r);
    return ok ? 1 : 0;
}
int can_join(void* actor, uintptr_t f, std::string* reason) {
    auto fn = reinterpret_cast<fn_can_join>(game_fn(ck3::faction_write::FACTION_CAN_JOIN));
    uint8_t ok = 0; std::string r;
    if (!guarded(3, [&] { ok = fn(actor, reinterpret_cast<void*>(f), kJoinCheckFlags, &r); })) return -1;
    if (reason) *reason = clean_reason(r);
    return ok ? 1 : 0;
}
int can_leave(uintptr_t f, void* actor, std::string* reason) {
    auto fn = reinterpret_cast<fn_can_leave>(game_fn(ck3::faction_write::FACTION_CAN_LEAVE));
    uint8_t ok = 0; std::string r;
    if (!guarded(3, [&] { ok = fn(reinterpret_cast<void*>(f), actor, &r); })) return -1;
    if (reason) *reason = clean_reason(r);
    return ok ? 1 : 0;
}
int can_press(uintptr_t f, std::string* reason) {
    auto fn = reinterpret_cast<fn_can_fire>(game_fn(ck3::faction_funcs::CAN_FIRE_ULTIMATUM));
    uint8_t ok = 0; std::string r;
    if (!guarded(3, [&] { ok = fn(reinterpret_cast<void*>(f), &r); })) return -1;
    if (reason) *reason = clean_reason(r);
    return ok ? 1 : 0;
}
int is_member(uintptr_t f, void* actor) {
    auto fn = reinterpret_cast<fn_is_member>(game_fn(ck3::faction_write::FACTION_IS_MEMBER));
    uint8_t m = 0;
    if (!guarded(3, [&] { m = fn(reinterpret_cast<void*>(f), actor); })) return -1;
    return m ? 1 : 0;
}

void emit_verdict(ResponseBuilder& r, const char* key, int v, const std::string& reason) {
    r.key(key);
    if (v < 0) r.value_null(); else r.value_bool(v == 1);
    if (v < 0) { r.key("verdict_error"); r.value_string("engine_call_faulted"); }
    else if (v == 0) {
        // Empty when the game gave no reason text (or only a compound header,
        // which clean_engine_text drops): the server says so in one sentence
        // (redesign.NO_GAME_DETAIL), not the bridge.
        r.key("reason"); r.value_string(reason.c_str(), reason.size());
    }
}

}  // namespace

// A faction's date field (declared in action_dispatch_internal.h). A CK3 date is
// 8 bytes: the int32 tick count, then a cached day/month/year (FUN_076e7460,
// FUN_0605a8d0), and the engine compares the int32 alone (FactionTick:
// *(int*)(f+0xB0) == *(int*)SENTINEL_DATE). Reading 8 bytes took the cache into
// the value: SENTINEL_DATE read as int64 is 0x00010000029C77F8, never equal to a
// field holding 0x029C77F8 (43808760, 0001.01.01), so every faction read as
// counting down to 0001.01.01 (the 867 dump: all 36 factions hold 43808760
// at +0xB0 and +0x74, each followed by 0x00010000 or 0xFFFFFFFF).
int32_t faction_date(uintptr_t faction, size_t field) {
    int32_t v = safe_read<int32_t>(faction + field);
    int32_t never = safe_read_offset<int32_t>(ck3::faction_globals::SENTINEL_DATE);
    return (v <= 0 || v == never) ? 0 : v;
}

// The faction's war (declared in action_dispatch_internal.h; the one definition,
// faction_war.h): its WAR_REF resolved through WAR_DB as the game's own press check
// resolves it. Pure reads, any thread.
FactionWar faction_war(uintptr_t faction) {
    uint32_t ref = safe_read<uint32_t>(faction + ck3::faction_v2::WAR_REF);
    uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
    auto slot_war = [db](uint32_t idx) -> uintptr_t {
        if (!db || !is_mapped(db)) return 0;
        // The engine's own reads (FUN_05f8ca90): capacity at +0x2C, the slot's war
        // at table + 8 + idx * 0x10.
        if (idx >= safe_read<uint32_t>(db + ck3::hash_map::CAPACITY)) return 0;
        uintptr_t table = safe_read<uintptr_t>(db + ck3::hash_map::TABLE_PTR);
        if (!table || !is_mapped(table)) return 0;
        uintptr_t war = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                             + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE);
        return (war && is_mapped(war)) ? war : 0;
    };
    auto war_id_of = [](uintptr_t war) { return safe_read<uint32_t>(war + ck3::war_obj::WAR_ID); };
    return faction_war_from(ref, slot_war, war_id_of);
}

void emit_faction_war(ResponseBuilder& r, uintptr_t faction) {
    FactionWar w = faction_war(faction);
    r.key("at_war"); r.value_bool(w.at_war);
    if (w.at_war) { r.key("war_id"); r.value_uint(w.war_id); }
}

// The demand state (declared in action_dispatch_internal.h; the one definition).
//
// The engine keeps no ultimatum countdown (re_factions.md section 7). At each
// faction tick (every 30 days) FactionTick (FUN_05f8c910) writes
// ULTIMATUM_FIRE_DATE: the "never" date unless discontent is full and
// CAN_FIRE_ULTIMATUM holds, then a date 91 days out that the next tick writes
// again, so it is never reached; and ULTIMATUM_PROGRESS. The tick's commit
// (FUN_05f8d3e0) then has an AI leader press with a chance of ULTIMATUM_PROGRESS
// percent. A press (FUN_05f8d900) writes COOLDOWN_DATE = today + the cooldown;
// the two tick fields keep the last tick's values until the next tick, so a
// faction pressed since that tick reads armed and is in its cooldown: none.
FactionDemandState classify_faction_demand(uintptr_t f) {
    FactionDemandState s;
    int32_t now = game_date_raw();
    int32_t cool = faction_date(f, ck3::faction_v2::COOLDOWN_DATE);
    if (cool && now > 0 && cool > now) s.cooldown_until = cool;
    bool armed = faction_date(f, ck3::faction_v2::ULTIMATUM_FIRE_DATE) != 0;
    double chance = fp(safe_read<int64_t>(f + ck3::faction_v2::ULTIMATUM_PROGRESS));
    if (armed && s.cooldown_until == 0 && chance > 0) {
        s.ai_may_press = true;
        s.chance_pct = chance > 100.0 ? 100.0 : chance;
    }
    return s;
}

void emit_faction_demand(ResponseBuilder& r, const FactionDemandState& s) {
    r.key("ultimatum"); r.value_string(s.ai_may_press ? "ai_may_press" : "none");
    if (s.ai_may_press) { r.key("ai_press_chance_pct"); r.value_double(s.chance_pct); }
    if (s.cooldown_until) emit_date(r, "demand_cooldown_until", s.cooldown_until);
}

// ============================================================================
// query_joinable_factions — the vassal's seat (deferred, game thread)
// ============================================================================

struct FactionQuery {
    uint32_t actor_id = 0;
    std::string result_json;
    char error[128] = {};
    std::atomic<int> state{0};   // 0 empty, 1 pending, 2 done, 3 error, 4 running
};
static FactionQuery g_faction_query CK3_EARLY_INIT;

static std::string build_faction_options(uint32_t actor_id) {
    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_joinable_factions");
    r.key("actor_id"); r.value_uint(actor_id);

    void* actor = actor_id ? lookup_character(actor_id) : nullptr;
    if (!actor || is_sentinel(actor)) {
        r.key("error"); r.value_string("actor_not_found");
        r.end_object();
        return r.take();
    }
    uint32_t liege_id = get_liege_id_from_addr(reinterpret_cast<uintptr_t>(actor));
    void* liege = liege_id ? lookup_character(liege_id) : nullptr;
    if (!liege || is_sentinel(liege)) {
        r.key("liege_id"); r.value_uint(0);
        r.key("independent"); r.value_bool(true);
        r.key("faction_types"); r.begin_array(); r.end_array();
        r.key("factions"); r.begin_array(); r.end_array();
        r.key("note"); r.value_string(
            "an independent ruler has no liege, so there is no faction to create or join");
        r.end_object();
        return r.take();
    }
    emit_char_ref(r, "liege_id", "liege_name", liege_id);

    if (scope_latched()) {
        g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        r.key("error"); r.value_string(scope_latched_reason());
        r.end_object();
        return r.take();
    }
    uint64_t saved_rng = 0;
    bool rng = arm_fake_random_scope(&saved_rng);

    std::vector<uint32_t> targeting = factions_targeting(reinterpret_cast<uintptr_t>(liege));

    // CREATE: every type the engine marks ruler-creatable (county factions are
    // formed by counties, not rulers), with the engine's can_create verdict.
    // On the game thread, holding the session mutex: the
    // engine verdicts stop at kGameThreadEvalBudgetMs and the types / factions
    // not reached are named (partial).
    const auto budget_start = std::chrono::steady_clock::now();
    std::vector<std::string> unevaluated_types;
    std::vector<uint32_t> unevaluated_factions;
    r.key("faction_types"); r.begin_array();
    for (uintptr_t t : all_faction_types()) {
        if (!type_is_creatable(t) || type_is_county(t)) continue;
        std::string key = type_key(t);
        if (key.empty()) continue;
        if (!unevaluated_types.empty() || game_thread_budget_spent(budget_start)) {
            unevaluated_types.push_back(key);
            continue;
        }
        r.begin_object();
        r.key("faction_type"); r.value_string(key.c_str(), key.size());
        std::string why;
        int v = can_create(t, actor, liege, &why);
        emit_verdict(r, "can_create", v, why);
        for (uint32_t fid : targeting) {
            uintptr_t f = faction_by_id(fid);
            if (f && safe_read<uintptr_t>(f + ck3::faction_v2::TYPE_PTR) == t) {
                r.key("existing_faction_id"); r.value_uint(fid);
                break;
            }
        }
        std::string cb = ad_read_std_string(t + ck3::faction_v2::type_scripts::WAR_CB_KEY);
        if (!cb.empty()) { r.key("war_if_refused"); r.value_string(cb.c_str(), cb.size()); }
        r.end_object();
    }
    r.end_array();

    // JOIN / LEAVE / PRESS: every faction aimed at the liege.
    r.key("factions"); r.begin_array();
    int member_of = 0;
    for (uint32_t fid : targeting) {
        uintptr_t f = faction_by_id(fid);
        if (!f) continue;
        if (!unevaluated_factions.empty() || game_thread_budget_spent(budget_start)) {
            unevaluated_factions.push_back(fid);
            continue;
        }
        uintptr_t t = safe_read<uintptr_t>(f + ck3::faction_v2::TYPE_PTR);
        std::string key = type_key(t);
        r.begin_object();
        r.key("faction_id"); r.value_uint(fid);
        r.key("faction_type"); r.value_string(key.empty() ? "unknown" : key.c_str());
        bool county = t && type_is_county(t);
        r.key("is_county_faction"); r.value_bool(county);
        uint32_t lead = leader_ref(f);
        if (lead) emit_char_ref(r, "leader_id", "leader_name", lead);
        int32_t cm = safe_read<int32_t>(f + ck3::faction_v2::CHAR_MEMBERS_COUNT);
        r.key("member_count"); r.value_int(cm > 0 ? cm : 0);
        int mem = is_member(f, actor);
        r.key("is_member"); if (mem < 0) r.value_null(); else r.value_bool(mem == 1);
        if (mem == 1) {
            ++member_of;
            std::string why;
            r.key("leave"); r.begin_object();
            emit_verdict(r, "can_leave", can_leave(f, actor, &why), why);
            r.end_object();
            bool leads = (lead == actor_id);
            r.key("is_leader"); r.value_bool(leads);
            r.key("demand"); r.begin_object();
            std::string pwhy;
            int pv = can_press(f, &pwhy);
            emit_demand_state(r, f, t);
            // The liege's standing answer and the answers given for him, from the
            // vassal's side too: a read that does not need the liege alive (a
            // faction test run: the liege answered, then died before his own read).
            emit_demand_response_state(r, liege_id);
            emit_verdict(r, "can_press_now", pv, pwhy);
            r.key("who_presses"); r.value_string(leads
                ? "you lead this faction: do(intent='faction', args={action:'press_demand'}) sends the demand when can_press_now is true"
                : "the faction leader presses the demand; the AI leader decides when");
            r.end_object();
        } else if (mem == 0) {
            std::string why;
            r.key("join"); r.begin_object();
            emit_verdict(r, "can_join", can_join(actor, f, &why), why);
            r.end_object();
        }
        r.end_object();
    }
    r.end_array();
    r.key("member_of_count"); r.value_int(member_of);
    r.key("partial"); r.value_bool(!unevaluated_types.empty() || !unevaluated_factions.empty());
    r.key("budget_ms"); r.value_int(kGameThreadEvalBudgetMs);
    r.key("unevaluated_faction_types"); r.begin_array();
    for (const auto& k : unevaluated_types) r.value_string(k.c_str(), k.size());
    r.end_array();
    r.key("unevaluated_faction_ids"); r.begin_array();
    for (uint32_t id : unevaluated_factions) r.value_uint(id);
    r.end_array();

    if (rng) restore_fake_random_scope(saved_rng);
    r.end_object();
    return r.take();
}

void process_faction_query() {   // GAME thread (drain_and_execute_intents)
    int expected = 1;
    if (!g_faction_query.state.compare_exchange_strong(expected, 4, std::memory_order_acq_rel))
        return;
    try {
        g_faction_query.result_json = build_faction_options(g_faction_query.actor_id);
        g_faction_query.state.store(2, std::memory_order_release);
    } catch (...) {
        snprintf(g_faction_query.error, sizeof g_faction_query.error, "faction_query_exception");
        g_faction_query.state.store(3, std::memory_order_release);
    }
}

bool faction_query_pending() {
    return g_faction_query.state.load(std::memory_order_acquire) == 1;
}

bool do_query_joinable_factions(const char* params_json, std::string* out) {
    // A game-thread read now; its slot is serviced here, so a faulted read
    // answers degraded, never a silent 0.
    FaultScope fault_scope("query_joinable_factions", out);
    if (!out) return true;
    int cid = json_get_int(params_json, "character_id");
    if (cid <= 0) cid = json_get_int(params_json, "actor_id");
    if (cid <= 0) {
        *out = R"({"action":"query_joinable_factions","error":"character_id_required"})";
        return true;
    }
    if (scope_latched()) {
        *out = std::string("{\"action\":\"query_joinable_factions\",\"error\":\"") +
               scope_latched_reason() + "\",\"retryable\":false}";
        return true;
    }
    int st = g_faction_query.state.load(std::memory_order_acquire);
    if (st == 1 || st == 4) {
        *out = R"({"action":"query_joinable_factions","error":"faction_query_busy","hint":"another faction read is running; retry"})";
        return true;
    }
    g_faction_query.actor_id = static_cast<uint32_t>(cid) & 0xFFFFFF;
    g_faction_query.error[0] = '\0';
    g_faction_query.result_json.clear();
    g_faction_query.state.store(1, std::memory_order_release);
    service_slots_if_game_thread();
    for (int i = 0; i < 140; ++i) {   // ~7 s; the evaluation is a handful of trigger calls
        slot_poll_wait();
        int s = g_faction_query.state.load(std::memory_order_acquire);
        if (s == 2) { *out = g_faction_query.result_json; g_faction_query.state.store(0, std::memory_order_release); return true; }
        if (s == 3) {
            *out = std::string("{\"action\":\"query_joinable_factions\",\"error\":\"") + g_faction_query.error + "\"}";
            g_faction_query.state.store(0, std::memory_order_release);
            return true;
        }
    }
    *out = std::string(R"({"action":"query_joinable_factions","error":"timeout_waiting_for_game_thread","hint":")") + kDrainTimeoutHint + "\"}";
    return true;
}

// ============================================================================
// faction_action — create | join | leave | press_demand (game thread)
// ============================================================================

static void reject(Intent* in, const char* code, const std::string& engine_text,
                   ResponseBuilder* r) {
    in->rejected = true;
    in->executed = false;
    in->consumed = true;
    snprintf(in->rejection_reason, sizeof in->rejection_reason, "%s", code);
    if (r) {
        if (!engine_text.empty()) { r->key("engine_reason"); r->value_string(engine_text.c_str(), engine_text.size()); }
        r->end_object();
        in->console_output = r->take();
    }
}

bool execute_faction_intent(Intent* intent) {
    const std::string& op = intent->console_command;      // create/join/leave/press_demand
    const std::string& ftype = intent->interaction_name;  // faction-type key (create)
    uint32_t actor_id = intent->actor_id & 0xFFFFFF;
    uint32_t faction_id = static_cast<uint32_t>(intent->title_id > 0 ? intent->title_id : 0) & 0xFFFFFF;
    FACLOG("execute_faction_intent: actor=%u op='%s' type='%s' faction=%u target=%u\n",
           actor_id, op.c_str(), ftype.c_str(), faction_id, intent->target_id & 0xFFFFFF);

    ResponseBuilder r;
    r.begin_object();
    r.key("faction_action"); r.value_string(op.c_str(), op.size());
    r.key("actor_id"); r.value_uint(actor_id);

    void* actor = lookup_character(actor_id);
    if (!actor || is_sentinel(actor)) { reject(intent, "actor_not_found", "", &r); return false; }
    if (g_char_is_alive && !g_char_is_alive(actor)) { reject(intent, "actor_not_alive", "", &r); return false; }
    if (scope_latched()) {
        g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        reject(intent, scope_latched_reason(), "", &r);
        return false;
    }
    uint64_t saved_rng = 0;
    bool rng = arm_fake_random_scope(&saved_rng);
    struct RngRestore {
        bool on; uint64_t saved;
        ~RngRestore() { if (on) restore_fake_random_scope(saved); }
    } restore_guard{rng, saved_rng};

    uintptr_t gsr = safe_read<uintptr_t>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
    auto get_mgr = reinterpret_cast<fn_get_mgr>(game_fn(ck3::faction_write::GET_FACTION_MANAGER));

    if (op == "create") {
        uint32_t liege_id = intent->target_id & 0xFFFFFF;
        if (!liege_id) liege_id = get_liege_id_from_addr(reinterpret_cast<uintptr_t>(actor));
        void* liege = liege_id ? lookup_character(liege_id) : nullptr;
        if (!liege || is_sentinel(liege)) { reject(intent, "actor_has_no_liege", "", &r); return false; }
        r.key("liege_id"); r.value_uint(liege_id);
        uintptr_t type = faction_type_by_name(ftype);
        r.key("faction_type"); r.value_string(ftype.c_str(), ftype.size());
        if (!type) { reject(intent, "faction_type_not_found", "", &r); return false; }
        std::string why;
        int v = can_create(type, actor, liege, &why);
        if (v < 0) { reject(intent, "faction_call_crashed", "", &r); return false; }
        if (v == 0) { reject(intent, "engine_refused_create", why, &r); return false; }
        void* f = nullptr;
        auto goc = reinterpret_cast<fn_get_or_create>(game_fn(ck3::faction_write::GET_OR_CREATE_AND_JOIN));
        if (!guarded(5, [&] {
                void* mgr = get_mgr(reinterpret_cast<void*>(gsr));
                if (mgr) f = goc(mgr, reinterpret_cast<void*>(type), actor, liege);
            })) { reject(intent, "faction_call_crashed", "", &r); return false; }
        uintptr_t fa = reinterpret_cast<uintptr_t>(f);
        if (!fa || fa == g_base_addr + ck3::faction_write::NULL_FACTION_SENTINEL) {
            reject(intent, "faction_returned_null_sentinel", "", &r); return false;
        }
        faction_id = safe_read<uint32_t>(fa + ck3::faction_v2::FACTION_ID) & 0xFFFFFF;
    } else if (op == "join" || op == "leave" || op == "press_demand") {
        if (!faction_id) { reject(intent, "faction_id_required", "", &r); return false; }
        uintptr_t f = faction_by_id(faction_id);
        r.key("faction_id"); r.value_uint(faction_id);
        if (!f) { reject(intent, "faction_not_found", "", &r); return false; }
        std::string key = type_key(safe_read<uintptr_t>(f + ck3::faction_v2::TYPE_PTR));
        r.key("faction_type"); r.value_string(key.c_str(), key.size());
        // leave / press_demand return from their own branch: name the character
        // the faction is aimed at here, before the call changes the faction (a
        // leave can disband it). It is the liege whose faction list the MCP
        // server's second read searches. join gets it from the tail below.
        if (op != "join") {
            uint32_t tgt = safe_read<uint32_t>(f + ck3::faction_v2::TARGET_CHAR_REF);
            if (tgt && tgt != 0xFFFFFFFF) { r.key("target_id"); r.value_uint(tgt & 0xFFFFFF); }
        }
        if (op == "join") {
            if (is_member(f, actor) == 1) { reject(intent, "already_a_member", "", &r); return false; }
            std::string why;
            int v = can_join(actor, f, &why);
            if (v < 0) { reject(intent, "faction_call_crashed", "", &r); return false; }
            if (v == 0) { reject(intent, "engine_refused_join", why, &r); return false; }
            auto join = reinterpret_cast<fn_join>(game_fn(ck3::faction_funcs::JOIN_FACTION));
            uint32_t ref = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::ID);
            void* claimant_default = reinterpret_cast<void*>(game_fn(ck3::faction_write::JOIN_DEFAULT_CLAIMANT_DATA));
            if (!guarded(5, [&] { join(reinterpret_cast<void*>(f), ref, -1, claimant_default, 0); })) {
                reject(intent, "faction_call_crashed", "", &r); return false;
            }
        } else if (op == "leave") {
            if (is_member(f, actor) != 1) { reject(intent, "not_a_member", "", &r); return false; }
            std::string why;
            int v = can_leave(f, actor, &why);
            if (v < 0) { reject(intent, "faction_call_crashed", "", &r); return false; }
            if (v == 0) { reject(intent, "engine_refused_leave", why, &r); return false; }
            auto rm = reinterpret_cast<fn_remove>(game_fn(ck3::faction_write::FACTION_REMOVE_MEMBER));
            uint64_t reason_value = safe_read_offset<uint64_t>(ck3::faction_write::LEAVE_REASON_VALUE);
            uint32_t ref = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(actor) + ck3::character::ID);
            uint8_t destroyed = 0;
            if (!guarded(5, [&] {
                    void* mgr = get_mgr(reinterpret_cast<void*>(gsr));
                    if (mgr) rm(mgr, reinterpret_cast<void*>(f), ref, reason_value, &destroyed);
                })) { reject(intent, "faction_call_crashed", "", &r); return false; }
            r.key("faction_disbanded"); r.value_bool(destroyed != 0);
            // Read back through the faction DB, not the call's own flag: a faction
            // that emptied is gone; one that remains must no longer list the actor.
            uintptr_t after = faction_by_id(faction_id);
            int still = after ? is_member(after, actor) : 0;
            r.key("is_member"); r.value_bool(still == 1);
            bool ok = (still == 0);
            intent->executed = ok; intent->rejected = !ok; intent->consumed = true;
            if (!ok) snprintf(intent->rejection_reason, sizeof intent->rejection_reason, "still_a_member_after_leave");
            r.end_object();
            intent->console_output = r.take();
            FACLOG("leave: actor=%u faction=%u ok=%d disbanded=%d\n", actor_id, faction_id, ok, destroyed);
            return ok;
        } else {   // press_demand
            // The bridge's own pre-check, not the game's: only the leader presses.
            // The reply names the leader so the refusal can say who he is.
            uint32_t lead = leader_ref(f);
            if (lead != actor_id) {
                if (lead) emit_char_ref(r, "leader_id", "leader_name", lead);
                reject(intent, "not_the_faction_leader", "", &r);
                return false;
            }
            std::string why;
            int v = can_press(f, &why);
            if (v < 0) { reject(intent, "faction_call_crashed", "", &r); return false; }
            if (v == 0) { reject(intent, "engine_refused_demand", why, &r); return false; }
            int32_t cool_before = faction_date(f, ck3::faction_v2::COOLDOWN_DATE);
            auto press = reinterpret_cast<fn_press_demand>(game_fn(ck3::faction_write::FACTION_PRESS_DEMAND));
            if (!guarded(5, [&] { press(reinterpret_cast<void*>(f)); })) {
                reject(intent, "faction_call_crashed", "", &r); return false;
            }
            // Read back: the engine re-gates the press; after a demand is sent it no
            // longer allows another (the cooldown or the war that follows).
            uintptr_t after = faction_by_id(faction_id);
            std::string after_why;
            int again = after ? can_press(after, &after_why) : 0;
            int32_t cool_after = after ? faction_date(after, ck3::faction_v2::COOLDOWN_DATE) : 0;
            r.key("demand_sent"); r.value_bool(again == 0);
            if (cool_after != cool_before && cool_after > 0) emit_date(r, "demand_cooldown_until", cool_after);
            if (after) { r.key("state_after"); r.begin_object(); emit_demand_state(r, after, safe_read<uintptr_t>(after + ck3::faction_v2::TYPE_PTR)); r.end_object(); }
            if (!after_why.empty()) { r.key("press_again_reason"); r.value_string(after_why.c_str(), after_why.size()); }
            bool ok = (again == 0);
            intent->executed = ok; intent->rejected = !ok; intent->consumed = true;
            if (!ok) snprintf(intent->rejection_reason, sizeof intent->rejection_reason, "demand_still_pressable_after_press");
            r.end_object();
            intent->console_output = r.take();
            FACLOG("press_demand: actor=%u faction=%u ok=%d\n", actor_id, faction_id, ok);
            return ok;
        }
    } else {
        reject(intent, "unknown_faction_action", "", &r);
        return false;
    }

    // create / join: read back membership in the faction.
    uintptr_t f = faction_by_id(faction_id);
    int mem = f ? is_member(f, actor) : 0;
    if (op == "create") { r.key("faction_id"); r.value_uint(faction_id); }   // join named it above
    if (f) {
        // The character the faction is aimed at: the liege whose faction list a
        // second read must find it in (a faction test run: the actor's liege is not
        // always the target).
        uint32_t tgt = safe_read<uint32_t>(f + ck3::faction_v2::TARGET_CHAR_REF);
        if (tgt && tgt != 0xFFFFFFFF) { r.key("target_id"); r.value_uint(tgt & 0xFFFFFF); }
    }
    r.key("is_member"); r.value_bool(mem == 1);
    if (f && leader_ref(f) == actor_id) { r.key("is_leader"); r.value_bool(true); }
    bool ok = (mem == 1);
    intent->executed = ok; intent->rejected = !ok; intent->consumed = true;
    if (!ok) snprintf(intent->rejection_reason, sizeof intent->rejection_reason, "actor_not_member_after_call");
    r.end_object();
    intent->console_output = r.take();
    FACLOG("%s: actor=%u faction=%u member=%d\n", op.c_str(), actor_id, faction_id, mem);
    return ok;
}

// IPC side: validate, enqueue IntentType::FACTION, poll for the game thread's result.
bool do_dispatch_faction_intent(const char* params_json, std::string* response_out) {
    safe_read_clear();
    int actor_raw = json_get_int(params_json, "actor_id");
    int target_raw = json_get_int(params_json, "target_id");
    int faction_raw = json_get_int(params_json, "faction_id");
    const char* ftype = json_get_string(params_json, "faction_type");
    const char* op = json_get_string(params_json, "faction_action");
    if (!op || !op[0]) op = "create";

    auto short_error = [&](const char* reason) {
        if (!response_out) return;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("faction_action");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *response_out = r.take();
    };
    if (actor_raw <= 0) { short_error("actor_id_required"); return false; }
    bool is_create = strcmp(op, "create") == 0;
    if (!is_create && strcmp(op, "join") != 0 && strcmp(op, "leave") != 0 &&
        strcmp(op, "press_demand") != 0) { short_error("unknown_faction_action"); return false; }
    if (is_create && (!ftype || !ftype[0])) { short_error("faction_type_required"); return false; }
    if (!is_create && faction_raw <= 0) { short_error("faction_id_required"); return false; }

    uint32_t actor_id = static_cast<uint32_t>(actor_raw) & 0xFFFFFF;
    if (reject_player_actor(actor_id, "faction_action", response_out)) return false;
    // Checked BEFORE the request id: generate_rng advances the game's AI RNG
    // seed, a write, so a fault so far refuses with nothing written.
    if (safe_read_had_fault()) return answer_memory_access_fault("faction_action", response_out);

    uint64_t tick = g_tick_id.load(std::memory_order_relaxed);
    char rid[64];
    snprintf(rid, sizeof rid, "fac_%u_%lu_%u", actor_id, (unsigned long)tick, generate_rng() & 0xFFFF);

    Intent intent = {};
    intent.type = IntentType::FACTION;
    intent.actor_id = actor_id;
    intent.target_id = target_raw > 0 ? (static_cast<uint32_t>(target_raw) & 0xFFFFFF) : 0;
    intent.title_id = faction_raw > 0 ? faction_raw : 0;   // the faction id rides in title_id
    intent.interaction_name = (ftype ? ftype : "");
    intent.console_command = op;
    intent.queued_tick = tick;
    intent.ttl_ticks = 100;
    intent.request_id = rid;
    // Checked BEFORE the enqueue: a player id that could not be read guarded
    // nothing (reject_player_actor reads it through safe_read).
    if (safe_read_had_fault()) return answer_memory_access_fault("faction_action", response_out);
    g_intent_queue.enqueue(std::move(intent));
    FACLOG("faction_action queued actor=%u op=%s type=%s faction=%d rid=%s\n",
           actor_id, op, ftype ? ftype : "", faction_raw, rid);

    bool found = false, ok = false;
    std::string output;
    char why[160] = {};
    for (int poll = 0; poll < 100 && !found; poll++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (const auto& c : g_intent_queue.completed_snapshot(16)) {
            if (c.request_id != rid) continue;
            found = true;
            ok = c.executed && !c.rejected;
            output = c.console_output;
            if (c.rejected && c.rejection_reason[0]) strncpy(why, c.rejection_reason, sizeof why - 1);
            break;
        }
    }
    if (response_out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(ok);
        r.key("action"); r.value_string("faction_action");
        r.key("faction_action"); r.value_string(op);
        r.key("actor_id"); r.value_uint(actor_id);
        r.key("intent_request_id"); r.value_string(rid);
        r.key("dispatch_id"); r.value_string(rid);
        if (found) {
            if (!output.empty() && output.front() == '{') { r.key("result"); r.value_raw(output); }
            if (!ok && why[0]) { r.key("reason"); r.value_string(why); }
        } else {
            r.key("reason"); r.value_string("intent_timeout_no_result");
        }
        r.end_object();
        *response_out = r.take();
    }
    return ok;
}

// ============================================================================
// The liege's answer to a demand — AI_PICK_EVENT_OPTION hook
// ============================================================================
// Every faction type's demand letter is
// recognised by its option keys (faction_demand.h classify: one FACTION_DEMAND_*REFUSE option,
// one or more FACTION_DEMAND_*ACCEPT*), the populist letter's dynamic refuse name included
// (name_key reads a composite name's fallback). The standing answer picks the refuse option or
// an accept option of whichever letter arrives. Every demand letter answered for a controlled
// character is recorded, whoever decided it (his event_rule, his standing answer, or the game's
// AI), so the seat reads a demand it set nothing for.
namespace {
namespace fd = faction_demand;

std::mutex g_demand_mutex;
std::unordered_map<uint32_t, int> g_demand_policy CK3_EARLY_INIT;   // liege id → fd::Want
std::atomic<int> g_demand_policy_count{0};

// Controlled characters, copied once per daily tick on the game thread (faction_demand_daily)
// so the hook reads them without a lock.
constexpr int kWatchCap = 16;
std::atomic<uint32_t> g_demand_watch[kWatchCap];
std::atomic<int> g_demand_watch_count{0};

struct DemandAnswer {
    uint64_t seq = 0;
    int32_t date_raw = 0;
    uint32_t liege_id = 0;
    std::string event;
    std::vector<std::string> options;   // keys in file order ("" unread)
    int64_t ai_pick = -1;
    int64_t applied = -1;
    int refuse = -1;                    // the letter's refuse option
    const char* by = "ai";             // event_rule | respond_demand | ai
    bool overridden = false;
};
std::deque<DemandAnswer> g_demand_answers CK3_EARLY_INIT;
uint64_t g_demand_seq = 0;            // under g_demand_mutex
constexpr size_t kDemandAnswerCap = 32;

void* g_orig_ai_pick = nullptr;
using fn_ai_pick = uint64_t (*)(void* event, void* ctx, int32_t rng, uint32_t flag);
using fn_option_available = uint8_t (*)(void* option, void* ctx, uint32_t flag);

// The game's memory for faction_demand::name_key: guarded volatile reads.
struct LiveMem {
    bool u64(uint64_t a, uint64_t* o) const {
        if (!is_mapped_range(static_cast<uintptr_t>(a), 8)) return false;
        *o = *reinterpret_cast<volatile uint64_t*>(static_cast<uintptr_t>(a));
        return true;
    }
    bool i32(uint64_t a, int32_t* o) const {
        if (!is_mapped_range(static_cast<uintptr_t>(a), 4)) return false;
        *o = *reinterpret_cast<volatile int32_t*>(static_cast<uintptr_t>(a));
        return true;
    }
    bool str(uint64_t a, size_t n, std::string* o) const {
        if (!is_mapped_range(static_cast<uintptr_t>(a), n)) return false;
        o->assign(reinterpret_cast<const char*>(static_cast<uintptr_t>(a)), n);
        return true;
    }
};

// The option's `name` key (ck3_offsets.h event_sys::OPTION_* chain, faction_demand.h
// name_key for a dynamic name), or empty.
std::string option_key(uintptr_t option) {
    if (!is_mapped_range(option + ck3::event_sys::OPTION_DESC_HOLDER, 8)) return std::string();
    uintptr_t holder = *reinterpret_cast<volatile uintptr_t*>(option + ck3::event_sys::OPTION_DESC_HOLDER);
    if (holder < 0x10000 || !is_mapped_range(holder, 8)) return std::string();
    uintptr_t desc = *reinterpret_cast<volatile uintptr_t*>(holder);
    if (desc < 0x10000 || !is_mapped_range(desc + ck3::event_sys::OPTION_DESC_FIRST_ENTRY, 8)) return std::string();
    uintptr_t entry = *reinterpret_cast<volatile uintptr_t*>(desc + ck3::event_sys::OPTION_DESC_FIRST_ENTRY);
    return fd::name_key(LiveMem{}, entry);
}

std::string event_name(uintptr_t ev) {
#ifdef _WIN32
    // CEvent's key is an MSVC std::string at +0x10 (not the database objects'
    // +0x18 that db_object::NAME_KEY carries on Windows): ck3_offsets_win.h, 3000 of 3000
    // events of a 1.18.3.1 core. Read with the option keys' layout reader.
    uint64_t data = 0, len = 0;
    if (!fd::string_at(LiveMem{}, ev + ck3_win::struct_layout::CEVENT_NAME_KEY, fd::StrAbi::kMsvc, &data, &len) ||
        len == 0 || len > 96 || data < 0x10000 || !is_mapped_range(static_cast<uintptr_t>(data), len))
        return std::string();
    return std::string(reinterpret_cast<const char*>(static_cast<uintptr_t>(data)), len);
#else
    if (!is_mapped_range(ev + ck3::db_object::NAME_KEY, 16)) return std::string();
    uintptr_t data = *reinterpret_cast<volatile uintptr_t*>(ev + ck3::db_object::NAME_KEY);
    uint64_t len = *reinterpret_cast<volatile uint64_t*>(ev + ck3::db_object::NAME_LENGTH);
    if (len == 0 || len > 96 || data < 0x10000 || !is_mapped_range(data, len)) return std::string();
    return std::string(reinterpret_cast<const char*>(data), len);
#endif
}

// The event's options: their keys and the array (0 when unread).
uintptr_t read_options(uintptr_t ev, std::vector<std::string>* keys) {
    if (!is_mapped_range(ev + ck3::event_sys::CEVENT_OPTION_COUNT, 4)) return 0;
    int32_t n = *reinterpret_cast<volatile int32_t*>(ev + ck3::event_sys::CEVENT_OPTION_COUNT);
    if (n <= 0 || n > 32 || !is_mapped_range(ev + ck3::event_sys::CEVENT_OPTIONS_ARRAY, 8)) return 0;
    uintptr_t arr = *reinterpret_cast<volatile uintptr_t*>(ev + ck3::event_sys::CEVENT_OPTIONS_ARRAY);
    if (arr < 0x10000 || !is_mapped_range(arr, static_cast<size_t>(n) * 8)) return 0;
    for (int32_t i = 0; i < n; i++) {
        uintptr_t opt = *reinterpret_cast<volatile uintptr_t*>(arr + static_cast<uintptr_t>(i) * 8);
        keys->push_back(opt >= 0x10000 ? option_key(opt) : std::string());
    }
    return arr;
}

bool watched(uint32_t root) {
    const int n = g_demand_watch_count.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < kWatchCap; ++i)
        if (g_demand_watch[i].load(std::memory_order_relaxed) == root) return true;
    return false;
}

int policy_of(uint32_t root) {
    std::lock_guard<std::mutex> lk(g_demand_mutex);
    auto it = g_demand_policy.find(root);
    return it == g_demand_policy.end() ? static_cast<int>(fd::Want::kAI) : it->second;
}

// The standing answer (respond_demand) for a liege who set one: the refuse or accept option
// of a demand letter; every other event, and any doubt, returns `picked`. *decided is set when
// this answered the pick.
uint64_t demand_answer(uintptr_t ev, void* ctx, uint32_t flag, uint32_t root, uint64_t picked, bool* decided) {
    const auto want = static_cast<fd::Want>(policy_of(root));
    if (want == fd::Want::kAI) return picked;
    std::vector<std::string> keys;
    uintptr_t arr = read_options(ev, &keys);
    if (!arr) return picked;
    const int64_t ai = (picked & 0xFFFFFFFFull) == 0xFFFFFFFFull ? -1 : static_cast<int64_t>(picked & 0xFFFFFFFFull);
    const int choose = fd::choose(want, fd::classify(keys), keys, ai);
    if (choose < 0 || ai < 0) return picked;   // not a demand letter, or the AI picked none
    uintptr_t opt = *reinterpret_cast<volatile uintptr_t*>(arr + static_cast<uintptr_t>(choose) * 8);
    auto available = reinterpret_cast<fn_option_available>(game_fn(ck3::event_sys::EVENT_OPTION_IS_AVAILABLE));
    const bool usable = opt >= 0x10000 && available(reinterpret_cast<void*>(opt), ctx, flag) != 0;
    FACLOG("demand answer: liege=%u choose=%d usable=%d ai=%lld\n", root, choose, usable ? 1 : 0,
           static_cast<long long>(ai));
    if (!usable) return picked;
    if (decided) *decided = true;
    return static_cast<uint64_t>(choose);
}

// Record a demand letter answered for a controlled character (or a liege with a standing
// answer): every other event is ignored.
void record_demand(uintptr_t ev, uint32_t root, uint64_t picked, uint64_t result, const char* by, bool policies) {
    if (!watched(root) && !(policies && policy_of(root) != static_cast<int>(fd::Want::kAI))) return;
    DemandAnswer a;
    if (!read_options(ev, &a.options)) return;
    const auto cls = fd::classify(a.options);
    if (!cls.is_demand()) return;
    a.event = event_name(ev);
    a.liege_id = root;
    a.date_raw = game_date_raw();
    a.ai_pick = (picked & 0xFFFFFFFFull) == 0xFFFFFFFFull ? -1 : static_cast<int64_t>(picked & 0xFFFFFFFFull);
    a.applied = (result & 0xFFFFFFFFull) == 0xFFFFFFFFull ? -1 : static_cast<int64_t>(result & 0xFFFFFFFFull);
    a.refuse = cls.refuse;
    a.by = by;
    a.overridden = a.applied != a.ai_pick;
    std::lock_guard<std::mutex> lk(g_demand_mutex);
    a.seq = ++g_demand_seq;
    FACLOG("demand letter: liege=%u event=%s ai=%lld applied=%lld by=%s\n", root, a.event.c_str(),
           static_cast<long long>(a.ai_pick), static_cast<long long>(a.applied), by);
    g_demand_answers.push_back(std::move(a));
    while (g_demand_answers.size() > kDemandAnswerCap) g_demand_answers.pop_front();
}

// FAIL-OPEN everywhere: any doubt returns the AI's own pick.
uint64_t on_ai_pick_event_option(void* event, void* ctx, int32_t rng, uint32_t flag) {
    auto orig = reinterpret_cast<fn_ai_pick>(g_orig_ai_pick);
    uint64_t picked = orig ? orig(event, ctx, rng, flag) : 0xFFFFFFFFull;
    const int n_demand = g_demand_policy_count.load(std::memory_order_relaxed);
    const int n_seats = event_rules_seat_count();   // One atomic load
    const int n_watch = g_demand_watch_count.load(std::memory_order_relaxed);
    if (n_demand == 0 && n_seats == 0 && n_watch == 0) return picked;

    uintptr_t c = reinterpret_cast<uintptr_t>(ctx);
    if (!is_mapped_range(c, ck3::event_sys::SCOPE_CTX_REF + 4)) return picked;
    if (*reinterpret_cast<volatile int16_t*>(c + ck3::event_sys::SCOPE_CTX_TYPE) !=
        ck3::event_sys::SCOPE_CTX_CHARACTER) return picked;
    uint32_t root = *reinterpret_cast<volatile uint32_t*>(c + ck3::event_sys::SCOPE_CTX_REF) & 0xFFFFFF;
    uintptr_t ev = reinterpret_cast<uintptr_t>(event);
    // A seated ruler in the event table: the table answers (and records); it falls to
    // the standing demand answer itself when it holds no rule for this event.
    bool handled = false, had_rule = false, decided = false;
    uint64_t result = picked;
    if (n_seats > 0) {
        uint64_t r = event_rules_on_pick(ev, ctx, flag, root, picked, n_demand > 0, &handled, &had_rule, &decided);
        if (handled) result = r;
    }
    if (!handled && n_demand > 0) result = demand_answer(ev, ctx, flag, root, picked, &decided);
    // had_rule is set only when the rule's option (or its else option) was applied;
    // a rule whose option the game did not offer leaves the AI's pick, recorded as the AI's
    record_demand(ev, root, picked, result, had_rule ? "event_rule" : decided ? "respond_demand" : "ai", n_demand > 0);
    return result;
}
}  // namespace

void init_faction_demand_hook() {
    event_rules_read_kill_switch();   // (seat_levers.cpp)
    uintptr_t target = game_fn(ck3::event_sys::AI_PICK_EVENT_OPTION);
    g_orig_ai_pick = install_hook(target, reinterpret_cast<void*>(on_ai_pick_event_option));
    if (g_orig_ai_pick)
        ADLOG("faction_demand_hook: installed at 0x%lx (records the demand letters of controlled characters)\n", target);
    else
        ADLOG("faction_demand_hook: FAILED to install at 0x%lx (set_faction_demand_response refuses)\n", target);
}

// Once per daily tick on the game thread: the controlled characters the hook records for.
void faction_demand_daily() {
    if (!g_orig_ai_pick) return;
    uint32_t ids[kWatchCap];
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        for (uint32_t c : g_controlled_chars) if (n < kWatchCap) ids[n++] = c & 0xFFFFFF;
    }
    // shrink the count first, write the slots, then publish the count: a concurrent reader
    // sees either the old ids or a prefix of the new ones, never a torn slot
    g_demand_watch_count.store(0, std::memory_order_release);
    for (int i = 0; i < n; ++i) g_demand_watch[i].store(ids[i], std::memory_order_relaxed);
    g_demand_watch_count.store(n, std::memory_order_release);
}

// What the event table needs of this file's hook.
std::string event_option_key(uintptr_t option) { return option_key(option); }
std::string event_key_of(uintptr_t ev) { return event_name(ev); }
uint64_t faction_demand_pick(uintptr_t ev, void* ctx, uint32_t flag, uint32_t root, uint64_t picked, bool* decided) {
    return demand_answer(ev, ctx, flag, root, picked, decided);
}
bool event_option_available(uintptr_t option, void* ctx, uint32_t flag) {
    auto fn = reinterpret_cast<fn_option_available>(game_fn(ck3::event_sys::EVENT_OPTION_IS_AVAILABLE));
    return option >= 0x10000 && fn(reinterpret_cast<void*>(option), ctx, flag) != 0;
}
bool event_pick_hook_installed() { return g_orig_ai_pick != nullptr; }
int32_t event_rules_game_date_raw() { return game_date_raw(); }
void event_rules_emit_date(ResponseBuilder& r, const char* key, int32_t raw) { emit_date(r, key, raw); }

// set_faction_demand_response {character_id, response: accept|refuse|ai}
bool do_set_faction_demand_response(const char* params_json, std::string* out) {
    int cid = json_get_int(params_json, "character_id");
    const char* resp = json_get_string(params_json, "response");
    auto fail = [&](const char* reason) {
        if (!out) return false;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("set_faction_demand_response");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *out = r.take();
        return false;
    };
    if (cid <= 0) return fail("character_id_required");
    fd::Want want;
    if (resp && strcmp(resp, "accept") == 0) want = fd::Want::kAccept;
    else if (resp && strcmp(resp, "refuse") == 0) want = fd::Want::kRefuse;
    else if (resp && strcmp(resp, "ai") == 0) want = fd::Want::kAI;
    else return fail("response_must_be_accept_refuse_or_ai");
    if (!g_orig_ai_pick) return fail("demand_hook_not_installed");
    uint32_t id = static_cast<uint32_t>(cid) & 0xFFFFFF;
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        bool controlled = false;
        for (uint32_t c : g_controlled_chars) if ((c & 0xFFFFFF) == id) { controlled = true; break; }
        if (!controlled) return fail("not_a_controlled_character");
    }
    {
        std::lock_guard<std::mutex> lk(g_demand_mutex);
        if (want == fd::Want::kAI) g_demand_policy.erase(id); else g_demand_policy[id] = static_cast<int>(want);
        g_demand_policy_count.store(static_cast<int>(g_demand_policy.size()), std::memory_order_relaxed);
    }
    ADLOG("set_faction_demand_response: %u -> %s\n", id, resp);
    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("set_faction_demand_response");
        r.key("character_id"); r.value_uint(id);
        r.key("response"); r.value_string(resp);
        r.end_object();
        *out = r.take();
    }
    return true;
}

namespace {
void emit_option(ResponseBuilder& r, const char* key, int64_t idx, const std::vector<std::string>& opts) {
    r.key(key);
    if (idx < 0 || static_cast<size_t>(idx) >= opts.size()) { r.value_null(); return; }
    const std::string& k = opts[static_cast<size_t>(idx)];
    if (k.empty()) r.value_int(idx); else r.value_string(k.c_str(), k.size());
}
}  // namespace

// For query_factions and query_faction_threat: the liege's standing answer and every demand
// letter answered for him (by his event_rule, his standing answer, or the game's AI).
void emit_demand_response_state(ResponseBuilder& r, uint32_t liege_id) {
    std::lock_guard<std::mutex> lk(g_demand_mutex);
    auto it = g_demand_policy.find(liege_id);
    const auto want = it == g_demand_policy.end() ? fd::Want::kAI : static_cast<fd::Want>(it->second);
    r.key("demand_response");
    r.value_string(want == fd::Want::kAccept ? "accept" : want == fd::Want::kRefuse ? "refuse" : "ai");
    r.key("demands_answered"); r.begin_array();
    for (const auto& a : g_demand_answers) {
        if (a.liege_id != liege_id) continue;
        r.begin_object();
        r.key("seq"); r.value_uint(static_cast<uint32_t>(a.seq));
        emit_date(r, "date", a.date_raw);
        r.key("event"); r.value_string(a.event.c_str(), a.event.size());
        r.key("options"); r.begin_array();
        for (const auto& k : a.options) r.value_string(k.c_str(), k.size());
        r.end_array();
        emit_option(r, "answered", a.applied, a.options);
        emit_option(r, "ai_would_have", a.ai_pick, a.options);
        emit_option(r, "refuse_option", a.refuse, a.options);
        r.key("refused"); r.value_bool(a.applied >= 0 && a.applied == a.refuse);
        r.key("by"); r.value_string(a.by);
        r.key("overridden"); r.value_bool(a.overridden);
        r.end_object();
    }
    r.end_array();
}

}  // namespace bridge
