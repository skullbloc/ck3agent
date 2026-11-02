// scheme_engine.cpp -- the scheme reads and the engine's own start check
// (l3-schemes, re/findings/re_schemes.md).
//
// What a player sees in the scheme window, read from the engine's own fields and
// getters: secrecy, success chance, progress, breaches, exposure and the agents in
// their slots. And what the game asks before a scheme may start: CanStartScheme, the
// body of the script trigger can_start_scheme. CreateActiveScheme checks only the
// target type, aliveness and an existing scheme; the scheme type's `allow` and
// `valid` triggers are CanStartScheme's, so a start goes through it first and a
// refusal names the trigger that failed, where the game's script wrote it.

#include "bridge.h"
#include "ck3_offsets.h"
#include "intent_queue.h"
#include "action_dispatch_internal.h"
#include "scope_slot_policy.h"
#include "scheme_engine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <setjmp.h>
#include <vector>

namespace bridge {

extern void* lookup_character(uint32_t char_id);

// ---------------------------------------------------------------------------
// type database
// ---------------------------------------------------------------------------

// inst = *(base+SCHEME_TYPE_DB); the CSchemeType* array and its count are
// scheme_type_db::TYPES_PTR / TYPES_COUNT; the key is scheme_type::KEY.
static bool scheme_type_db(uintptr_t* arr, uint32_t* count) {
    uintptr_t inst = safe_read<uintptr_t>(g_base_addr + ck3::globals::SCHEME_TYPE_DB);
    if (!inst || !is_mapped(inst)) return false;
    *arr = safe_read<uintptr_t>(inst + ck3::scheme_type_db::TYPES_PTR);
    *count = safe_read<uint32_t>(inst + ck3::scheme_type_db::TYPES_COUNT);
    return *arr && is_mapped(*arr) && *count > 0 && *count <= 512;
}

static std::string scheme_type_key(uintptr_t type_ptr) {
    std::string key;
    safe_read_game_string(type_ptr + ck3::scheme_type::KEY, key);
    return key;
}

uintptr_t scheme_type_by_name(const std::string& key) {
    uintptr_t arr = 0; uint32_t count = 0;
    if (key.empty() || !scheme_type_db(&arr, &count)) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t t = safe_read<uintptr_t>(arr + static_cast<uintptr_t>(i) * 8);
        if (t && is_mapped(t) && scheme_type_key(t) == key) return t;
    }
    return 0;
}

extern int scheme_arg_template_status(const std::string& type_name);   // scheme_init_hook.cpp

const char* scheme_bridge_refusal(uintptr_t type_ptr, const std::string& key) {
    if (safe_read<int32_t>(type_ptr + ck3::scheme_type::CATEGORY) == 1)
        return "scheme_type_needs_contract_or_artifact";
    if (scheme_arg_template_status(key) == 2)
        return "scheme_type_needs_contract_or_artifact";
    return nullptr;
}

// The script's own words for scheme_type::CATEGORY (0 hostile, 1 contract,
// 2 personal, 3 political: measured against the 46 non-contract types' scripts).
const char* scheme_category_name(int32_t category) {
    switch (category) {
        case 0: return "hostile";
        case 1: return "contract";
        case 2: return "personal";
        case 3: return "political";
        default: return "";
    }
}

// scheme_type::TARGET_TYPE, in the scheme documentation's order.
static const char* scheme_target_type_name(int32_t t) {
    switch (t) {
        case 0: return "character";
        case 1: return "title";
        case 2: return "culture";
        case 3: return "faith";
        case 4: return "nothing";
        default: return "";
    }
}

// scheme_agent_slot_type::CONTRIBUTION_TYPE values the dump shows (0, 3, 4).
static const char* agent_contribution_name(int32_t t) {
    switch (t) {
        case 0: return "secrecy";
        case 3: return "success_chance_max";
        case 4: return "speed";
        default: return "";
    }
}

