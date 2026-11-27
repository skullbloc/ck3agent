// seat_levers.cpp — a seated ruler's levers beside the game's AI.
//
// The event route (re/findings/re_events_purse.md §1): the
// event answer table. The game's AI answers an AI ruler's event inside CoreEventQueue, in
// the same call (AI_PICK_EVENT_OPTION returns the option and the engine executes it at
// once), so a seat answers in advance: per seated (controlled) character, event key ->
// rule (event_rules.h). factions.cpp's AI_PICK_EVENT_OPTION hook calls the game's picker
// first, then event_rules_on_pick() below, which applies a rule and records the pick.
// With nobody in the table the hook pays one atomic load; the kill switch
// (CK3_BRIDGE_EVENT_RULES=0, read once at install) keeps it empty. Windows: the
// hook target is reviewed (hooks.cpp), the option gate is a recipe, the event and option keys
// read as MSVC strings (re/findings/windows/re_win2_veto_events.md §2).
//
// Route P3: query_ai_budget, the four AI budget buckets (ck3_offsets.h ai_budget).
#include "bridge.h"
#include "init_order.h"
#include "ck3_offsets.h"
#include "action_dispatch_internal.h"
#include "event_rules.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bridge {
namespace er = event_rules;
namespace {

// ── The seat's event answer table ─────────────────────────
// Per seated (controlled) character: event key -> rule (event_rules.h), and a ring
// of every pick the game made for that character while it is in the table. A
// character enters the table with `watch` or its first rule; with nobody in it the
// hook pays one atomic load (g_event_seat_count == 0). The kill switch
// (event_rules::kKillSwitchEnv = "0", read once at install) keeps it empty.

struct SeatTable {
    std::vector<std::pair<std::string, er::Rule>> rules;   // event key -> rule
};
std::mutex g_event_mutex;
std::unordered_map<uint32_t, SeatTable> g_event_seats CK3_EARLY_INIT;
std::atomic<int> g_event_seat_count{0};
std::deque<er::Pick> g_event_picks CK3_EARLY_INIT;
uint64_t g_event_seq = 0;                 // under g_event_mutex
uint64_t g_event_no_option_picks = 0;     // picks of events with no option (not recorded)
std::atomic<bool> g_event_rules_killed{false};

bool read_currency(uint32_t char_id, er::Currency c, int64_t* out) {
    void* ch = lookup_character(char_id);
    if (!ch || is_sentinel(ch)) return false;
    uintptr_t cp = reinterpret_cast<uintptr_t>(ch);
    if (!is_mapped(cp)) return false;
    if ((safe_read<uint32_t>(cp + ck3::character::ID) & 0xFFFFFF) != (char_id & 0xFFFFFF)) return false;
    uintptr_t core = safe_read<uintptr_t>(cp + ck3::character::REALM_DATA);
    if (!core || !is_mapped(core)) return false;
    size_t off = c == er::Currency::kGold ? ck3::core_data::GOLD
               : c == er::Currency::kPrestige ? ck3::core_data::PRESTIGE : ck3::core_data::PIETY;
    safe_read_clear();
    int64_t v = safe_read<int64_t>(core + off);
    if (safe_read_had_fault()) return false;
    *out = v;
    return true;
}

}  // namespace

int event_rules_seat_count() { return g_event_seat_count.load(std::memory_order_relaxed); }

// The seat's event rules follow it to the heir at the handoff (bridge_init.cpp, beside ai_veto_carry). The heir
// enters the table when the dead ruler had a table (rules or a watch); returns the rules added (event_rules.h carry_rules).
size_t event_rules_carry(uint32_t from_id, uint32_t to_id) {
    const uint32_t from = from_id & 0xFFFFFF, to = to_id & 0xFFFFFF;
    if (from == to || !to || g_event_rules_killed.load(std::memory_order_relaxed)) return 0;
    size_t added = 0;
    {
        std::lock_guard<std::mutex> lk(g_event_mutex);
        auto src = g_event_seats.find(from);
        if (src == g_event_seats.end()) return 0;
        const auto rules = src->second.rules;              // a copy: the emplace below may rehash
        auto dst = g_event_seats.find(to);
        if (dst == g_event_seats.end()) {
            if (g_event_seats.size() >= er::kMaxSeated) return 0;
            dst = g_event_seats.emplace(to, SeatTable{}).first;
        }
        added = er::carry_rules(rules, dst->second.rules);
        g_event_seat_count.store(static_cast<int>(g_event_seats.size()), std::memory_order_relaxed);
    }
    ADLOG("event_rules: CARRIED at the handoff %u -> %u rules=%zu\n", from, to, added);
    return added;
}

