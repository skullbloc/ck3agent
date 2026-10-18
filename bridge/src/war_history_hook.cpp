// war_history_hook.cpp — engine-facing half of the ended-war ledger (ck-sd4d).
//
// Hooks funcs::WAR_END_CORE, the single function every war in the game ends
// through. The hook copies the CWar out BEFORE calling the original, because
// the original destroys it: dtor, memset(war, 0, 0x300), WAR_DB slot freed —
// all inside the same call that sets IS_ENDED. See the long note in
// war_history.h for why an "include_ended" flag on the enumerators cannot
// work, and re/findings/re_war_end_outcome.md for the decompile.
//
// Pure observer: the hook reads with safe_read (process_vm_readv — never
// faults), calls the original unconditionally, and appends to a bridge-local
// ledger. It writes nothing into the game.

#include "action_dispatch_internal.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "war_history.h"
#include "game_date.h"
// ck-hxh7 landed: the world-reset registry is on main, so the __has_include
// guard this file shipped with is gone and the include is unconditional.
#include "world_epoch.h"

#include <chrono>

namespace bridge {

static void* g_orig_war_end = nullptr;

// void WAR_END_CORE(void* war_manager, CWar* war, int outcome, bool notify)
using fn_war_end = void (*)(void*, void*, int, unsigned int);

// ck-hxh7 discipline: the ledger is keyed by war id, and war ids are reused
// across saves. Its lifetime is the loaded WORLD, not the process. Without the
// reset, loading a different save leaves terminations recorded in the PREVIOUS
// world matchable by a reused war id, so query_war_history could answer a
// question about this world with a row from the last one. The gap the bead
// documented as "closes by itself when cdd1701 merges" is closed here.
static void reset_war_history() { war_history().clear(); }

[[maybe_unused]] static const WorldResetRegistrar g_war_history_world_scoped CK3_EARLY_INIT([] {
    register_world_reset("war_history_ledger", &reset_war_history);
});

// Read the live game clock. Returns false when GAME_STATE_ROOT is unreadable,
// in which case the record says so rather than carrying a fabricated date.
static bool read_game_clock(int32_t* raw_out) {
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (!gs) return false;
    int32_t raw = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
    if (raw <= 0) return false;
    *raw_out = raw;
    return true;
}

static void read_participants(uintptr_t container, std::vector<uint32_t>* out) {
    uintptr_t arr = safe_read<uintptr_t>(container);
    int32_t count = safe_read<int32_t>(container + 0x0C);
    if (!arr || count <= 0 || count > 100) return;
    for (int32_t i = 0; i < count && i < 32; i++) {
        uintptr_t part = safe_read<uintptr_t>(arr + i * 8);
        if (!part || part < 0x10000) continue;
        out->push_back(safe_read<uint32_t>(part + ck3::war_participant::CHAR_ID) & 0xFFFFFF);
    }
}

// Copy everything worth keeping out of a CWar that is about to be destroyed.
static bool capture_war(uintptr_t wp, int outcome, WarEndRecord* rec) {
    if (!wp || wp < 0x10000 || !is_mapped(wp)) return false;

    // Only a real CWar. A NullWar sentinel or a recycled slot carries another
    // vtable, and copying one would put a phantom war in the paper's record.
    uintptr_t vtable = safe_read<uintptr_t>(wp + ck3::war_obj::VTABLE);
    if (vtable != g_base_addr + ck3::war_obj::CWAR_VTABLE) return false;

    uint32_t war_id = safe_read<uint32_t>(wp + ck3::war_obj::WAR_ID);
    if (war_id == 0xFFFFFFFF) return false;

    rec->war_id  = war_id;
    rec->outcome = outcome;

    int32_t now_raw = 0;
    if (read_game_clock(&now_raw)) {
        rec->end_raw_date   = now_raw;
        rec->end_total_days = ck3::game_date::raw_to_total_days(now_raw);
        rec->end_date_known = true;
    }

    int32_t start_raw = safe_read<int32_t>(wp + ck3::war_obj::START_DATE_RAW);
    if (start_raw > 0) {
        rec->start_raw_date    = start_raw;
        rec->start_total_days  = ck3::game_date::raw_to_total_days(start_raw);
        rec->start_date_known  = true;
    }

    // The field re/offsets used to call END_DATE_PACKED. Carried raw so its
    // refutation stays visible in the data rather than in a comment only.
    rec->end_date_field_raw = safe_read<uint64_t>(wp + ck3::war_obj::END_DATE_PACKED);

    rec->attacker_id = safe_read<uint32_t>(wp + ck3::war_obj::PRIMARY_ATK_REF) & 0xFFFFFF;
    rec->defender_id = safe_read<uint32_t>(wp + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF;
    uint32_t claimant = safe_read<uint32_t>(wp + ck3::war_obj::CLAIMANT_REF);
    rec->claimant_id = (claimant == 0xFFFFFFFF || (claimant & 0xFFFFFF) == 0xFFFFFF)
                       ? 0xFFFFFFFFu : (claimant & 0xFFFFFF);

    uintptr_t cb_type_ref = safe_read<uintptr_t>(wp + ck3::war_obj::CB_TYPE_REF);
    if (cb_type_ref && is_mapped(cb_type_ref)) {
        rec->cb_type = ad_read_std_string(cb_type_ref + ck3::db_object::NAME_KEY);
    }
    rec->war_name = ad_read_std_string(wp + ck3::war_obj::NAME_DATA_PTR);

    uintptr_t titles = safe_read<uintptr_t>(wp + ck3::war_obj::CB_TITLE_ARRAY);
    int32_t title_count = safe_read<int32_t>(wp + ck3::war_obj::CB_TITLE_COUNT);
    if (titles && title_count > 0 && title_count <= 20) {
        for (int32_t i = 0; i < title_count; i++) {
            uint64_t t = safe_read<uint64_t>(titles + i * 8);
            rec->target_titles.push_back(static_cast<uint32_t>(t & 0xFFFFFF));
        }
    }

    read_participants(wp + ck3::war_obj::ATK_PARTICIPANT, &rec->atk_participants);
    read_participants(wp + ck3::war_obj::DEF_PARTICIPANT, &rec->def_participants);

    rec->tick = g_tick_id.load(std::memory_order_relaxed);
    rec->timestamp_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    return true;
}

static void on_war_end(void* war_manager, void* war, int outcome, unsigned int notify) {
    uintptr_t wp = reinterpret_cast<uintptr_t>(war);

    // WAR_END_CORE returns immediately when IS_ENDED is already set, and the
    // object may already have been wiped. Only the call that actually ends the
    // war is a termination event.
    bool already_ended = true;
    if (wp && wp >= 0x10000 && is_mapped(wp)) {
        already_ended = safe_read<uint8_t>(wp + ck3::war_obj::IS_ENDED) != 0;
    }

    WarEndRecord rec;
    bool captured = false;
    if (!already_ended) captured = capture_war(wp, outcome, &rec);

    auto orig = reinterpret_cast<fn_war_end>(g_orig_war_end);
    if (orig) orig(war_manager, war, outcome, notify);

    if (!already_ended && !captured) {
        // ck-yifp. The engine really did terminate a war here and the hook
        // could not copy it out. Count it, so an absent ledger row is
        // distinguishable from a hook that saw nothing — otherwise a dropped
        // termination and a war that never ended look identical downstream.
        war_history().note_uncaptured();
        ADLOG("war_history: TERMINATION NOT CAPTURED (war=%p outcome=%d) - "
              "ledger will be short a row\n", war, outcome);
    }

    if (captured) {
        war_history().record(rec);
        char when[24] = "unknown";
        if (rec.end_date_known) {
            ck3::game_date::format_total_days(
                static_cast<int32_t>(rec.end_total_days), when, sizeof(when));
        }
        ADLOG("war_history: war %u ended outcome=%s(%d) at %s (cb=%s %u->%u)\n",
              rec.war_id, war_outcome_name(rec.outcome), rec.outcome, when,
              rec.cb_type.c_str(), rec.attacker_id, rec.defender_id);
    }
}

void init_war_history_hook() {
    uintptr_t target = g_base_addr + ck3::funcs::WAR_END_CORE;
    g_orig_war_end = install_hook(target, reinterpret_cast<void*>(on_war_end));
    if (g_orig_war_end) {
        ADLOG("war_history_hook: installed at 0x%lx (trampoline=%p)\n",
              target, g_orig_war_end);
    } else {
        ADLOG("war_history_hook: FAILED to install at 0x%lx\n", target);
    }
}

// =============================================================================
// query_war_history — the read side.
//
// Params (all optional):
//   war_id, cb_type, attacker_id, defender_id, target_title, since_tick, limit
//
// A query that names ONLY target_title is REFUSED, not answered. Both scripted
// 1066 wars target k_england; answering such a query is how the meter lane
// ended up metering the Norwegian Invasion in place of the Norman Conquest.
// =============================================================================
bool do_query_war_history(const char* params_json, std::string* out) {
    if (!out) return false;

    WarHistoryFilter f;
    if (json_has_key(params_json, "war_id")) {
        f.has_war_id = true;
        f.war_id = static_cast<uint32_t>(json_get_int(params_json, "war_id"));
    }
    const char* cb = json_get_string(params_json, "cb_type");
    if (cb && *cb) { f.has_cb_type = true; f.cb_type = cb; }
    if (json_has_key(params_json, "attacker_id")) {
        f.has_attacker = true;
        f.attacker_id = static_cast<uint32_t>(json_get_int(params_json, "attacker_id"));
    }
    if (json_has_key(params_json, "defender_id")) {
        f.has_defender = true;
        f.defender_id = static_cast<uint32_t>(json_get_int(params_json, "defender_id"));
    }
    if (json_has_key(params_json, "target_title")) {
        f.has_target_title = true;
        f.target_title = static_cast<uint32_t>(json_get_int(params_json, "target_title"));
    }
    if (json_has_key(params_json, "since_tick")) {
        f.since_tick = static_cast<uint64_t>(json_get_int(params_json, "since_tick"));
    }
    int limit = json_get_int(params_json, "limit");
    f.limit = (limit > 0 && limit <= 512) ? limit : 64;

    if (filter_is_title_only(f)) {
        *out = R"({"action":"query_war_history","success":false,)"
               R"("reason":"identity_by_target_title_refused",)"
               R"("detail":"a target title does not identify a war - the 1066 start )"
               R"(has two wars targeting k_england (29767->30853 and 31808->30853). )"
               R"(Pass war_id, or cb_type together with attacker_id/defender_id."})";
        return false;
    }

    std::vector<WarEndRecord> rows = war_history().query(f);

    ResponseBuilder r;
    r.begin_object();
    r.key("action"); r.value_string("query_war_history");
    r.key("success"); r.value_bool(true);
    r.key("hook_installed"); r.value_bool(g_orig_war_end != nullptr);
    r.key("current_tick"); r.value_uint(g_tick_id.load(std::memory_order_relaxed));
    r.key("ledger_size"); r.value_uint(static_cast<uint64_t>(war_history().size()));
    r.key("total_terminations_observed"); r.value_uint(war_history().total_recorded());
    // ck-yifp: terminations the hook saw and could not copy out. A non-zero
    // value here is the difference between "this war did not end" and "this
    // bridge dropped an end", and a consumer must be able to tell.
    r.key("terminations_uncaptured"); r.value_uint(war_history().total_uncaptured());
    r.key("identifying_filter"); r.value_bool(filter_is_identifying(f));

    r.key("wars"); r.begin_array();
    for (const WarEndRecord& rec : rows) {
        r.begin_object();
        r.key("war_id"); r.value_uint(rec.war_id);
        r.key("is_ended"); r.value_bool(true);
        r.key("outcome"); r.value_string(war_outcome_name(rec.outcome));
        r.key("outcome_code"); r.value_int(rec.outcome);
        if (rec.end_date_known) {
            char d[24];
            ck3::game_date::format_total_days(
                static_cast<int32_t>(rec.end_total_days), d, sizeof(d));
            r.key("end_date"); r.value_string(d);
            r.key("end_date_raw"); r.value_int(rec.end_raw_date);
        } else {
            r.key("end_date"); r.value_string("");
            r.key("end_date_unavailable"); r.value_string("game_clock_unreadable");
        }
        if (rec.start_date_known) {
            char d[24];
            ck3::game_date::format_total_days(
                static_cast<int32_t>(rec.start_total_days), d, sizeof(d));
            r.key("start_date"); r.value_string(d);
            r.key("start_date_raw"); r.value_int(rec.start_raw_date);
        }
        // The refuted engine field, carried so nobody re-guesses it (ck-sd4d).
        r.key("end_date_field_raw"); r.value_uint(rec.end_date_field_raw);
        r.key("cb_type"); r.value_string(rec.cb_type.c_str(), rec.cb_type.size());
        r.key("attacker_id"); r.value_uint(rec.attacker_id);
        r.key("defender_id"); r.value_uint(rec.defender_id);
        r.key("claimant_id"); r.value_uint(rec.claimant_id);
        r.key("war_name"); r.value_string(rec.war_name.c_str(), rec.war_name.size());
        r.key("target_titles"); r.begin_array();
        for (uint32_t t : rec.target_titles) r.value_uint(t);
        r.end_array();
        r.key("atk_participants"); r.begin_array();
        for (uint32_t p : rec.atk_participants) r.value_uint(p);
        r.end_array();
        r.key("def_participants"); r.begin_array();
        for (uint32_t p : rec.def_participants) r.value_uint(p);
        r.end_array();
        r.key("tick"); r.value_uint(rec.tick);
        r.key("timestamp_ms"); r.value_uint(rec.timestamp_ms);
        r.end_object();
    }
    r.end_array();
    r.key("war_count"); r.value_int(static_cast<int>(rows.size()));
    r.end_object();
    *out = r.take();
    return true;
}

}  // namespace bridge