static void emit_type_fields(ResponseBuilder& r, uintptr_t type_ptr) {
    int32_t cat = safe_read<int32_t>(type_ptr + ck3::scheme_type::CATEGORY);
    int32_t tt = safe_read<int32_t>(type_ptr + ck3::scheme_type::TARGET_TYPE);
    const char* cn = scheme_category_name(cat);
    const char* tn = scheme_target_type_name(tt);
    r.key("category"); if (cn[0]) r.value_string(cn); else r.value_int(cat);
    r.key("target_type"); if (tn[0]) r.value_string(tn); else r.value_int(tt);
    r.key("is_secret"); r.value_bool(safe_read<uint8_t>(type_ptr + ck3::scheme_type::IS_SECRET) != 0);
    r.key("is_basic"); r.value_bool(safe_read<uint8_t>(type_ptr + ck3::scheme_type::IS_BASIC) != 0);
    r.key("max_breaches"); r.value_int(safe_read<int32_t>(type_ptr + ck3::scheme_type::MAX_BREACHES));
}

bool do_query_scheme_types(const char* params_json, std::string* out) {
    FaultScope fault_scope("query_scheme_types", out);
    const char* want = json_get_string(params_json, "target_type");
    uintptr_t arr = 0; uint32_t count = 0;
    ResponseBuilder r; r.begin_object();
    if (!scheme_type_db(&arr, &count)) {
        r.key("error"); r.value_string("scheme_type_db_unreadable");
        r.end_object();
        if (out) *out = r.take();
        return false;
    }
    r.key("scheme_types"); r.begin_array();
    int emitted = 0;
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t t = safe_read<uintptr_t>(arr + static_cast<uintptr_t>(i) * 8);
        if (!t || !is_mapped(t)) continue;
        std::string key = scheme_type_key(t);
        if (key.empty()) continue;
        if (want && want[0] &&
            strcmp(scheme_target_type_name(safe_read<int32_t>(t + ck3::scheme_type::TARGET_TYPE)), want) != 0)
            continue;
        r.begin_object();
        r.key("scheme"); r.value_string(key.c_str(), key.size());
        emit_type_fields(r, t);
        r.end_object();
        ++emitted;
    }
    r.end_array();
    r.key("count"); r.value_int(emitted);
    r.key("database_count"); r.value_uint(count);
    r.end_object();
    if (out) *out = r.take();
    return true;
}

// ---------------------------------------------------------------------------
// one scheme's state
// ---------------------------------------------------------------------------

bool scheme_success_chance(uintptr_t scheme_ptr, double* out_pct) {
    auto fn = reinterpret_cast<int64_t(*)(uintptr_t)>(
        bridge::game_fn(ck3::funcs::SCHEME_GET_SUCCESS_CHANCE));
    if (!scheme_ptr || !is_mapped(reinterpret_cast<uintptr_t>(fn))) return false;
    sigjmp_buf saved;
    memcpy(&saved, &g_recovery_point, sizeof(sigjmp_buf));
    const bool saved_prot = g_in_protected_region;
    bool ok = false;
    g_in_protected_region = true;
    if (sigsetjmp(g_recovery_point, 1) == 0) {
        const int64_t fp = fn(scheme_ptr);
        *out_pct = static_cast<double>(fp) / 100000.0;
        ok = true;
    }
    memcpy(&g_recovery_point, &saved, sizeof(sigjmp_buf));
    g_in_protected_region = saved_prot;
    return ok;
}

static std::string char_name_of(uint32_t id) {
    void* c = lookup_character(id & 0xFFFFFF);
    if (!c || is_sentinel(c)) return {};
    return ad_read_char_name(reinterpret_cast<uintptr_t>(c));
}