void event_rules_read_kill_switch() {
    g_event_rules_killed.store(er::kill_switch_off(getenv("CK3_BRIDGE_EVENT_RULES")), std::memory_order_relaxed);
    if (g_event_rules_killed.load(std::memory_order_relaxed))
        ADLOG("event_rules: disabled by CK3_BRIDGE_EVENT_RULES=0 (set_event_rule refuses)\n");
}

// One pick of the game's picker for `root`. *handled = false when root is not in the
// table (the caller answers as before). Every read guarded; any doubt keeps `picked`.
uint64_t event_rules_on_pick(uintptr_t ev, void* ctx, uint32_t flag, uint32_t root, uint64_t picked,
                             bool demand_on, bool* handled, bool* had_rule, bool* demand_decided) {
    *handled = false;
    *had_rule = false;
    *demand_decided = false;
    {
        std::lock_guard<std::mutex> lk(g_event_mutex);
        if (g_event_seats.count(root) == 0) return picked;
    }
    *handled = true;
    // A watched character: read the event and its options (each read guarded).
    const int64_t ai = (picked & 0xFFFFFFFFull) == 0xFFFFFFFFull ? -1 : static_cast<int64_t>(picked & 0xFFFFFFFFull);
    er::Pick rec;
    rec.character_id = root;
    rec.event = event_key_of(ev);
    rec.ai_pick = ai;
    rec.applied = ai;
    int32_t n = -1;
    uintptr_t arr = 0;
    if (is_mapped_range(ev + ck3::event_sys::CEVENT_OPTION_COUNT, 4))
        n = *reinterpret_cast<volatile int32_t*>(ev + ck3::event_sys::CEVENT_OPTION_COUNT);
    if (n == 0) {   // nothing to answer (a hidden event with no option): counted, not recorded
        std::lock_guard<std::mutex> lk(g_event_mutex);
        g_event_no_option_picks++;
        return picked;
    }
    std::vector<er::OptionView> opts;
    if (n > 0 && n <= er::kMaxOptions && is_mapped_range(ev + ck3::event_sys::CEVENT_OPTIONS_ARRAY, 8)) {
        arr = *reinterpret_cast<volatile uintptr_t*>(ev + ck3::event_sys::CEVENT_OPTIONS_ARRAY);
        if (arr >= 0x10000 && is_mapped_range(arr, static_cast<size_t>(n) * 8)) {
            for (int32_t i = 0; i < n; i++) {
                uintptr_t opt = *reinterpret_cast<volatile uintptr_t*>(arr + static_cast<uintptr_t>(i) * 8);
                opts.push_back({opt >= 0x10000 ? event_option_key(opt) : std::string()});
            }
        } else {
            arr = 0;
        }
    }
    for (const auto& o : opts) rec.options.push_back(o.key);

    bool has_rule = false;
    er::Rule rule;
    {
        std::lock_guard<std::mutex> lk(g_event_mutex);
        auto it = g_event_seats.find(root);
        if (it != g_event_seats.end() && !rec.event.empty())
            for (const auto& kv : it->second.rules)
                if (kv.first == rec.event) { rule = kv.second; has_rule = true; break; }
    }
    rec.gold_read = read_currency(root, er::Currency::kGold, &rec.gold_fp);
    if (rec.gold_read) {
        read_currency(root, er::Currency::kPrestige, &rec.prestige_fp);
        read_currency(root, er::Currency::kPiety, &rec.piety_fp);
    }
    uint64_t result = picked;
    if (has_rule) {
        rec.had_rule = true;
        rec.rule = er::rule_text(rule);
        int64_t have_v = 0;
        bool have = true;
        if (rule.kind == er::Kind::kPayIf) {
            if (rule.currency == er::Currency::kGold) { have = rec.gold_read; have_v = rec.gold_fp; }
            else have = read_currency(root, rule.currency, &have_v);
        }
        er::Target t = er::target_of(rule, arr ? n : -1, opts, have, have_v);
        rec.covered = t.covered;
        bool available = false;
        if (t.index >= 0 && ai >= 0 && arr) {
            uintptr_t opt = *reinterpret_cast<volatile uintptr_t*>(arr + static_cast<uintptr_t>(t.index) * 8);
            available = event_option_available(opt, ctx, flag);
        }
        er::Decision d0 = er::decide(t, ai, available);
        // The rule's own option could not be taken: its else option when the game
        // offers it, else the AI's pick, recorded as the AI's (never under the rule's name)
        rec.wanted = !rule.option_key.empty() ? er::index_of(opts, rule.option_key)
                     : (rule.kind == er::Kind::kOption ? (rule.option_index < 0 ? 0 : rule.option_index) : t.index);
        rec.else_key = rule.else_key;
        const int ei = arr ? er::index_of(opts, rule.else_key) : -1;
        bool else_ok = false;
        if (ei >= 0 && ai >= 0 && d0.why != er::Why::kApplied && d0.why != er::Why::kSameAsAi) {
            uintptr_t eo = *reinterpret_cast<volatile uintptr_t*>(arr + static_cast<uintptr_t>(ei) * 8);
            else_ok = event_option_available(eo, ctx, flag);
        }
        const er::Fallback fb = er::with_else(d0, ai, ei, else_ok);
        const er::Decision& d = fb.d;
        rec.why = d.why;
        rec.applied = d.returned;
        rec.fell_back_else = fb.fell_back_else;
        *had_rule = fb.rule_decided || fb.fell_back_else;
        if (d.returned >= 0) result = static_cast<uint64_t>(d.returned);
    } else if (demand_on) {
        bool decided = false;
        result = faction_demand_pick(ev, ctx, flag, root, picked, &decided);
        rec.by_faction_demand = decided;
        *demand_decided = decided;
        rec.applied = (result & 0xFFFFFFFFull) == 0xFFFFFFFFull ? -1 : static_cast<int64_t>(result & 0xFFFFFFFFull);
    }
    rec.date_raw = event_rules_game_date_raw();
    {
        std::lock_guard<std::mutex> lk(g_event_mutex);
        rec.seq = ++g_event_seq;
        g_event_picks.push_back(std::move(rec));
        while (g_event_picks.size() > er::kPickRingCap) g_event_picks.pop_front();
    }
    ADLOG("event pick: char=%u rule=%d ai=%lld applied=%llu\n", root, has_rule ? 1 : 0,
           static_cast<long long>(ai), static_cast<unsigned long long>(result));
    return result;
}

}  // namespace bridge

