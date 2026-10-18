// offgoal_war_veto_hook.cpp — Per-character OFF-GOAL routine-war veto.
//
// (re/findings/re_offgoal_veto_FINAL.md — the build-ready spec.)
//
// Hooks ai_war_post_decl(ai_ctrl, rng, bypass) at entry (header AI_WAR_POST_DECL).
// For a bridge-controlled ruler on the ROUTINE engine path (bypass==0), and only
// while the runtime flag g_offgoal_veto_enabled is true, RETURN WITHOUT calling
// the original — that ruler declares no routine offensive war this tick. Every
// other realm, and the bridge's own steered wars (bypass==1, via
// intent_dispatch's war_post_decl-by-address call), fall through to the original
// unchanged.
//
// Mirrors council_suppress_hook.cpp (the proven per-ruler suppression precedent):
// atomic fast-path flag, fail-OPEN on any doubt, direct bounds-checked reads only,
// NO allocation, NO engine calls, NO name-cache drain, return-without-orig as the
// suppression. ai_war_post_decl is void and owns all its allocations in its own
// (now-unexecuted) stack frame, so skipping it leaks nothing and is equivalent to
// the engine's own many internal early-returns (HasVassals/pacing gates).
//
// CRASH-HARDENING (crash2 class — all avoided): no engine calls under the hook,
// no allocation, no name-cache drain. Fail-open is total — every uncertain branch
// runs the original, so a mis-firing veto can only ever LET A WAR THROUGH (status
// quo); it can never crash, corrupt, or wedge the game thread. No sigsetjmp region
// is used here (like council_suppress_hook): is_mapped_range IS the guard for the
// two raw field reads, avoiding any nested-guard hazard.
//
// DEFAULT OFF: g_offgoal_veto_enabled initialises to false, so a fresh bridge
// ships INERT — the hook installs but is a pure pass-through for everyone until
// the agent arms it via the set_offgoal_veto IPC command.

#include "action_dispatch_internal.h"

#include <atomic>
#include <algorithm>
#include <mutex>

namespace bridge {

// Runtime policy flag — DEFAULT OFF. Ships inert.
std::atomic<bool> g_offgoal_veto_enabled{false};

// g_controlled_chars / g_controlled_mutex are declared in bridge.h and mutated by
// the IPC thread (dispatch_router.cpp) — the SAME set the veto scopes against.

static void* g_orig_ai_war_post_decl = nullptr;
using fn_war_post_decl = void(*)(void* ai_ctrl, int rng, int bypass);

static void on_ai_war_post_decl(void* ai_ctrl, int rng, int bypass) {
    auto orig = reinterpret_cast<fn_war_post_decl>(g_orig_ai_war_post_decl);

    // FAST PATH / FAIL-OPEN #1: flag off → run original immediately, zero work.
    // This is the shipping default and the steady state. No lock, no read.
    if (!g_offgoal_veto_enabled.load(std::memory_order_relaxed)) {
        if (orig) orig(ai_ctrl, rng, bypass);
        return;
    }

    // FAIL-OPEN #2: NEVER veto the bridge's own steered war (force/bypass=1).
    if (bypass != 0) { if (orig) orig(ai_ctrl, rng, bypass); return; }

    // FAIL-OPEN #3: bogus ai_ctrl → run original (never suppress on doubt).
    uintptr_t ac = reinterpret_cast<uintptr_t>(ai_ctrl);
    if (!is_mapped_range(ac + ck3::ai_ctrl::CHARACTER_PTR, sizeof(uintptr_t))) {
        if (orig) orig(ai_ctrl, rng, bypass);
        return;
    }
    uintptr_t ch = *reinterpret_cast<volatile uintptr_t*>(ac + ck3::ai_ctrl::CHARACTER_PTR);
    if (ch <= 0x10000 || !is_mapped_range(ch + ck3::character::ID, sizeof(uint32_t))) {
        if (orig) orig(ai_ctrl, rng, bypass);
        return;
    }
    uint32_t acting_id =
        (*reinterpret_cast<volatile uint32_t*>(ch + ck3::character::ID)) & 0xFFFFFF;

    // Is this one of OUR controlled rulers?
    bool controlled = false;
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        controlled = std::find(g_controlled_chars.begin(),
                               g_controlled_chars.end(), acting_id)
                     != g_controlled_chars.end();
    }

    if (controlled) {
        // SUPPRESS: skip routine offensive-war declaration for THIS ruler only.
        // Return without calling the original — no engine work, no memory write.
        if (g_trace)
            ADLOG("offgoal_veto: suppressed routine war for ruler %u\n", acting_id);
        return;                                   // <-- the entire veto
    }
    if (orig) orig(ai_ctrl, rng, bypass);         // not controlled → unchanged
}

void init_offgoal_war_veto_hook() {
    uintptr_t target = g_base_addr + ck3::funcs::AI_WAR_POST_DECL;
    g_orig_ai_war_post_decl =
        install_hook(target, reinterpret_cast<void*>(on_ai_war_post_decl));
    if (g_orig_ai_war_post_decl)
        ADLOG("offgoal_war_veto_hook: installed at 0x%lx (trampoline=%p, default OFF)\n",
              target, g_orig_ai_war_post_decl);
    else
        ADLOG("offgoal_war_veto_hook: FAILED to install at 0x%lx "
              "(off-goal veto disabled; behavior unchanged)\n", target);
}

// IPC setter — set_offgoal_veto {enabled:bool}. Mirrors the toggle handlers.
bool do_set_offgoal_veto(const char* params_json, std::string* out) {
    bool enabled = json_get_bool(params_json, "enabled", false);
    g_offgoal_veto_enabled.store(enabled, std::memory_order_relaxed);
    ADLOG("set_offgoal_veto: %s\n", enabled ? "ENABLED" : "disabled");
    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action");  r.value_string("set_offgoal_veto");
        r.key("enabled"); r.value_bool(enabled);
        r.end_object();
        *out = r.take();
    }
    return true;
}

} // namespace bridge