void scheme_emit_state(ResponseBuilder& r, uintptr_t sp) {
    uintptr_t type_ptr = safe_read<uintptr_t>(sp + ck3::active_scheme::SCHEME_TYPE_PTR);
    if (type_ptr && is_mapped(type_ptr)) {
        std::string key = scheme_type_key(type_ptr);
        r.key("type"); r.value_string(key.empty() ? "unknown" : key.c_str());
        emit_type_fields(r, type_ptr);
    } else {
        r.key("type"); r.value_string("unknown");
    }

    double succ = 0.0;
    if (scheme_success_chance(sp, &succ)) {
        r.key("success_chance"); r.value_double(succ);
    } else {
        r.key("success_chance"); r.value_null();
    }
    r.key("success_chance_max");
    r.value_double(static_cast<double>(safe_read<int64_t>(sp + ck3::active_scheme::SUCCESS_MAX)) / 100000.0);
    r.key("secrecy"); r.value_int(safe_read<int32_t>(sp + ck3::active_scheme::SECRECY));
    int32_t progress = safe_read<int32_t>(sp + ck3::active_scheme::PROGRESS);
    int32_t max_prog = safe_read<int32_t>(sp + ck3::active_scheme::MAX_PROGRESS);
    r.key("progress"); r.value_int(progress);
    r.key("max_progress"); r.value_int(max_prog);
    r.key("breaches"); r.value_int(safe_read<int32_t>(sp + ck3::active_scheme::BREACHES));
    r.key("agent_charges"); r.value_int(safe_read<int32_t>(sp + ck3::active_scheme::AGENT_CHARGES));
    const bool exposed = safe_read<uint8_t>(sp + ck3::active_scheme::IS_EXPOSED) != 0;
    r.key("exposed"); r.value_bool(exposed);
    if (exposed) {
        r.key("exposed_date"); r.value_int(safe_read<int64_t>(sp + ck3::active_scheme::EXPOSURE_DATE));
    }
    r.key("is_frozen"); r.value_bool(safe_read<uint8_t>(sp + ck3::active_scheme::IS_FROZEN) != 0);
    int64_t start = safe_read<int64_t>(sp + ck3::active_scheme::START_DATE);
    if (start != 0) { r.key("start_date"); r.value_int(start); }

    // The agent slots: inline CSchemeAgentSlot objects, stride 0x38.
    uintptr_t slots = safe_read<uintptr_t>(sp + ck3::active_scheme::AGENT_SLOTS_PTR);
    int32_t nslots = safe_read<int32_t>(sp + ck3::active_scheme::AGENT_SLOTS_COUNT);
    if (nslots < 0 || nslots > 32 || (slots && !is_mapped(slots))) nslots = 0;
    int filled = 0;
    r.key("agents"); r.begin_array();
    for (int32_t i = 0; i < nslots && slots; ++i) {
        uintptr_t e = slots + static_cast<uintptr_t>(i) * ck3::scheme_agent_slot::STRIDE;
        uint32_t agent = safe_read<uint32_t>(e + ck3::scheme_agent_slot::AGENT_CHAR);
        uintptr_t st = safe_read<uintptr_t>(e + ck3::scheme_agent_slot::SLOT_TYPE_PTR);
        r.begin_object();
        std::string slot_key;
        if (st && is_mapped(st)) safe_read_game_string(st + ck3::scheme_agent_slot_type::KEY, slot_key);
        r.key("slot"); r.value_string(slot_key.c_str(), slot_key.size());
        if (st && is_mapped(st)) {
            int32_t ct = safe_read<int32_t>(st + ck3::scheme_agent_slot_type::CONTRIBUTION_TYPE);
            const char* cn = agent_contribution_name(ct);
            r.key("contribution_type"); if (cn[0]) r.value_string(cn); else r.value_int(ct);
        }
        if (agent == 0xFFFFFFFFu) {
            r.key("character_id"); r.value_null();
        } else {
            ++filled;
            r.key("character_id"); r.value_uint(agent & 0xFFFFFF);
            std::string nm = char_name_of(agent);
            if (!nm.empty()) { r.key("name"); r.value_string(nm.c_str(), nm.size()); }
            r.key("contribution");
            r.value_double(static_cast<double>(safe_read<int64_t>(e + ck3::scheme_agent_slot::CONTRIBUTION)) / 100000.0);
        }
        r.end_object();
    }
    r.end_array();
    r.key("agent_slots"); r.value_int(nslots);
    r.key("agents_filled"); r.value_int(filled);
}