namespace bridge {
namespace {

void emit_event_rule(ResponseBuilder& r, const std::string& ev, const event_rules::Rule& x) {
    namespace er = event_rules;
    r.begin_object();
    r.key("event"); r.value_string(ev.c_str(), ev.size());
    r.key("answer"); r.value_string(er::kind_name(x.kind));
    if (!x.option_key.empty()) { r.key("option"); r.value_string(x.option_key.c_str(), x.option_key.size()); }
    else { r.key("option_index"); r.value_int(x.option_index < 0 ? 0 : x.option_index); }
    if (x.kind == er::Kind::kPayIf) {
        r.key("currency"); r.value_string(er::currency_name(x.currency));
        r.key("amount"); r.value_int(static_cast<int64_t>(x.amount_fp / er::kFp));
    }
    r.key("else"); r.value_string(x.else_key.empty() ? "ai" : x.else_key.c_str());
    r.end_object();
}

void emit_option_ref(ResponseBuilder& r, const char* key, int64_t idx, const std::vector<std::string>& opts) {
    r.key(key);
    if (idx < 0) { r.value_null(); return; }
    r.begin_object();
    r.key("index"); r.value_int(idx);
    if (static_cast<size_t>(idx) < opts.size() && !opts[static_cast<size_t>(idx)].empty()) {
        r.key("key"); r.value_string(opts[static_cast<size_t>(idx)].c_str(), opts[static_cast<size_t>(idx)].size());
    }
    r.end_object();
}

}  // namespace

// set_event_rule {character_id, op: set|clear|watch|unwatch, event, answer: option|decline|pay_if,
//                 option (key) | option_index, currency: gold|prestige|piety, amount}
bool do_set_event_rule(const char* params_json, std::string* out) {
    namespace er = event_rules;
    auto fail = [&](const char* reason) {
        if (!out) return false;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("set_event_rule");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *out = r.take();
        return false;
    };
    if (g_event_rules_killed.load(std::memory_order_relaxed)) return fail("event_rules_disabled");
    if (!event_pick_hook_installed()) return fail("event_hook_not_installed");
    int cid = json_get_int(params_json, "character_id");
    if (cid <= 0) return fail("character_id_required");
    const char* op_s = json_get_string(params_json, "op");
    std::string op = (op_s && op_s[0]) ? op_s : "set";
    if (op != "set" && op != "clear" && op != "watch" && op != "unwatch") return fail("op_must_be_set_clear_watch_or_unwatch");
    uint32_t id = static_cast<uint32_t>(cid) & 0xFFFFFF;
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        bool controlled = false;
        for (uint32_t c : g_controlled_chars) if ((c & 0xFFFFFF) == id) { controlled = true; break; }
        if (!controlled) return fail("not_a_controlled_character");
    }
    const char* ev_s = json_get_string(params_json, "event");
    std::string ev = ev_s ? ev_s : "";
    er::Rule rule;
    if (op == "set" || op == "clear") {
        if (ev.empty()) return fail("event_required");
        if (!er::event_key_ok(ev)) return fail("bad_event_key");
    }
    if (op == "set") {
        const char* ans = json_get_string(params_json, "answer");
        if (!ans || !er::kind_by_name(ans, &rule.kind)) return fail("answer_must_be_option_decline_or_pay_if");
        const char* opt = json_get_string(params_json, "option");
        const bool has_index = json_has_key(params_json, "option_index");
        if (opt && opt[0]) {
            if (has_index) return fail("option_and_option_index_both_given");
            if (!er::option_key_ok(opt)) return fail("bad_option_key");
            rule.option_key = opt;
        } else if (has_index) {
            int oi = json_get_int(params_json, "option_index");
            if (oi < 0 || oi >= er::kMaxOptions) return fail("bad_option_index");
            rule.option_index = oi;
        } else if (rule.kind == er::Kind::kOption) {
            return fail("option_required");
        }
        // `else` = "ai" (default) or a second option key, used when the rule's
        // option cannot be taken
        const char* els = json_get_string(params_json, "else");
        if (els && els[0] && std::string(els) != "ai") {
            if (!er::option_key_ok(els)) return fail("bad_else_option_key");
            if (rule.option_key == els) return fail("else_is_the_rules_own_option");
            rule.else_key = els;
        }
        if (rule.kind == er::Kind::kPayIf) {
            const char* cur = json_get_string(params_json, "currency");
            if (cur && cur[0] && !er::currency_by_name(cur, &rule.currency)) return fail("bad_currency");
            if (!json_has_key(params_json, "amount")) return fail("amount_required");
            int amt = json_get_int(params_json, "amount");
            if (amt < 0 || amt > er::kMaxAmount) return fail("bad_amount");
            rule.amount_fp = static_cast<int64_t>(amt) * er::kFp;
        }
    }
    bool removed = false, watched = false;
    std::vector<std::pair<std::string, er::Rule>> now;
    {
        std::lock_guard<std::mutex> lk(g_event_mutex);
        auto it = g_event_seats.find(id);
        if (op == "unwatch") {
            if (it != g_event_seats.end()) { g_event_seats.erase(it); removed = true; }
        } else {
            if (it == g_event_seats.end()) {
                if (op == "clear") return fail("no_rule_for_event");
                if (g_event_seats.size() >= er::kMaxSeated) return fail("too_many_seated");
                it = g_event_seats.emplace(id, SeatTable{}).first;
            }
            auto& rules = it->second.rules;
            if (op == "set") {
                bool replaced = false;
                for (auto& kv : rules) if (kv.first == ev) { kv.second = rule; replaced = true; break; }
                if (!replaced) {
                    if (rules.size() >= er::kMaxRulesPerChar) return fail("too_many_rules");
                    rules.emplace_back(ev, rule);
                }
            } else if (op == "clear") {
                for (size_t i = 0; i < rules.size(); i++)
                    if (rules[i].first == ev) { rules.erase(rules.begin() + static_cast<long>(i)); removed = true; break; }
                if (!removed) return fail("no_rule_for_event");
            }
            now = it->second.rules;
            watched = true;
        }
        g_event_seat_count.store(static_cast<int>(g_event_seats.size()), std::memory_order_relaxed);
    }
    ADLOG("set_event_rule: %u %s %s\n", id, op.c_str(), ev.c_str());
    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("set_event_rule");
        r.key("op"); r.value_string(op.c_str(), op.size());
        r.key("character_id"); r.value_uint(id);
        r.key("watched"); r.value_bool(watched);
        if (op == "clear" || op == "unwatch") { r.key("removed"); r.value_bool(removed); }
        r.key("rules"); r.begin_array();
        for (const auto& kv : now) emit_event_rule(r, kv.first, kv.second);
        r.end_array();
        r.end_object();
        *out = r.take();
    }
    return true;
}

