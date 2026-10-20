// ai_cb_selector_hook.cpp — AI_CB_SELECTOR observer + Path B' result-struct override.
//
// Two responsibilities, one function-entry hook on AI_CB_SELECTOR (0x04d980d0):
//
// 1. OBSERVATION (ck-2x2z): when g_in_bridge_war_intent is set by
//    intent_dispatch.cpp, log the S1 verdict (cb_ref / titles / target / score)
//    that the engine wrote to the caller-owned result buffer. Replaces the
//    ck-2x8w replay probe which crashed CK3 (see ck-10f5).
//
// 2. PATH B' OVERRIDE (ck-8weg): if a global override slot is armed AND the
//    g_in_bridge_war_intent gate is set on this thread, OVERWRITE the
//    result-struct fields after the engine's writes. This rescues the
//    NO_VIABLE_PAIR / score≤0 cases by injecting a bridge-vouched
//    (cb_ref, cb_titles_ref, target_ref) triple immediately before
//    AI_WAR_POST_DECL reads them at +0x4e0.
//
//    Layout per re/findings/re_a1_path_a_re_20260425.md (CB_RESULT_*):
//      +0x00 cb_ref          (8B ptr)
//      +0x08 cb_titles_ref   (8B ptr) — caller must supply a real engine titles list
//      +0x10 target_ref      (8B ptr)
//      +0x18 sentinel_u32    (0xFFFFFFFF == NO_VIABLE_PAIR; we set 0)
//      +0xb0 score           (i32; >0 required to dispatch)
//      +0xb4 extra_roll      (u8; nonzero triggers RNG gate; we set 0)
//      sub-CDynArrays at +0x20/+0x68/+0xb8/+0x4e0 are left as the engine
//      initialised them (empty CDynArray header is acceptable for the
//      NO_VIABLE_PAIR init path).
//
//    The override is one-shot: cleared after a single hook invocation
//    consumes it, so a stale vouched triple cannot leak across war intents.

#include "action_dispatch_internal.h"
#include <cstdlib>