// ---------------------------------------------------------------------------
// CanStartScheme and its failure sink
// ---------------------------------------------------------------------------

namespace {

// The sink CanStartScheme reports its refusals to (vt slots 0x00-0x18, read off
// its users: re_schemes.md §2). It records which top-level blocks (allow, valid)
// failed and the engine's own entries (steps 3/4); nothing else is read.
//
// L14 S4, 2026-10-30: what a refusal can say. The engine's entries carry the
// game's words (scheme_fail_entry::TEXT). A failing trigger carries none the
// bridge can read: with wants_text() answering true, the descriptions the game
// handed this sink on one paused live867 launch (eval_d_words_court, abduct by
// 9238, 10 refused targets) held an empty text (+0x10), an empty loc key (+0x30)
// and no lines object (+0x08) for every failing statement, and every leaf entry
// was empty as well: the game renders a trigger's words later, from the trigger
// itself, not into the description (evidence/l14/s4). So the sink answers false
// (no description is built) and a trigger row says the game gives no detail. The
// script place (block, trigger, file, line) is not reported: no player reads a
// script file.

struct SinkTree;
struct Sink { void** vt; SinkTree* tree; int node; };
struct SinkNode { uintptr_t trigger; int parent; int status; };

constexpr int kMaxNodes = 1024;

struct SinkTree {
    SinkNode nodes[kMaxNodes];
    int n = 0;
    Sink sinks[kMaxNodes];
    int nsinks = 0;
    Sink overflow;               // handed out when the tree is full (node -1)
    std::string entry_text[4];   // add_entry on the root sink: scheme_fail_entry::TEXT
    int entries = 0;
    int unknown_slot = -1;       // a sink method the engine called that we do not model
};

void** sink_vtable();

// add_entry on the root sink is CanStartScheme's own refusal (steps 3/4); on a
// node's sink it is a leaf trigger reporting itself (FUN_04f1e000), which holds
// no words (above) and is not kept.
uintptr_t sink_add_entry(Sink* s, uintptr_t entry, uintptr_t /*kind*/) {
    SinkTree* t = s->tree;
    if (s->node < 0 && t->entries < 4 && entry)
        safe_read_game_string(entry + ck3::scheme_fail_entry::TEXT, t->entry_text[t->entries++]);
    return 0;
}

Sink* sink_open_child(Sink* s, uintptr_t desc) {
    SinkTree* t = s->tree;
    if (t->n >= kMaxNodes || t->nsinks >= kMaxNodes) return &t->overflow;
    int idx = t->n++;
    t->nodes[idx] = SinkNode{desc ? safe_read<uintptr_t>(desc + ck3::scheme_fail_entry::TRIGGER) : 0, s->node, 0};
    Sink* c = &t->sinks[t->nsinks++];
    c->vt = sink_vtable(); c->tree = t; c->node = idx;
    return c;
}

uintptr_t sink_close(Sink* s, uintptr_t status) {
    if (s->node >= 0 && s->node < s->tree->n) s->tree->nodes[s->node].status = static_cast<int>(status);
    return 0;
}

uintptr_t sink_wants_text(Sink*) { return 0; }

template <int N>
uintptr_t sink_unknown(Sink* s) {
    if (s->tree->unknown_slot < 0) s->tree->unknown_slot = N;
    return 0;
}

void** sink_vtable() {
    static void* vt[16] = {
        reinterpret_cast<void*>(&sink_add_entry), reinterpret_cast<void*>(&sink_open_child),
        reinterpret_cast<void*>(&sink_close), reinterpret_cast<void*>(&sink_wants_text),
        reinterpret_cast<void*>(&sink_unknown<4>), reinterpret_cast<void*>(&sink_unknown<5>),
        reinterpret_cast<void*>(&sink_unknown<6>), reinterpret_cast<void*>(&sink_unknown<7>),
        reinterpret_cast<void*>(&sink_unknown<8>), reinterpret_cast<void*>(&sink_unknown<9>),
        reinterpret_cast<void*>(&sink_unknown<10>), reinterpret_cast<void*>(&sink_unknown<11>),
        reinterpret_cast<void*>(&sink_unknown<12>), reinterpret_cast<void*>(&sink_unknown<13>),
        reinterpret_cast<void*>(&sink_unknown<14>), reinterpret_cast<void*>(&sink_unknown<15>),
    };
    return vt;
}

// The reasons, as a player reads them: {check, text}. One row per failing
// top-level block (check allow, valid, or trigger for a block that is neither),
// one per engine entry with words (check engine_entry, text the game's words),
// and sink_method_not_modelled if the engine called a sink slot past 0x18. The
// block rows and sink_method_not_modelled carry text "": the bridge captures no
// words for them (a failing block's own wording is not read here; whether the
// engine would supply any is unwitnessed), never "the game gives no detail".
// scheme_can_start adds owner_invalid / target_invalid, text "", when nothing
// reached the sink.
void emit_reasons(ResponseBuilder& r, const SinkTree& t, uintptr_t type_ptr) {
    const uintptr_t allow = type_ptr + ck3::scheme_type::ALLOW_TRIGGER;
    const uintptr_t valid = type_ptr + ck3::scheme_type::VALID_TRIGGER;
    for (int top = 0; top < t.n; ++top) {
        if (t.nodes[top].parent != -1 || t.nodes[top].status != 2) continue;
        const uintptr_t block = t.nodes[top].trigger;
        r.begin_object();
        r.key("check"); r.value_string(block == allow ? "allow" : block == valid ? "valid" : "trigger");
        r.key("text"); r.value_string("");
        r.end_object();
    }
    for (int i = 0; i < t.entries; ++i) {
        if (t.entry_text[i].empty()) continue;
        r.begin_object();
        r.key("check"); r.value_string("engine_entry");
        r.key("text"); r.value_string(t.entry_text[i].c_str(), t.entry_text[i].size());
        r.end_object();
    }
    if (t.unknown_slot >= 0) {
        r.begin_object();
        r.key("check"); r.value_string("sink_method_not_modelled");
        r.key("text"); r.value_string("");
        r.end_object();
    }
}

}  // namespace