// query_event_rules {character_id (0 = every seated), since_seq}: the tables and the
// recorded picks after since_seq. Bridge-local state only (no game memory).
bool do_query_event_rules(const char* params_json, std::string* out) {
    namespace er = event_rules;
    int cid = json_get_int(params_json, "character_id");
    uint32_t want = cid > 0 ? (static_cast<uint32_t>(cid) & 0xFFFFFF) : 0;
    int since_i = json_get_int(params_json, "since_seq");
    uint64_t since = since_i > 0 ? static_cast<uint64_t>(since_i) : 0;
    ResponseBuilder r; r.begin_object();
    r.key("success"); r.value_bool(true);
    r.key("action"); r.value_string("query_event_rules");
    r.key("hook_installed"); r.value_bool(event_pick_hook_installed());
    r.key("disabled_by_env"); r.value_bool(g_event_rules_killed.load(std::memory_order_relaxed));
    std::lock_guard<std::mutex> lk(g_event_mutex);
    r.key("last_seq"); r.value_uint(static_cast<uint32_t>(g_event_seq));
    r.key("no_option_picks"); r.value_uint(static_cast<uint32_t>(g_event_no_option_picks));
    r.key("ring_cap"); r.value_uint(static_cast<uint32_t>(er::kPickRingCap));
    r.key("seats"); r.begin_array();
    for (const auto& kv : g_event_seats) {
        if (want && kv.first != want) continue;
        r.begin_object();
        r.key("character_id"); r.value_uint(kv.first);
        r.key("rules"); r.begin_array();
        for (const auto& rr : kv.second.rules) emit_event_rule(r, rr.first, rr.second);
        r.end_array();
        r.end_object();
    }
    r.end_array();
    r.key("picks"); r.begin_array();
    for (const auto& p : g_event_picks) {
        if (p.seq <= since || (want && p.character_id != want)) continue;
        r.begin_object();
        r.key("seq"); r.value_uint(static_cast<uint32_t>(p.seq));
        event_rules_emit_date(r, "date", p.date_raw);
        r.key("character_id"); r.value_uint(p.character_id);
        r.key("event"); r.value_string(p.event.c_str(), p.event.size());
        r.key("options"); r.begin_array();
        for (const auto& o : p.options) r.value_string(o.c_str(), o.size());
        r.end_array();
        emit_option_ref(r, "ai_pick", p.ai_pick, p.options);
        emit_option_ref(r, "applied", p.applied, p.options);
        r.key("rule");
        if (p.had_rule) r.value_string(p.rule.c_str(), p.rule.size()); else r.value_null();
        if (p.had_rule) {
            r.key("why"); r.value_string(er::why_name(p.why));
            if (p.rule.rfind("pay_if", 0) == 0) { r.key("covered"); r.value_bool(p.covered); }
            // What the rule wanted, and who answered when it could not be taken
            emit_option_ref(r, "wanted", p.wanted, p.options);
            const bool own = p.why == er::Why::kApplied || p.why == er::Why::kSameAsAi;
            r.key("answered_by");
            r.value_string(own ? "event_rule" : p.fell_back_else ? "event_rule_else" : "ai");
            r.key("else"); r.value_string(p.else_key.empty() ? "ai" : p.else_key.c_str());
        }
        if (p.by_faction_demand) { r.key("by_faction_demand"); r.value_bool(true); }
        if (p.gold_read) {
            r.key("gold"); r.value_float(static_cast<double>(p.gold_fp) / er::kFp);
            r.key("prestige"); r.value_float(static_cast<double>(p.prestige_fp) / er::kFp);
            r.key("piety"); r.value_float(static_cast<double>(p.piety_fp) / er::kFp);
        } else {
            r.key("gold"); r.value_null();
        }
        r.end_object();
    }
    r.end_array();
    r.end_object();
    if (out) *out = r.take();
    return true;
}

}  // namespace bridge