namespace bridge {

// =============================================================================
// Gate — set by intent_dispatch.cpp around execute_war_intent's war_post_decl
// call. When false, the hook is a pure pass-through (no reads, no logging).
// =============================================================================

thread_local bool g_in_bridge_war_intent = false;

// Last AI_CB_SELECTOR verdict captured while g_in_bridge_war_intent was set.
// execute_war_intent reads this after war_post_decl returns to surface the
// engine-selected CB into the IQLOG stream alongside the rest of the intent
// trace (without it, the [CB_PROBE] line is the only record and it can be
// hard to correlate when multiple intents drain in the same tick). Cleared
// by capture_cb_verdict_reset() at execute_war_intent ENTRY.
struct CBSelectorObservation {
    bool     captured;
    uint64_t cb_ref;
    uint64_t cb_titles;
    uint64_t target_ref;
    uint32_t sentinel;
    int32_t  score;
    uint8_t  extra_roll;
    char     verdict[64];
};
thread_local CBSelectorObservation g_last_cb_verdict = {};

void capture_cb_verdict_reset() {
    g_last_cb_verdict = {};
}

bool capture_cb_verdict_get(uint64_t* cb_ref, uint64_t* cb_titles,
                            uint64_t* target_ref, int32_t* score,
                            uint8_t* extra_roll, uint32_t* sentinel,
                            const char** verdict) {
    if (!g_last_cb_verdict.captured) return false;
    if (cb_ref)     *cb_ref     = g_last_cb_verdict.cb_ref;
    if (cb_titles)  *cb_titles  = g_last_cb_verdict.cb_titles;
    if (target_ref) *target_ref = g_last_cb_verdict.target_ref;
    if (score)      *score      = g_last_cb_verdict.score;
    if (extra_roll) *extra_roll = g_last_cb_verdict.extra_roll;
    if (sentinel)   *sentinel   = g_last_cb_verdict.sentinel;
    if (verdict)    *verdict    = g_last_cb_verdict.verdict;
    return true;
}

// =============================================================================
// Path B' override — global mutex-protected one-shot slot. When `enabled`,
// the next AI_CB_SELECTOR call gated by g_in_bridge_war_intent (i.e., one
// originating from the bridge's own execute_war_intent flow) patches the
// engine's result struct with these values before returning.
//
// Why global, not thread_local: setters (IPC handler) run on the IPC server
// thread, while the hook callback runs on the game thread (AI_CB_SELECTOR is
// invoked under war_post_decl, which the bridge calls from the game thread
// inside drain_and_execute_intents). A thread_local set on the IPC thread
// would never be visible to the game thread.
//
// Why gated by g_in_bridge_war_intent (which IS thread_local on the game
// thread): without it, the engine's routine per-character AI ticks could
// consume an armed override before our intended war_post_decl reaches the
// hook. The gate ensures only OUR war_post_decl call applies the override.
// =============================================================================

struct CBSelectorOverride {
    bool     enabled;
    uint64_t cb_ref;
    uint64_t cb_titles_ref;
    uint64_t target_ref;
    int32_t  score;
    uint8_t  extra_roll;
    // Titles CDynArray payload (result+0x20). REQUIRED for war materialisation:
    // ck-6h1v showed the override-only-gate path dispatches an EMPTY title list,
    // so WAR_CREATE_CORE produces no CWar. When titles_count > 0 the hook writes
    // {data, cap, count} at +0x20/+0x28/+0x2c AND repoints the +0x30 allocator
    // holder at a fake no-op-free allocator (the epilog frees +0x20 via that
    // holder; see re_path_b_prime_titles_array_20260610.md).
    uint64_t titles_data;    // u32* to title-handle array (lifetime ≥ war_post_decl)
    int32_t  titles_count;
    // Claimant char handle written to CB_RESULT +0x18. Despite the legacy
    // "sentinel" name, +0x18 is the CLAIMANT: WarCmd_SetTitleList copies it to
    // the war-goal struct +0x28, and WAR_DECLARE_SPECIAL_HANDLER passes it to
    // WAR_CREATE_CORE as the claimant arg. 0xFFFFFFFF = none (claimant-less CB);
    // for claim_cb it must be the claimant's canonical handle (char+0x18).
    // (re_claimant_is_cb_result_0x18_20260611.md)
    uint32_t claimant_handle;
};

static std::mutex             g_cb_override_mutex;
static CBSelectorOverride     g_cb_selector_override = {};

// =============================================================================
// Fake no-op-free allocator for the overridden titles CDynArray (+0x20).
//
// AI_WAR_POST_DECL's epilog frees the +0x20 data buffer via
// `(*(result+0x30))->vtable[0x18](holder, data, elemsize)`. If we leave the
// engine's real allocator at +0x30, it would free our bridge-owned title
// buffer with the engine heap allocator → corruption. Instead we repoint
// +0x30 at this fake allocator object whose vtable[3] (offset 0x18) is a
// no-op, so the epilog free does nothing and our buffer is untouched.
//
// Layout the engine expects:
//   result+0x30  -> g_fake_alloc_obj          (the allocator OBJECT)
//   g_fake_alloc_obj[0] == g_fake_alloc_vtable (object's vtable ptr)
//   g_fake_alloc_vtable[3] (byte +0x18) == bridge_noop_free
// =============================================================================

static void bridge_noop_free(void* /*self*/, void* /*data*/, size_t /*elemsize*/) {
    // Intentionally empty — the title buffer is bridge-owned and outlives the
    // single war_post_decl call that consumes the override.
}

// 8 slots so any incidental vtable read stays in-bounds; only the free slot is
// ever called. Itanium: [3] (+0x18, after the two destructors). MSVC: [2] (+0x10,
// one deleting destructor; ck3_offsets_win.h struct_layout::ALLOCATOR_FREE_SLOT).
static void* g_fake_alloc_vtable[8] = {
#ifdef _WIN32
    nullptr, nullptr,
    reinterpret_cast<void*>(&bridge_noop_free),  // [2] == offset 0x10 == free (MSVC)
    nullptr,
#else
    nullptr, nullptr, nullptr,
    reinterpret_cast<void*>(&bridge_noop_free),  // [3] == offset 0x18 == free
#endif
    nullptr, nullptr, nullptr, nullptr,
};
// The allocator object: first qword is the vtable pointer.
static void* g_fake_alloc_obj = static_cast<void*>(g_fake_alloc_vtable);

// Public C-callable API used by action handlers / future intent-dispatch
// integration to vouch a (cb, titles, target) triple for the next gated
// AI_CB_SELECTOR call.
void set_cb_selector_override(uint64_t cb_ref,
                              uint64_t cb_titles_ref,
                              uint64_t target_ref,
                              int32_t  score,
                              uint8_t  extra_roll,
                              uint64_t titles_data,
                              int32_t  titles_count,
                              uint32_t claimant_handle) {
    std::lock_guard<std::mutex> lock(g_cb_override_mutex);
    g_cb_selector_override.enabled         = true;
    g_cb_selector_override.cb_ref          = cb_ref;
    g_cb_selector_override.cb_titles_ref   = cb_titles_ref;
    g_cb_selector_override.target_ref      = target_ref;
    g_cb_selector_override.score           = score;
    g_cb_selector_override.extra_roll      = extra_roll;
    g_cb_selector_override.titles_data     = titles_data;
    g_cb_selector_override.titles_count    = titles_count;
    g_cb_selector_override.claimant_handle = claimant_handle;
}

void clear_cb_selector_override() {
    std::lock_guard<std::mutex> lock(g_cb_override_mutex);
    g_cb_selector_override = {};
}

bool cb_selector_override_active() {
    std::lock_guard<std::mutex> lock(g_cb_override_mutex);
    return g_cb_selector_override.enabled;
}

// Atomically take and consume the override (returns enabled=false if not set).
static CBSelectorOverride take_cb_selector_override() {
    std::lock_guard<std::mutex> lock(g_cb_override_mutex);
    CBSelectorOverride out = g_cb_selector_override;
    g_cb_selector_override = {};
    return out;
}

// =============================================================================
// Trampoline to original AI_CB_SELECTOR
//   void ai_cb_selector(void* result_buf, void* ai_ctrl, int rng)
//   result_buf is a ~0x908-byte caller-owned buffer (stack-allocated).
// =============================================================================

static void* g_orig_ai_cb_selector = nullptr;

using fn_ai_cb_selector = void(*)(void* result_buf, void* ai_ctrl, int rng);

// Patch the result struct on the caller's stack. Returns true on success.
static bool apply_cb_override(uintptr_t rb, const CBSelectorOverride& ov) {
    // The buffer is the caller's stack frame in AI_WAR_POST_DECL — it's RW
    // by definition, so direct writes are safe. We still bounds-check via
    // is_mapped before each write to defend against an unexpected pointer.
    if (!is_mapped(rb) || !is_mapped(rb + ck3::funcs::CB_RESULT_EXTRA_ROLL)) {
        return false;
    }

    auto* p_cb_ref     = reinterpret_cast<volatile uint64_t*>(rb + ck3::funcs::CB_RESULT_CB_REF);
    auto* p_cb_titles  = reinterpret_cast<volatile uint64_t*>(rb + ck3::funcs::CB_RESULT_CB_TITLES);
    auto* p_target_ref = reinterpret_cast<volatile uint64_t*>(rb + ck3::funcs::CB_RESULT_TARGET_REF);
    auto* p_sentinel   = reinterpret_cast<volatile uint32_t*>(rb + ck3::funcs::CB_RESULT_SENTINEL_U32);
    auto* p_score      = reinterpret_cast<volatile int32_t*> (rb + ck3::funcs::CB_RESULT_SCORE);
    auto* p_extra_roll = reinterpret_cast<volatile uint8_t*> (rb + ck3::funcs::CB_RESULT_EXTRA_ROLL);

    *p_cb_ref     = ov.cb_ref;
    *p_cb_titles  = ov.cb_titles_ref;
    *p_target_ref = ov.target_ref;
    // +0x18 is the CLAIMANT handle (NOT a sentinel — re_claimant_is_cb_result_0x18).
    // It flows via WarCmd_SetTitleList → war-goal+0x28 → WAR_CREATE_CORE claimant
    // arg. Writing 0 here (the old behaviour) set claimant=char 0 → claim_cb
    // silent-fails. The G1 dispatch gate does not read +0x18, so a real handle
    // (or 0xFFFFFFFF=none for claimant-less CBs) is safe for the gate.
    *p_sentinel   = ov.claimant_handle ? ov.claimant_handle : 0xFFFFFFFFu;
    *p_score      = ov.score;          // post_decl gate: score > 0
    *p_extra_roll = ov.extra_roll;     // 0 = skip the CB-level RNG gate

    // Titles CDynArray at +0x20 (ck-6h1v): without this the dispatched war
    // command carries an empty title list and WAR_CREATE_CORE makes no CWar.
    // The engine's NO_VIABLE_PAIR init left {data=null, cap=0, count=0} here.
    // We write our bridge-owned u32 title array AND repoint the +0x30 allocator
    // holder at the fake no-op-free allocator, because AI_WAR_POST_DECL's epilog
    // frees +0x20's data via (*(result+0x30))->vtable[0x18] when data != null
    // (re_path_b_prime_titles_array_20260610.md).
    if (ov.titles_data != 0 && ov.titles_count > 0) {
        if (!is_mapped(rb + ck3::funcs::CB_RESULT_TITLES_ALLOC)) {
            return false;
        }
        auto* p_titles_data  = reinterpret_cast<volatile uint64_t*>(rb + ck3::funcs::CB_RESULT_TITLES_DATA);
        auto* p_titles_cap   = reinterpret_cast<volatile int32_t*> (rb + ck3::funcs::CB_RESULT_TITLES_CAP);
        auto* p_titles_count = reinterpret_cast<volatile int32_t*> (rb + ck3::funcs::CB_RESULT_TITLES_COUNT);
        auto* p_titles_alloc = reinterpret_cast<volatile uint64_t*>(rb + ck3::funcs::CB_RESULT_TITLES_ALLOC);
        *p_titles_data  = ov.titles_data;
        *p_titles_cap   = ov.titles_count;
        *p_titles_count = ov.titles_count;
        *p_titles_alloc = reinterpret_cast<uint64_t>(&g_fake_alloc_obj);
    }
    return true;
}

// =============================================================================
// Hook callback: run original, then (when set) apply the override; otherwise
// fall through to the existing observation/log path.
// =============================================================================

static void on_ai_cb_selector(void* result_buf, void* ai_ctrl, int rng) {
    // Always call the real function first. The engine writes a valid result
    // struct (sub-CDynArrays initialised, sentinel set per its decision); the
    // override path then mutates only the fields it cares about, leaving the
    // sub-arrays as engine-owned valid empties.
    auto orig = reinterpret_cast<fn_ai_cb_selector>(g_orig_ai_cb_selector);
    orig(result_buf, ai_ctrl, rng);

    if (!result_buf) return;
    uintptr_t rb = reinterpret_cast<uintptr_t>(result_buf);

    // Override consumption + observational logging are both gated by
    // g_in_bridge_war_intent so the engine's routine per-tick AI_CB_SELECTOR
    // calls (one per AI character per evaluation) cannot accidentally consume
    // an armed override or spam logs.
    if (!g_in_bridge_war_intent) return;

    // Path B' override consumption — one-shot, takes precedence over logging.
    CBSelectorOverride ov = take_cb_selector_override();
    if (ov.enabled) {
        bool ok = apply_cb_override(rb, ov);
        ADLOG("[CB_OVERRIDE] %s: cb_ref=0x%lx titles_ref=0x%lx target=0x%lx "
              "score=%d extra_roll=%u titles_data=0x%lx titles_count=%d claimant=0x%08x\n",
              ok ? "applied" : "FAILED (rb unmapped)",
              (unsigned long)ov.cb_ref, (unsigned long)ov.cb_titles_ref,
              (unsigned long)ov.target_ref, ov.score, ov.extra_roll,
              (unsigned long)ov.titles_data, ov.titles_count,
              ov.claimant_handle ? ov.claimant_handle : 0xFFFFFFFFu);
        return;
    }

    // Observational path — log the engine's verdict.

    if (!is_mapped(rb) || !is_mapped(rb + ck3::funcs::CB_RESULT_EXTRA_ROLL)) {
        ADLOG("[CB_PROBE] result buffer not mapped: %p\n", result_buf);
        return;
    }

    uint64_t cb_ref     = safe_read<uint64_t>(rb + ck3::funcs::CB_RESULT_CB_REF);
    uint64_t cb_titles  = safe_read<uint64_t>(rb + ck3::funcs::CB_RESULT_CB_TITLES);
    uint64_t target_ref = safe_read<uint64_t>(rb + ck3::funcs::CB_RESULT_TARGET_REF);
    uint32_t sentinel   = safe_read<uint32_t>(rb + ck3::funcs::CB_RESULT_SENTINEL_U32);
    int32_t  score      = safe_read<int32_t> (rb + ck3::funcs::CB_RESULT_SCORE);
    uint8_t  extra_roll = safe_read<uint8_t> (rb + ck3::funcs::CB_RESULT_EXTRA_ROLL);

    const char* verdict;
    if (cb_ref == 0 && target_ref == 0 && sentinel == 0xFFFFFFFFu) {
        verdict = "NO_VIABLE_PAIR (AI_CB_EVALUATE_ALL returned 0)";
    } else if (score <= 0) {
        verdict = "ZERO_SCORE (weighted pick landed on 0-score entry)";
    } else if (extra_roll != 0) {
        verdict = "EXTRA_ROLL_REQUIRED (CB-level probability gate — may pass or fail rng)";
    } else if (cb_ref != 0 && target_ref != 0 && score > 0) {
        verdict = "CB_SELECTION_OK (dispatch should have fired — check vtable[0x10] probes)";
    } else {
        verdict = "PARTIAL (unexpected state)";
    }

    ADLOG("[CB_PROBE] cb_ref=0x%lx titles=0x%lx target=0x%lx sentinel=0x%08x "
          "score=%d extra_roll=%u → %s\n",
          (unsigned long)cb_ref, (unsigned long)cb_titles,
          (unsigned long)target_ref, sentinel, score, extra_roll, verdict);

    // Capture for execute_war_intent's epilogue (ck-l8ge): last verdict wins
    // if multiple AI_CB_SELECTOR calls fire while gated, which matches the
    // engine semantics — only the last result-buf state feeds AI_WAR_POST_DECL.
    g_last_cb_verdict.captured   = true;
    g_last_cb_verdict.cb_ref     = cb_ref;
    g_last_cb_verdict.cb_titles  = cb_titles;
    g_last_cb_verdict.target_ref = target_ref;
    g_last_cb_verdict.sentinel   = sentinel;
    g_last_cb_verdict.score      = score;
    g_last_cb_verdict.extra_roll = extra_roll;
    snprintf(g_last_cb_verdict.verdict, sizeof(g_last_cb_verdict.verdict),
             "%s", verdict);
}

// =============================================================================
// IPC: agent-callable override setter. Accepts a (cb, titles, target) triple
// as hex strings ("0x..." or decimal) so 64-bit pointer values survive JSON
// transport without precision loss in IsInt/IsUint paths.
//
// Params (all required for set; clear takes only "enabled":false):
//   {
//     "enabled":       bool,
//     "cb_ref":        "0x...",     // 8B ptr
//     "cb_titles_ref": "0x...",     // 8B ptr (caller-vouched titles list)
//     "target_ref":    "0x...",     // 8B ptr
//     "score":         100000,      // i32, must be > 0 to pass post_decl gate
//     "extra_roll":    0            // u8, 0 to skip RNG gate
//   }
//
// Lifetime: armed in a global mutex-protected slot. Consumed on the first
// AI_CB_SELECTOR call gated by g_in_bridge_war_intent (i.e., the bridge's
// next execute_war_intent → war_post_decl). Engine-driven routine AI ticks
// do not consume the override.
//
// Race window: between arming and consumption, the agent must not arm a
// second override or it overwrites the first. There is exactly one override
// slot — Path B' is single-character-per-call by design.
// =============================================================================

// rc2 B2: the IPC verb takes three raw 64-bit pointers from the client and
// the engine later calls vtable[0x10] through them (re_ai_cb_selector_consumer.md),
// so it is a debug verb: compiled in only with CK3_BRIDGE_DEBUG_VERBS. The
// in-process arming in intent_dispatch.cpp (engine-resolved pointers) stays.
#ifdef CK3_BRIDGE_DEBUG_VERBS
static uint64_t parse_u64_str(const char* s) {
    if (!s || !s[0]) return 0;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    return strtoull(s, nullptr, base);
}

bool do_set_cb_selector_override(const char* params_json,
                                 std::string* response_out) {
    // N3 (surface-audit 2026-08-01): "enabled":"banana" used to fall through
    // json_get_bool's default(false) → CLEARED the override with success:true —
    // a typo'd flag silently misconfigured CB steering. Present-but-non-boolean
    // is now a typed reject. An ABSENT key keeps the documented legacy clear
    // semantics ("clear takes only enabled:false"; {} has always cleared).
    bool enabled = false;
    if (json_has_key(params_json, "enabled") &&
        !json_get_bool_strict(params_json, "enabled", &enabled)) {
        if (response_out) {
            *response_out =
                R"({"success":false,"action":"set_cb_selector_override","reason":"invalid_enabled_type","hint":"enabled must be a JSON boolean (true/false), not a string or number"})";
        }
        ADLOG("[CB_OVERRIDE] rejected: non-boolean enabled param\n");
        return false;
    }

    if (!enabled) {
        clear_cb_selector_override();
        if (response_out) {
            *response_out =
                R"({"success":true,"action":"set_cb_selector_override","enabled":false})";
        }
        ADLOG("[CB_OVERRIDE] cleared via IPC\n");
        return true;
    }

    const char* cb_str     = json_get_string(params_json, "cb_ref");
    const char* titles_str = json_get_string(params_json, "cb_titles_ref");
    const char* target_str = json_get_string(params_json, "target_ref");

    uint64_t cb_ref     = parse_u64_str(cb_str);
    uint64_t cb_titles  = parse_u64_str(titles_str);
    uint64_t target_ref = parse_u64_str(target_str);

    int  score_in     = json_get_int(params_json, "score");
    int  extra_roll_in = json_get_int(params_json, "extra_roll");
    int32_t score      = (score_in <= 0) ? 100000 : score_in;
    uint8_t extra_roll = (extra_roll_in < 0) ? 0
                       : static_cast<uint8_t>(extra_roll_in & 0xFF);

    // All three pointers are required — refusing to enable a partial override
    // protects against the AI_WAR_POST_DECL gate reading a half-patched struct
    // (e.g., cb_ref set but target_ref left as 0xFFFFFFFF sentinel).
    if (cb_ref == 0 || cb_titles == 0 || target_ref == 0) {
        if (response_out) {
            *response_out =
                R"({"success":false,"action":"set_cb_selector_override","reason":"missing_triple"})";
        }
        ADLOG("[CB_OVERRIDE] rejected: missing triple "
              "(cb=0x%lx titles=0x%lx target=0x%lx)\n",
              (unsigned long)cb_ref, (unsigned long)cb_titles, (unsigned long)target_ref);
        return false;
    }

    set_cb_selector_override(cb_ref, cb_titles, target_ref, score, extra_roll);

    ADLOG("[CB_OVERRIDE] armed via IPC: cb=0x%lx titles=0x%lx target=0x%lx "
          "score=%d extra_roll=%u\n",
          (unsigned long)cb_ref, (unsigned long)cb_titles,
          (unsigned long)target_ref, (int)score, (unsigned)extra_roll);

    if (response_out) {
        ResponseBuilder rb;
        rb.begin_object();
        rb.key("success");    rb.value_bool(true);
        rb.key("action");     rb.value_string("set_cb_selector_override");
        rb.key("enabled");    rb.value_bool(true);
        rb.key("score");      rb.value_int(score);
        rb.key("extra_roll"); rb.value_int(static_cast<int>(extra_roll));
        rb.end_object();
        *response_out = rb.take();
    }
    return true;
}
#endif  // CK3_BRIDGE_DEBUG_VERBS

// =============================================================================
// Initialization — called once from bridge_init.cpp deferred init.
// =============================================================================

void init_ai_cb_selector_hook() {
    uintptr_t target = bridge::game_fn(ck3::funcs::AI_CB_SELECTOR);
    g_orig_ai_cb_selector = install_hook(target,
        reinterpret_cast<void*>(on_ai_cb_selector));

    if (g_orig_ai_cb_selector) {
        ADLOG("ai_cb_selector_hook: installed at 0x%lx (trampoline=%p)\n",
              target, g_orig_ai_cb_selector);
    } else {
        ADLOG("ai_cb_selector_hook: FAILED to install at 0x%lx\n", target);
    }
}

// =============================================================================
// DIAGNOSTIC: DECLARE_WAR_SPECIAL_HANDLER hook (raw 0x065db970).
//
// The tiny handler the engine calls (in its scoped command-processing context)
// when a code-7 declare_war command is dequeued. It calls WAR_CREATE_CORE with:
//   req(param_1)+0x08 = CCasusBelliType*   req+0x10 = cb-params   req+0x28 = claimant
//   ctx(param_2)+0x2d8 = attacker id       ctx+0x2dc = defender id
// (re_claimant_is_cb_result_0x18 / WAR_DECLARE decompile). Logging every call
// reveals BOTH the engine's own valid declarations (reference arg patterns) and
// whether our AI-path steered command actually reaches WCC in the scoped context.
// This is the diagnostic that breaks the AI-path invisibility.
// =============================================================================

static void* g_orig_war_decl_handler = nullptr;
using fn_war_decl_handler = void(*)(void* req, void* ctx);

static void on_war_decl_handler(void* req, void* ctx) {
    uintptr_t r = reinterpret_cast<uintptr_t>(req);
    uintptr_t c = reinterpret_cast<uintptr_t>(ctx);
    uint64_t cb_type  = (req && is_mapped(r + 8))     ? safe_read<uint64_t>(r + 8)     : 0;
    uint32_t claimant = (req && is_mapped(r + 0x28))  ? safe_read<uint32_t>(r + 0x28)  : 0;
    uint32_t atk      = (ctx && is_mapped(c + 0x2d8)) ? safe_read<uint32_t>(c + 0x2d8) : 0;
    uint32_t def      = (ctx && is_mapped(c + 0x2dc)) ? safe_read<uint32_t>(c + 0x2dc) : 0;
    ADLOG("[WAR_DECL_HANDLER] FIRED: atk=%u def=%u claimant=0x%08x cb_type=0x%lx (req=%p ctx=%p)\n",
          atk & 0xFFFFFF, def & 0xFFFFFF, claimant, (unsigned long)cb_type, req, ctx);
    auto orig = reinterpret_cast<fn_war_decl_handler>(g_orig_war_decl_handler);
    orig(req, ctx);
}

void init_war_decl_handler_hook() {
    // raw 0x065db970 = DECLARE_WAR_SPECIAL_HANDLER (ck3_offsets.h).
    uintptr_t target = bridge::game_fn(::event_weight::DECLARE_WAR_SPECIAL_HANDLER);
    g_orig_war_decl_handler = install_hook(target,
        reinterpret_cast<void*>(on_war_decl_handler));
    if (g_orig_war_decl_handler) {
        ADLOG("war_decl_handler_hook: installed at 0x%lx (trampoline=%p)\n",
              target, g_orig_war_decl_handler);
    } else {
        ADLOG("war_decl_handler_hook: FAILED to install at 0x%lx\n", target);
    }
}

} // namespace bridge