int scheme_can_start(uintptr_t type_ptr, void* owner, void* target, bool want_reasons,
                     std::string* reasons_json) {
    using fn_can_start = bool (*)(uintptr_t, void*, void*, void*, uintptr_t);
    using fn_validate = bool (*)(void*);
    auto can_start = reinterpret_cast<fn_can_start>(bridge::game_fn(ck3::funcs::CAN_START_SCHEME));
    auto validate = reinterpret_cast<fn_validate>(bridge::game_fn(ck3::funcs::VALIDATE_SCHEME_TARGET));
    auto fail = [&](const char* why) { if (reasons_json) *reasons_json = why; return -1; };
    if (!is_mapped(reinterpret_cast<uintptr_t>(can_start))) return fail("can_start_scheme_unmapped");
    if (!type_ptr || !owner || !target) return fail("missing_argument");
    if (bridge::scope_latched()) return fail(bridge::scope_latched_reason());

    // SSchemeTarget {int type; u32 ref}: a character target, by its full handle.
    struct { int32_t type; uint32_t ref; } tgt{0, safe_read<uint32_t>(
        reinterpret_cast<uintptr_t>(target) + ck3::funcs::CHAR_CANONICAL_HANDLE)};
    if (tgt.ref == 0) return fail("target_handle_zero");

    std::unique_ptr<SinkTree> tree;
    Sink root{};
    if (want_reasons) {
        tree.reset(new SinkTree());
        tree->overflow = Sink{sink_vtable(), tree.get(), -1};
        root = Sink{sink_vtable(), tree.get(), -1};
    }

    uint64_t saved_rng = 0;
    const bool rng = arm_fake_random_scope(&saved_rng);
    sigjmp_buf saved;
    memcpy(&saved, &g_recovery_point, sizeof(sigjmp_buf));
    const bool saved_prot = g_in_protected_region;
    volatile int verdict = -1;
    volatile bool target_ok = true;
    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig == 0) {
        bool ok = can_start(type_ptr, owner, &tgt, want_reasons ? &root : nullptr, 0);
        verdict = ok ? 1 : 0;
        if (!ok && want_reasons && is_mapped(reinterpret_cast<uintptr_t>(validate)))
            target_ok = validate(&tgt);
    }
    watchdog_disarm();
    memcpy(&g_recovery_point, &saved, sizeof(sigjmp_buf));
    g_in_protected_region = saved_prot;
    if (rng) restore_fake_random_scope(saved_rng);
    if (sig != 0) {
        return fail(sig == 2 ? "can_start_scheme_timeout" : "can_start_scheme_crashed");
    }
    if (verdict == 0 && reasons_json && want_reasons) {
        ResponseBuilder r; r.begin_array();
        const size_t before = r.str().size();
        emit_reasons(r, *tree, type_ptr);
        if (r.str().size() == before) {
            // Nothing reached the sink: one of the two gates CanStartScheme runs before
            // it uses the sink (the owner's validity, ValidateSchemeTarget) said no.
            r.begin_object();
            r.key("check"); r.value_string(target_ok ? "owner_invalid" : "target_invalid");
            r.key("text"); r.value_string("");
            r.end_object();
        }
        r.end_array();
        *reasons_json = r.take();
    }
    return verdict;
}