// ── The ruler's AI budget buckets ───────────────────────────
namespace bridge {

// query_ai_budget {character_id}: the four gold buckets (ck3_offsets.h ai_budget), the
// reserved maximum and the war chest target, beside the gold field they sum to.
bool do_query_ai_budget(const char* params_json, std::string* out) {
    FaultScope fault_scope("query_ai_budget", out);
    safe_read_clear();
    auto fail = [&](const char* reason) {
        if (!out) return false;
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(false);
        r.key("action"); r.value_string("query_ai_budget");
        r.key("reason"); r.value_string(reason);
        r.end_object();
        *out = r.take();
        return false;
    };
    int cid = json_get_int(params_json, "character_id");
    if (cid <= 0) return fail("character_id_required");
    const uint32_t id = static_cast<uint32_t>(cid) & 0xFFFFFF;
    void* chp = lookup_character(id);
    if (!chp || is_sentinel(chp)) return fail("character_not_found");
    const uintptr_t ch = reinterpret_cast<uintptr_t>(chp);
    if (!is_mapped(ch) || (safe_read<uint32_t>(ch + ck3::character::ID) & 0xFFFFFF) != id)
        return fail("character_not_found");
    const uintptr_t core = safe_read<uintptr_t>(ch + ck3::character::REALM_DATA);
    if (!core || !is_mapped(core)) return fail("no_core_data");
    const int64_t gold = safe_read<int64_t>(core + ck3::core_data::GOLD);
    const uintptr_t strat = safe_read<uintptr_t>(core + ck3::core_data::AI_CONTROLLER);
    const uintptr_t data = (strat && is_mapped(strat)) ? safe_read<uintptr_t>(strat + ck3::ai_budget::STRATEGY_BUDGET_DATA) : 0;
    if (safe_read_had_fault()) return answer_memory_access_fault("query_ai_budget", out);
    if (!data || !is_mapped(data)) return fail("no_ai_budget");
    int64_t b[ck3::ai_budget::BUCKET_COUNT];
    int64_t sum = 0;
    for (int i = 0; i < ck3::ai_budget::BUCKET_COUNT; i++) {
        b[i] = safe_read<int64_t>(data + ck3::ai_budget::GOLD_BUCKETS + static_cast<uintptr_t>(i) * 8);
        sum += b[i];
    }
    const int32_t resmax = safe_read<int32_t>(data + ck3::ai_budget::RESERVED_MAX);
    const int64_t want_wc = safe_read<int64_t>(data + ck3::ai_budget::DESIRED_WAR_CHEST);
    if (safe_read_had_fault()) return answer_memory_access_fault("query_ai_budget", out);
    static const char* names[] = {"reserved", "war_chest", "long_term", "short_term"};
    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action"); r.value_string("query_ai_budget");
        r.key("character_id"); r.value_uint(id);
        r.key("gold"); r.value_float(static_cast<double>(gold) / 100000.0);
        r.key("buckets"); r.begin_object();
        for (int i = 0; i < ck3::ai_budget::BUCKET_COUNT; i++) {
            r.key(names[i]); r.value_float(static_cast<double>(b[i]) / 100000.0);
        }
        r.end_object();
        // The witness of the layout: the buckets sum to the gold field.
        r.key("buckets_sum_to_gold"); r.value_bool(sum == gold);
        r.key("reserved_max"); r.value_int(resmax);
        r.key("war_chest_target"); r.value_float(static_cast<double>(want_wc) / 100000.0);
        r.end_object();
        *out = r.take();
    }
    return true;
}

}  // namespace bridge