// ---------------------------------------------------------------------------
// the check intent: one target, or a scan
// ---------------------------------------------------------------------------

// console_command: "check:<max_results>:<id,id,...>" (ids optional).
bool execute_scheme_check_intent(Intent* intent) {
    auto reject = [&](const char* reason) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason), "%s", reason);
        intent->consumed = true;
        return false;
    };
    const uint32_t actor_id = intent->actor_id & 0xFFFFFF;
    void* owner = lookup_character(actor_id);
    if (!owner || is_sentinel(owner)) return reject("owner_not_found");
    if (g_char_is_alive && !g_char_is_alive(owner)) return reject("owner_not_alive");
    uintptr_t type_ptr = scheme_type_by_name(intent->interaction_name);
    if (!type_ptr) {
        char buf[128];
        snprintf(buf, sizeof(buf), "scheme_type_not_found:%.64s", intent->interaction_name.c_str());
        return reject(buf);
    }
    if (safe_read<int32_t>(type_ptr + ck3::scheme_type::TARGET_TYPE) != 0)
        return reject("scheme_type_targets_a_title");
    if (const char* why = scheme_bridge_refusal(type_ptr, intent->interaction_name)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s:%.64s", why, intent->interaction_name.c_str());
        return reject(buf);
    }

    int max_results = 20;
    std::vector<uint32_t> candidates;
    {
        const std::string& c = intent->console_command;   // check:<max>:<ids>
        size_t a = c.find(':');
        size_t b = (a == std::string::npos) ? std::string::npos : c.find(':', a + 1);
        if (a != std::string::npos) max_results = atoi(c.c_str() + a + 1);
        if (b != std::string::npos) {
            const char* p = c.c_str() + b + 1;
            while (*p) {
                char* end = nullptr;
                unsigned long v = strtoul(p, &end, 10);
                if (end == p) break;
                if (v) candidates.push_back(static_cast<uint32_t>(v) & 0xFFFFFF);
                p = (*end == ',') ? end + 1 : end;
            }
        }
    }
    if (max_results <= 0 || max_results > 200) max_results = 20;
    const uint32_t single = intent->target_id & 0xFFFFFF;
    const std::string key = intent->interaction_name;

    ResponseBuilder r; r.begin_object();
    r.key("scheme"); r.value_string(key.c_str(), key.size());
    r.key("owner_id"); r.value_uint(actor_id);
    emit_type_fields(r, type_ptr);

    if (single) {
        void* target = lookup_character(single);
        if (!target || is_sentinel(target)) return reject("target_not_found");
        std::string reasons;
        int v = scheme_can_start(type_ptr, owner, target, true, &reasons);
        if (v < 0) return reject(reasons.c_str());
        r.key("target_id"); r.value_uint(single);
        std::string nm = char_name_of(single);
        if (!nm.empty()) { r.key("target_name"); r.value_string(nm.c_str(), nm.size()); }
        r.key("valid"); r.value_bool(v == 1);
        if (v == 0) { r.key("reasons"); r.value_raw(reasons); }
        r.end_object();
        intent->console_output = r.take();
        intent->executed = true; intent->rejected = false; intent->consumed = true;
        return true;
    }

    // A scan over the given candidates, until max_results are valid or the time
    // budget is spent. There is no scan of the whole character database: one ran
    // live (l3m-c1, abduct, 5,308 characters) and one of those evaluations left the
    // engine's random-scope slot pointing into the game thread's stack, which trips
    // the bridge's scope latch for the rest of the process (ck-zgh4). The latch is
    // checked after every evaluation; the target that tripped it is named.
    if (candidates.empty()) return reject("candidates_required");
    const auto t0 = std::chrono::steady_clock::now();
    const long kBudgetMs = 1500;
    int checked = 0, valid = 0;
    bool budget_spent = false;
    std::string valid_rows, refused_rows;
    for (uint32_t id : candidates) {
        void* target = lookup_character(id);
        if (!target || is_sentinel(target)) continue;
        if (g_char_is_alive && !g_char_is_alive(target)) continue;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count() > kBudgetMs) { budget_spent = true; break; }
        std::string reasons;
        int v = scheme_can_start(type_ptr, owner, target, true, &reasons);
        ++checked;
        if (bridge::scope_latched()) {
            ADLOG("scheme check: the scope latch tripped evaluating %s owner=%u target=%u\n",
                  key.c_str(), actor_id, id);
            char buf[120];
            snprintf(buf, sizeof(buf), "%s:target=%u", bridge::scope_latched_reason(), id);
            return reject(buf);
        }
        if (v < 0) return reject(reasons.c_str());
        ResponseBuilder row; row.begin_object();
        row.key("id"); row.value_uint(id);
        std::string nm = char_name_of(id);
        if (!nm.empty()) { row.key("name"); row.value_string(nm.c_str(), nm.size()); }
        if (v == 1) {
            row.end_object();
            valid_rows += row.take(); valid_rows += ',';
            if (++valid >= max_results) break;
        } else {
            row.key("reasons"); row.value_raw(reasons);
            row.end_object();
            refused_rows += row.take(); refused_rows += ',';
        }
    }
    if (!valid_rows.empty()) valid_rows.pop_back();
    if (!refused_rows.empty()) refused_rows.pop_back();
    r.key("checked"); r.value_int(checked);
    r.key("valid_count"); r.value_int(valid);
    r.key("targets"); r.value_raw("[" + valid_rows + "]");
    r.key("refused"); r.value_raw("[" + refused_rows + "]");
    r.key("stopped"); r.value_string(valid >= max_results ? "max_results" : budget_spent ? "time_budget" : "exhausted");
    r.end_object();
    intent->console_output = r.take();
    intent->executed = true; intent->rejected = false; intent->consumed = true;
    return true;
}

}  // namespace bridge
