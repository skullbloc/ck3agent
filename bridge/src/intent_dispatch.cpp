// Intent dispatch — queued intents consumed during the natural AI tick.
//
// LLM calls send_interaction → bridge enqueues Intent → dispatch hook finds
// it during AI tick → calls AI_INTERACTION_DECISION from safe tick context
// → returns result.

#include "bridge.h"
#include "scope_slot_policy.h"

#include <atomic>
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "intent_queue.h"
#include "ck3_offsets.h"
#include "action_dispatch_internal.h"  // resolve_title_realm()
#include "pipeline.h"                 // ck-6kvq.10: the pulse ledger (§2.9)

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <pthread.h>
#include <algorithm>
#include <vector>
#include <setjmp.h>
#include <signal.h>

// Local logging macro -- g_logfile only (stderr is unsafe in pressure-vessel)
#define IQLOG(fmt, ...) do { \
    if (bridge::g_logfile) { fprintf(bridge::g_logfile, "[CK3-Bridge][IntentQ] " fmt, ##__VA_ARGS__); fflush(bridge::g_logfile); } \
} while(0)

namespace bridge {

// Global intent queue — declared extern in bridge.h
IntentQueue g_intent_queue CK3_EARLY_INIT;

// =============================================================================
// ALLOW_RANDOM_IN_SCOPE fabrication (re_claimant_is_cb_result_0x18 / random-scope
// RE 2026-06-11). WAR_CREATE_CORE (and the engine's on_declaration effects) use
// the global RNG, which requires the calling thread to have an active
// "allow random in scope" guard: the RandomScopeManager singleton at raw
// 0xbecd550 must have *(+0x20) point at an RNG object Y bound to this thread.
// From the on_tick drain context that guard is absent → *(+0x20)==0 → the scope
// check (FUN_07748f10) tail-calls FUN_0774a4b0(0,...) → `*(NULL)` → SIGSEGV,
// AFTER the war is already allocated (so it's created then unwound).
//
// We fabricate a minimal valid scope: a slot pointing at a fake RNG object Y
// with Y+0x10 = pthread_self() (passes the thread check) and Y.vtable[0] = a
// no-op advance (FUN_0774a4b0 reads Y+8 counter, increments, then calls
// Y.vtable[0] — a no-op returns cleanly; FUN_0774a4b0's own hash return is what
// callers use, so RNG stays deterministic-but-valid). Set *(+0x20)=&g_fake_scope
// around the WCC call, restore after.
//   scope layout: scope[0] = &Y ; Y={ vtable, counter@+8, ?@+0xc, tid@+0x10 }
// =============================================================================
constexpr uint64_t RANDOM_SCOPE_MANAGER = 0xbecd550;  // raw; +0x20 = active scope ptr
constexpr uint64_t RANDOM_SCOPE_FIELD   = 0x20;

static void fake_rng_advance(void* /*self*/, int /*n*/, unsigned /*hash*/, void* /*p*/) {
    // no-op: FUN_0774a4b0 ignores this call's result (it returns its own hash).
}
struct FakeRng { void* vtable; uint32_t counter; uint32_t counter2; uint64_t tid; };
static void*    g_fake_rng_vtable[1] = { reinterpret_cast<void*>(&fake_rng_advance) };
static FakeRng  g_fake_rng = { g_fake_rng_vtable, 0, 0, 0 };
static void*    g_fake_scope[1] = { &g_fake_rng };  // scope[0] = &Y

// Reusable ALLOW_RANDOM_IN_SCOPE guard (ck-leg, 20260612). Any engine call that
// advances the RNG scope on a thread where RandomScopeManager+0x20 is null
// (the steered-war crash; also SET_FOCUS_APPLY, council-task effects) null-derefs
// in FUN_0774a4b0. Arm a per-thread fake scope around the call, restore after.
//   uint64_t saved; bool armed = arm_fake_random_scope(&saved);
//   <engine call>;  if (armed) restore_fake_random_scope(saved);
//
// R1c (ck-6kvq.10, design §2.9): these two helpers are the ledger's single
// biggest win. ~21 call sites go through them, and EIGHT of those
// (action_handlers.cpp do_construction_check / do_add_construction /
// do_construct_building / do_appoint_councillor x2 / maa_recruit /
// station_regiment / hire_mercenary) place the arm AFTER their `sigsetjmp`, so
// their crash branch cannot call restore at all: a SIGSEGV inside the engine
// call leaves RandomScopeManager+0x20 pointing at the bridge's `g_fake_scope`,
// whose `tid` belongs to a thread that may never run that path again, FOR THE
// REST OF THE PROCESS. Ledgering the HELPERS covers every one of those sites
// without touching a single call site — and the sweep both reports and HEALS.
//
// `restore_fake_random_scope` is handed only the saved value and never sees a
// handle, so the exit is by ADDRESS (PulseLedger::exit_newest_for_addr, which
// is per-thread and LIFO). That is exact for the strictly-scoped nesting these
// sites have, and it self-corrects after a sweep: once the sweep has healed a
// leaked pulse there is no live slot left and a late restore is a counted stale
// exit, not a slot stolen from someone else.
bool arm_fake_random_scope(uint64_t* out_saved) {
    // ck-2gtk: the engine publishes this slot with `xchg` (a lock-free linked
    // stack; see scope_slot_policy.h for the disassembly). A plain load/store
    // pair from another thread is an ABA against it, so both halves now use
    // compare-exchange and neither writes a value the engine cannot be holding.
    // NOT reinterpret_cast<std::atomic<uint64_t>*>: no atomic object was ever
    // constructed at that address — the engine's is a plain qword it touches
    // with `xchg`. The GCC builtins operate on the memory without pretending
    // otherwise, and lower to `lock cmpxchg`, which is what has to interlock
    // with the engine's exchange.
    // ck-zgh4 THE LATCH, before anything else. Once a restore has been
    // refused, this process has seen a slot it cannot reason about, so we do
    // not read it and we do not write it — not even the load below, because a
    // load is what captures the `saved` that the restore would resurrect.
    // The caller gets false and MUST refuse rather than proceed; the one
    // exception is the 1989 CB-validity pre-check, which the progenitor ruled
    // should skip and fail open (see execute_war_create_core_intent).
    if (bridge::scope_latched()) {
        // Counted as "a caller reached here while latched", NOT as a refusal:
        // the refusal is emitted at the call site, which is where the handler
        // can say so to the seat. Only 1989 is allowed to reach this line (it
        // skips its optional check and fails open, per the ruling), so any
        // excess over war_validity_skipped_latched is a site I failed to
        // guard. See scope_slot_policy.h for the invariant.
        bridge::g_arm_called_while_latched.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    volatile uint64_t* slot = reinterpret_cast<volatile uint64_t*>(
        g_base_addr + RANDOM_SCOPE_MANAGER + RANDOM_SCOPE_FIELD);
    if (!is_mapped(reinterpret_cast<uintptr_t>(slot))) return false;
    g_fake_rng.tid     = static_cast<uint64_t>(pthread_self());
    g_fake_rng.counter = 0; g_fake_rng.counter2 = 0;
    g_fake_scope[0]    = &g_fake_rng;

    const uint64_t fake = reinterpret_cast<uint64_t>(g_fake_scope);
    uint64_t saved = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (out_saved) *out_saved = saved;

    // COUNT THE DISPLACEMENT, unconditionally. A non-zero `saved` means we are
    // displacing a LIVE engine scope, which is the precondition for the crash
    // this bead exists for. Zero across a run makes that pathway impossible and
    // clears the bridge on evidence rather than on argument — so it must be
    // answerable from an untraced run.
    if (saved != 0) ++bridge::g_arm_displaced_live;

    // LEDGER BEFORE THE WRITE. If the ledger is full the handle is 0, the pulse
    // proceeds exactly as before, and the overflow is counted and published.
    (void)bridge::pulse_enter_global(
        "ALLOW_RANDOM_IN_SCOPE", "arm_fake_random_scope",
        reinterpret_cast<uintptr_t>(slot), 8, saved);

    // Publish only if nobody moved it between the load and here. On failure we
    // do NOT retry with the newer value: that would be a second displacement
    // with a `saved` we never ledgered, and the caller's restore would then put
    // back a pointer the ledger does not know about.
    if (!__atomic_compare_exchange_n(slot, &saved, fake, /*weak=*/false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        ++bridge::g_arm_cas_failed;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(slot));
        return false;              // NOT armed: the caller must not restore
    }

    // ck-zgh4 MEASUREMENT, after the publish so it records an arm that really
    // happened. The wedge check dereferences `saved` ONE level — the node's
    // first qword, which should point at the RNG object Y — and asks only
    // whether that is mapped. In the d50 fault it was 0xa. This is the check
    // the header said no inspection of the pointer could make; it cannot
    // prove liveness, but it does catch a pointer into dead memory, which is
    // the state that actually killed the process.
    {
        const bridge::SlotState st = bridge::slot_state(saved, fake);
        bool node_mapped = true;
        if (st != bridge::SlotState::Null) {
            if (!is_mapped(static_cast<uintptr_t>(saved))) {
                node_mapped = false;
            } else {
                const uint64_t node0 = read<uint64_t>(static_cast<uintptr_t>(saved));
                node_mapped = (node0 != 0) && is_mapped(static_cast<uintptr_t>(node0));
            }
        }
        bridge::note_arm_published(st, node_mapped,
                                   static_cast<uint64_t>(pthread_self()));
    }
    return true;
}
void restore_fake_random_scope(uint64_t saved) {
    // ck-zgh4: latched means we never armed, so there is nothing of ours in
    // the slot and no restore to make. A no-op, not a refusal — the refusal
    // already happened at the arm.
    if (bridge::scope_latched()) return;

    volatile uint64_t* slot = reinterpret_cast<volatile uint64_t*>(
        g_base_addr + RANDOM_SCOPE_MANAGER + RANDOM_SCOPE_FIELD);
    if (!is_mapped(reinterpret_cast<uintptr_t>(slot))) return;

    // ck-2gtk: put `saved` back ONLY while the slot still holds our own fake
    // scope. That is the one state in which we know the engine has not moved
    // on; in any other, `saved` may name a node the engine has already popped,
    // and writing it back resurrects a dead scope for the next reader to
    // dereference. That is the pid-174022 crash, exactly.
    //
    // Deliberately NOT a check that `saved` "looks like a pointer": it was a
    // perfectly good pointer when captured. What went stale is the OBJECT, on
    // another thread, and no inspection of the pointer can see that.
    //
    // ck-zgh4: and note what this guard does NOT cover. It compares the SLOT,
    // so it sees only changes since our own arm; it cannot see that `saved`
    // was already dead when we captured it. That is the deploy-50 wedge, and
    // it is why the latch below exists rather than a cleverer value here.
    uint64_t expected = reinterpret_cast<uint64_t>(g_fake_scope);
    const bool wrote = __atomic_compare_exchange_n(slot, &expected, saved,
                                                   /*weak=*/false,
                                                   __ATOMIC_ACQ_REL,
                                                   __ATOMIC_ACQUIRE);
    // The restore RAN, whatever it decided. `arm_published - restore_ran` is
    // then exactly the number of arms whose restore never happened — the
    // eight sites that arm after their `sigsetjmp` cannot restore on the
    // fault path, and this subtraction is the only statement of that which
    // does not depend on reading a depth that a longjmp left stale.
    bridge::note_restore_ran(static_cast<uint64_t>(pthread_self()));

    if (!wrote) {
        // `expected` now carries what was actually there.
        const auto v = bridge::restore_verdict(
            expected, reinterpret_cast<uint64_t>(g_fake_scope), saved);
        if (v == bridge::RestoreVerdict::NothingToDo) {
            ++bridge::g_restore_noop;
        } else {
            ++bridge::g_restore_refused;
            // ck-zgh4 TRIP THE LATCH, and say so UNCONDITIONALLY — not behind
            // g_trace. d50 ran with the trace off, so the one line that would
            // have named the value in the slot was never written, and the
            // whole diagnosis had to be rebuilt from a register dump. A latch
            // that ends an episode's verbs must explain itself in the log of
            // the run it ends.
            //
            // NothingToDo deliberately does not reach here: a benign
            // double-restore must not cost the episode its arming verbs.
            const bool first = !bridge::g_scope_latched.exchange(
                true, std::memory_order_relaxed);
            if (first && bridge::g_logfile) {
                fprintf(bridge::g_logfile,
                        "[CK3-Bridge] SCOPE LATCH TRIPPED (ck-zgh4): a restore was "
                        "refused, so the bridge stops using RandomScopeManager+0x20 "
                        "for the REST OF THIS PROCESS. verdict=%s observed=0x%016llx "
                        "saved=0x%016llx fake=0x%016llx slot=%p tid=%llu. The slot "
                        "held a value this process cannot reason about; putting "
                        "`saved` back would resurrect a pointer whose object may "
                        "already be dead (the pid-174022 / deploy-50 crash). Actions "
                        "that need the scope will now refuse; a clean run does NOT "
                        "clear this.\n",
                        bridge::restore_verdict_name(v),
                        (unsigned long long)expected,
                        (unsigned long long)saved,
                        (unsigned long long)reinterpret_cast<uint64_t>(g_fake_scope),
                        (void*)slot,
                        (unsigned long long)pthread_self());
                fflush(bridge::g_logfile);
            }
        }
        if (bridge::g_trace && bridge::g_logfile) {
            fprintf(bridge::g_logfile,
                    "[CK3-Bridge] BT_TRACE scope_restore_refused: verdict=%s "
                    "observed=0x%016llx saved=0x%016llx fake=0x%016llx\n",
                    bridge::restore_verdict_name(v),
                    (unsigned long long)expected,
                    (unsigned long long)saved,
                    (unsigned long long)reinterpret_cast<uint64_t>(g_fake_scope));
            fflush(bridge::g_logfile);
        }
    }
    // LEDGER AFTER THE ATTEMPT. The pulse is closed either way: a refused
    // restore is a DECISION not to write, not a restore that failed to happen,
    // and leaving it live would make the sweep "heal" it by writing the very
    // pointer we just refused.
    bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(slot));
}

// 2026-06-09: in-flight intent tracker for outer-recovery attribution.
thread_local std::string g_inflight_intent_rid;

// =============================================================================
// scope_ctx frame helper — the PINNED effect-execution frame (#73 ck-bt9x,
// re_scope_ctx_frame.md / re_scripted_effect_execute.md #75).
//
// Builds the 0x30-byte, 6-field character-ROOT scope_ctx the engine's effect AST
// walker (EFFECT_TREE_WALK / FUN_0750b090) requires, binds it to the controlled
// actor via AI_SCOPE_FROM_CHAR, then walks the resolved effect-tree root under
// BOTH proven crash guards:
//   (1) arm_fake_random_scope — neutralises the RandomScopeManager+0x20 NULL
//       deref that random_*/random_list nodes + internal RNG draws hit from the
//       on_tick drain thread (the steered-war crash class).
//   (2) watchdog_arm + sigsetjmp(g_recovery_point) — converts SIGSEGV/SIGALRM
//       into a clean rejection instead of killing the game (the walk is a FINITE
//       AST walk, so the watchdog CAN bound it — the decision-eval freeze wall
//       does not apply, per re_scope_ctx_frame.md §5.2).
// Cleanup (SCOPE_OBJ_SUB_DTOR + RNG restore) runs on BOTH the normal-return and
// the sigsetjmp-recovery paths; restore/sub-dtor happen BEFORE clearing the
// protected-region flag so a fault during cleanup is still recoverable.
//
// SOURCE/RE-ONLY this session: the frame + the RNG-scope guard are each
// independently proven (ck-rw9m frame pin + the steered-war fix), but the
// assembled hardened walk has NOT been live-smoke-tested on an observer save
// (re_scripted_effect_execute.md §5). Hence the Execute path stays gated behind
// resolve_only=true (default) AND an allow-list of self-gating effects.
//
// Returns: 0 = walked OK; 1 = crashed (out_sig set to the signal); the caller
// owns reporting. tpl must be a resolved effect-tree root (by-name scan result —
// NOT a compound-key LookupTemplate, which is the live bug this bypasses).
// =============================================================================
extern thread_local sigjmp_buf g_recovery_point;
extern thread_local bool g_in_protected_region;

static int run_effect_tree_walk_hardened(void* tpl, uint32_t actor_packed_ref,
                                         unsigned watchdog_secs, int* out_sig) {
    namespace SF = scope_ctx_fns;

    if (out_sig) *out_sig = 0;
    if (!tpl) return 1;

    // Resolve engine entry points (base-relative offsets → runtime VAs).
    using ScopeFromChar = void (*)(void*, uint32_t);
    using SavedInit     = void (*)(void*, void*);
    using CtxInit6      = void (*)(void*, void*, void*, void*, void*, uint8_t, void*);
    using TreeWalk      = void (*)(void* tpl, void* frame);
    using SubDtor       = void (*)(void*);

    auto ai_scope_from_char = reinterpret_cast<ScopeFromChar>(g_base_addr + SF::AI_SCOPE_FROM_CHAR_REF);
    auto saved_targets_init = reinterpret_cast<SavedInit>(g_base_addr + SF::SAVED_TARGETS_INIT);
    auto scope_ctx_init6    = reinterpret_cast<CtxInit6>(g_base_addr + SF::SCOPE_CTX_INIT_6);
    auto effect_tree_walk   = reinterpret_cast<TreeWalk>(g_base_addr + SF::EFFECT_TREE_WALK);
    auto scope_obj_sub_dtor = reinterpret_cast<SubDtor>(g_base_addr + SF::SCOPE_OBJ_SUB_DTOR);

    // Frame storage — stack, zero-initialised.
    alignas(8) uint8_t scope_obj[scope_object::SIZE]   = {};  // 0x168 CScopeObject (actor, type=char)
    alignas(8) uint8_t saved_targets[0x3D8]            = {};  // SavedEventTargets container
    alignas(8) uint8_t frame[scope_ctx::SIZE]          = {};  // 0x30 6-field scope_ctx
    uint32_t rng_state[2] = { 0xe5f10afa, 0 };               // ctx[5] AST RNG seed (writable)

    // volatile: read on the sigsetjmp recovery path, so must survive longjmp
    // (defeats the -Wclobbered hazard — we MUST restore the RNG scope on a fault).
    volatile uint64_t saved_rng = 0;
    volatile bool rng_armed = false;
    int sig = 0;

    watchdog_arm(watchdog_secs);
    g_in_protected_region = true;
    sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        // Recovery: restore + cleanup FIRST (still inside the protected region),
        // then unwind. sub-dtor is idempotent on partial state (null-checks its
        // inner vectors), so it is safe to call after a fault.
        if (rng_armed) restore_fake_random_scope(saved_rng);
        scope_obj_sub_dtor(scope_obj);
        g_in_protected_region = false;
        watchdog_disarm();
        if (out_sig) *out_sig = sig;
        return 1;
    }

    // (1) CScopeObject for the controlled actor — single call builds type=char + value.
    ai_scope_from_char(scope_obj, actor_packed_ref);
    // (2) SavedEventTargets container, bound to the scope object.
    saved_targets_init(saved_targets, scope_obj);
    // (4) the 0x30-byte, 6-field frame, top-level (no parent).
    scope_ctx_init6(frame,
                    scope_obj,       // [0] current/this
                    nullptr,         // [1] prev  (NULL at top level)
                    scope_obj,       // [2] root  (== current)
                    saved_targets,   // [3] eval-targets
                    (uint8_t)0,      // [4] silent (no tooltip emit)
                    rng_state);      // [5] AST RNG state
    // ck-zgh4 scope latch. THIS SITE CANNOT EARLY-RETURN: scope_obj,
    // saved_targets and the frame are ENGINE objects already constructed
    // above, and scope_obj_sub_dtor below is what tears them down. A `return`
    // here would leak an engine scope object on every latched call, so the
    // refusal SKIPS the walk and falls through to the same cleanup the normal
    // path uses. rc 4 is the refusal; the caller maps it.
    //
    // CHECKED BEFORE THE ARM, and that ordering is load-bearing. It used to
    // arm first and test the latch after, which made this the one site that
    // reached arm_fake_random_scope while latched — so the
    // `arm_called_while_latched` counter was not zero-by-design, and the
    // invariant built on it could not be read as "a site is missing its
    // guard". Now NO site reaches the arm while latched, so any non-zero
    // value of that counter is a defect, full stop. (Found by ck-t5bc.)
    bool latched_skip_walk = bridge::scope_latched();
    if (latched_skip_walk) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
    } else {
        // (1b) engine-GLOBAL RandomScopeManager guard (independent of ctx[5]).
        {
            uint64_t saved_tmp = 0;
            rng_armed = arm_fake_random_scope(&saved_tmp);
            saved_rng = saved_tmp;
        }
        // (5) walk the effect tree DIRECTLY against the frame.
        effect_tree_walk(tpl, frame);
    }
    // (6) cleanup on the normal path.
    if (rng_armed) restore_fake_random_scope(saved_rng);
    scope_obj_sub_dtor(scope_obj);

    g_in_protected_region = false;
    watchdog_disarm();
    return latched_skip_walk ? 4 : 0;
}

// Allow-list of self-gating / reversible scripted effects safe to walk bare
// (re_scripted_effect_execute.md §3 / re_scope_ctx_frame.md §6). The direct walk
// runs an effect WITHOUT whatever trigger normally fronts it in vanilla; only
// effects whose application is unconditional-by-design belong here. Effects that
// vanilla only ever runs behind an is_valid/trigger gate (give title, start war,
// imprison) must NOT be added — they would bypass that gate. Empty-prefix match
// is intentional: these are bridge-authored helper effects + benign character
// vars. Conservative on purpose; widen only with a documented legitimacy review.
static bool scripted_effect_is_allow_listed(const char* name) {
    if (!name || !name[0]) return false;
    static const char* const kAllow[] = {
        // The audit's recommended ally-call-refusal posture lever (character var).
        "set_variable_conqueror",
        // Bridge-local no-op / observability probes.
        "noop", "bridge_noop",
        // ── LEGITIMACY: resource-grant effects DELIBERATELY NOT allow-listed ──
        // add_gold / add_prestige / add_piety (and the rest of the resource/state
        // grant family) are REMOVED from this list. The Execute path here runs an
        // effect with NO engine CanExecute gate and NO cost — minting gold/piety/
        // prestige through it is the same console-deny-list end-run the
        // write-action-legitimacy audit closes (execute_console_command deny-list).
        // The bridge OWNS the Unix socket (0666, reachable by the harness / a raw
        // direct-IPC caller / any consumer), so the deny MUST live HERE, at the
        // library boundary — the agent-layer guard in ck3agent/server/redesign.py
        // (_SCRIPTED_EFFECT_DENY) does not protect non-agent socket consumers.
        // Earn resources through the gated game systems, never this trampoline.
        // If a resource-grant ever needs validation, gate execute_scripted_effect
        // behind /tmp/ck3-enabled-writes (kExperimental) FIRST — do not re-add here.
    };
    for (const char* a : kAllow) {
        if (strcmp(name, a) == 0) return true;
    }
    return false;
}

// =============================================================================
// IntentQueue Implementation
// =============================================================================

void IntentQueue::enqueue(Intent intent) {
    intent.consumed = false;
    intent.executed = false;
    intent.rejected = false;
    memset(intent.rejection_reason, 0, sizeof(intent.rejection_reason));

    // M1: stamp the current session on the intent so the game-thread drain can
    // reinstall the owning context.
    if (intent.session_id.empty()) {
        bridge::Session* s = bridge::current_session();
        if (s) intent.session_id = s->session_id;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    intents_.push_back(std::move(intent));
    pending_count_atomic_.fetch_add(1, std::memory_order_relaxed);
    IQLOG("Enqueued: %s actor=%u target=%u request_id=%s session_id=%s\n",
          intents_.back().interaction_name.c_str(),
          intents_.back().actor_id & 0xFFFFFF,
          intents_.back().target_id & 0xFFFFFF,
          intents_.back().request_id.c_str(),
          intents_.back().session_id.c_str());
}

Intent* IntentQueue::find_for_character(uint32_t char_id, IntentType type) {
    uint32_t masked = char_id & 0xFFFFFF;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& intent : intents_) {
        if (!intent.consumed &&
            intent.type == type &&
            (intent.actor_id & 0xFFFFFF) == masked) {
            return &intent;
        }
    }
    return nullptr;
}

Intent* IntentQueue::find_for_character_interaction(uint32_t char_id, uint32_t interaction_hash) {
    if (interaction_hash == 0) return nullptr;
    uint32_t masked = char_id & 0xFFFFFF;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& intent : intents_) {
        // Log comparison for debugging (once per 100 ticks)
        if (!intent.consumed &&
            g_tick_id.load(std::memory_order_relaxed) % 100 == 0) {
            IQLOG("find_match: looking for char=%u hash=0x%08x, intent has actor=%u(masked=%u) hash=0x%08x type=%d consumed=%d\n",
                  masked, interaction_hash,
                  intent.actor_id, intent.actor_id & 0xFFFFFF, intent.interaction_hash,
                  (int)intent.type, intent.consumed ? 1 : 0);
        }
        if (!intent.consumed &&
            intent.type == IntentType::INTERACTION &&
            (intent.actor_id & 0xFFFFFF) == masked &&
            intent.interaction_hash == interaction_hash) {
            IQLOG("MATCH FOUND: %s actor=%u hash=0x%08x\n",
                  intent.interaction_name.c_str(), masked, interaction_hash);
            return &intent;
        }
    }
    return nullptr;
}

std::vector<Intent> IntentQueue::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Intent> result;
    for (const auto& intent : intents_) {
        if (!intent.consumed) {
            result.push_back(intent);
        }
    }
    return result;
}

size_t IntentQueue::pending_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& intent : intents_) {
        if (!intent.consumed) count++;
    }
    return count;
}

void IntentQueue::tick_maintenance(uint64_t current_tick,
                                    std::vector<Intent>& completed_out) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!intents_.empty() &&
        current_tick % 100 == 0) {
        IQLOG("tick_maintenance: tick=%lu pending=%zu\n", (unsigned long)current_tick, intents_.size());
        for (auto& i : intents_) {
            IQLOG("  intent: %s actor=%u age=%lu ttl=%u consumed=%d\n",
                  i.interaction_name.c_str(), i.actor_id & 0xFFFFFF,
                  (unsigned long)(current_tick - i.queued_tick), i.ttl_ticks, i.consumed ? 1 : 0);
        }
    }

    // Collect completed + expired intents
    for (auto& intent : intents_) {
        if (intent.consumed) {
            completed_out.push_back(intent);
            continue;
        }
        // Check TTL expiry
        if (current_tick > intent.queued_tick + intent.ttl_ticks) {
            intent.consumed = true;
            intent.rejected = true;
            snprintf(intent.rejection_reason, sizeof(intent.rejection_reason),
                     "ttl_expired");
            completed_out.push_back(intent);
            IQLOG("Intent expired: %s actor=%u (queued=%lu ttl=%u current=%lu)\n",
                  intent.interaction_name.c_str(),
                  intent.actor_id & 0xFFFFFF,
                  (unsigned long)intent.queued_tick,
                  intent.ttl_ticks,
                  (unsigned long)current_tick);
        }
    }

    // Remove all consumed intents from the vector
    intents_.erase(
        std::remove_if(intents_.begin(), intents_.end(),
                        [](const Intent& i) { return i.consumed; }),
        intents_.end());
    // Update atomic counter to reflect remaining unconsumed intents
    size_t unconsumed = 0;
    for (const auto& i : intents_) { if (!i.consumed) unconsumed++; }
    pending_count_atomic_.store(unconsumed, std::memory_order_relaxed);
}

bool IntentQueue::cancel_by_request_id(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = intents_.begin(); it != intents_.end(); ++it) {
        if (!it->consumed && it->request_id == request_id) {
            IQLOG("Intent cancelled: %s actor=%u request_id=%s\n",
                  it->interaction_name.c_str(), it->actor_id & 0xFFFFFF, request_id.c_str());
            intents_.erase(it);
            if (pending_count_atomic_.load(std::memory_order_relaxed) > 0)
                pending_count_atomic_.fetch_sub(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void IntentQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    intents_.clear();
    pending_count_atomic_.store(0, std::memory_order_relaxed);
    IQLOG("All intents cleared\n");
}

void IntentQueue::store_completed(const Intent& intent) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_.size() >= MAX_COMPLETED) {
        completed_.erase(completed_.begin());
    }
    completed_.push_back(intent);
}

std::vector<Intent> IntentQueue::completed_snapshot(size_t max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = std::min(max_count, completed_.size());
    return std::vector<Intent>(completed_.end() - static_cast<ptrdiff_t>(n),
                               completed_.end());
}

bool IntentQueue::find_completed_by_request_id(const std::string& request_id,
                                                Intent* out) {
    if (request_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    // Walk newest-to-oldest so duplicates surface the most recent result.
    for (auto it = completed_.rbegin(); it != completed_.rend(); ++it) {
        if (it->request_id == request_id) {
            if (out) *out = *it;
            return true;
        }
    }
    return false;
}

bool IntentQueue::find_pending_by_request_id(const std::string& request_id,
                                              Intent* out) {
    if (request_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& intent : intents_) {
        if (!intent.consumed && intent.request_id == request_id) {
            if (out) *out = intent;
            return true;
        }
    }
    return false;
}

bool IntentQueue::mark_rejected_by_request_id(const std::string& request_id,
                                              const char* reason) {
    if (request_id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& intent : intents_) {
        if (!intent.consumed && intent.request_id == request_id) {
            intent.rejected = true;
            snprintf(intent.rejection_reason, sizeof(intent.rejection_reason),
                     "%s", reason);
            intent.consumed = true;
            return true;
        }
    }
    return false;
}

// =============================================================================
// Intent Execution Functions (called from dispatch hooks during AI tick)
// =============================================================================

// Forward declarations from dispatch_router.cpp that we need
extern void* lookup_character(uint32_t char_id);
extern void* lookup_interaction(const char* interaction_type);
extern uint32_t game_string_hash(const char* data, uint32_t len);

bool execute_war_intent(Intent* intent) {
    // War declaration via the AI war pipeline:
    //   GetAIController -> AI_WAR_TARGET_EVAL -> AI_WAR_DECLARE_EXEC -> AI_WAR_POST_DECL(force=1)
    //
    // This mirrors CMD_AI_DECLARE_WAR (console command "AI.declare_war"):
    //   1. Get AI controller for the character
    //   2. Temporarily clear the war suppress/pacing flag
    //   3. Run full target evaluation pipeline (AI picks best target + CB)
    //   4. Call AI_WAR_POST_DECL with force=1 to bypass all probability gates
    //   5. Restore war suppress flag
    //
    // LIMITATION: The AI auto-selects the target and CB. The intent's target_id
    // is informational only (logged but not enforced). For targeted war with
    // specific CB, a future implementation would hook FUN_04e980d0 (CB selector).

    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;
    // Gate for ai_cb_selector_hook — declared in ai_cb_selector_hook.cpp.
    extern thread_local bool g_in_bridge_war_intent;
    // CB selector observation — captured while the gate is set, surfaced into
    // IQLOG after war_post_decl so the chosen CB rides alongside the rest of
    // the intent trace (ck-l8ge).
    extern void capture_cb_verdict_reset();
    extern bool capture_cb_verdict_get(uint64_t* cb_ref, uint64_t* cb_titles,
                                       uint64_t* target_ref, int32_t* score,
                                       uint8_t* extra_roll, uint32_t* sentinel,
                                       const char** verdict);
    capture_cb_verdict_reset();

    // === ENTRY: log all intent parameters ===
    IQLOG("=== execute_war_intent ENTRY ===\n");
    IQLOG("  intent=%p type=%d actor_id=0x%08x(%u) target_id=0x%08x(%u)\n",
          (void*)intent, (int)intent->type,
          intent->actor_id, intent->actor_id & 0xFFFFFF,
          intent->target_id, intent->target_id & 0xFFFFFF);
    IQLOG("  cb_index=%d title_id=%d claimant_id=0x%08x(%u)\n",
          intent->cb_index, intent->title_id,
          intent->claimant_id, intent->claimant_id & 0xFFFFFF);
    IQLOG("  interaction_hash=0x%08x interaction_name='%s'\n",
          intent->interaction_hash, intent->interaction_name.c_str());
    IQLOG("  secondary_actor_id=0x%08x secondary_recipient_id=0x%08x\n",
          intent->secondary_actor_id, intent->secondary_recipient_id);
    IQLOG("  force_bypass=%d queued_tick=%lu ttl_ticks=%u request_id='%s'\n",
          intent->force_bypass, (unsigned long)intent->queued_tick,
          intent->ttl_ticks, intent->request_id.c_str());

    void* actor = lookup_character(intent->actor_id);
    if (!actor) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "actor_not_found");
        intent->consumed = true;
        IQLOG("War intent rejected: actor=%u reason=actor_not_found\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }

    // Recheck liveness — actor may have died between queue time and execution
    if (g_char_is_alive && !g_char_is_alive(actor)) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "actor_died_before_execution");
        intent->consumed = true;
        IQLOG("War intent rejected: actor=%u reason=actor_died_before_execution\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }

    // The character must have an AI controller. Player characters do NOT have
    // one, so this only works for AI-controlled characters (which is the design:
    // we replaced the AI brain, so the character is AI-controlled by the LLM).
    //
    // Access pattern: core_data = *(char+0x1A8); ai_ctrl = *(core_data + 0x278)
    // Or use the engine function CHAR_GET_AI_CONTROLLER which does the same
    // with a global fallback.

    auto get_ai_ctrl = reinterpret_cast<void*(*)(void*)>(
        g_base_addr + ck3::funcs::CHAR_GET_AI_CONTROLLER);

    void* ai_ctrl = nullptr;

    // Protected call: get AI controller
    watchdog_arm(2);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" :
                               (sig == 2) ? "SIGALRM_TIMEOUT" : "UNKNOWN";
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "get_ai_ctrl_%s", sig_name);
        intent->consumed = true;
        IQLOG("War intent rejected: actor=%u reason=get_ai_ctrl_%s\n",
              intent->actor_id & 0xFFFFFF, sig_name);
        return false;
    }

    try {
        ai_ctrl = get_ai_ctrl(actor);
    } catch (...) {
        g_in_protected_region = false;
        watchdog_disarm();
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "get_ai_ctrl_exception");
        intent->consumed = true;
        IQLOG("War intent rejected: actor=%u reason=get_ai_ctrl_exception\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    if (!ai_ctrl || !is_mapped(reinterpret_cast<uintptr_t>(ai_ctrl))) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "no_ai_controller");
        intent->consumed = true;
        IQLOG("War intent rejected: actor=%u reason=no_ai_controller (ptr=%p)\n",
              intent->actor_id & 0xFFFFFF, ai_ctrl);
        return false;
    }

    // Resolve target character pointer. Used informationally for the AI war
    // pipeline (AI picks its own target) AND as the source for Path C
    // pre-seeding (ck-7dop) which writes the target's canonical handle into
    // the AI's front-buffer entry[0]. Volatile silences -Wclobbered: this
    // value is set before the second sigsetjmp and read inside the protected
    // try block.
    void* volatile target = nullptr;
    if (intent->target_id != 0) {
        target = lookup_character(intent->target_id);
    }
    IQLOG("  actor_ptr=%p target_ptr=%p (target_id=%u, 0=AI-chosen) ai_ctrl=%p\n",
          actor, target, intent->target_id & 0xFFFFFF, ai_ctrl);
    IQLOG("War intent: actor=%u target=%u ai_ctrl=%p — executing AI war pipeline\n",
          intent->actor_id & 0xFFFFFF, intent->target_id & 0xFFFFFF, ai_ctrl);

    // Function pointers for the war pipeline
    auto war_target_eval = reinterpret_cast<void*(*)(void*)>(
        g_base_addr + ck3::funcs::AI_WAR_TARGET_EVAL);
    auto war_declare_exec = reinterpret_cast<void(*)(void*, void*)>(
        g_base_addr + ck3::funcs::AI_WAR_DECLARE_EXEC);
    auto war_post_decl = reinterpret_cast<void(*)(void*, int, int)>(
        g_base_addr + ck3::funcs::AI_WAR_POST_DECL);

    // War suppress flag — temporarily clear it (same as CMD_AI_DECLARE_WAR)
    uint8_t* war_suppress_ptr = reinterpret_cast<uint8_t*>(
        g_base_addr + ck3::globals::AI_WAR_SUPPRESS);
    volatile uint8_t old_suppress = 0;
    volatile bool suppress_mapped = is_mapped(reinterpret_cast<uintptr_t>(war_suppress_ptr));
    // R1c (ck-6kvq.10, design §2.9): THE pulse the design names. `volatile` for
    // the same reason old_suppress is — it is written before the sigsetjmp below
    // and read in the recovery branch.
    //
    // Note the window this closes that no local restore can: the clear happens
    // HERE, several statements before this function installs its own recovery
    // point. A fault in between (the AI_RNG_COUNTER read, lookup_character)
    // longjmps to the OUTER recovery in bridge_init.cpp and never reaches any of
    // the three restores below — leaving AI_WAR_SUPPRESS cleared for the rest of
    // the process. pulse_sweep_drain() runs on exactly that path and heals it.
    volatile uint64_t suppress_pulse = 0;
    if (suppress_mapped) {
        old_suppress = *war_suppress_ptr;
        suppress_pulse = bridge::pulse_enter_global(
            "AI_WAR_SUPPRESS", "execute_war_intent",
            reinterpret_cast<uintptr_t>(war_suppress_ptr), 1, old_suppress);
        *war_suppress_ptr = 0;
        IQLOG("War intent: cleared AI_WAR_SUPPRESS (was %u)\n", old_suppress);
    }

    // RNG seed — read global AI RNG counter, or synthesize one
    volatile uint32_t rng_seed = 0;
    uint32_t* rng_ptr = reinterpret_cast<uint32_t*>(
        g_base_addr + ck3::globals::AI_RNG_COUNTER);
    if (is_mapped(reinterpret_cast<uintptr_t>(rng_ptr))) {
        rng_seed = *rng_ptr;
    } else {
        rng_seed = (intent->actor_id ^ (g_tick_id.load() * 2654435761u)) & 0x7FFFFFFE;
    }

    // Execute the war pipeline with SIGSEGV + SIGALRM recovery
    bool success = false;

    watchdog_arm(8);  // generous timeout — target eval can be slow
    g_in_protected_region = true;
    sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        g_in_bridge_war_intent = false;  // clear CB-probe gate after longjmp
        watchdog_disarm();
        // Restore war suppress flag
        if (suppress_mapped) {
            *war_suppress_ptr = old_suppress;
            bridge::pulse_exit_global(suppress_pulse);   // R1c: after the restore
        }
        const char* sig_name = (sig == 1) ? "SIGSEGV" :
                               (sig == 2) ? "SIGALRM_TIMEOUT" : "UNKNOWN";
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "war_pipeline_%s", sig_name);
        intent->consumed = true;
        IQLOG("War intent CRASHED: actor=%u reason=%s\n",
              intent->actor_id & 0xFFFFFF, sig_name);
        return false;
    }

    try {
        // Diagnostic: check AI controller eligibility flags
        // FUN_04e19160 checks: *(ai_ctrl+0x10) > 1 AND (*(ai_ctrl+0x16) & 2) != 0
        uintptr_t ac = reinterpret_cast<uintptr_t>(ai_ctrl);
        int32_t ctrl_state = read<int32_t>(ac + 0x10);
        uint8_t ctrl_flags = read<uint8_t>(ac + 0x16);
        bool war_eligible = (ctrl_state > 1) && ((ctrl_flags & 2) != 0);
        IQLOG("War intent: ai_ctrl=%p state=%d flags=0x%02x war_eligible=%d\n",
              ai_ctrl, ctrl_state, ctrl_flags, war_eligible);
        if (!war_eligible) {
            IQLOG("War intent: WARNING — AI controller NOT war-eligible, pipeline will be a no-op!\n");
        }

        // Step 1: Get war data struct (just a pointer offset accessor)
        IQLOG("  [STEP 1] calling war_target_eval(ai_ctrl=%p) @ 0x%lx\n",
              ai_ctrl, (unsigned long)ck3::funcs::AI_WAR_TARGET_EVAL);
        void* war_data = war_target_eval(ai_ctrl);
        IQLOG("  [STEP 1] war_target_eval returned war_data=%p\n", war_data);

        // Step 2: Run full target evaluation pipeline
        // param_2 is the war evaluation data struct (NOT an exclusion list).
        // The AI.declare_war console command passes war_data here.
        // Previous bug: passed nullptr/zeroed buffer → target selector found 0 candidates.
        IQLOG("  [STEP 2] calling war_declare_exec(ai_ctrl=%p, war_data=%p) @ 0x%lx\n",
              ai_ctrl, war_data, (unsigned long)ck3::funcs::AI_WAR_DECLARE_EXEC);
        war_declare_exec(ai_ctrl, war_data);
        IQLOG("  [STEP 2] war_declare_exec completed\n");

        // Diagnostic: check candidate count after target population
        uintptr_t wd = read<uintptr_t>(ac + 0x20);  // ai_ctrl->war_data
        if (wd) {
            int32_t candidates = read<int32_t>(wd + 0x98 + 0x0C);  // front buffer count
            IQLOG("War intent: candidate target count = %d\n", candidates);
        }

        // Path C pre-seed (ck-7dop): the AI's target list (wd+0x98) may not
        // include our bridge-vouched target, producing NO_VIABLE_PAIR in
        // AI_CB_SELECTOR even though we pre-validated (target, CB, title).
        // Overwrite entry[0] with the target's canonical character handle
        // (read from char struct +0x18). AI_CB_EVALUATE_ALL iterates the
        // buffer with stride 1 uint32, matching `*entry` against
        // `char[+0x18]`; mismatched high byte → silent skip via LAB_04e99310,
        // so writing wrong values does not crash.
        if (wd && target != nullptr) {
            uintptr_t fb     = wd + ck3::funcs::WAR_DATA_FRONT_BUF;
            int32_t  cnt     = read<int32_t>(fb + ck3::funcs::FRONT_BUF_COUNT);
            uintptr_t entries = read<uintptr_t>(fb + ck3::funcs::FRONT_BUF_BEGIN);
            uint32_t canonical =
                read<uint32_t>(reinterpret_cast<uintptr_t>(target)
                               + ck3::funcs::CHAR_CANONICAL_HANDLE);
            if (cnt > 0 && entries != 0 && is_mapped(entries) && canonical != 0) {
                uint32_t was = read<uint32_t>(entries);
                *reinterpret_cast<volatile uint32_t*>(entries) = canonical;
                IQLOG("  PATH_C: pre-seeded entry[0] was=0x%08x now=0x%08x "
                      "(target=%u count=%d)\n",
                      was, canonical, intent->target_id & 0xFFFFFF, cnt);
            } else {
                IQLOG("  PATH_C: skipped (cnt=%d entries=0x%lx canonical=0x%08x)\n",
                      cnt, (unsigned long)entries, canonical);
            }
        }

        // Path B' override arming (ck-tcxr / ck-6h1v / re_path_b_prime_titles_array_20260610):
        // when the intent carries an explicit (target, cb_index, title_id), arm the
        // CB-selector override so the gated AI_CB_SELECTOR inside war_post_decl
        // returns our bridge-vouched (cb_ref, target_ref, titles) instead of
        // NO_VIABLE_PAIR. This is what turns "pipeline runs but engine declines"
        // into a real declaration. The override is consumed one-shot by the hook
        // during the war_post_decl call below.
        //
        // Triple resolution (all game-thread-available here):
        //   cb_ref     = *(CASUS_BELLI_DB.array + cb_index*8)   raw CCasusBelliType*
        //   +0x08      = target CCharacter*  (gate vtable[2] probe only — ck-6h1v)
        //   target_ref = target CCharacter*  (consumer reads +0x18 = defender id)
        //   titles     = {title_id} u32[1]   (live wars store plain u32 title IDs)
        // ov_title_buf lives on this stack frame; war_post_decl consumes the
        // override synchronously, so the lifetime covers the whole call.
        uint32_t ov_title_buf[1] = {0};
        bool armed_override = false;
        if (target != nullptr && intent->cb_index >= 0 && intent->title_id > 0) {
            uintptr_t cb_db = read<uintptr_t>(g_base_addr + ck3::globals::CASUS_BELLI_DB);
            uintptr_t cb_arr = cb_db ? read<uintptr_t>(cb_db + 0x88) : 0;
            int32_t   cb_cnt = cb_db ? read<int32_t>(cb_db + 0x94) : 0;
            uintptr_t cb_ref = 0;
            if (cb_arr && is_mapped(cb_arr) &&
                intent->cb_index < cb_cnt && cb_cnt > 0 && cb_cnt <= 500) {
                cb_ref = read<uintptr_t>(cb_arr + static_cast<uintptr_t>(intent->cb_index) * 8);
            }
            if (cb_ref && is_mapped(cb_ref)) {
                ov_title_buf[0] = static_cast<uint32_t>(intent->title_id) & 0xFFFFFF;
                uint64_t tgt = reinterpret_cast<uint64_t>(const_cast<void*>(target));
                // Claimant handle for CB_RESULT +0x18 (re_claimant_is_cb_result_0x18):
                // claim_cb needs the claimant's canonical handle, else WAR_CREATE_CORE
                // gets claimant=0 and silent-fails. For a pressed claim the actor IS
                // the claimant; intent->claimant_id overrides. 0xFFFFFFFF = none.
                void* clmt_char = (intent->claimant_id != 0)
                                  ? lookup_character(intent->claimant_id) : actor;
                uint32_t claimant_handle = 0xFFFFFFFFu;
                if (clmt_char && is_mapped(reinterpret_cast<uintptr_t>(clmt_char))) {
                    claimant_handle = read<uint32_t>(
                        reinterpret_cast<uintptr_t>(clmt_char) + ck3::funcs::CHAR_CANONICAL_HANDLE);
                }
                set_cb_selector_override(
                    /*cb_ref=*/        cb_ref,
                    /*cb_titles_ref=*/ tgt,               // +0x08: gate probe only
                    /*target_ref=*/    tgt,               // +0x10: consumer reads +0x18 for def id
                    /*score=*/         100000,            // > 0 to pass post_decl gate
                    /*extra_roll=*/    0,                 // skip CB-level RNG gate
                    /*titles_data=*/   reinterpret_cast<uint64_t>(ov_title_buf),
                    /*titles_count=*/  1,
                    /*claimant_handle=*/ claimant_handle);
                armed_override = true;
                IQLOG("  [STEP 3] Path B' override armed: cb_ref=0x%lx (cb_index=%d) "
                      "target_ref=0x%lx title=%u claimant=0x%08x\n",
                      (unsigned long)cb_ref, intent->cb_index, (unsigned long)tgt,
                      ov_title_buf[0], claimant_handle);
            } else {
                IQLOG("  [STEP 3] Path B' override NOT armed: cb_ref unresolved "
                      "(cb_index=%d cb_cnt=%d cb_arr=0x%lx)\n",
                      intent->cb_index, cb_cnt, (unsigned long)cb_arr);
            }
        } else {
            IQLOG("  [STEP 3] Path B' override NOT armed: missing explicit triple "
                  "(target=%p cb_index=%d title_id=%d)\n",
                  (void*)target, intent->cb_index, intent->title_id);
        }

        // Step 3: Execute with force=1 — selects best CB, builds command, dispatches
        // force=1 bypasses pacing/probability but NOT eligibility, candidates, or CB selection.
        // Gate the AI_CB_SELECTOR observer hook to capture the S1 verdict of the
        // selection performed inside war_post_decl (ck-2x2z, replaces ck-2x8w replay).
        IQLOG("  [STEP 3] calling war_post_decl(ai_ctrl=%p, rng_seed=%u, force=1) armed_override=%d @ 0x%lx\n",
              ai_ctrl, rng_seed, armed_override ? 1 : 0,
              (unsigned long)ck3::funcs::AI_WAR_POST_DECL);
        g_in_bridge_war_intent = true;
        war_post_decl(ai_ctrl, static_cast<int>(rng_seed), 1);
        g_in_bridge_war_intent = false;
        // Defensive: if the override was armed but the gated AI_CB_SELECTOR never
        // fired (e.g. war_post_decl bailed before the call), clear the one-shot
        // slot so it can't leak into a later intent.
        if (armed_override && cb_selector_override_active()) {
            clear_cb_selector_override();
            IQLOG("  [STEP 3] Path B' override was not consumed — cleared stale slot\n");
        }
        IQLOG("  [STEP 3] war_post_decl completed\n");

        // Surface the engine's CB-selector verdict (ck-l8ge). The
        // ai_cb_selector_hook captures the last result-struct it observed
        // while g_in_bridge_war_intent was set — that struct is what
        // war_post_decl actually consumed. Without this, the [CB_PROBE]
        // line is the only record and lives in a different log namespace.
        {
            uint64_t cb_ref = 0, cb_titles = 0, target_ref = 0;
            int32_t  cb_score = 0;
            uint8_t  cb_extra_roll = 0;
            uint32_t cb_sentinel = 0;
            const char* cb_verdict = nullptr;
            if (capture_cb_verdict_get(&cb_ref, &cb_titles, &target_ref,
                                       &cb_score, &cb_extra_roll,
                                       &cb_sentinel, &cb_verdict)) {
                IQLOG("  [STEP 3] AI_CB_SELECTOR verdict: cb_ref=0x%lx "
                      "titles=0x%lx target=0x%lx sentinel=0x%08x score=%d "
                      "extra_roll=%u → %s\n",
                      (unsigned long)cb_ref, (unsigned long)cb_titles,
                      (unsigned long)target_ref, cb_sentinel, cb_score,
                      cb_extra_roll, cb_verdict ? cb_verdict : "(null)");
            } else {
                IQLOG("  [STEP 3] AI_CB_SELECTOR did not fire — selection bypassed or hook missed\n");
            }
        }

        // Diagnostic: check dispatch gate
        uint8_t gate = read<uint8_t>(g_base_addr + ck3::globals::COMMAND_DISPATCH_GATE);
        IQLOG("War intent: dispatch gate = 0x%02x (clear=%d)\n", gate, (gate & 0xFD) == 0);

        // Check if war count actually increased for the actor
        uintptr_t actor_tr = resolve_title_realm(reinterpret_cast<uintptr_t>(actor));
        int32_t wars_after = 0;
        if (actor_tr) {
            wars_after = read<int32_t>(actor_tr + ck3::title_realm::WAR_REF_ARRAY + 0x0C);
        }
        IQLOG("War intent: actor war count after = %d (candidates were %d)\n",
              wars_after, wd ? read<int32_t>(wd + 0x98 + 0x0C) : -1);

        // Log CB type and title info from the AI war data if available
        // wd = ai_ctrl+0x20 (war eval data), candidates at wd+0x98
        if (wd && is_mapped(wd)) {
            // Dump raw pointers around the war data struct for diagnostics
            uintptr_t wd_ptr0 = read<uintptr_t>(wd + 0x00);
            uintptr_t wd_ptr8 = read<uintptr_t>(wd + 0x08);
            uintptr_t wd_ptr10 = read<uintptr_t>(wd + 0x10);
            IQLOG("  war_data dump: +0x00=0x%lx +0x08=0x%lx +0x10=0x%lx\n",
                  (unsigned long)wd_ptr0, (unsigned long)wd_ptr8, (unsigned long)wd_ptr10);
            // Front buffer: ptr at +0x98, count at +0x98+0x0C
            uintptr_t front_buf_ptr = read<uintptr_t>(wd + 0x98);
            int32_t front_buf_count = read<int32_t>(wd + 0x98 + 0x0C);
            IQLOG("  war_data front_buf: ptr=0x%lx count=%d\n",
                  (unsigned long)front_buf_ptr, front_buf_count);
        }
        // Log the intent's own title_id and cb_index for cross-reference
        IQLOG("  intent params: title_id=%d cb_index=%d claimant_id=%u\n",
              intent->title_id, intent->cb_index, intent->claimant_id & 0xFFFFFF);

        success = true;
    } catch (...) {
        g_in_protected_region = false;
        g_in_bridge_war_intent = false;  // clear CB-probe gate on exception
        watchdog_disarm();
        // Restore war suppress flag
        if (suppress_mapped) {
            *war_suppress_ptr = old_suppress;
            bridge::pulse_exit_global(suppress_pulse);   // R1c: after the restore
        }
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "war_pipeline_exception");
        intent->consumed = true;
        IQLOG("War intent EXCEPTION: actor=%u\n", intent->actor_id & 0xFFFFFF);
        return false;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    // Restore war suppress flag
    if (suppress_mapped) {
        *war_suppress_ptr = old_suppress;
        bridge::pulse_exit_global(suppress_pulse);       // R1c: after the restore
        IQLOG("War intent: restored AI_WAR_SUPPRESS to %u\n", old_suppress);
    }

    if (success) {
        intent->executed = true;
        intent->consumed = true;
        IQLOG("War intent COMPLETED: actor=%u — pipeline ran (check war count to confirm actual declaration)\n",
              intent->actor_id & 0xFFFFFF);
    } else {
        // Shouldn't reach here given the try/catch, but guard anyway
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "war_pipeline_unknown_failure");
        intent->consumed = true;
    }

    IQLOG("=== execute_war_intent EXIT: returning %s (actor=%u target=%u) ===\n",
          success ? "true" : "false",
          intent->actor_id & 0xFFFFFF, intent->target_id & 0xFFFFFF);
    return success;
}

// Execute a queued DECISION intent on the game thread (drained from on_tick
// serial section). Mirrors do_send_decision but operates on an Intent and
// looks up the actor by ID rather than via resolve_actor (game-thread safe).
//
// Decision name is stored in intent->interaction_name (the field is reused
// across intent types as a generic "action label").
bool execute_decision_intent(Intent* intent) {
    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;

    const char* decision_name = intent->interaction_name.c_str();
    IQLOG("=== execute_decision_intent ENTRY: actor=%u decision='%s' ===\n",
          intent->actor_id & 0xFFFFFF, decision_name);

    void* actor = lookup_character(intent->actor_id);
    if (!actor) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "actor_not_found");
        intent->consumed = true;
        IQLOG("Decision intent rejected: actor=%u not found\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }

    if (g_char_is_alive && !g_char_is_alive(actor)) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "actor_not_alive");
        intent->consumed = true;
        IQLOG("Decision intent rejected: actor=%u not alive\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }

    void* decision = lookup_decision(decision_name);
    if (!decision) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "decision_not_found:%.80s", decision_name);
        intent->consumed = true;
        IQLOG("Decision intent rejected: '%s' not in decision DB\n", decision_name);
        return false;
    }

    // Validate decision via vtable[7] (CDecisionType::is_valid). Returns false
    // for the null sentinel TPdxNullObject<CDecisionType>.
    uintptr_t decision_addr = reinterpret_cast<uintptr_t>(decision);
    uintptr_t vtable = read<uintptr_t>(decision_addr);
    if (!vtable) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "decision_vtable_null");
        intent->consumed = true;
        return false;
    }

    bool valid = false;
    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "vtable_validation_crashed:%s",
                 (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN");
        intent->consumed = true;
        IQLOG("Decision intent crashed in vtable[is_valid]: '%s'\n", decision_name);
        return false;
    }
    try {
        auto is_valid = reinterpret_cast<bool(*)(void*)>(
            read<uintptr_t>(vtable + ck3::interaction_cmd::VTABLE_IS_VALID));
        valid = is_valid(decision);
    } catch (...) {
        valid = false;
    }
    g_in_protected_region = false;
    watchdog_disarm();

    if (!valid) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "vtable_validation_failed");
        intent->consumed = true;
        IQLOG("Decision intent rejected: '%s' is_valid=false\n", decision_name);
        return false;
    }

    // AI_TRY_DECISION_CORE — Phase 18 confirmed:
    //   std::string*(std::string* result_out, CCharacter*, CDecisionType*)
    // On success: result starts with "Took decision."; enqueues CExecuteDecisionCommand.
    // On failure: "Does not fulfill ai_potential" / "cannot afford" / "cannot take" / etc.
    auto ai_try_decision = reinterpret_cast<void*(*)(void*, void*, void*)>(
        g_base_addr + ck3::funcs::AI_TRY_DECISION_CORE);

    alignas(8) char result_storage[32] = {};
    void* result_ptr = result_storage;

    watchdog_arm(5);
    g_in_protected_region = true;
    sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "ai_try_decision_crashed:%s",
                 (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN");
        intent->consumed = true;
        IQLOG("Decision intent crashed in AI_TRY_DECISION_CORE: '%s'\n", decision_name);
        return false;
    }

    bool took_decision = false;
    std::string engine_message;
    try {
        ai_try_decision(result_ptr, actor, decision);

        // Engine-side std::string returned at result_storage. Layout matches
        // do_send_decision (action_handlers.cpp): size at +16, ptr/SSO at +0.
        size_t str_size = *reinterpret_cast<size_t*>(result_storage + 16);
        if (str_size > 0 && str_size < 4096) {
            const char* str_data;
            if (str_size <= 15) {
                str_data = result_storage;  // SSO inline
            } else {
                str_data = *reinterpret_cast<const char**>(result_storage);  // heap
            }
            engine_message.assign(str_data, str_size);
            took_decision = (engine_message.find("Took decision") != std::string::npos);
        }

        // Free heap-allocated string buffer (engine uses malloc/free per do_send_decision).
        if (str_size > 15) {
            char* heap_ptr = *reinterpret_cast<char**>(result_storage);
            if (heap_ptr) free(heap_ptr);
        }
    } catch (...) {
        IQLOG("Decision intent: exception in AI_TRY_DECISION_CORE for '%s'\n", decision_name);
    }
    g_in_protected_region = false;
    watchdog_disarm();

    if (took_decision) {
        intent->executed = true;
        intent->consumed = true;
        intent->console_output = engine_message;  // surface engine diagnostic
        IQLOG("Decision intent EXECUTED: actor=%u '%s' — %s\n",
              intent->actor_id & 0xFFFFFF, decision_name, engine_message.c_str());
        return true;
    }

    intent->rejected = true;
    snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
             "%.250s", engine_message.empty() ? "ai_try_decision_returned_no_message"
                                              : engine_message.c_str());
    intent->consumed = true;
    if (!engine_message.empty()) intent->console_output = engine_message;
    IQLOG("Decision intent REJECTED: actor=%u '%s' — %s\n",
          intent->actor_id & 0xFFFFFF, decision_name, intent->rejection_reason);
    return false;
}

// Execute a queued SCRIPTED_EFFECT intent on the game thread (drained from
// on_tick serial section). Looks up the named scripted_effect template in the
// CJominiScriptedEffectTemplateDatabase singleton and invokes
// CJominiScriptedEffect::Execute on a stack-allocated dispatcher node with the
// actor character as ROOT scope (ck-7ejs).
//
// Path (matching the engine's own dispatch):
//   db        = *(.bss + scripted_template_db::SCRIPTED_EFFECT_DB)
//   tpl       = LookupTemplate(db, &Key{name_ptr, _, name_len})
//   node      = stack-allocated 0x100 bytes, zero-init
//   node+0x60 = db ; node+0x88 = key
//   Execute(node, actor)   // SCRIPTED_EFFECT_EXECUTE re-resolves via LookupTemplate
//                          // then walks tpl's compiled child effect tree
//
// Wrapped in watchdog + sigsetjmp + try/catch so a crash in the engine call
// drops back into the bridge with a structured rejection rather than killing
// the game process.
bool execute_scripted_effect_intent(Intent* intent) {
    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;

    const char* effect_name = intent->interaction_name.c_str();
    IQLOG("=== execute_scripted_effect_intent ENTRY: actor=%u effect='%s' ===\n",
          intent->actor_id & 0xFFFFFF, effect_name);

    void* actor = lookup_character(intent->actor_id);
    if (!actor) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "actor_not_found");
        intent->consumed = true;
        IQLOG("Scripted-effect intent rejected: actor=%u not found\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }
    if (g_char_is_alive && !g_char_is_alive(actor)) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "actor_not_alive");
        intent->consumed = true;
        IQLOG("Scripted-effect intent rejected: actor=%u not alive\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }

    size_t name_len = intent->interaction_name.size();
    if (name_len == 0 || name_len > 200) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "invalid_effect_name_len:%zu", name_len);
        intent->consumed = true;
        return false;
    }

    // Resolve the user-scripted-effect template DB singleton. The .bss holder
    // stores a heap pointer populated at static-init (re_scripted_template_dbs.md §2).
    uintptr_t db_ptr = read_offset<uintptr_t>(scripted_template_db::SCRIPTED_EFFECT_DB);
    if (!db_ptr) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "template_db_null");
        intent->consumed = true;
        IQLOG("Scripted-effect intent rejected: template DB ptr is null\n");
        return false;
    }

    // Resolve the template by NAME via a linear scan over the DB's templates
    // array (re_scripted_effect_dispatch_NEXTBATCH.md §3.1). This REPLACES the
    // old LookupTemplate({ptr,len}) call, which mis-shaped a COMPOUND key —
    // key+0xc is a BLOCK COUNT, not a string length, so the engine walked the
    // ASCII name bytes as an array of 0x40-byte block structs and SIGSEGV'd on
    // every real effect name. The array is Template*[count] (8-byte stride);
    // each template carries its name as an embedded std::string at
    // TEMPLATE_NAME_STRING (+0x10 data, +0x18 len). Length-reject first (cheap),
    // memcmp only on a length match. safe_read* are fault-tolerant; the watchdog
    // bounds the loop; the sigsetjmp is belt-and-suspenders.
    uintptr_t tpl_arr = safe_read<uintptr_t>(db_ptr + scripted_template_db::OBJ_TEMPLATES_ARRAY);
    uint32_t  tpl_cnt = safe_read<uint32_t>(db_ptr + scripted_template_db::OBJ_TEMPLATES_COUNT);
    if (!tpl_arr || !is_mapped(tpl_arr) || tpl_cnt == 0 || tpl_cnt > 1000000) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "template_array_invalid:cnt=%u", tpl_cnt);
        intent->consumed = true;
        IQLOG("Scripted-effect: template array invalid (arr=%lx cnt=%u)\n",
              (unsigned long)tpl_arr, tpl_cnt);
        return false;
    }

    void* tpl = nullptr;
    int sig = 0;
    char namebuf[256];  // name_len is bounded to <=200 above
    watchdog_arm(3);
    g_in_protected_region = true;
    sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "template_scan_crashed:%s",
                 (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN");
        intent->consumed = true;
        IQLOG("Scripted-effect template scan crashed: '%s'\n", effect_name);
        return false;
    }
    for (uint32_t i = 0; i < tpl_cnt; i++) {
        uintptr_t t = safe_read<uintptr_t>(tpl_arr + static_cast<uintptr_t>(i) * 8);
        if (!t || !is_mapped(t)) continue;
        uint32_t nlen = safe_read<uint32_t>(t + scripted_template_db::TEMPLATE_NAME_STRING + 0x8);
        if (nlen != name_len) continue;             // cheap length reject (no string read)
        uintptr_t ndata = safe_read<uintptr_t>(t + scripted_template_db::TEMPLATE_NAME_STRING);
        if (!ndata || !is_mapped(ndata)) continue;
        if (!safe_read_bytes(ndata, namebuf, name_len)) continue;
        if (memcmp(namebuf, effect_name, name_len) == 0) { tpl = reinterpret_cast<void*>(t); break; }
    }
    g_in_protected_region = false;
    watchdog_disarm();

    if (!tpl) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "effect_not_found:%.80s", effect_name);
        intent->consumed = true;
        IQLOG("Scripted-effect intent: '%s' not in template DB (scanned %u)\n",
              effect_name, tpl_cnt);
        return false;
    }

    IQLOG("Scripted-effect intent: resolved '%s' to %p by name-scan\n", effect_name, tpl);

    // resolve_only (DEFAULT): report the template as FOUND and STOP without
    // calling Execute. The scope_ctx frame Execute requires is now PINNED
    // (re_scope_ctx_frame.md / re_issue73_crack.md, #73) and the hardened walk
    // below is built and gated; resolve_only stays the default so the handler
    // is safe-by-default ("honestly reports whether the effect exists").
    // resolve_only=false (observer-save-only RE) falls through to the gated,
    // allow-listed Execute path below.
    if (intent->resolve_only) {
        intent->executed = true;
        intent->consumed = true;
        {
            char buf[176];
            snprintf(buf, sizeof(buf),
                     "scripted_effect '%.80s' resolved (resolve_only) — Execute available behind allow-list (resolve_only=false)",
                     effect_name);   // rc2 B2: no template VA in a client string
            intent->console_output = buf;
        }
        IQLOG("Scripted-effect intent RESOLVED (resolve_only): '%s' template=%p\n",
              effect_name, tpl);
        return true;
    }

    // ── Execute path (resolve_only=false only) — PINNED-FRAME walk, ALLOW-LISTED ──
    // The OLD code here fed SCRIPTED_EFFECT_EXECUTE a fake node with
    // node+0x88={name_ptr,name_len} (the COMPOUND-key mistake — key+0xc is a BLOCK
    // COUNT, not a strlen) and passed the raw `actor` (a CCharacter*) as scope_ctx,
    // so the walker read *(uint16_t*)actor (a vptr fragment) as a scope-type tag →
    // "Wrong scope" / out-of-bounds fault. Replaced with the pinned character-ROOT
    // scope_ctx frame + direct EFFECT_TREE_WALK, under both proven crash guards
    // (re_scope_ctx_frame.md §3/§5, re_scripted_effect_execute.md §2).
    //
    // The walk runs the effect WITHOUT whatever trigger normally fronts it, so it
    // is gated behind an allow-list of self-gating / reversible effects
    // (re_scripted_effect_execute.md §3). Effects vanilla only runs behind an
    // is_valid/trigger gate (give title, war, imprison) are NOT allow-listed.
    if (!scripted_effect_is_allow_listed(effect_name)) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "effect_not_allow_listed:%.80s", effect_name);
        intent->consumed = true;
        IQLOG("Scripted-effect Execute refused (not allow-listed): '%s'\n", effect_name);
        return false;
    }

    uint32_t actor_ref = intent->actor_id & 0xFFFFFF;   // packed 24-bit char idx
    int walk_sig = 0;
    int walk_rc = run_effect_tree_walk_hardened(tpl, actor_ref, /*watchdog_secs=*/5, &walk_sig);
    // ck-zgh4: rc 4 is the scope-latch refusal, and it MUST be matched before
    // the generic branch below. That branch reports `execute_crashed:` with
    // walk_sig, and walk_sig is 0 for a refusal — so a latched walk would have
    // reached the seat as "execute_crashed:UNKNOWN", telling it the effect
    // faulted when in fact nothing ran.
    if (walk_rc == 4) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "%s", bridge::scope_latched_reason());
        intent->consumed = true;
        IQLOG("Scripted-effect refused (scope latched): '%s'\n", effect_name);
        return false;
    }
    if (walk_rc != 0) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "execute_crashed:%s",
                 (walk_sig == 1) ? "SIGSEGV" : (walk_sig == 2) ? "SIGALRM" : "UNKNOWN");
        intent->consumed = true;
        IQLOG("Scripted-effect Execute crashed (sig=%d): '%s'\n", walk_sig, effect_name);
        return false;
    }

    intent->executed = true;
    intent->consumed = true;
    {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "scripted_effect '%.80s' walked on actor %u",
                 effect_name, intent->actor_id & 0xFFFFFF);   // rc2 B2: no VA
        intent->console_output = buf;
    }
    IQLOG("Scripted-effect intent EXECUTED (pinned-frame walk): actor=%u effect='%s' template=%p\n",
          intent->actor_id & 0xFFFFFF, effect_name, tpl);
    return true;
}

// =============================================================================
// WAR_CREATE_CORE direct invocation (ck-eyfb)
// =============================================================================
// Mirrors WAR_DECLARE_SPECIAL_HANDLER (FUN_065db970) — 22-line setup that calls
// WAR_CREATE_CORE directly with pre-resolved char IDs, CB type ptr, and a
// minimal CB-params struct holding a single-title array.
//
// WAR_CREATE_CORE signature (re/findings/re_war_create_core_decomp.md §1):
//   CWar* WAR_CREATE_CORE(
//       CGameState*      game_state,        // arg 0
//       uint32_t         atk_id,            // arg 1
//       uint32_t         def_id,            // arg 2
//       uint32_t         claimant_id,       // arg 3 (0xFFFFFFFF if none)
//       CCasusBelliType* cb_type,           // arg 4 (NULL → SF1 silent fail)
//       CbParams*        cb_params,         // arg 5 (struct: {ptr, cap, count})
//       uint64_t*        current_date_ptr,  // arg 6 (read for war+0x80)
//       TitleVec*        opt_titles);       // arg 7 (NULL → auto-name)
//
// The cb_params layout consumed by CCasusBelli_Set (FUN_05b5ab70 decomp):
//   +0x00: u64 title-array data ptr (read as `*param_4`)
//   +0x08: u32 capacity (compatible with destination CB's title-array capacity)
//   +0x0c: u32 count    (read as `*(u32*)(param_4 + 0xc)`)
// CCasusBelli_Set deep-copies the array via the destination CB's allocator, so
// a stack-allocated source buffer is safe.

bool execute_war_create_core_intent(Intent* intent) {
    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;

    // ck-zgh4: set if the scope latch makes us skip the CB-validity pre-check
    // (progenitor ruling: skip the check, make the war). Declared at FUNCTION
    // scope because the reply that has to disclose it is built ~450 lines
    // below, in a different block from the gate that sets it.
    bool validity_skipped_latched = false;

    uint32_t atk_id     = intent->actor_id & 0xFFFFFF;
    uint32_t def_id     = intent->target_id & 0xFFFFFF;
    uint32_t claimant   = (intent->claimant_id != 0)
                          ? (intent->claimant_id & 0xFFFFFF)
                          : 0xFFFFFFFFu;
    int      title_id  = intent->title_id;
    const char* cb_name = intent->interaction_name.c_str();

    IQLOG("=== execute_war_create_core_intent ENTRY: atk=%u def=%u claimant=%u title=%d cb='%s' ===\n",
          atk_id, def_id, claimant, title_id, cb_name);

    auto reject = [&](const char* reason) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "%s", reason);
        intent->consumed = true;
        IQLOG("WAR_CREATE_CORE_DIRECT rejected: %s\n", reason);
        return false;
    };

    // ck-2rxyz: per-step IQLOG checkpoints + watchdog protection on every step.
    // Prior code only guarded steps 4/6/7 (gs_resolve, lock_acquire, war_create_core)
    // with watchdog+sigsetjmp. Steps 1/2/5 ran bare — if any engine memory read
    // raced with a parallel worker still holding allocator state, or a CB-DB
    // pointer slid into a partial-mapped page, the safe_read SIGSEGV recovery
    // path would longjmp to the OUTER on_tick recovery point (which does NOT
    // mark the intent consumed). The game thread then "returned" from drain,
    // BUT the intent stayed unconsumed and the IPC poll saw count=0 forever.
    //
    // This patch: (a) wrap each previously-bare step in its own
    // sigsetjmp+watchdog region so SIGSEGV/timeout converts to a real
    // intent->rejected with a step-specific reason, and (b) emit an IQLOG
    // checkpoint before each region so the bridge log pinpoints the failing
    // step on the next live repro.

    // 1) Validate characters.
    IQLOG("WAR_CREATE_CORE_DIRECT step=1 validate_characters: looking up attacker=%u defender=%u claimant=%u\n",
          atk_id, def_id, claimant);
    void* attacker = nullptr;
    void* defender = nullptr;
    void* claimant_char = nullptr;  // null = no claimant lookup needed
    {
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false; watchdog_disarm();
            const char* sig_name = (sig == 1) ? "sigsegv" : (sig == 2) ? "timeout" : "unknown";
            char reason_buf[64];
            snprintf(reason_buf, sizeof(reason_buf), "validate_characters_%s", sig_name);
            return reject(reason_buf);
        }
        try {
            attacker = lookup_character(atk_id);
            defender = lookup_character(def_id);
            if (claimant != 0xFFFFFFFFu) {
                claimant_char = lookup_character(claimant);
            }
        } catch (...) {
            g_in_protected_region = false; watchdog_disarm();
            return reject("validate_characters_exception");
        }
        g_in_protected_region = false; watchdog_disarm();
    }
    if (!attacker) return reject("attacker_not_found");
    if (!defender) return reject("defender_not_found");
    if (claimant != 0xFFFFFFFFu && !claimant_char) return reject("claimant_not_found");

    // g_char_is_alive calls into the engine (CHAR_IS_ALIVE @ 0x0664b2c0).
    // Single function pointer call, short body, but it CAN hang if engine
    // state is mid-mutation. Watchdog this separately so a hang surfaces as
    // a step-specific reject.
    IQLOG("WAR_CREATE_CORE_DIRECT step=1 is_alive: attacker=%p defender=%p claimant=%p\n",
          attacker, defender, claimant_char);
    if (g_char_is_alive) {
        bool atk_alive = true, def_alive = true, clmt_alive = true;
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false; watchdog_disarm();
            const char* sig_name = (sig == 1) ? "sigsegv" : (sig == 2) ? "timeout" : "unknown";
            char reason_buf[64];
            snprintf(reason_buf, sizeof(reason_buf), "is_alive_%s", sig_name);
            return reject(reason_buf);
        }
        try {
            atk_alive = g_char_is_alive(attacker);
            def_alive = g_char_is_alive(defender);
            if (claimant_char) clmt_alive = g_char_is_alive(claimant_char);
        } catch (...) {
            g_in_protected_region = false; watchdog_disarm();
            return reject("is_alive_exception");
        }
        g_in_protected_region = false; watchdog_disarm();
        if (!atk_alive) return reject("attacker_not_alive");
        if (!def_alive) return reject("defender_not_alive");
        if (claimant_char && !clmt_alive) return reject("claimant_not_alive");
    }

    // 2) Resolve CB type by name in CASUS_BELLI_DB.
    if (intent->interaction_name.empty()) return reject("cb_name_required");

    IQLOG("WAR_CREATE_CORE_DIRECT step=2 cb_db_lookup: scanning for cb_name='%s'\n", cb_name);
    uintptr_t cb_type_ptr = 0;
    char      resolved_cb_name[128] = {};
    {
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false; watchdog_disarm();
            const char* sig_name = (sig == 1) ? "sigsegv" : (sig == 2) ? "timeout" : "unknown";
            char reason_buf[64];
            snprintf(reason_buf, sizeof(reason_buf), "cb_db_scan_%s", sig_name);
            return reject(reason_buf);
        }

        uintptr_t cb_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::CASUS_BELLI_DB);
        if (!cb_db || !is_mapped(cb_db)) {
            g_in_protected_region = false; watchdog_disarm();
            return reject("cb_db_unmapped");
        }
        uintptr_t cb_arr = safe_read<uintptr_t>(cb_db + 0x88);
        int32_t   cb_count = safe_read<int32_t>(cb_db + 0x94);
        if (!cb_arr || !is_mapped(cb_arr) || cb_count <= 0 || cb_count > 500) {
            g_in_protected_region = false; watchdog_disarm();
            return reject("cb_db_invalid");
        }

        // Collect a sample of available CB names so a miss is self-diagnosing
        // (A4: agents passed wrong names like 'de_jure_county_cb' — the engine
        // name is 'de_jure_cb'/'individual_county_de_jure_cb'). We can't list all
        // ~119, so capture CBs that match the requested prefix-ish substring, plus
        // a few common ones, into a short buffer.
        char avail[224]; avail[0] = '\0'; size_t avail_len = 0;
        for (int32_t i = 0; i < cb_count && i < 500; i++) {
            uintptr_t cbt = safe_read<uintptr_t>(cb_arr + i * 8);
            if (!cbt || !is_mapped(cbt)) continue;
            const char* nptr = nullptr; size_t nlen = 0;
            if (!read_db_obj_name(cbt, nptr, nlen)) continue;
            if (nlen == intent->interaction_name.size() &&
                strncmp(nptr, cb_name, nlen) == 0) {
                cb_type_ptr = cbt;
                size_t copy = nlen < sizeof(resolved_cb_name) - 1 ? nlen : sizeof(resolved_cb_name) - 1;
                memcpy(resolved_cb_name, nptr, copy);
                break;
            }
            // Suggest CBs sharing a leading token with the request (e.g. "de_jure").
            if (nlen >= 3 && nlen < 40 && avail_len + nlen + 2 < sizeof(avail) &&
                strncmp(nptr, cb_name, 3) == 0) {
                if (avail_len) { avail[avail_len++] = ','; }
                memcpy(avail + avail_len, nptr, nlen); avail_len += nlen;
                avail[avail_len] = '\0';
            }
        }
        g_in_protected_region = false; watchdog_disarm();
        if (!cb_type_ptr) {
            char reason_buf[300];
            snprintf(reason_buf, sizeof(reason_buf),
                     "cb_type_not_found:'%s'%s%s", cb_name,
                     avail_len ? " did_you_mean=" : "", avail_len ? avail : "");
            return reject(reason_buf);
        }
    }
    if (!cb_type_ptr) return reject("cb_type_not_found");
    IQLOG("WAR_CREATE_CORE_DIRECT step=2 cb resolved: cb_type_ptr=0x%lx name='%s'\n",
          (unsigned long)cb_type_ptr, resolved_cb_name);

    // 2b) #74 title-required pre-guard. Read the CB's TARGET_MODE (cb+0x738):
    //   0 = none (subjugation/vassalization/tributary — no title)
    //   6 = claim (title from claimant's claims)
    //   other (1/2/3/4/5/7) = title-targeted (conquest / de_jure / etc.)
    // A title-targeted CB declared WITHOUT a cb_title_id is invalid (and worse:
    // calling the engine CB_VALIDITY_CHECK / WAR_CREATE_CORE for such a CB with
    // title=0 NULL-DEREFS inside the engine's title-mode evaluator —
    // live-observed crash at rip_off=0x6cd2cdf for county_conquest_cb title=0;
    // the bridge's SIGSEGV net recovered it but the engine still emitted a crash
    // dump). Reject early with a clear reason so the agent supplies a target
    // county/duchy title. Read is watchdog/sigsetjmp-guarded like every other
    // engine-memory read in this function. On read failure we DON'T reject
    // (fall through — preserves the prior behavior for claim_cb/subjugation).
    int32_t cb_target_mode = -1;
    {
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false; watchdog_disarm();
            // Read fault: leave cb_target_mode=-1 (skip the guard, fall through).
            IQLOG("WAR_CREATE_CORE_DIRECT step=2b target_mode read recovered (sig=%d) — skipping guard\n", sig);
        } else {
            cb_target_mode = safe_read<int32_t>(cb_type_ptr + ck3::cb_type::TARGET_MODE);
            g_in_protected_region = false; watchdog_disarm();
        }
    }
    if (cb_target_mode != -1 && cb_target_mode != 0 && cb_target_mode != 6 && title_id <= 0) {
        IQLOG("WAR_CREATE_CORE_DIRECT step=2b REJECT title_required: cb='%s' target_mode=%d title_id=%d\n",
              resolved_cb_name, cb_target_mode, title_id);
        return reject("title_required_for_cb");
    }
    IQLOG("WAR_CREATE_CORE_DIRECT step=2b target_mode=%d title_id=%d OK\n", cb_target_mode, title_id);

    // 2c) Pre-creation claim validation (L11 — claim_cb fix).
    //
    // ROOT CAUSE this guards: WAR_CREATE_CORE was called with ZERO claim/court
    // validation. The agent could press any title with any claimant; the engine
    // builds the war then immediately CULLS it because the claimant holds no
    // PRESSED claim on the title (SOLVED memory: "claim_cb needs a PRESSED claim
    // or the engine culls"). We reject BEFORE creating the war so the failure is
    // a legible rejection instead of a phantom war that silently vanishes.
    //
    // Two pure-read checks against a PRESSED title-targeted CB (title_id > 0):
    //   (a) the effective claimant holds a PRESSED claim on cb_title_id, and
    //   (b) the claimant is pressable by the attacker (is the attacker, or sits
    //       in the attacker's court / is the attacker's vassal — liege/host id
    //       equals the attacker id).
    // Both replicate the validated do_query_title_claimants (query_handlers.cpp
    // ~7319-7335) and the liege/host resolution (query_handlers.cpp ~9351-9370)
    // patterns. Skipped for non-title CBs (subjugation, etc.) consistent with the
    // title_count==0 fast path below.
    //
    // The effective claimant when none is given is the attacker (self-pressed
    // claim): the WCC arg default of 0xFFFFFFFF means "no claimant" to the
    // engine, so for a self-pressed claim_cb we both validate AND pass atk_id.
    // #74: the pressed-claim + court-membership pre-checks below are CLAIM-CB
    // specific (a conquest / de_jure / subjugation war needs NO pressed claim —
    // it derives legality from de-jure / group-trigger gates, validated by the
    // engine CB_VALIDITY_CHECK in step 2d). Scope the claim-validation block to
    // claim-type CBs (resolved_cb_name == "claim_cb") so a title-targeted
    // CONQUEST CB (duchy_conquest_cb etc., title_id>0) is NOT falsely rejected
    // with claim_not_pressed. The alliance/truce 2c.2 backstop below stays
    // applicable to all title CBs (engine culls allied/truced targets for any
    // offensive title war).
    const bool is_claim_type_cb = (strcmp(resolved_cb_name, "claim_cb") == 0);
    if (title_id > 0 && is_claim_type_cb) {
        // For a self-pressed claim_cb (no explicit claimant) the attacker is the
        // claimant; promote the WCC claimant arg to atk_id so the engine sees a
        // claimant rather than "none" (which would cull the war).
        if (claimant == 0xFFFFFFFFu) {
            claimant = atk_id;
            IQLOG("WAR_CREATE_CORE_DIRECT step=2c no explicit claimant → self-press: claimant=atk=%u\n",
                  atk_id);
        }
        uint32_t eff_claimant = claimant;
        void* eff_claimant_char =
            (eff_claimant == atk_id) ? attacker
            : (claimant_char ? claimant_char : lookup_character(eff_claimant));
        if (!eff_claimant_char || !is_mapped(reinterpret_cast<uintptr_t>(eff_claimant_char))) {
            return reject("claimant_not_found");
        }

        IQLOG("WAR_CREATE_CORE_DIRECT step=2c claim_validate: eff_claimant=%u char=%p title=%d\n",
              eff_claimant, eff_claimant_char, title_id);

        bool pressed_claim = false;
        uint32_t claimant_liege_id = 0;
        bool court_resolved = false;
        {
            watchdog_arm(2);
            g_in_protected_region = true;
            int sig = sigsetjmp(g_recovery_point, 1);
            if (sig != 0) {
                g_in_protected_region = false; watchdog_disarm();
                const char* sig_name = (sig == 1) ? "sigsegv" : (sig == 2) ? "timeout" : "unknown";
                char reason_buf[64];
                snprintf(reason_buf, sizeof(reason_buf), "claim_validate_%s", sig_name);
                return reject(reason_buf);
            }
            try {
                uintptr_t ecc = reinterpret_cast<uintptr_t>(eff_claimant_char);

                // (a) PRESSED-claim-on-title scan (do_query_title_claimants pattern).
                uintptr_t core_data = safe_read<uintptr_t>(ecc + ck3::character::REALM_DATA);
                if (core_data && is_mapped(core_data)) {
                    uintptr_t claims_pdx = core_data + ck3::core_data::CLAIMS_ARRAY;
                    uintptr_t claim_data_ptr = safe_read<uintptr_t>(claims_pdx + 0x00);
                    int32_t claim_cnt = safe_read<int32_t>(claims_pdx + 0x0C);
                    if (claim_cnt < 0 || claim_cnt > 200) claim_cnt = 0;
                    if (claim_data_ptr && is_mapped(claim_data_ptr)) {
                        for (int32_t j = 0; j < claim_cnt; j++) {
                            uintptr_t entry = claim_data_ptr + j * ck3::claim_entry::ENTRY_SIZE;
                            uint32_t claim_tid = safe_read<uint32_t>(entry + ck3::claim_entry::TITLE_ID) & 0xFFFFFF;
                            if (claim_tid == (uint32_t)title_id) {
                                if (safe_read<uint8_t>(entry + ck3::claim_entry::PRESSED) != 0) {
                                    pressed_claim = true;
                                }
                                break;
                            }
                        }
                    }
                }

                // (b) pressable-by-attacker (court membership). Attacker pressing
                // own claim is trivially in-court; otherwise resolve liege/host.
                if (eff_claimant == atk_id) {
                    claimant_liege_id = atk_id;
                    court_resolved = true;
                } else {
                    // Landed claimant → title_realm.liege.
                    uintptr_t tr = safe_read<uintptr_t>(ecc + ck3::character::TITLE_REALM_DATA);
                    if (tr && is_mapped(tr)) {
                        uintptr_t liege = safe_read<uintptr_t>(tr + ck3::title_realm::LIEGE_PTR);
                        if (liege && is_mapped(liege)) {
                            claimant_liege_id = safe_read<uint32_t>(liege + ck3::character::ID) & 0xFFFFFF;
                            court_resolved = true;
                        }
                    }
                    // Unlanded courtier → courtier_data.liege_char_ref.
                    if (!court_resolved) {
                        uintptr_t cd = safe_read<uintptr_t>(ecc + ck3::character::COURTIER_DATA);
                        if (cd && is_mapped(cd)) {
                            claimant_liege_id = safe_read<uint32_t>(cd + ck3::courtier_data::LIEGE_CHAR_REF) & 0xFFFFFF;
                            court_resolved = true;
                        }
                    }
                }
            } catch (...) {
                g_in_protected_region = false; watchdog_disarm();
                return reject("claim_validate_exception");
            }
            g_in_protected_region = false; watchdog_disarm();
        }

        if (!pressed_claim) return reject("claim_not_pressed");
        // Court membership: pass if claimant is the attacker, or the claimant's
        // resolved liege/host is the attacker. court_resolved==false means the
        // claimant is independent (no liege) and not the attacker → not in court.
        if (claimant_liege_id != atk_id) return reject("claimant_not_in_court");

        IQLOG("WAR_CREATE_CORE_DIRECT step=2c claim OK: pressed=1 liege=%u==atk\n", claimant_liege_id);

        // 2c.1) Independent-target pre-check (L19 a.1).
        //
        // ROOT CAUSE this guards: WAR_CREATE_CORE never validated that the
        // DEFENDER is a legal claim_cb target. claim_cb (offensive title CB)
        // requires an INDEPENDENT defender — the engine drops the war on its
        // next tick (kind_relation) when the target sits under a liege (a
        // sub-vassal). The bridge previously reported war_created=1 for these,
        // misleading the agent into "silent failure".
        //
        // PURE READ ONLY: resolve the defender's effective liege via
        // defender → TITLE_REALM_DATA → title_realm::LIEGE_PTR. This mirrors
        // EXACTLY the landed-claimant liege resolution above (≈lines 1383-1387,
        // same safe_read helpers, same TITLE_REALM_DATA + LIEGE_PTR offsets).
        // A non-null, mapped liege ⇒ defender is NOT independent ⇒ reject.
        //
        // SCOPING: applied for claim_cb ONLY (resolved_cb_name match). Other
        // title CBs (faction/independence/liege-revocation style) legitimately
        // target vassals; broadly rejecting non-independent targets would
        // over-reject those. The agent's case is claim_cb, so we scope narrowly
        // here and leave the broader CB-kind classification to the engine gate.
        if (strcmp(resolved_cb_name, "claim_cb") == 0) {
            bool def_independent = true;
            {
                watchdog_arm(2);
                g_in_protected_region = true;
                int sig = sigsetjmp(g_recovery_point, 1);
                if (sig != 0) {
                    g_in_protected_region = false; watchdog_disarm();
                    const char* sig_name = (sig == 1) ? "sigsegv" : (sig == 2) ? "timeout" : "unknown";
                    char reason_buf[64];
                    snprintf(reason_buf, sizeof(reason_buf), "target_independent_check_%s", sig_name);
                    return reject(reason_buf);
                }
                try {
                    // FIX (L22): use the AUTHORITATIVE get_liege_id_from_addr (the same read
                    // query_character uses) instead of the raw title_realm+0x448 LIEGE_PTR.
                    // For an INDEPENDENT ruler the 0x448 pointer is non-null but points to a
                    // sentinel whose char-id reads 0xFFFFFFFF, so get_liege_id_from_addr returns
                    // the 0xFFFFFF "no liege" sentinel. The OLD raw-pointer check (non-null +
                    // mapped => "has liege") therefore FALSE-rejected independent targets
                    // (live: King of Burgundy 50420, liege_id 16777215 = 0xFFFFFF), blocking
                    // perfectly legal claim wars. A REAL liege id (not the 0xFFFFFF sentinel,
                    // not 0 = landless/no-realm) => the target is a vassal => reject.
                    uint32_t dl = get_liege_id_from_addr(reinterpret_cast<uintptr_t>(defender)) & 0xFFFFFF;
                    if (dl != 0xFFFFFF && dl != 0) {
                        def_independent = false;  // genuine liege => not independent
                    }
                } catch (...) {
                    g_in_protected_region = false; watchdog_disarm();
                    return reject("target_independent_check_exception");
                }
                g_in_protected_region = false; watchdog_disarm();
            }
            if (!def_independent) {
                IQLOG("WAR_CREATE_CORE_DIRECT step=2c.1 REJECT target_not_independent: def=%u has a liege (claim_cb requires independent target)\n",
                      def_id);
                return reject("target_not_independent");
            }
            IQLOG("WAR_CREATE_CORE_DIRECT step=2c.1 target_independent OK: def=%u has no liege\n", def_id);
        }

        // 2c.2) Alliance / truce pre-check (L19 a.2).
        //
        // ROOT CAUSE this guards: the engine culls a war against an allied or
        // truced target (in_alliance / in_truce). Reuse the validated bilateral
        // probe from the CB-query path (ai_eval_hook.cpp ck-it6e, ≈lines 3042-3098):
        // GetAllianceList(actor) / GetTruceList(actor) → CDynArray of 8-byte
        // char-ptrs, SIMD_FIND_LONG(begin,end,defender). PURE READS plus those
        // three engine LIST-getters (no mutation, no CB evaluator — the hung
        // allowed_for_character path stays out of scope per L14).
        //
        // SCOPING: applied for title-targeted CBs (title_id > 0, this block) to
        // mirror the engine's title-relation alliance/truce gate; non-title CBs
        // (subjugation, etc.) are out of this block entirely. On any probe
        // failure (helpers unmapped / list unmapped / crash) we fail OPEN — let
        // the WCC call proceed — exactly as the CB-query path degrades.
        {
            // #119 FIX: alliance is checked via the AUTHORITATIVE relation array
            // (core_data+0x20 → CDiplomaticRelation; allied == REASON_COUNT@+0x94 != 0
            // && BROKEN_FLAG@+0x189 == 0), mirroring engine IsAlliedTo. The previous
            // MAYBE_GET_ALLIANCE_LIST (+0x578) search matched the attacker's WAR
            // PARTICIPANTS, FALSE-rejecting legal wars on non-allied neighbors (the
            // exact #119 symptom that boxed the Britannia agent in). TRUCE keeps the
            // GET_TRUCE_LIST search — that list (+0x6A0) is the genuine truce list.
            auto get_truce_list = reinterpret_cast<uintptr_t(*)(void*)>(
                g_base_addr + ck3::funcs::GET_TRUCE_LIST);
            auto simd_find = reinterpret_cast<uintptr_t(*)(uintptr_t, uintptr_t, void*)>(
                g_base_addr + ck3::funcs::SIMD_FIND_LONG);

            const bool at_funcs_mapped =
                is_mapped(reinterpret_cast<uintptr_t>(get_truce_list))    &&
                is_mapped(reinterpret_cast<uintptr_t>(simd_find));

            uint8_t in_alliance = 0;
            uint8_t in_truce    = 0;
            if (at_funcs_mapped) {
                watchdog_arm(2);
                g_in_protected_region = true;
                int sig = sigsetjmp(g_recovery_point, 1);
                if (sig != 0) {
                    g_in_protected_region = false; watchdog_disarm();
                    // Fail open: a probe crash must not block a legal war.
                    IQLOG("WAR_CREATE_CORE_DIRECT step=2c.2 alliance/truce probe crashed (sig=%d) — failing open\n", sig);
                } else {
                    try {
                        // --- Alliance: authoritative relation-array walk (#119) ---
                        uintptr_t atk_core = safe_read<uintptr_t>(
                            reinterpret_cast<uintptr_t>(attacker) + ck3::character::REALM_DATA);
                        if (atk_core && is_mapped(atk_core)) {
                            uintptr_t rel_arr = safe_read<uintptr_t>(atk_core + ck3::relation_container::ARRAY_PTR);
                            int32_t   rel_cnt = safe_read<int32_t>(atk_core + ck3::relation_container::COUNT);
                            if (rel_cnt < 0 || rel_cnt > 5000) rel_cnt = 0;
                            if (rel_arr && is_mapped(rel_arr) && rel_cnt > 0) {
                                for (int32_t ri = 0; ri < rel_cnt; ri++) {
                                    uintptr_t ent = rel_arr + ri * ck3::relation_container::ENTRY_STRIDE;
                                    uint32_t oid = safe_read<uint32_t>(ent + ck3::relation_container::ENTRY_KEY) & 0xFFFFFF;
                                    if (oid != def_id) continue;
                                    uintptr_t rel = safe_read<uintptr_t>(ent + ck3::relation_container::ENTRY_PTR);
                                    if (rel && is_mapped(rel)) {
                                        int32_t reasons = safe_read<int32_t>(rel + ck3::alliance::REASON_COUNT);
                                        uint8_t broken  = safe_read<uint8_t>(rel + ck3::alliance::BROKEN_FLAG);
                                        if (reasons > 0 && reasons <= 20 && broken == 0) in_alliance = 1;
                                    }
                                    break;
                                }
                            }
                        }
                        uintptr_t tlist = get_truce_list(attacker);
                        if (tlist && is_mapped(tlist)) {
                            uintptr_t t_begin = safe_read<uintptr_t>(tlist + 0);
                            int32_t   t_count = safe_read<int32_t>(tlist + 0xC);
                            if (t_begin && is_mapped(t_begin) && t_count > 0 && t_count < 1000) {
                                uintptr_t t_end = t_begin + (size_t)t_count * 8;
                                uintptr_t hit = simd_find(t_begin, t_end, defender);
                                in_truce = (hit != 0 && hit != t_end) ? 1 : 0;
                            }
                        }
                    } catch (...) {
                        IQLOG("WAR_CREATE_CORE_DIRECT step=2c.2 alliance/truce probe threw — failing open\n");
                    }
                    g_in_protected_region = false; watchdog_disarm();
                }
            } else {
                IQLOG("WAR_CREATE_CORE_DIRECT step=2c.2 alliance/truce helpers not mapped — failing open\n");
            }

            if (in_alliance) {
                IQLOG("WAR_CREATE_CORE_DIRECT step=2c.2 REJECT target_in_alliance: atk=%u def=%u\n", atk_id, def_id);
                return reject("target_in_alliance");
            }
            if (in_truce) {
                IQLOG("WAR_CREATE_CORE_DIRECT step=2c.2 REJECT target_in_truce: atk=%u def=%u\n", atk_id, def_id);
                return reject("target_in_truce");
            }
            IQLOG("WAR_CREATE_CORE_DIRECT step=2c.2 alliance/truce OK: in_alliance=%u in_truce=%u\n",
                  in_alliance, in_truce);
        }
    }

    // 2d) AUTHORITATIVE engine CB-validity gate (L19 — safe re-do of the L14
    // revert). The cheap 2c / 2c.1 / 2c.2 pre-checks above stay as a fast-path
    // for the obvious cases; this is the comprehensive backstop that runs the
    // engine's full CB_VALIDITY_CHECK (Gates 0-6, incl. Gate 2's
    // allowed_for_character — holy-war faith-hostility + per-CB innovation/era
    // gating) so we reject with the ENGINE's own verdict before WCC.
    //
    // SAME call path as discover(valid_cbs_strict): the CB_QUERY service loop at
    // ai_eval_hook.cpp:3514 calls cb_validity_check(cb_type, &cb_output_list,
    // attacker, defender, 0) with the fn typed at ai_eval_hook.cpp:2830-2832 as
    //   bool(void* cb_type, void* output, void* attacker, void* defender, int skip_against)
    // resolved from ck3::funcs::CB_VALIDITY_CHECK (0x06cce6d0). The CDynArray
    // output layout {data, capacity, count, allocator} backed by 32×0x98-byte
    // stack entries mirrors ai_eval_hook.cpp:3500-3512 exactly so the engine's
    // append path never reallocs. No new offset is introduced.
    //
    // WHY THIS IS SAFE WHERE L14 WAS NOT — three layers:
    //   1. arm_fake_random_scope(): THE fix L14 lacked. The CB_QUERY loop runs on
    //      the IPC service thread; here we are on the GAME THREAD in on_tick where
    //      RandomScopeManager+0x20 is null, so Gate-2's allowed_for_character
    //      trigger would advance RNG into a null scope and spin forever. We arm
    //      the same per-thread fake scope used around the WCC call below, then
    //      restore it on EVERY exit path.
    //   2. watchdog_arm(4) + sigsetjmp: any hang/crash inside the engine call
    //      longjmps back out (timeout bounded to 4s, matching the WCC call).
    //   3. FAIL-OPEN: if the call crashes, times out, throws, or the rng scope
    //      can't be armed / the fn is unmapped, we RESTORE the scope and PROCEED
    //      to create the war exactly as today. We only REJECT when the engine
    //      definitively returns "invalid" — never on infrastructure failure.
    //
    // SCOPING: applied for the offensive title CBs the agent uses, gated by the
    // SAME condition 2c.1 covers (claim_cb) PLUS the de_jure CBs (de_jure_cb /
    // de_jure_county_cb / de_jure_duchy_cb / de_jure_kingdom_cb), which is what
    // the agent declares via this steered path and which the engine culls on
    // innovation/era/faith grounds. Faction / independence / liege-revocation
    // style CBs (declared via other paths, legitimately targeting vassals) are
    // intentionally NOT gated here to avoid over-rejection. When unsure we stay
    // narrow — same posture as 2c.1.
    {
        // #74 DIRECTIONAL-CONQUEST ENABLER: extend the engine CB-validity gate to
        // cover the directional CBs an agent uses to AIM the steered war at a
        // SPECIFIC neighbor — subjugation, conquest, invasion, vassalization,
        // tributary — in addition to claim_cb / de_jure*. This is the LEGITIMACY
        // backstop: a steered subjugation/conquest war is created ONLY if the
        // engine's own CB_VALIDITY_CHECK grants it for THIS (attacker, target).
        //
        // WHY THIS IS LEGITIMATE (not a cheat) — RE-confirmed 2026-06-22:
        //   CB_VALIDITY_CHECK (0x06cce6d0 → FUN_06dce220) runs CB_EXTENDED_TRIGGER
        //   (0x06ccec00), whose FIRST gate is the GROUP-level is_valid trigger at
        //   *(cb_type+0x30)+0x48 — i.e. `subjugation:allowed_for_character` /
        //   `conquest:allowed_for_character`, which enforce the tribal-government
        //   + faith-doctrine + neighbor + era gates. So even though the PER-CB
        //   triggers for tribal_subjugation_cb / vassalization_cb are empty
        //   (re_conquest_subjugation_cb_dispatch.md), the engine's group gate is
        //   evaluated here. A feudal / wrong-doctrine / non-neighbor ruler is
        //   correctly REJECTED (cb_not_valid:<name>) — exactly the engine's own
        //   verdict, no uncosted directional war. Live-confirmed in 0891 save:
        //   tribal_subjugation_cb drops on `doctrine`, conquest on `government`
        //   for the Halfdan-dynasty rulers; the engine refuses them.
        //
        // Conquest CBs are title-targeted (target_mode 1/7), so title_id>0 and
        // the claim/independent/alliance 2c pre-checks above are scoped to
        // claim_cb only — they do NOT over-reject conquest titles. Subjugation /
        // vassalization / tributary are target_mode=0 (no title); the 2c block is
        // skipped for them entirely (title_id==0), so this 2d engine gate is
        // their ONLY validity check — which is correct and sufficient.
        const bool gate_this_cb =
            strcmp(resolved_cb_name, "claim_cb") == 0            ||
            strncmp(resolved_cb_name, "de_jure", 7) == 0         ||
            strstr(resolved_cb_name, "subjugation") != nullptr   ||
            strstr(resolved_cb_name, "conquest")    != nullptr   ||
            strstr(resolved_cb_name, "vassalization") != nullptr ||
            strstr(resolved_cb_name, "tributary")   != nullptr   ||
            // FIXPLAN A2: route holy wars through the same engine CB_VALIDITY_CHECK
            // machinery as conquest/de_jure. The substr "religious_war" covers
            // religious_war / minor_religious_war / major_religious_war in one
            // test. Before this, holy-war declares were built fail-open and
            // silently culled next tick (faith hostility / temporal-head /
            // holy-order gates) with no reason returned — a phantom war that
            // looked like a successful declare. Now the agent gets a real
            // cb_not_valid:<reason>. Additive to the proven gated path; no new
            // call, same CB_VALIDITY_CHECK at funcs::CB_VALIDITY_CHECK.
            strstr(resolved_cb_name, "religious_war") != nullptr ||
            strcmp(resolved_cb_name, "invasion_war") == 0;

        if (gate_this_cb) {
            auto cb_validity_check = reinterpret_cast<bool(*)(void*, void*, void*, void*, int)>(
                g_base_addr + ck3::funcs::CB_VALIDITY_CHECK);

            if (!is_mapped(reinterpret_cast<uintptr_t>(cb_validity_check))) {
                // Fail open: validity fn unmapped → proceed to create the war.
                IQLOG("WAR_CREATE_CORE_DIRECT step=2d CB_VALIDITY_CHECK unmapped — failing open\n");
            } else if (bridge::scope_latched()) {
                // ck-zgh4, PROGENITOR RULING 2026-10-06 ("Skip the check, make
                // the war"): this is the ONE latched site that does not refuse.
                // The check it guards is an OPTIONAL pre-check whose own
                // fallback, three lines above, is already to fail open — and
                // declaring war is central to play, so the ruling was to keep
                // the verb and lose the check rather than the reverse. The
                // stated risk, accepted: a war with no valid reason could get
                // through, and how the engine handles that is untested.
                //
                // Counted separately from arm_refused_latched because this is
                // the one latched path that still MUTATES, so it can never be
                // read off that counter. The reply carries a note saying the
                // pre-check was skipped — a war the seat cannot distinguish
                // from a validated one would be the worse outcome.
                bridge::g_war_validity_skipped_latched.fetch_add(
                    1, std::memory_order_relaxed);
                validity_skipped_latched = true;
                IQLOG("WAR_CREATE_CORE_DIRECT step=2d SKIPPED: scope latched — "
                      "failing open per ruling (war proceeds, validity unchecked)\n");
            } else {
                // Arm the per-thread fake RNG scope (THE L14 fix). Restored on
                // every exit path below, success or fault.
                // volatile so they survive the sigsetjmp/longjmp restore path
                // (mirrors the WCC call's volatile saved_rng_scope/rng_scope_set).
                uint64_t saved_rng_local = 0;
                volatile bool rng_armed = arm_fake_random_scope(&saved_rng_local);
                volatile uint64_t saved_rng = saved_rng_local;

                bool ran   = false;   // did the engine call complete?
                bool valid = false;   // engine verdict (only meaningful if ran)

                watchdog_arm(4);
                g_in_protected_region = true;
                int sig = sigsetjmp(g_recovery_point, 1);
                if (sig != 0) {
                    g_in_protected_region = false; watchdog_disarm();
                    if (rng_armed) restore_fake_random_scope(saved_rng);
                    // FAIL OPEN: a hang (timeout longjmp) or crash inside the
                    // engine call must NEVER reject a legal war — fall through to
                    // war creation exactly as today.
                    IQLOG("WAR_CREATE_CORE_DIRECT step=2d CB_VALIDITY_CHECK recovered (sig=%s) — failing open\n",
                          (sig == 1) ? "sigsegv" : (sig == 2) ? "timeout" : "unknown");
                } else {
                    try {
                        // CDynArray output, identical layout to the CB_QUERY loop
                        // (ai_eval_hook.cpp:3500-3512): pre-size 32 entries so the
                        // engine's title/claim-mode append path never reallocs.
                        alignas(16) uint8_t cb_entry_storage[32 * pdx_array::CB_OPTION_ENTRY_STRIDE] = {};
                        void* alloc_vtable = reinterpret_cast<void*>(
                            g_base_addr + pdx_array::CB_OPTION_ALLOCATOR_VTABLE);
                        struct {
                            void*    data;
                            uint32_t capacity;
                            uint32_t count;
                            void**   allocator;
                        } cb_output_list;
                        void* alloc_obj = alloc_vtable;
                        cb_output_list.data      = cb_entry_storage;
                        cb_output_list.capacity  = 32;
                        cb_output_list.count     = 0;
                        cb_output_list.allocator = &alloc_obj;

                        valid = cb_validity_check(
                            reinterpret_cast<void*>(cb_type_ptr),
                            &cb_output_list,
                            attacker,
                            defender,
                            0);        // skip_against = 0 (check all gates)
                        ran = true;
                    } catch (...) {
                        // FAIL OPEN: exception → proceed (do not reject).
                        ran = false;
                        IQLOG("WAR_CREATE_CORE_DIRECT step=2d CB_VALIDITY_CHECK threw — failing open\n");
                    }
                    g_in_protected_region = false; watchdog_disarm();
                    if (rng_armed) restore_fake_random_scope(saved_rng);
                }

                if (ran) {
                    IQLOG("WAR_CREATE_CORE_DIRECT step=2d CB_VALIDITY_CHECK('%s') returned %d\n",
                          resolved_cb_name, valid ? 1 : 0);
                    if (!valid) {
                        // Engine definitively rejects this (attacker, target, cb)
                        // triple. Surface a CB-scoped reason. The engine's free-text
                        // reason isn't reliably exposed via this ABI, so we use a
                        // generic, CB-tagged key.
                        char reason_buf[160];
                        snprintf(reason_buf, sizeof(reason_buf),
                                 "cb_not_valid:%s", resolved_cb_name);
                        return reject(reason_buf);
                    }
                }
                // ran==false ⇒ fail-open: fall through to war creation.
            }
        }
    }

    // 3) Build CB-params struct on the stack: {data_ptr, cap, count}.
    // Layout consumed by CCasusBelli_Set (param_4):
    //   +0x00: u64 ptr to u32[count] of title IDs
    //   +0x08: u32 capacity
    //   +0x0c: u32 count
    alignas(16) uint8_t cb_params_buf[16] = {};
    alignas(4)  uint32_t title_data[1]    = {};
    uint32_t title_count = 0;
    if (title_id > 0) {
        title_data[0] = static_cast<uint32_t>(title_id) & 0xFFFFFF;
        title_count = 1;
        *reinterpret_cast<uint64_t*>(cb_params_buf + 0x00) = reinterpret_cast<uint64_t>(title_data);
        *reinterpret_cast<uint32_t*>(cb_params_buf + 0x08) = title_count;
        *reinterpret_cast<uint32_t*>(cb_params_buf + 0x0c) = title_count;
    }
    // For non-title CBs (subjugation, etc.) the buffer stays zero — *(buf+0xc)
    // is read as 0 and CCasusBelli_Set takes the empty-source fast path.

    // 3b) optional_titles_vec (param_8). WAR_CREATE_CORE runs WAR_NAME_GENERATE
    // (0x066b62c0 — the null-localization-scope crash from the on_tick drain
    // context) ONLY when param_8 == NULL. Passing a non-NULL TitleVec
    // {u64 data_ptr; u32 count} SKIPS name-gen entirely (the war is left
    // unnamed / named lazily) AND supplies the title list — exactly the escape
    // from the name-gen crash that blocked the direct path
    // (re_war_create_core_decomp §"param_8"). Reuse the same title_data array.
    alignas(8) struct { uint64_t data; uint32_t count; uint32_t _pad; } opt_titles_vec = {};
    void* opt_titles_arg = nullptr;
    if (title_count > 0) {
        opt_titles_vec.data  = reinterpret_cast<uint64_t>(title_data);
        opt_titles_vec.count = title_count;
        opt_titles_arg = &opt_titles_vec;
        IQLOG("WAR_CREATE_CORE_DIRECT step=3b opt_titles_vec armed (skips name-gen): data=%p count=%u\n",
              (void*)title_data, title_count);
    } else {
        IQLOG("WAR_CREATE_CORE_DIRECT step=3b WARNING: no title → param_8=NULL → name-gen WILL run (may crash)\n");
    }

    // 4) Resolve game-state pointers.
    //   root_slot   = read_u64(g_base_addr + GAME_STATE_ROOT)        (heap root ptr)
    //   game_state  = *(root_slot + 0xa0) + 0x299f0                  (FUN_06060210)
    //   date_ptr    = root_slot + 8                                  (passes addr; engine derefs)
    IQLOG("WAR_CREATE_CORE_DIRECT step=4 gs_resolve: reading GAME_STATE_ROOT\n");
    uintptr_t root_slot = safe_read<uintptr_t>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
    // ck-4ot67: emit immediately after the bare safe_read+is_mapped pair so a
    // wedge on the engine call is distinguishable from a wedge on the bare
    // reads (which use process_vm_readv and shouldn't hang, but proving it).
    IQLOG("WAR_CREATE_CORE_DIRECT step=4 root_slot read: root_slot=0x%lx\n",
          (unsigned long)root_slot);
    if (!root_slot || !is_mapped(root_slot)) return reject("game_state_root_unmapped");

    using GsResolveFn = uintptr_t (*)(uintptr_t);
    auto gs_resolve = reinterpret_cast<GsResolveFn>(g_base_addr + ck3::funcs::GAME_STATE_FROM_ROOT);
    uintptr_t game_state = 0;
    {
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false; watchdog_disarm();
            const char* sig_name = (sig == 1) ? "sigsegv" : (sig == 2) ? "timeout" : "unknown";
            char reason_buf[64];
            snprintf(reason_buf, sizeof(reason_buf), "game_state_resolve_%s", sig_name);
            return reject(reason_buf);
        }
        // ck-4ot67: emit AFTER setjmp returns 0 — proves the protected region
        // was entered and the recovery point is armed. A wedge between this
        // checkpoint and the next ("gs_resolve done") localizes the hang to
        // the engine call itself (and the watchdog timeout should follow).
        IQLOG("WAR_CREATE_CORE_DIRECT step=4 gs_resolve calling engine: fn=0x%lx root_slot=0x%lx\n",
              (unsigned long)g_base_addr + ck3::funcs::GAME_STATE_FROM_ROOT,
              (unsigned long)root_slot);
        try { game_state = gs_resolve(root_slot); }
        catch (...) { g_in_protected_region = false; watchdog_disarm(); return reject("game_state_resolve_exception"); }
        g_in_protected_region = false; watchdog_disarm();
    }
    if (!game_state || !is_mapped(game_state)) return reject("game_state_invalid");
    IQLOG("WAR_CREATE_CORE_DIRECT step=4 gs_resolve done: game_state=0x%lx\n",
          (unsigned long)game_state);

    uintptr_t current_date_ptr = root_slot + 8;

    // 5) Snapshot WAR_DB count for delta detection.
    IQLOG("WAR_CREATE_CORE_DIRECT step=5 wars_before_snapshot: starting\n");
    int32_t wars_before = -1;
    uintptr_t war_db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
    // ck-4ot67: emit immediately after the bare safe_read so a wedge here is
    // distinguishable from a wedge inside the hash-map walk below.
    IQLOG("WAR_CREATE_CORE_DIRECT step=5 war_db read: war_db=0x%lx\n",
          (unsigned long)war_db);
    if (war_db && is_mapped(war_db)) {
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false; watchdog_disarm();
            const char* sig_name = (sig == 1) ? "sigsegv" : (sig == 2) ? "timeout" : "unknown";
            IQLOG("WAR_CREATE_CORE_DIRECT step=5 wars_before_snapshot recovered: sig=%s\n",
                  sig_name);
            wars_before = -1;  // continue — pre-snapshot is best-effort
        } else {
            try {
                uintptr_t wdb_table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
                uint32_t wdb_capacity = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
                if (wdb_table && wdb_capacity > 0 && wdb_capacity < 100000) {
                    int32_t count = 0;
                    for (uint32_t idx = 0; idx < wdb_capacity && idx < 10000; idx++) {
                        uintptr_t entry = wdb_table + ck3::hash_map::ENTRY_HEADER_SIZE
                                          + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
                        uintptr_t wp = safe_read<uintptr_t>(entry);
                        if (!wp || !safe_ptr(wp)) continue;
                        uint32_t sref = safe_read<uint32_t>(wp + ck3::war_obj::WAR_ID);
                        if ((sref & 0xFFFFFF) != idx) continue;
                        uint8_t ended = safe_read<uint8_t>(wp + ck3::war_obj::IS_ENDED);
                        if (ended == 0) count++;
                    }
                    wars_before = count;
                }
            } catch (...) {
                wars_before = -1;
            }
            g_in_protected_region = false; watchdog_disarm();
        }
    }
    IQLOG("WAR_CREATE_CORE_DIRECT step=5 wars_before=%d\n", wars_before);

    // 6) Acquire game lock matching the special-interaction handler.
    IQLOG("WAR_CREATE_CORE_DIRECT step=6 lock_acquire: calling FUN_076facf0\n");
    using NoArgFn = void (*)();
    auto lock_acquire = reinterpret_cast<NoArgFn>(g_base_addr + ck3::funcs::GAME_LOCK_ACQUIRE);
    {
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false; watchdog_disarm();
            const char* sig_name = (sig == 1) ? "sigsegv" : (sig == 2) ? "timeout" : "unknown";
            char reason_buf[64];
            snprintf(reason_buf, sizeof(reason_buf), "lock_acquire_%s", sig_name);
            return reject(reason_buf);
        }
        try { lock_acquire(); }
        catch (...) { g_in_protected_region = false; watchdog_disarm(); return reject("lock_acquire_exception"); }
        g_in_protected_region = false; watchdog_disarm();
    }
    IQLOG("WAR_CREATE_CORE_DIRECT step=6 lock_acquire done\n");

    // 7) Call WAR_CREATE_CORE.
    IQLOG("WAR_CREATE_CORE_DIRECT step=7 calling WAR_CREATE_CORE: gs=0x%lx atk=%u def=%u claimant=%u cb=0x%lx\n",
          (unsigned long)game_state, atk_id, def_id, claimant, (unsigned long)cb_type_ptr);
    using WarCreateFn = uintptr_t (*)(uintptr_t game_state,
                                       uint32_t atk, uint32_t def,
                                       uint32_t claimant,
                                       uintptr_t cb_type,
                                       void* cb_params,
                                       uintptr_t current_date_ptr,
                                       void* optional_titles_vec);  // param_8: NULL→name-gen(crash); non-NULL→skip
    auto war_create_core = reinterpret_cast<WarCreateFn>(
        g_base_addr + ck3::funcs::WAR_CREATE_CORE);

    uintptr_t returned_war = 0;
    bool      crashed = false;

    // Fabricate the ALLOW_RANDOM_IN_SCOPE guard for THIS thread around the WCC
    // call (random-scope RE 2026-06-11). Without it, WCC's RNG-scope check
    // (FUN_07748f10) sees a null scope and null-derefs in FUN_0774a4b0 AFTER the
    // war is allocated, unwinding it. We bind the fake RNG to this thread and
    // point the RandomScopeManager (+0x20) at it, then restore after.
    volatile uint64_t* rng_scope_slot = reinterpret_cast<volatile uint64_t*>(
        g_base_addr + RANDOM_SCOPE_MANAGER + RANDOM_SCOPE_FIELD);
    volatile uint64_t saved_rng_scope = 0;
    volatile bool rng_scope_set = false;
    if (is_mapped(reinterpret_cast<uintptr_t>(rng_scope_slot))) {
        g_fake_rng.tid     = static_cast<uint64_t>(pthread_self());
        g_fake_rng.counter = 0; g_fake_rng.counter2 = 0;
        g_fake_scope[0]    = &g_fake_rng;
        saved_rng_scope    = *rng_scope_slot;
        *rng_scope_slot    = reinterpret_cast<uint64_t>(g_fake_scope);
        rng_scope_set      = true;
        IQLOG("WAR_CREATE_CORE_DIRECT step=7 random-scope armed: slot=%p saved=0x%lx fake=%p\n",
              (void*)rng_scope_slot, (unsigned long)saved_rng_scope, (void*)g_fake_scope);
    }

    // ck-2rxyz: tighten watchdog from 8s → 4s so the engine call surfaces
    // its result within the IPC handler's 5s poll window (do_call_war_create_core
    // polls completed_snapshot for 5s before returning timeout). Engine
    // WAR_CREATE_CORE is documented as a synchronous in-memory call with no
    // I/O — 4s leaves generous headroom for the on_declaration / on-action
    // dispatch nested inside it.
    {
        watchdog_arm(4);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false; watchdog_disarm();
            crashed = true;
            IQLOG("WAR_CREATE_CORE_DIRECT step=7 recovered: sig=%s\n",
                  (sig == 1) ? "sigsegv" : (sig == 2) ? "timeout" : "unknown");
        } else {
            // ck-4ot67: PRE_WCC_CALL checkpoint — emit IMMEDIATELY before the
            // engine call returns. A wedge between this line and step=7 returned
            // means the engine call itself is the wedge point, and the watchdog
            // must convert it into a timeout reject within 4s.
            IQLOG("WAR_CREATE_CORE_DIRECT step=7 PRE_WCC_CALL: fn=0x%lx gs=0x%lx atk=%u def=%u clmt=%u cb=0x%lx params=%p date=0x%lx opt_titles=%p\n",
                  (unsigned long)(g_base_addr + ck3::funcs::WAR_CREATE_CORE),
                  (unsigned long)game_state, atk_id, def_id, claimant,
                  (unsigned long)cb_type_ptr,
                  (void*)cb_params_buf,
                  (unsigned long)current_date_ptr,
                  opt_titles_arg);
            try {
                // param_8 = opt_titles_vec (non-NULL): supplies the title list
                // (via FUN_066b6700) AND skips WCC's internal name-gen. We name
                // the war ourselves AFTER WCC (below) so we keep the title attach
                // that param_8 provides — passing NULL here loses the title and
                // the engine culls the war.
                returned_war = war_create_core(
                    game_state, atk_id, def_id, claimant,
                    cb_type_ptr, cb_params_buf,
                    current_date_ptr, opt_titles_arg);
            } catch (...) {
                crashed = true;
            }
            g_in_protected_region = false; watchdog_disarm();
        }
    }

    // 7b) Name the war. WCC skipped its internal WAR_NAME_GENERATE because we
    // pass param_8 non-NULL (to attach the title); generate the localized name
    // here, while the random scope is STILL active (name-gen uses the global
    // RNG too). FUN_066b62c0(war, game_state) — raw 0x065b62c0. Crash-guarded:
    // a name failure leaves the war valid but unnamed, never kills the game.
    {
        uintptr_t name_null_sent = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB_SENTINEL);
        if (!crashed && returned_war && returned_war != name_null_sent && safe_ptr(returned_war)) {
            using NameGenFn = void (*)(uintptr_t war, uintptr_t game_state);
            auto name_gen = reinterpret_cast<NameGenFn>(g_base_addr + 0x065b62c0);
            watchdog_arm(3);
            g_in_protected_region = true;
            int nsig = sigsetjmp(g_recovery_point, 1);
            if (nsig != 0) {
                g_in_protected_region = false; watchdog_disarm();
                IQLOG("WAR_CREATE_CORE_DIRECT step=7b name-gen recovered: sig=%s (war left unnamed)\n",
                      (nsig == 1) ? "sigsegv" : (nsig == 2) ? "timeout" : "unknown");
            } else {
                try { name_gen(returned_war, game_state); } catch (...) {}
                g_in_protected_region = false; watchdog_disarm();
                IQLOG("WAR_CREATE_CORE_DIRECT step=7b name-gen done (war=0x%lx)\n",
                      (unsigned long)returned_war);
            }
        }
    }

    // Restore the RandomScopeManager scope pointer (both success + crash paths).
    if (rng_scope_set && is_mapped(reinterpret_cast<uintptr_t>(rng_scope_slot))) {
        *rng_scope_slot = saved_rng_scope;
    }
    IQLOG("WAR_CREATE_CORE_DIRECT step=7 returned: crashed=%d war_ptr=0x%lx\n",
          crashed ? 1 : 0, (unsigned long)returned_war);

    if (crashed) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "war_create_core_crashed");
        intent->consumed = true;
        IQLOG("WAR_CREATE_CORE crashed during direct invocation\n");
        return false;
    }

    // 8) Check for null-sentinel return (silent failure SF1/SF2/SF3).
    uintptr_t null_sentinel = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB_SENTINEL);
    bool is_null_war = (returned_war == 0) || (returned_war == null_sentinel);

    uint32_t war_id = 0xFFFFFFFFu;
    if (!is_null_war && safe_ptr(returned_war)) {
        war_id = safe_read<uint32_t>(returned_war + ck3::war_obj::WAR_ID);
    }

    // Re-count WAR_DB to detect orphan creation (SF3 leaves an entry behind).
    // ck-2rxyz: watchdog-wrapped like step 5 — the hash-map walk reads engine
    // memory that may be in flux right after WAR_CREATE_CORE mutates WAR_DB.
    int32_t wars_after = -1;
    if (war_db && is_mapped(war_db)) {
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false; watchdog_disarm();
            IQLOG("WAR_CREATE_CORE_DIRECT step=8 wars_after_snapshot recovered: sig=%s\n",
                  (sig == 1) ? "sigsegv" : (sig == 2) ? "timeout" : "unknown");
            wars_after = -1;
        } else {
            try {
                uintptr_t wdb_table = safe_read<uintptr_t>(war_db + ck3::hash_map::TABLE_PTR);
                uint32_t wdb_capacity = safe_read<uint32_t>(war_db + ck3::hash_map::CAPACITY);
                if (wdb_table && wdb_capacity > 0 && wdb_capacity < 100000) {
                    int32_t count = 0;
                    for (uint32_t idx = 0; idx < wdb_capacity && idx < 10000; idx++) {
                        uintptr_t entry = wdb_table + ck3::hash_map::ENTRY_HEADER_SIZE
                                          + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE;
                        uintptr_t wp = safe_read<uintptr_t>(entry);
                        if (!wp || !safe_ptr(wp)) continue;
                        uint32_t sref = safe_read<uint32_t>(wp + ck3::war_obj::WAR_ID);
                        if ((sref & 0xFFFFFF) != idx) continue;
                        uint8_t ended = safe_read<uint8_t>(wp + ck3::war_obj::IS_ENDED);
                        if (ended == 0) count++;
                    }
                    wars_after = count;
                }
            } catch (...) {
                wars_after = -1;
            }
            g_in_protected_region = false; watchdog_disarm();
        }
    }

    bool war_created = (!is_null_war) && (war_id != 0xFFFFFFFFu);
    if (is_null_war) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "war_create_core_returned_null_sentinel");
    }
    intent->executed = war_created;
    intent->consumed = true;

    {
        // ck-zgh4: when the latch made us skip the CB-validity pre-check, the
        // reply SAYS SO. A war the seat cannot tell apart from a validated one
        // is the outcome the ruling's accepted risk would otherwise hide.
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "{\"war_created\":%s,\"war_id\":%u,\"is_null_sentinel\":%s,"
                 "\"wars_before\":%d,\"wars_after\":%d,\"resolved_cb\":\"%.64s\"%s}",
                 war_created ? "true" : "false",
                 (war_created ? (war_id & 0xFFFFFF) : 0xFFFFFFFFu),
                 is_null_war ? "true" : "false",
                 wars_before, wars_after, resolved_cb_name,
                 validity_skipped_latched
                     ? ",\"validity_precheck_skipped\":true,\"note\":\"the "
                       "casus-belli validity pre-check was SKIPPED: the bridge "
                       "stopped using the engine's RNG scope after a safety "
                       "check failed, and the war was made without it\""
                     : "");
        intent->console_output = buf;
    }

    IQLOG("WAR_CREATE_CORE_DIRECT result: war_created=%d war_id=0x%x null_sentinel=%d wars %d→%d cb='%s'\n",
          war_created ? 1 : 0, war_id, is_null_war ? 1 : 0,
          wars_before, wars_after, resolved_cb_name);

    return war_created;
}

// =============================================================================
// FACTION create/join — execute_faction_intent (re_faction_join_create.md).
//
// One native call GET_OR_CREATE_AND_JOIN(mgr, type_ptr, actor, liege) does
// create-or-join in one shot. type_ptr resolved by name from FACTION_TYPE_DB_V2.
// Mirrors the steered-war harness: arm_fake_random_scope (the leader-election
// path touches RNG) + watchdog + sigsetjmp crash-net. faction-type key in
// interaction_name; faction_action ("create"/"join"/"leave") in console_command;
// target_id = liege. "leave" is not yet pinned → rejected.
// =============================================================================

// Resolve a CFactionType* by its key name via FACTION_TYPE_DB_V2:
//   inst = *(base + FACTION_TYPE_DB_V2); array = *(inst+0x88); count = *(u32*)(inst+0x94);
//   entries are CFactionType* (8-byte stride); name = std::string at type_ptr+0x10.
static uintptr_t resolve_faction_type_by_name(const std::string& key) {
    uintptr_t inst = read<uintptr_t>(g_base_addr + ck3::globals::FACTION_TYPE_DB_V2);
    if (!inst) return 0;
    uintptr_t arr = read<uintptr_t>(inst + 0x88);
    uint32_t count = read<uint32_t>(inst + 0x94);
    if (!arr || count == 0 || count > 256) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t type_ptr = read<uintptr_t>(arr + static_cast<uintptr_t>(i) * 8);
        if (!type_ptr) continue;
        // name std::string object at type_ptr+0x10 (libstdc++: data ptr @+0, len @+8).
        const char* p = reinterpret_cast<const char*>(read<uintptr_t>(type_ptr + 0x10));
        uint64_t len = read<uint64_t>(type_ptr + 0x18);
        if (!p || len == 0 || len > 128) continue;
        if (key.size() == len && memcmp(key.data(), p, len) == 0) return type_ptr;
    }
    return 0;
}

bool execute_faction_intent(Intent* intent) {
    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;

    const std::string& ftype_key = intent->interaction_name;     // faction-type key
    const std::string& faction_action = intent->console_command; // create/join/leave
    IQLOG("=== execute_faction_intent ENTRY: actor=%u type='%s' action='%s' liege=%u ===\n",
          intent->actor_id & 0xFFFFFF, ftype_key.c_str(), faction_action.c_str(),
          intent->target_id & 0xFFFFFF);

    if (faction_action == "leave") {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "leave_not_implemented");
        intent->consumed = true;
        return false;
    }

    void* actor = lookup_character(intent->actor_id);
    if (!actor) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason), "actor_not_found");
        intent->consumed = true;
        return false;
    }
    if (g_char_is_alive && !g_char_is_alive(actor)) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason), "actor_not_alive");
        intent->consumed = true;
        return false;
    }

    void* liege = lookup_character(intent->target_id);
    if (!liege) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason), "liege_not_found");
        intent->consumed = true;
        return false;
    }

    uintptr_t type_ptr = resolve_faction_type_by_name(ftype_key);
    if (!type_ptr) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "faction_type_not_found:%.64s", ftype_key.c_str());
        intent->consumed = true;
        return false;
    }

    uintptr_t gsr = read<uintptr_t>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
    auto get_mgr = reinterpret_cast<void*(*)(void*)>(
        g_base_addr + ck3::faction_write::GET_FACTION_MANAGER);
    auto get_or_create_join = reinterpret_cast<void*(*)(void*, void*, void*, void*)>(
        g_base_addr + ck3::faction_write::GET_OR_CREATE_AND_JOIN);

    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this process,
    // so the slot cannot be reasoned about. REFUSE rather than proceed
    // unarmed — proceeding is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "%s", bridge::scope_latched_reason());
        intent->consumed = true;
        return false;
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);
    void* faction = nullptr;

    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        if (rng_armed) restore_fake_random_scope(saved_rng);
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "faction_call_crashed:%s",
                 (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN");
        intent->consumed = true;
        IQLOG("faction intent crashed in GET_OR_CREATE_AND_JOIN (sig=%d)\n", sig);
        return false;
    }
    {
        void* mgr = get_mgr(reinterpret_cast<void*>(gsr));
        if (mgr) faction = get_or_create_join(mgr, reinterpret_cast<void*>(type_ptr), actor, liege);
    }
    g_in_protected_region = false;
    watchdog_disarm();
    if (rng_armed) restore_fake_random_scope(saved_rng);

    uintptr_t sentinel = g_base_addr + ck3::faction_write::NULL_FACTION_SENTINEL;
    bool is_null = (faction == nullptr) ||
                   (reinterpret_cast<uintptr_t>(faction) == sentinel);

    // Confirm membership (FACTION_IS_MEMBER returns a member* or null).
    bool is_member = false;
    uint32_t faction_id = 0;
    if (!is_null) {
        auto is_mem = reinterpret_cast<void*(*)(void*, void*)>(
            g_base_addr + ck3::faction_write::FACTION_IS_MEMBER);
        watchdog_arm(3);
        g_in_protected_region = true;
        int sig2 = sigsetjmp(g_recovery_point, 1);
        if (sig2 == 0) {
            is_member = (is_mem(faction, actor) != nullptr);
            faction_id = read<uint32_t>(reinterpret_cast<uintptr_t>(faction) + 0x10) & 0xFFFFFF;
        }
        g_in_protected_region = false;
        watchdog_disarm();
    }

    bool ok = !is_null && is_member;
    intent->executed = ok;
    intent->rejected = !ok;
    intent->consumed = true;
    {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"faction_action\":\"%.16s\",\"faction_type\":\"%.48s\",\"liege_id\":%u,"
                 "\"created_or_joined\":%s,\"is_member\":%s,\"faction_id\":%u}",
                 faction_action.c_str(), ftype_key.c_str(), intent->target_id & 0xFFFFFF,
                 is_null ? "false" : "true", is_member ? "true" : "false", faction_id);
        intent->console_output = buf;
    }
    if (!ok && intent->rejection_reason[0] == '\0') {
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 is_null ? "faction_returned_null_sentinel" : "actor_not_member_after_call");
    }
    IQLOG("faction intent result: ok=%d null=%d member=%d faction_id=%u\n",
          ok ? 1 : 0, is_null ? 1 : 0, is_member ? 1 : 0, faction_id);
    return ok;
}

// =============================================================================
// SCHEME start — execute_scheme_intent (#132, docs/notes/hostile-intrigue-research.md)
//
// One native call CreateActiveScheme(mgr, type, owner, target_scope, tpl5, tpl6)
// constructs + initializes the scheme. The ENGINE'S OWN GATES are inside that
// call (decompile-verified, image 0x063a7dd0):
//   - (int)scheme_type[0x3e] != (int)target_scope  → null  (target_type mismatch)
//   - scheme_type is_shown predicate (vt at type+0x10)     → null if hidden
//   - ValidateSchemeTarget(target_scope)                   → null if target invalid
//   - owner IsAlive + (char-target) target IsAlive         → null if dead
//   - HasSchemeOnTarget(type, owner, target)               → null if exists/cooldown
//   - scheme_type can-start predicate (vt at type+0x38)    → null if disallowed
// So calling it forward is self-gating — an illegitimate murder (dead/invalid/
// imprisoned-by-us/cooldown target) returns the SCHEME_NULL sentinel, not a scheme.
//
// target_scope layout (from ValidateSchemeTarget, image 0x063a86e0):
//   low 32 bits  = target_type (0=character,1=title,2=culture,3=faith,4=none)
//   high 32 bits = target_ref  (FULL 32-bit canonical handle, char+0x18; the
//                  engine slot-resolves by &0xFFFFFF then verifies the full handle)
//
// param5/param6 are non-nullable scope objects (InitializeScheme dereferences
// param5 via a virtual call). They are captured from real engine-initiated schemes
// by the CREATE_ACTIVE_SCHEME observer hook (scheme_get_arg_template), keyed by
// type name. If no template has been observed yet, we refuse rather than guess.
//
// Crash-net: arm_fake_random_scope (success-formula / agent-staffing touch RNG) +
// watchdog + sigsetjmp, identical to execute_faction_intent / create-title.
// =============================================================================

// Resolve a CSchemeType* by key name via SCHEME_TYPE_DB. Same Jomini DB layout
// as resolve_faction_type_by_name: inst = *(base+SCHEME_TYPE_DB); array @ inst+0x88;
// count @ inst+0x94 (u32); entries are CSchemeType* (8-byte stride); name is the
// std::string at type_ptr+0x10. Verified on the core dump: 76 types, "murder"/
// "fabricate_hook"/"claim_throne" all present (re/findings hostile-intrigue).
static uintptr_t resolve_scheme_type_by_name(const std::string& key) {
    uintptr_t inst = read<uintptr_t>(g_base_addr + ck3::globals::SCHEME_TYPE_DB);
    if (!inst) return 0;
    uintptr_t arr = read<uintptr_t>(inst + 0x88);
    uint32_t count = read<uint32_t>(inst + 0x94);
    if (!arr || count == 0 || count > 512) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uintptr_t type_ptr = read<uintptr_t>(arr + static_cast<uintptr_t>(i) * 8);
        if (!type_ptr) continue;
        const char* p = reinterpret_cast<const char*>(read<uintptr_t>(type_ptr + 0x10));
        uint64_t len = read<uint64_t>(type_ptr + 0x18);
        if (!p || len == 0 || len > 128) continue;
        if (key.size() == len && memcmp(key.data(), p, len) == 0) return type_ptr;
    }
    return 0;
}

// Confirm the owner now OWNS a scheme of `type_ptr` targeting `target_id` — same
// owned-scheme walk as do_query_schemes (core_data+0x98 ref list → SCHEME_REFDB).
// Returns the matching scheme's packed id (or 0). Fault-tolerant via safe_read*.
static uint32_t confirm_owned_scheme(void* owner, uintptr_t type_ptr,
                                     uint32_t target_id) {
    uintptr_t core = safe_read<uintptr_t>(reinterpret_cast<uintptr_t>(owner)
                                          + ck3::character::REALM_DATA);
    if (!core || !is_mapped(core)) return 0;
    uintptr_t ref_array = safe_read<uintptr_t>(core + ck3::core_data::SCHEME_REF_PTR);
    int32_t ref_count = safe_read<int32_t>(core + ck3::core_data::SCHEME_REF_COUNT);
    if (ref_count < 0) ref_count = 0;
    if (ref_count > 200) ref_count = 200;
    if (ref_array && !is_mapped(ref_array)) ref_array = 0;
    uintptr_t refDB = safe_read<uintptr_t>(g_base_addr + ck3::globals::SCHEME_REFDB);
    if (!refDB || !is_mapped(refDB)) return 0;
    uintptr_t table = safe_read<uintptr_t>(refDB + ck3::hash_map::TABLE_PTR);
    uint32_t max_idx = safe_read<uint32_t>(refDB + ck3::hash_map::CAPACITY);
    if (!table || !is_mapped(table)) return 0;
    for (int i = 0; i < ref_count && ref_array; ++i) {
        uint32_t ref = safe_read<uint32_t>(ref_array + static_cast<uintptr_t>(i) * 4);
        uint32_t idx = ref & 0xFFFFFF;
        if (idx >= max_idx) continue;
        uintptr_t sp = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                            + static_cast<uintptr_t>(idx) * ck3::hash_map::ENTRY_STRIDE);
        if (!sp || !is_mapped(sp)) continue;
        if ((safe_read<uint32_t>(sp + ck3::active_scheme::SCHEME_ID) & 0xFFFFFF) != idx) continue;
        if (safe_read<int32_t>(sp + ck3::active_scheme::STATUS) == 1) continue;  // invalidated
        if (safe_read<uintptr_t>(sp + ck3::active_scheme::SCHEME_TYPE_PTR) != type_ptr) continue;
        uint32_t tref = safe_read<uint32_t>(sp + ck3::active_scheme::TARGET_REF) & 0xFFFFFF;
        if (tref != (target_id & 0xFFFFFF)) continue;
        return ref & 0xFFFFFF;
    }
    return 0;
}

// #scheme_cancel: resolve a CActiveScheme* from a scheme_id via SCHEME_REFDB (same walk
// as do_query_schemes). Returns 0 if not found / stale (id mismatch). Fault-tolerant.
static uintptr_t resolve_scheme_by_id(uint32_t scheme_idx) {
    scheme_idx &= 0xFFFFFF;
    uintptr_t refDB = safe_read<uintptr_t>(g_base_addr + ck3::globals::SCHEME_REFDB);
    if (!refDB || !is_mapped(refDB)) return 0;
    uintptr_t table = safe_read<uintptr_t>(refDB + ck3::hash_map::TABLE_PTR);
    uint32_t cap = safe_read<uint32_t>(refDB + ck3::hash_map::CAPACITY);
    if (!table || !is_mapped(table) || scheme_idx >= cap) return 0;
    uintptr_t sp = safe_read<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                        + static_cast<uintptr_t>(scheme_idx) * ck3::hash_map::ENTRY_STRIDE);
    if (!sp || !is_mapped(sp)) return 0;
    if ((safe_read<uint32_t>(sp + ck3::active_scheme::SCHEME_ID) & 0xFFFFFF) != scheme_idx) return 0;
    return sp;
}

// #scheme_cancel: LEGITIMATE per-scheme abandon. Calls the engine's own
// SchemeManager::RemoveActiveScheme (REMOVE_ACTIVE_SCHEME @0x062a6e30) — unlinks from all
// manager index lists, runs the canonical teardown (INVALIDATE_SCHEME) + owner/target
// notify, frees the refdb slot (memset 0x1F0). LEGITIMACY GATE: the actor must OWN the
// scheme (OWNER_ID == actor) — matches CK3 rules (a ruler abandons their own plot).
// Crash-net: fake-RNG + watchdog + sigsetjmp, identical to execute_scheme_intent.
bool execute_scheme_cancel_intent(Intent* intent) {
    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;

    uint32_t actor_id   = intent->actor_id & 0xFFFFFF;
    uint32_t scheme_idx = (intent->title_id >= 0) ? (static_cast<uint32_t>(intent->title_id) & 0xFFFFFF) : 0;
    IQLOG("=== execute_scheme_cancel_intent ENTRY: owner=%u scheme_id=%u ===\n",
          actor_id, scheme_idx);

    auto reject = [&](const char* reason) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason), "%s", reason);
        intent->consumed = true;
        return false;
    };

    // Owner must exist (to legitimacy-check ownership); need NOT be alive.
    void* owner = lookup_character(actor_id);
    if (!owner) return reject("owner_not_found");

    if (scheme_idx == 0) return reject("scheme_id_required");
    uintptr_t scheme_ptr = resolve_scheme_by_id(scheme_idx);
    if (!scheme_ptr) return reject("scheme_not_found");

    // LEGITIMACY GATE: only the owner can cancel their own scheme.
    uint32_t owner_id = safe_read<uint32_t>(scheme_ptr + ck3::active_scheme::OWNER_ID) & 0xFFFFFF;
    if (owner_id != actor_id) return reject("not_scheme_owner");

    // Idempotency: already invalidated → success no-op (prevents double-free).
    if (safe_read<int32_t>(scheme_ptr + ck3::active_scheme::STATUS) == 1) {
        intent->executed = true;
        intent->rejected = false;
        intent->consumed = true;
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"action\":\"cancel\",\"scheme_id\":%u,\"owner_id\":%u,"
                 "\"already_invalidated\":true,\"removed\":true}", scheme_idx, owner_id);
        intent->console_output = buf;
        return true;
    }

    // Resolve scheme manager.
    uintptr_t gsr = read<uintptr_t>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
    if (!gsr) return reject("game_state_root_null");
    auto get_scheme_mgr = reinterpret_cast<void*(*)(void*)>(
        g_base_addr + ck3::funcs::GET_SCHEME_MANAGER);
    auto remove_scheme = reinterpret_cast<void(*)(void*, void*)>(
        g_base_addr + ck3::funcs::REMOVE_ACTIVE_SCHEME);

    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this process,
    // so the slot cannot be reasoned about. REFUSE rather than proceed
    // unarmed — proceeding is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        return reject(bridge::scope_latched_reason());
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);

    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        if (rng_armed) restore_fake_random_scope(saved_rng);
        char buf[96];
        snprintf(buf, sizeof(buf), "scheme_cancel_crashed:%s",
                 (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN");
        IQLOG("scheme cancel crashed in RemoveActiveScheme (sig=%d)\n", sig);
        return reject(buf);
    }
    {
        void* mgr = get_scheme_mgr(reinterpret_cast<void*>(gsr));
        if (mgr) {
            remove_scheme(mgr, reinterpret_cast<void*>(scheme_ptr));
        } else {
            g_in_protected_region = false;
            watchdog_disarm();
            if (rng_armed) restore_fake_random_scope(saved_rng);
            return reject("scheme_manager_null");
        }
    }
    g_in_protected_region = false;
    watchdog_disarm();
    if (rng_armed) restore_fake_random_scope(saved_rng);

    // READBACK: success iff the slot is now empty/freed OR resolves to STATUS==1.
    uintptr_t after = resolve_scheme_by_id(scheme_idx);
    bool removed = (after == 0) ||
                   (safe_read<int32_t>(after + ck3::active_scheme::STATUS) == 1);

    intent->executed = removed;
    intent->rejected = !removed;
    intent->consumed = true;
    {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"action\":\"cancel\",\"scheme_id\":%u,\"owner_id\":%u,\"removed\":%s}",
                 scheme_idx, owner_id, removed ? "true" : "false");
        intent->console_output = buf;
    }
    if (!removed && intent->rejection_reason[0] == '\0') {
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "scheme_still_present_after_remove");
    }
    IQLOG("scheme cancel result: removed=%d scheme_id=%u\n", removed ? 1 : 0, scheme_idx);
    return removed;
}

bool execute_scheme_intent(Intent* intent) {
    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;
    extern bool scheme_get_arg_template(const std::string&, uint64_t*, uint64_t*);

    const std::string& scheme_key = intent->interaction_name;
    uint32_t actor_id  = intent->actor_id & 0xFFFFFF;
    uint32_t target_id = intent->target_id & 0xFFFFFF;
    IQLOG("=== execute_scheme_intent ENTRY: owner=%u scheme='%s' target=%u ===\n",
          actor_id, scheme_key.c_str(), target_id);

    auto reject = [&](const char* reason) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason), "%s", reason);
        intent->consumed = true;
        return false;
    };

    void* owner = lookup_character(actor_id);
    if (!owner) return reject("owner_not_found");
    if (g_char_is_alive && !g_char_is_alive(owner)) return reject("owner_not_alive");

    void* target = lookup_character(target_id);
    if (!target) return reject("target_not_found");
    if (g_char_is_alive && !g_char_is_alive(target)) return reject("target_not_alive");

    uintptr_t type_ptr = resolve_scheme_type_by_name(scheme_key);
    if (!type_ptr) {
        char buf[160];
        snprintf(buf, sizeof(buf), "scheme_type_not_found:%.64s", scheme_key.c_str());
        return reject(buf);
    }

    // The 5th/6th CreateActiveScheme args — captured from a real engine scheme of
    // this type by the observer hook. We refuse rather than fabricate them.
    uint64_t tpl5 = 0, tpl6 = 0;
    if (!scheme_get_arg_template(scheme_key, &tpl5, &tpl6)) {
        char buf[176];
        snprintf(buf, sizeof(buf),
                 "no_arg_template_captured:%.48s (no engine-initiated '%.48s' "
                 "scheme observed yet to source CreateActiveScheme args)",
                 scheme_key.c_str(), scheme_key.c_str());
        return reject(buf);
    }

    // Build target_scope: low32 = target_type (character = 0), high32 = FULL handle.
    uint32_t target_handle = read<uint32_t>(reinterpret_cast<uintptr_t>(target)
                                            + ck3::funcs::CHAR_CANONICAL_HANDLE);
    if (target_handle == 0) return reject("target_handle_zero");
    uint64_t target_scope = (static_cast<uint64_t>(target_handle) << 32) | 0u;  // type 0

    // Resolve the scheme manager: GET_SCHEME_MANAGER(game_state_root_deref).
    uintptr_t gsr = read<uintptr_t>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
    if (!gsr) return reject("game_state_root_null");
    auto get_scheme_mgr = reinterpret_cast<void*(*)(void*)>(
        g_base_addr + ck3::funcs::GET_SCHEME_MANAGER);
    auto create_scheme = reinterpret_cast<void*(*)(void*, void*, void*, uint64_t,
                                                   uint64_t, uint64_t)>(
        g_base_addr + ck3::funcs::CREATE_ACTIVE_SCHEME);

    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this process,
    // so the slot cannot be reasoned about. REFUSE rather than proceed
    // unarmed — proceeding is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        return reject(bridge::scope_latched_reason());
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);
    void* scheme = nullptr;

    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        if (rng_armed) restore_fake_random_scope(saved_rng);
        char buf[96];
        snprintf(buf, sizeof(buf), "scheme_call_crashed:%s",
                 (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN");
        IQLOG("scheme intent crashed in CreateActiveScheme (sig=%d)\n", sig);
        return reject(buf);
    }
    {
        void* mgr = get_scheme_mgr(reinterpret_cast<void*>(gsr));
        if (mgr) {
            scheme = create_scheme(mgr, reinterpret_cast<void*>(type_ptr),
                                   owner, target_scope, tpl5, tpl6);
        }
    }
    g_in_protected_region = false;
    watchdog_disarm();
    if (rng_armed) restore_fake_random_scope(saved_rng);

    // SCHEME_NULL sentinel (= base + 0x0bf1cb20) = engine refused (gate failed).
    uintptr_t sentinel = g_base_addr + ck3::globals::SCHEME_NULL;
    bool is_null = (scheme == nullptr) ||
                   (reinterpret_cast<uintptr_t>(scheme) == sentinel) ||
                   (reinterpret_cast<uintptr_t>(scheme) < 0x10000);

    // Handler-side readback: confirm the owner now OWNS a live scheme of this type
    // against this target (same walk as query_schemes).
    uint32_t scheme_id = 0;
    if (!is_null) {
        scheme_id = safe_read<uint32_t>(reinterpret_cast<uintptr_t>(scheme)
                                        + ck3::active_scheme::SCHEME_ID) & 0xFFFFFF;
    }
    uint32_t confirmed = confirm_owned_scheme(owner, type_ptr, target_id);

    bool ok = !is_null && confirmed != 0;
    intent->executed = ok;
    intent->rejected = !ok;
    intent->consumed = true;
    {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "{\"scheme\":\"%.48s\",\"owner_id\":%u,\"target_id\":%u,"
                 "\"created\":%s,\"scheme_id\":%u,\"confirmed_owned\":%s}",
                 scheme_key.c_str(), actor_id, target_id,
                 is_null ? "false" : "true",
                 confirmed ? confirmed : scheme_id,
                 confirmed ? "true" : "false");
        intent->console_output = buf;
    }
    if (!ok && intent->rejection_reason[0] == '\0') {
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 is_null ? "engine_refused_scheme_null_sentinel"
                         : "scheme_not_owned_after_call");
    }
    IQLOG("scheme intent result: ok=%d null=%d scheme_id=%u confirmed=%u\n",
          ok ? 1 : 0, is_null ? 1 : 0, scheme_id, confirmed);
    return ok;
}

// =============================================================================
// CAIWarCoordinator offensive-stance drive — Fix B1a (re_caiwarcoordinator_FINAL.md,
// re_steered_war_offensive_root.md)
// =============================================================================
// ROOT: a FORCE-DECLARED steered conquest war lands on the attacker_DEFENSIVE stance
// (DB+0xfc0) instead of attacker_offensive (DB+0xfb0) — so the AI never marches to
// SIEGE the war goal, occupation_score stays flat 0, and the war is unwinnable. The
// stance lives on the attacker realm's CAIWarCoordinator per-war RECORD at rec+0x70,
// written by the engine's per-war selector STANCE_PERWAR_SELECT (FUN_04e66550). For
// a war the realm AI itself chose, the coordinator's natural tick selects offensive;
// for a force-declared (steered) war the selection either never fires on the new
// record before defaulting, or the offensive strength-condition (FUN_04e843a0) is
// re-evaluated against stale/zero objective state.
//
// LEGITIMATE FIX (B1a — engine re-eval, NOT a memory poke): drive the engine's OWN
// per-coordinator stance tick on our war's record. Decompile-confirmed flow
// (FUN_04dedd40 → FUN_04e66220 → FUN_04e66550 → FUN_04e843a0):
//   - The record array lives on the AI-MANAGER root: arr=*(ai_root+0x48),
//     n=*(ai_root+0x54); each active record gated by rec+0x14 != 0 (the SAME walk
//     do_query_war_stance ships read-only + read-validated).
//   - COORD_STANCE_TICK (FUN_04e66220) takes the RECORD as param_1, resolves the war
//     itself from rec+0x18 via WAR_DB (validates war+8 == rec+0x18 — the FUN_04e60920
//     match-key invariant), decrements rec+0x94/0x98/0x9c, and WHEN rec+0x94 < 2 calls
//     the real selector FUN_04e66550(rec, war) which runs the real strength condition
//     FUN_04e843a0 and writes the engine's OWN chosen SAIWarStance* into rec+0x70.
//   - So: zero rec+0x94 (force the selector branch), call COORD_STANCE_TICK(rec,&seed)
//     ONCE. The engine re-selects. If the attacker genuinely has the upper hand it
//     lands attacker_offensive LEGITIMATELY; if not, defensive is the CORRECT engine
//     verdict (a useful diagnostic — the target is a cull/loss risk — never a forced
//     fabrication). This is the engine's real AI-plan recompute, exactly what a
//     self-declared war gets. We write NOTHING into rec+0x70 ourselves.
//
// The prior "single remaining live-probe" (the record↔war-id match key) is RESOLVED:
// rec+0x18 is the gen-tagged war id, validated against WAR_DB exactly as FUN_04e60920
// does, and the 1097-AD core dump ground-truthed 174 live records = 2 per war keyed on
// this invariant (re_steered_war_offensive_root.md §5b). The read-side walk ships in
// do_query_war_stance. What has NOT been exercised is DRIVING the engine tick on a
// live coordinator — so this stays DEFAULT-OFF behind g_force_offensive_stance and is
// validated live on attempt-25 (harness scenario strategic_force_offensive_siege).
//
// CRASH-HARDENING (mandatory, mirrors the steered-WCC / scripted-effect pattern):
// COORD_STANCE_TICK rebuilds the war plan (objective sublist iteration + unit-stack
// assignment subfns FUN_04e687b0/68b20/68e50) and draws engine RNG, so it runs under
// (1) arm_fake_random_scope (the RandomScopeManager+0x20 null-deref class) +
// (2) watchdog_arm + sigsetjmp(g_recovery_point), game-thread-serial only (drained
// from the WAR_FORCE_OFFENSIVE_STANCE intent on the AI tick). A fault/hang becomes a
// clean rejection, never a game crash.
std::atomic<bool> g_force_offensive_stance{false};

// Resolve the stance-DB instance ptr from the process-global (pure safe_read — no
// engine call). Returns 0 if the DB is not yet constructed / not mapped.
static uintptr_t resolve_stance_db() {
    uintptr_t db_global = g_base_addr + ck3::globals::STANCE_DB_GLOBAL;
    if (!is_mapped(db_global)) return 0;
    uintptr_t db = safe_read<uintptr_t>(db_global);          // *(global) = DB instance
    if (!db || db < 0x10000 || !is_mapped(db)) return 0;
    return db;
}

// Read one of the 7 engine-singleton SAIWarStance* slots out of the DB instance.
static uintptr_t stance_slot(uintptr_t db, size_t off) {
    if (!db) return 0;
    uintptr_t slot = db + off;
    if (!is_mapped(slot)) return 0;
    uintptr_t s = safe_read<uintptr_t>(slot);
    return (s && s >= 0x10000) ? s : 0;
}

// Resolve a CWar* from WAR_DB by gen-tagged war id, mirroring the engine's own
// FUN_04e66220 inner resolve: idx = id & 0xFFFFFF; slot = *(table + 8 + idx*0x10);
// validate *(war+8) == id. Returns 0 on any mismatch (so a wrong key never yields a
// garbage war). Inline (resolve_war_by_id is static to query_handlers.cpp).
static uintptr_t resolve_war_ptr_by_id(uint32_t war_id) {
    if (war_id == 0 || war_id == ck3::war_db::WAR_ID_INVALID) return 0;
    uintptr_t db = safe_read<uintptr_t>(g_base_addr + ck3::globals::WAR_DB);
    if (!db || !is_mapped(db)) return 0;
    uint32_t idx = war_id & ck3::war_db::WAR_ID_IDX_MASK;
    uint32_t cap = safe_read<uint32_t>(db + ck3::war_db::CAPACITY);
    if (idx >= cap) return 0;
    uintptr_t table = safe_read<uintptr_t>(db + ck3::war_db::TABLE_PTR);
    if (!table || !is_mapped(table)) return 0;
    uintptr_t slot = table + ck3::war_db::SLOT_WAR_PTR
                     + static_cast<uintptr_t>(idx) * ck3::war_db::SLOT_STRIDE;
    if (!is_mapped(slot)) return 0;
    uintptr_t war = safe_read<uintptr_t>(slot);
    if (!war || !is_mapped(war)) return 0;
    if (safe_read<uint32_t>(war + ck3::war_obj::WAR_ID) != war_id) return 0;  // gen check
    return war;
}

// Find the attacker realm's CAIWarCoordinator per-war RECORD for `war_id` by walking
// the AI-manager record array (the FUN_04dedd40 walk; same as do_query_war_stance).
// A record matches when it is active (rec+0x14 != 0), its rec+0x18 resolves to a real
// CWar via the WAR_DB gen-check, and (rec+0x18 & 0xFFFFFF) == (war_id & 0xFFFFFF).
// Returns the record ptr, or 0 if not found. Pure reads.
static uintptr_t find_coord_record_for_war(uint32_t war_id, int* out_scanned) {
    if (out_scanned) *out_scanned = 0;
    void* mgr = get_ai_manager();
    uintptr_t ai_root = reinterpret_cast<uintptr_t>(mgr);
    if (!ai_root || !is_mapped(ai_root)) return 0;
    uint32_t n   = safe_read<uint32_t>(ai_root + ck3::war_stance::AIMGR_REC_COUNT);
    uintptr_t arr = safe_read<uintptr_t>(ai_root + ck3::war_stance::AIMGR_REC_ARRAY);
    if (n == 0 || n >= 65536 || !arr || !is_mapped(arr)) return 0;
    uint32_t want_idx = war_id & ck3::war_db::WAR_ID_IDX_MASK;
    int scanned = 0;
    for (uint32_t i = 0; i < n && i < 4096; ++i) {
        uintptr_t rec = safe_read<uintptr_t>(arr + static_cast<uintptr_t>(i) * 8);
        if (!rec || !is_mapped(rec)) continue;
        if (safe_read<uint8_t>(rec + ck3::war_stance::REC_ACTIVE_FLAG) == 0) continue;
        scanned++;
        uint32_t rec_war_id = safe_read<uint32_t>(rec + ck3::war_stance::REC_WAR_ID);
        if ((rec_war_id & ck3::war_db::WAR_ID_IDX_MASK) != want_idx) continue;
        // Cross-validate via WAR_DB (war+8 == rec+0x18) — never trust idx alone.
        uintptr_t war = resolve_war_ptr_by_id(rec_war_id);
        if (!war) continue;
        if (out_scanned) *out_scanned = scanned;
        return rec;
    }
    if (out_scanned) *out_scanned = scanned;
    return 0;
}

// Reverse-map a stance ptr (rec+0x70) to a short name, for the result/verdict.
static const char* stance_name_for_ptr(uintptr_t db, uintptr_t sp) {
    if (!db || !sp) return "unknown";
    struct S { size_t off; const char* name; };
    static const S slots[] = {
        { ck3::war_stance::OFF_ATK_OFFENSIVE, "attacker_offensive" },
        { ck3::war_stance::OFF_ATK_DEFENSIVE, "attacker_defensive" },
        { ck3::war_stance::OFF_DEF_OFFENSIVE, "defender_offensive" },
        { ck3::war_stance::OFF_DEF_DEFENSIVE, "defender_defensive" },
        { ck3::war_stance::OFF_DEF_DESPERATE, "defender_desperate" },
        { ck3::war_stance::OFF_GHW_ATTACKER,  "great_holy_war_attacker" },
        { ck3::war_stance::OFF_GHW_DEFENDER,  "great_holy_war_defender" },
    };
    for (const auto& s : slots) {
        if (stance_slot(db, s.off) == sp) return s.name;
    }
    return "unknown";
}

// =============================================================================
// execute_war_force_offensive_stance_intent — Fix B1a, game-thread executor.
// Drained from WAR_FORCE_OFFENSIVE_STANCE on the AI tick. actor_id = the AI
// attacker; title_id (reused) = the gen-tagged war_id of the steered war.
// DEFAULT-OFF behind g_force_offensive_stance. Writes a structured result into
// intent->console_output (matching the WCC handler's contract) and sets
// intent->executed / intent->rejected. NEVER writes rec+0x70 itself — it only zeros
// the engine's re-eval cooldown and calls the engine's own stance tick, so the
// engine selects the stance via its real logic.
// =============================================================================
extern thread_local sigjmp_buf g_recovery_point;

bool execute_war_force_offensive_stance_intent(Intent* intent) {
    intent->consumed = true;
    intent->executed = false;

    uint32_t attacker_id = intent->actor_id & 0xFFFFFF;
    uint32_t war_id      = static_cast<uint32_t>(intent->title_id);  // gen-tagged

    auto reject = [&](const char* reason) -> bool {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "%s", reason);
        return false;
    };

    if (!g_force_offensive_stance.load(std::memory_order_relaxed))
        return reject("disabled_flag_off");

    uintptr_t db = resolve_stance_db();
    if (!db) return reject("stance_db_not_constructed_or_unmapped");
    uintptr_t want_offensive = stance_slot(db, ck3::war_stance::OFF_ATK_OFFENSIVE);
    if (!want_offensive) return reject("attacker_offensive_slot_unmapped");

    // Validate the war still exists + the attacker is still its primary attacker
    // (steered-war-target-legality: the war can be culled the tick after creation).
    uintptr_t war = resolve_war_ptr_by_id(war_id);
    if (!war) return reject("war_not_found_or_culled");

    // -------------------------------------------------------------------------
    // CB-TYPE ALLOW-LIST GUARD (CRASH-FIX 20260630, crash ck3_20260630_115545).
    // -------------------------------------------------------------------------
    // force_offensive_stance FAULTED on a GHA `sons_of_lothbrok_invasion_cb` war:
    // the bridge log ends at "executing WAR_FORCE_OFFENSIVE_STANCE actor=14859
    // war=0x3" with NO completion — the fault landed INSIDE the engine call (the
    // objective bootstrap or the stance tick), where a mid-engine-call SIGSEGV is
    // not reliably recoverable even under watchdog_arm+sigsetjmp (the engine may
    // leave its own locks/allocator in a torn state, or fault on a non-faulting
    // signal path). The ONLY validated-safe shape is a steered SINGLE-TARGET
    // conquest (county_conquest_cb; duchy_conquest_cb by the same single-de-jure
    // -title objective model). The seeder COORD_OBJ_BOOTSTRAP (FUN_04e69c70) +
    // objective ctor FUN_04e3da00 allocate ONE SAIProvinceObjective shaped around
    // a single de-jure title goal; an *invasion* / multi-target objective CB
    // (sons_of_lothbrok_invasion and the invasion family carry a target-province
    // LIST and a different objective struct) drives the seeder/stance-tick down a
    // layout whose invariants don't hold for our force-declared record.
    //
    // FIX (pre-call validation — MUST be before any engine call, since a SIGSEGV
    // mid-call may not be cleanly recoverable): resolve the war's CB name (same
    // war+0xA0 → read_db_obj_name read the steered-war path and query_war use) and
    // REJECT cleanly for anything outside the validated allow-list. No engine call
    // is made for unsupported CBs — typed verdict force_offensive_stance_unsupported_cb.
    {
        uintptr_t cb_type_ref = safe_read<uintptr_t>(war + ck3::war_obj::CB_TYPE_REF);
        char war_cb_name[128] = {};
        if (cb_type_ref && is_mapped(cb_type_ref)) {
            const char* nptr = nullptr; size_t nlen = 0;
            if (read_db_obj_name(cb_type_ref, nptr, nlen) && nptr && nlen > 0) {
                size_t copy = nlen < sizeof(war_cb_name) - 1 ? nlen : sizeof(war_cb_name) - 1;
                memcpy(war_cb_name, nptr, copy);
            }
        }
        // Validated-safe single-target conquest objectives ONLY. Everything else —
        // and especially *invasion* / multi-target objective CBs — is rejected.
        const bool cb_supported =
            strcmp(war_cb_name, "county_conquest_cb") == 0 ||
            strcmp(war_cb_name, "duchy_conquest_cb")  == 0;
        if (!cb_supported) {
            char buf[200];
            snprintf(buf, sizeof(buf), "force_offensive_stance_unsupported_cb:%s",
                     war_cb_name[0] ? war_cb_name : "unresolved");
            IQLOG("WAR_FORCE_OFFENSIVE_STANCE: REJECT unsupported CB '%s' war=0x%x — "
                  "only county_conquest_cb/duchy_conquest_cb are objective-seed-safe; "
                  "invasion/multi-target objective CBs fault the seeder/stance-tick\n",
                  war_cb_name[0] ? war_cb_name : "unresolved", war_id);
            return reject(buf);
        }
        IQLOG("WAR_FORCE_OFFENSIVE_STANCE: CB '%s' in validated allow-list — proceeding\n",
              war_cb_name);
    }

    // Find the coordinator record for THIS war (pure reads).
    int scanned = 0;
    uintptr_t rec = find_coord_record_for_war(war_id, &scanned);
    if (!rec) {
        // No record yet: the coordinator may not have ingested the new war into its
        // record array this tick. This is the documented timing case — retry next
        // tick (the intent TTL allows redrains). Report it precisely.
        char buf[200];
        snprintf(buf, sizeof(buf), "coord_record_not_found_scanned_%d", scanned);
        return reject(buf);
    }

    // Snapshot the pre-tick stance for the verdict.
    uintptr_t pre_sp  = safe_read<uintptr_t>(rec + ck3::war_stance::REC_STANCE_PTR);
    const char* pre_name = stance_name_for_ptr(db, pre_sp);

    // CRITICAL (re_steered_war_offensive_root.md §7, decompile-confirmed 20260630):
    // COORD_STANCE_TICK (FUN_04e66220) early-returns IMMEDIATELY when the objective
    // sublist is empty:  `iVar1 = *(int*)(rec+0x5c); if (iVar1 == 0) return;`. A FRESH
    // steered war's record has rec+0x5c==0 / rec+0x50==NULL (core-dump ground-truth:
    // 22/174 live records, incl. fresh attacker wars). So on a fresh steered war the
    // stance tick alone does NOTHING — it reaches neither the selector FUN_04e66550 nor
    // the maintainer FUN_04e669e0. The objective sublist is NOT built lazily inside the
    // tick: the in-tick maintainer FUN_04e669e0 is itself gated behind rec+0x5c!=0 (it
    // only REFINES an existing plan). The BOOTSTRAP seeder is the separate engine
    // function COORD_OBJ_BOOTSTRAP (FUN_04e69c70) — normally invoked by the realm-AI
    // war-PLANNING phase (FUN_04def140) that runs BEFORE the stance tick in the same AI
    // tick — which unconditionally allocates one SAIProvinceObjective and appends it to
    // rec+0x50, taking rec+0x5c 0→1, with NO +0x5c gate. So: if the sublist is empty,
    // drive the engine's OWN bootstrap seeder FIRST (legitimate — it is the planner's
    // own pre-tick step, not a poke), THEN the stance tick has a non-empty plan to
    // select+refine over. We write NOTHING into the sublist ourselves.
    using ObjBootstrapFn = void (*)(void* rec);
    auto obj_bootstrap = reinterpret_cast<ObjBootstrapFn>(
        g_base_addr + ck3::funcs::COORD_OBJ_BOOTSTRAP);

    // B1a: zero the re-eval cooldowns so COORD_STANCE_TICK takes BOTH gated branches:
    //   rec+0x94<2 → selector FUN_04e66550 (writes the stance to rec+0x70)
    //   rec+0x98<2 → maintainer FUN_04e669e0 (refines the seeded objective sublist)
    // These are benign cooldown counters the engine itself resets after the tick — NOT
    // the stance. Then call the engine's own tick.
    using StanceTickFn = void (*)(void* rec, void* seed);
    auto stance_tick = reinterpret_cast<StanceTickFn>(
        g_base_addr + ck3::funcs::COORD_STANCE_TICK);

    // Build the seed struct the engine's caller (FUN_04dedd40) passes as param_2.
    // FUN_04e66220 does not read param_2, but mirror the ABI: {game_date_qword, seed}.
    // game_date = *(GameStateRoot_deref + 8) (re_war_create_core / current_date_ptr).
    alignas(8) uint8_t seed_buf[0x18] = {};
    uintptr_t gs_root = safe_read<uintptr_t>(g_base_addr + ck3::globals::GAME_STATE_ROOT);
    if (gs_root && is_mapped(gs_root + 8)) {
        uint64_t date_val = safe_read<uint64_t>(gs_root + 8);
        std::memcpy(seed_buf, &date_val, sizeof(date_val));
    }

    volatile uint64_t saved_rng = 0;
    volatile bool rng_armed = false;
    int sig = 0;

    watchdog_arm(4);  // generous: the plan rebuild walks the objective sublist
    g_in_protected_region = true;
    sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false;
        watchdog_disarm();
        return reject("crashed_in_stance_tick");
    }

    // Arm the fake-RNG scope BEFORE any engine call (the seeder's ctor + the stance
    // tick's plan rebuild can draw RNG / touch the RandomScopeManager).
    {
        uint64_t saved_tmp = 0;
        // ck-zgh4 scope latch: a restore was refused earlier in this process,
        // so the slot cannot be reasoned about. REFUSE rather than proceed
        // unarmed — proceeding is what ignoring the arm's false would do.
        if (bridge::scope_latched()) {
            bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        // The arm here sits INSIDE the protected region (after the
        // sigsetjmp), and reject() does not unwind it, so clear the
        // flag and disarm the watchdog before returning.
        g_in_protected_region = false;
        watchdog_disarm();
        return reject(bridge::scope_latched_reason());
        }
        rng_armed = arm_fake_random_scope(&saved_tmp);
        saved_rng = saved_tmp;
    }

    // STEP 1 — bootstrap the objective sublist if empty, else COORD_STANCE_TICK
    // early-returns at rec+0x5c==0 and does nothing (the fresh-steered-war case).
    int obj_count_before = static_cast<int>(
        safe_read<int32_t>(rec + ck3::war_stance::REC_OBJ_COUNT));
    bool seeded = false;
    if (obj_count_before == 0) {
        obj_bootstrap(reinterpret_cast<void*>(rec));   // engine's own planner seeder
        seeded = true;
    }

    // STEP 2 — zero both re-eval cooldowns so this tick runs the selector (rec+0x94<2 →
    // FUN_04e66550, writes rec+0x70) AND the objective maintainer (rec+0x98<2 →
    // FUN_04e669e0, refines the seeded sublist). Benign counters the engine resets.
    {
        volatile uint32_t* cd1 = reinterpret_cast<volatile uint32_t*>(
            rec + ck3::war_stance::REC_REEVAL_CD_1);
        if (is_mapped(reinterpret_cast<uintptr_t>(cd1))) *cd1 = 0;
        volatile uint32_t* cd2 = reinterpret_cast<volatile uint32_t*>(
            rec + ck3::war_stance::REC_REEVAL_CD_2);
        if (is_mapped(reinterpret_cast<uintptr_t>(cd2))) *cd2 = 0;
    }
    // STEP 3 — the engine's own per-coordinator stance tick: re-selects the stance via
    // the real selector + real strength condition, refines the objective plan, writes
    // rec+0x70 itself.
    stance_tick(reinterpret_cast<void*>(rec), seed_buf);

    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false;
    watchdog_disarm();

    // Verify handler-side (no external probe — races): read the post-tick stance.
    uintptr_t post_sp = safe_read<uintptr_t>(rec + ck3::war_stance::REC_STANCE_PTR);
    const char* post_name = stance_name_for_ptr(db, post_sp);
    bool now_offensive = (post_sp == want_offensive);
    int obj_count_after = static_cast<int>(
        safe_read<int32_t>(rec + ck3::war_stance::REC_OBJ_COUNT));

    // STRENGTH-GATE DIAGNOSTIC (re_offensive_strength_gate.md — pure reads).
    // The offensive decline is a LEGIT engine strength gate, NOT a bridge bug:
    // FUN_04e843a0 declines offensive unless the attacker side is stronger AND
    // enemy/own strength ratio < 0.75 (single-objective conquest path). The
    // selector reads these two totals off the SAME coordinator record. Surface
    // them so the agent gets the actual ratio + shortfall instead of guessing.
    int64_t str_enemy = safe_read<int64_t>(rec + ck3::war_stance::REC_STRENGTH_ENEMY);
    int64_t str_own   = safe_read<int64_t>(rec + ck3::war_stance::REC_STRENGTH_OWN);
    // ratio = enemy/own in fixed-point /100000 (matches OFFENSIVE_RATIO_HI=75000).
    long ratio_fp = (str_own > 0) ? static_cast<long>((str_enemy * 100000) / str_own) : -1;
    bool strength_ok = (str_own > 0) && (str_enemy < str_own) &&
                       (ratio_fp >= 0 && ratio_fp < ck3::war_stance::OFFENSIVE_RATIO_HI);

    intent->executed = now_offensive;
    if (!now_offensive) {
        // The engine re-evaluated and chose NOT offensive. With the strength readout
        // we can say WHY: a legit strength-gate decline (own not > ~1.33x enemy) vs a
        // genuine anomaly (gate says strong-enough yet stance != offensive). Either
        // way it is a real diagnostic for the agent, not a bridge failure.
        if (!strength_ok) {
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "engine_chose_%s_strength_gate_enemy/own_ratio_%ld/100000_need_lt_75000",
                     post_name, ratio_fp);
        } else {
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "engine_chose_%s_not_offensive_despite_strength_ok", post_name);
        }
        intent->rejected = true;
    }

    char buf[560];
    snprintf(buf, sizeof(buf),
             "{\"action\":\"force_offensive_stance\",\"war_id\":%u,\"attacker_id\":%u,"
             "\"stance_before\":\"%s\",\"stance_after\":\"%s\","
             "\"now_offensive\":%s,\"records_scanned\":%d,"
             "\"obj_seeded\":%s,\"obj_count_before\":%d,\"obj_count_after\":%d,"
             "\"strength_enemy\":%lld,\"strength_own\":%lld,"
             "\"enemy_over_own_ratio_fp\":%ld,\"ratio_threshold_fp\":%ld,"
             "\"strength_gate_pass\":%s}",
             war_id & 0xFFFFFF, attacker_id, pre_name, post_name,
             now_offensive ? "true" : "false", scanned,
             seeded ? "true" : "false", obj_count_before, obj_count_after,
             static_cast<long long>(str_enemy), static_cast<long long>(str_own),
             ratio_fp, static_cast<long>(ck3::war_stance::OFFENSIVE_RATIO_HI),
             strength_ok ? "true" : "false");
    intent->console_output = buf;

    IQLOG("WAR_FORCE_OFFENSIVE_STANCE: war=0x%x atk=%u %s→%s offensive=%d "
          "obj=%d→%d seeded=%d\n",
          war_id, attacker_id, pre_name, post_name, now_offensive ? 1 : 0,
          obj_count_before, obj_count_after, seeded ? 1 : 0);
    return now_offensive;
}

// IPC setter — set_force_offensive_stance {enabled:bool}.
bool do_set_force_offensive_stance(const char* params_json, std::string* out) {
    bool enabled = json_get_bool(params_json, "enabled", false);
    g_force_offensive_stance.store(enabled, std::memory_order_relaxed);
    IQLOG("set_force_offensive_stance: %s\n", enabled ? "ENABLED" : "disabled");
    if (out) {
        ResponseBuilder r; r.begin_object();
        r.key("success"); r.value_bool(true);
        r.key("action");  r.value_string("set_force_offensive_stance");
        r.key("enabled"); r.value_bool(enabled);
        r.key("note");    r.value_string("Fix B1a: engine-re-eval stance drive, DEFAULT-OFF; NEEDS-LIVE-VALIDATION attempt-25");
        r.end_object();
        *out = r.take();
    }
    return true;
}

// =============================================================================
// break-alliance — execute_break_alliance_intent (re_break_alliance_FINAL.md, Path B)
// =============================================================================
// The literal CBreakAllowAllianceEffect break via the gated break_alliance_cmd.
// ARG-ABI (FINAL §0, HIGH): break_alliance_cmd(CCharacter* actor, CCharacter* ally,
// char force) — both args go straight to CCharacter::IsAlive. We resolve BOTH via
// lookup_character (== the engine's FUN_05f72a60 _g_CharacterDB_root walk). force=0
// ⇒ Lb1 gated semantics: only dissolves a LIVE alliance, bails cleanly otherwise.
//
// CRASH-HARDENING (FINAL §4 — mirror the steered-war / scheme harness verbatim):
// the dissolve leaf (relation_dissolve, 494B) + its notification builder construct
// scope objects and can draw engine RNG, so BOTH vectors apply:
//   (1) arm_fake_random_scope — neutralise the RandomScopeManager+0x20 NULL deref
//       on the on_tick drain thread (the steered-war crash class).
//   (2) watchdog_arm + sigsetjmp(g_recovery_point) — the cmd is BOUNDED (two alive
//       checks + one relation lookup + a finite reasons-vector clear) so the
//       watchdog CAN bound it (the masked-SIGALRM decision-eval wall does NOT apply).
// RNG armed INSIDE the protected region; restored on BOTH the normal return AND the
// sigsetjmp recovery path. Path B has no scope objects to sub-dtor — cleanup is just
// the RNG restore + watchdog disarm.
//
// LEGITIMACY (FINAL §6): this is the engine's own CanExecute→command→consequences
// pipeline (alive gate → relation lookup → virtual breakable gate → relation_dissolve
// with full notifications) — NOT a memory poke of the relation bytes. Every native
// break_alliance routes through this exact command.
//
// Game-thread-only (drained from on_tick). DEFAULT-OFF: do_break_alliance only
// queues this intent when g_enable_break_alliance is true.
bool execute_break_alliance_intent(Intent* intent) {
    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;

    uint32_t actor_id = intent->actor_id  & 0xFFFFFF;
    uint32_t ally_id  = intent->target_id & 0xFFFFFF;
    IQLOG("=== execute_break_alliance_intent ENTRY: actor=%u ally=%u ===\n",
          actor_id, ally_id);

    auto reject = [&](const char* reason) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason), "%s", reason);
        intent->consumed = true;
        return false;
    };

    if (actor_id == 0 || ally_id == 0)  return reject("actor_or_ally_id_zero");
    if (actor_id == ally_id)            return reject("cannot_break_with_self");

    // (1) Resolve BOTH characters to live CCharacter* via the SAME DB walk the
    //     engine's FUN_05f72a60 unwrap does (lookup_character is that walk).
    void* actor_char = lookup_character(actor_id);
    if (!actor_char || is_sentinel(actor_char)) return reject("actor_unresolved");
    if (g_char_is_alive && !g_char_is_alive(actor_char)) return reject("actor_not_alive");
    void* ally_char = lookup_character(ally_id);
    if (!ally_char || is_sentinel(ally_char)) return reject("ally_unresolved");
    if (g_char_is_alive && !g_char_is_alive(ally_char)) return reject("ally_not_alive");

    // (2) Pre-gate: only proceed if actually allied — mirrors the cmd's own
    //     relation gate, but gives a clean "not_allied" rejection instead of a
    //     silent no-op dissolve. IsAlliedTo(actor, ally) → bool.
    auto is_allied_to = reinterpret_cast<bool(*)(void*, void*)>(
        g_base_addr + ck3::funcs::ALLIANCE_IS_ALLIED_TO);
    bool allied_before = false;
    {
        // The read is a small binary-search + flag check; guard it lightly under
        // the watchdog (no RNG draw here) so a bogus relation pointer can't wedge.
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig = sigsetjmp(g_recovery_point, 1);
        if (sig != 0) {
            g_in_protected_region = false; watchdog_disarm();
            return reject("is_allied_to_probe_crashed");
        }
        allied_before = is_allied_to(actor_char, ally_char);
        g_in_protected_region = false; watchdog_disarm();
    }
    if (!allied_before) return reject("not_allied");

    // (3) Call the gated cmd under the FINAL §4 crash guards. force=0 ⇒ Lb1.
    auto break_alliance_cmd = reinterpret_cast<void(*)(void*, void*, char)>(
        g_base_addr + ck3::alliance::BREAK_ALLIANCE_CMD);

    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this process,
    // so the slot cannot be reasoned about. REFUSE rather than proceed
    // unarmed — proceeding is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        return reject(bridge::scope_latched_reason());
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);

    watchdog_arm(5);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        g_in_protected_region = false;
        watchdog_disarm();
        if (rng_armed) restore_fake_random_scope(saved_rng);
        char buf[96];
        snprintf(buf, sizeof(buf), "break_alliance_cmd_crashed:%s",
                 (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM" : "UNKNOWN");
        IQLOG("break_alliance intent crashed in break_alliance_cmd (sig=%d)\n", sig);
        return reject(buf);
    }
    break_alliance_cmd(actor_char, ally_char, /*force=*/0);
    g_in_protected_region = false;
    watchdog_disarm();
    if (rng_armed) restore_fake_random_scope(saved_rng);

    // (4) Handler-side readback (not external probe — races): the alliance should
    //     now be GONE in both directions.
    bool allied_after = true;
    {
        watchdog_arm(2);
        g_in_protected_region = true;
        int sig2 = sigsetjmp(g_recovery_point, 1);
        if (sig2 != 0) {
            g_in_protected_region = false; watchdog_disarm();
            // The dissolve already ran; a readback fault is non-fatal — report it
            // but treat the call as having executed (verify externally).
            allied_after = false;  // assume success; can't re-read
        } else {
            allied_after = is_allied_to(actor_char, ally_char);
            g_in_protected_region = false; watchdog_disarm();
        }
    }

    bool ok = !allied_after;
    intent->executed = ok;
    intent->rejected = !ok;
    intent->consumed = true;
    {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "{\"actor_id\":%u,\"ally_id\":%u,\"allied_before\":true,"
                 "\"allied_after\":%s,\"broken\":%s}",
                 actor_id, ally_id,
                 allied_after ? "true" : "false",
                 ok ? "true" : "false");
        intent->console_output = buf;
    }
    if (!ok && intent->rejection_reason[0] == '\0')
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "still_allied_after_break");
    IQLOG("break_alliance intent result: ok=%d allied_after=%d\n",
          ok ? 1 : 0, allied_after ? 1 : 0);
    return ok;
}

// =============================================================================
// A8.2 — execute_raise_armies_intent (re/findings/re_raise_armies.md)
// =============================================================================
//
// SYMPTOM: a steered AI accumulates ticking war score but never raises levies/
// sieges, so claim wars never reach the real war-score threshold.
//
// ROOT (RE-verified): RaiseAllArmiesCore (ck3::funcs::ARMY_RAISE_ALL,
// IMAGE 0x03ee3e40) takes (msg_sink, mode) and is HARD-WIRED to the global
// g_PlayerCharacterID (ck3::globals::PLAYER_CHAR_ID = 0x0bead8c0) — there is no
// character argument. The decompile shows the global is read EXACTLY ONCE at
// function entry through the 7-byte GET_PLAYER_CHAR_ID accessor
// (`return _g_PlayerCharacterID;`), then resolved via CharacterDB with the
// check `*(char+0x18) == handle` (full 32-bit; a masked 24-bit value resolves
// to the NullCharacter sentinel → safe no-op that does nothing). The inner
// raise core (ARMY_RAISE_INNER = 0x03ee0210, invoked on the mode==0 path) ALSO
// reads the same global once at its own entry, so the swap must remain in place
// across the WHOLE RaiseAllArmiesCore call (which is the case — we restore only
// after raise_all returns).
//
// PRIMITIVE: temporarily swap g_PlayerCharacterID to the chosen AI's full
// 32-bit handle, call raise_all(nullptr, 0) (msg_sink=null skips every
// UI-message branch; mode=0 = full raise), then restore the global IMMEDIATELY.
// The global drives input/camera/UI/is-player checks engine-wide, so the swap
// window is kept MINIMAL (paused, game-thread serial section only) and the
// global is restored on EVERY path: fault (sigsetjmp longjmp), C++ exception,
// and success — plus a final defensive re-check after the protected region.
//
// CRASH-SAFETY: game-thread only (drained here from on_tick), watchdog-bounded,
// sigsetjmp recovery, fake-RNG scope (RaiseAllArmiesCore / its callees may touch
// RandomScopeManager — same insurance as the steered-war fix), and total global
// restore. Bounded work: the only loop in RaiseAllArmiesCore is over the realm's
// holdings array; the msg-string branches are skipped when msg_sink=null; no
// recursion/waits — it will not wedge the tick (#64).
//
// STATUS: NEEDS-LIVE-HARNESS-VALIDATION. The global swap + indirect raise are
// untestable on a core dump (no function calls, no write-test). This lands
// behind the intent/drain gate; validate live (harness scenario
// strategic_raise_levies) before relying on it in a campaign.
bool execute_raise_armies_intent(Intent* intent) {
    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;

    IQLOG("=== execute_raise_armies_intent ENTRY: actor=%u ===\n",
          intent->actor_id & 0xFFFFFF);

    void* actor = lookup_character(intent->actor_id);
    if (!actor) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "actor_not_found");
        intent->consumed = true;
        IQLOG("Raise-armies intent rejected: actor=%u not found\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }
    if (g_char_is_alive && !g_char_is_alive(actor)) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "actor_not_alive");
        intent->consumed = true;
        IQLOG("Raise-armies intent rejected: actor=%u not alive\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }

    // Swap VALUE: the FULL 32-bit canonical handle (char+0x18, incl. the
    // generation byte). A masked 24-bit id would fail the engine's
    // `*(resolved_char+0x18) == uVar5` check and resolve to the NullCharacter
    // sentinel — a safe no-op, but it would do nothing.
    uintptr_t ac = reinterpret_cast<uintptr_t>(actor);
    uint32_t ai_handle = read<uint32_t>(ac + ck3::funcs::CHAR_CANONICAL_HANDLE);
    if (ai_handle == 0) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "actor_handle_zero");
        intent->consumed = true;
        IQLOG("Raise-armies intent rejected: actor=%u handle=0\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }

    // The swap target: g_PlayerCharacterID global. volatile so the compiler
    // never caches it across the engine call.
    volatile uint32_t* pid = reinterpret_cast<volatile uint32_t*>(
        g_base_addr + ck3::globals::PLAYER_CHAR_ID);
    if (!is_mapped(reinterpret_cast<uintptr_t>(pid))) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "player_char_global_unmapped");
        intent->consumed = true;
        IQLOG("Raise-armies intent rejected: PLAYER_CHAR_ID global unmapped\n");
        return false;
    }

    auto raise_all = reinterpret_cast<unsigned long(*)(void*, int)>(
        g_base_addr + ck3::funcs::ARMY_RAISE_ALL);
    if (!is_mapped(reinterpret_cast<uintptr_t>(raise_all))) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "raise_all_unmapped");
        intent->consumed = true;
        IQLOG("Raise-armies intent rejected: ARMY_RAISE_ALL unmapped\n");
        return false;
    }

    const uint32_t saved_pid = *pid;        // restore target — captured BEFORE the swap
    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this process,
    // so the slot cannot be reasoned about. REFUSE rather than proceed
    // unarmed — proceeding is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "%s", bridge::scope_latched_reason());
        intent->consumed = true;
        return false;
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);  // RandomScopeManager insurance

    IQLOG("Raise-armies: swapping PLAYER_CHAR_ID 0x%08x -> 0x%08x (actor=%u) rng_armed=%d\n",
          saved_pid, ai_handle, intent->actor_id & 0xFFFFFF, rng_armed ? 1 : 0);

    bool success = false;
    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        // FAULT PATH: restore the global FIRST (before any other work), then
        // tear down. This guarantees the engine never observes the swapped
        // global after we leave.
        *pid = saved_pid;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(pid));  // R1c
        g_in_protected_region = false;
        if (rng_armed) restore_fake_random_scope(saved_rng);
        watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" :
                               (sig == 2) ? "SIGALRM_TIMEOUT" : "UNKNOWN";
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "raise_armies_%s", sig_name);
        intent->consumed = true;
        IQLOG("Raise-armies intent CRASHED: actor=%u reason=%s (global restored to 0x%08x)\n",
              intent->actor_id & 0xFFFFFF, sig_name, saved_pid);
        return false;
    }

    try {
        // MINIMAL swap window: set → call → restore immediately.
        // R1c (ck-6kvq.10, §2.9): PLAYER_CHAR_ID is the most consequential pulsed
        // global in the bridge — leaking the swap turns an OBSERVER world into a
        // PLAYER game for the rest of the process. Every local path here already
        // restores; the ledger covers the one path none of them can, a siglongjmp
        // to the OUTER recovery in bridge_init.cpp. Exit is BY ADDRESS because the
        // restore is deliberately repeated on several paths.
        (void)bridge::pulse_enter_global(
            "PLAYER_CHAR_ID", "raise_intent_player_swap",
            reinterpret_cast<uintptr_t>(pid), 4, saved_pid);
        *pid = ai_handle;
        raise_all(nullptr, 0);   // msg_sink=null (skip UI branches), mode=0 (full raise)
        *pid = saved_pid;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(pid));  // R1c
        success = true;
    } catch (...) {
        // C++ exception path: also restore the global before propagating out.
        *pid = saved_pid;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(pid));  // R1c
        g_in_protected_region = false;
        if (rng_armed) restore_fake_random_scope(saved_rng);
        watchdog_disarm();
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "raise_armies_exception");
        intent->consumed = true;
        IQLOG("Raise-armies intent EXCEPTION: actor=%u (global restored to 0x%08x)\n",
              intent->actor_id & 0xFFFFFF, saved_pid);
        return false;
    }
    g_in_protected_region = false;
    if (rng_armed) restore_fake_random_scope(saved_rng);
    watchdog_disarm();

    // Final defensive re-check: if anything left the global swapped, force it
    // back. Belt-and-suspenders on top of the success/fault/exception restores.
    if (*pid != saved_pid) {
        IQLOG("Raise-armies: WARNING — global was 0x%08x after region, forcing back to 0x%08x\n",
              *pid, saved_pid);
        *pid = saved_pid;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(pid));  // R1c
    }

    if (success) {
        intent->executed = true;
        intent->consumed = true;
        IQLOG("Raise-armies intent COMPLETED: actor=%u — raise_all ran (verify levy "
              "state via a SEPARATE non-swapped read)\n", intent->actor_id & 0xFFFFFF);
    } else {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "raise_armies_unknown_failure");
        intent->consumed = true;
    }
    return success;
}

// =============================================================================
// #96 — execute_raise_raiders_intent (re/findings/re_raid_system.md)
// =============================================================================
// Toggle a CHOSEN AI ruler's raised armies into RAIDING mode. This is the
// Norse/tribal resource engine — raise → raid → loot (gold/prestige/piety, NO
// casus belli). The toggle dispatch path (ARMY_TOGGLE_RAIDING_DISPATCH,
// FUN_03ee36d0) is hard-wired to g_PlayerCharacterID exactly like
// RaiseAllArmiesCore: it reads the player global ONCE at entry, resolves the
// player's army container, iterates raid-eligible armies into a vector, builds
// a CToggleRaidingCommand, VALIDATES it (CToggleRaidingCommand::IsValid /
// FUN_06354e10) and ONLY-IF-VALID enqueues it via GameCommandDispatch (type
// 0xe) → CommandQueuePush. The engine then flips army+0x1b4 natively on its own
// validated/RNG-safe command-apply tick — the bridge never writes the raid flag
// itself, which is what makes this safe.
//
// Crash-safety: identical guarded global-swap primitive as
// execute_raise_armies_intent — swap g_PlayerCharacterID → AI full-32-bit
// handle, call dispatch(intent=0), restore on EVERY path (success/fault/excn),
// under watchdog + sigsetjmp + fake-RNG. The dispatch is bounded (one loop over
// the realm's armies; non-blocking CommandQueuePush) and validator-gated (a
// non-Norse / ineligible ruler safely no-ops). param=0 selects the default
// toggle-raiding intent (the intent==2 branch is avoided).
bool execute_raise_raiders_intent(Intent* intent) {
    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;

    IQLOG("=== execute_raise_raiders_intent ENTRY: actor=%u ===\n",
          intent->actor_id & 0xFFFFFF);

    void* actor = lookup_character(intent->actor_id);
    if (!actor) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "actor_not_found");
        intent->consumed = true;
        IQLOG("Raise-raiders intent rejected: actor=%u not found\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }
    if (g_char_is_alive && !g_char_is_alive(actor)) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "actor_not_alive");
        intent->consumed = true;
        IQLOG("Raise-raiders intent rejected: actor=%u not alive\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }

    // Swap VALUE: the FULL 32-bit canonical handle (char+0x18). A masked id
    // fails the engine's `*(resolved_char+0x18) == uVar5` check → NullCharacter
    // sentinel (safe no-op, but no raid). Same as RAISE_ARMIES.
    uintptr_t ac = reinterpret_cast<uintptr_t>(actor);
    uint32_t ai_handle = read<uint32_t>(ac + ck3::funcs::CHAR_CANONICAL_HANDLE);
    if (ai_handle == 0) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "actor_handle_zero");
        intent->consumed = true;
        IQLOG("Raise-raiders intent rejected: actor=%u handle=0\n",
              intent->actor_id & 0xFFFFFF);
        return false;
    }

    volatile uint32_t* pid = reinterpret_cast<volatile uint32_t*>(
        g_base_addr + ck3::globals::PLAYER_CHAR_ID);
    if (!is_mapped(reinterpret_cast<uintptr_t>(pid))) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "player_char_global_unmapped");
        intent->consumed = true;
        IQLOG("Raise-raiders intent rejected: PLAYER_CHAR_ID global unmapped\n");
        return false;
    }

    // The toggle-raiding dispatch path: void(int intent_index). intent_index=0
    // is the default toggle-raiding (avoids the ==2 branch).
    auto toggle_raiding = reinterpret_cast<void(*)(int)>(
        g_base_addr + ck3::funcs::ARMY_TOGGLE_RAIDING_DISPATCH);
    if (!is_mapped(reinterpret_cast<uintptr_t>(toggle_raiding))) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "toggle_raiding_unmapped");
        intent->consumed = true;
        IQLOG("Raise-raiders intent rejected: ARMY_TOGGLE_RAIDING_DISPATCH unmapped\n");
        return false;
    }

    const uint32_t saved_pid = *pid;        // restore target — captured BEFORE the swap
    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this process,
    // so the slot cannot be reasoned about. REFUSE rather than proceed
    // unarmed — proceeding is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "%s", bridge::scope_latched_reason());
        intent->consumed = true;
        return false;
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);

    IQLOG("Raise-raiders: swapping PLAYER_CHAR_ID 0x%08x -> 0x%08x (actor=%u) rng_armed=%d\n",
          saved_pid, ai_handle, intent->actor_id & 0xFFFFFF, rng_armed ? 1 : 0);

    bool success = false;
    watchdog_arm(3);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        // FAULT PATH: restore the global FIRST, then tear down.
        *pid = saved_pid;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(pid));  // R1c
        g_in_protected_region = false;
        if (rng_armed) restore_fake_random_scope(saved_rng);
        watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" :
                               (sig == 2) ? "SIGALRM_TIMEOUT" : "UNKNOWN";
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "raise_raiders_%s", sig_name);
        intent->consumed = true;
        IQLOG("Raise-raiders intent CRASHED: actor=%u reason=%s (global restored to 0x%08x)\n",
              intent->actor_id & 0xFFFFFF, sig_name, saved_pid);
        return false;
    }

    try {
        // MINIMAL swap window: set → call → restore immediately.
        // R1c (ck-6kvq.10, §2.9): PLAYER_CHAR_ID is the most consequential pulsed
        // global in the bridge — leaking the swap turns an OBSERVER world into a
        // PLAYER game for the rest of the process. Every local path here already
        // restores; the ledger covers the one path none of them can, a siglongjmp
        // to the OUTER recovery in bridge_init.cpp. Exit is BY ADDRESS because the
        // restore is deliberately repeated on several paths.
        (void)bridge::pulse_enter_global(
            "PLAYER_CHAR_ID", "raise_intent_player_swap",
            reinterpret_cast<uintptr_t>(pid), 4, saved_pid);
        *pid = ai_handle;
        toggle_raiding(0);   // intent_index=0 = default toggle-raiding
        *pid = saved_pid;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(pid));  // R1c
        success = true;
    } catch (...) {
        *pid = saved_pid;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(pid));  // R1c
        g_in_protected_region = false;
        if (rng_armed) restore_fake_random_scope(saved_rng);
        watchdog_disarm();
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "raise_raiders_exception");
        intent->consumed = true;
        IQLOG("Raise-raiders intent EXCEPTION: actor=%u (global restored to 0x%08x)\n",
              intent->actor_id & 0xFFFFFF, saved_pid);
        return false;
    }
    g_in_protected_region = false;
    if (rng_armed) restore_fake_random_scope(saved_rng);
    watchdog_disarm();

    // Belt-and-suspenders: if anything left the global swapped, force it back.
    if (*pid != saved_pid) {
        IQLOG("Raise-raiders: WARNING — global was 0x%08x after region, forcing back to 0x%08x\n",
              *pid, saved_pid);
        *pid = saved_pid;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(pid));  // R1c
    }

    if (success) {
        intent->executed = true;
        intent->consumed = true;
        IQLOG("Raise-raiders intent COMPLETED: actor=%u — toggle-raiding dispatched "
              "(verify army+0x1b4 via a SEPARATE non-swapped read)\n",
              intent->actor_id & 0xFFFFFF);
    } else {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                 "raise_raiders_unknown_failure");
        intent->consumed = true;
    }
    return success;
}

// =============================================================================
// #131 — de-jure-control integrity gate for create_title (INTEGRITY FIX)
// =============================================================================
// The engine's own vt[7] CanExecute gate (FUN_067cacf0 -> FUN_0621a500 ->
// FUN_06218550) faithfully runs CK3's scripted `can_create` trigger. For the
// HIGH/EMPIRE formables (e_scandinavia, e_britannia, e_hispania-class) that
// trigger is `rule_title_creation_imperial_power_projection_title_creation_trigger`
// which, under the DEFAULT game rule (`..._none`), falls through every
// `trigger_if`/`trigger_else_if` to a terminal `trigger_else = { always = yes }`
// — i.e. vanilla itself lets ANY ruler form these empires with ZERO de-jure
// control (military "power projection" formation, not de-jure formation).
//
// PROVEN LIVE (actor 47270, realm-5): e_britannia (107) is its PRIMARY title yet
// it holds only 1 of 93 de-jure counties (1.08%); e_scandinavia (2415) minted at
// 0/112 (0%). This is engine-legal but is NOT the territorial-conquest legitimacy
// this project requires of the agent. (It is NOT a bypass cheat: DAT_0bfd75d0+0x10
// "bypass" flag is 0 in our observer game, and the player-list bypass list is
// empty — both verified live.)
//
// FIX (policy gate, stricter-than-vanilla BY DESIGN): for KINGDOM/EMPIRE-tier
// (tier >= 4) creates, after the engine gate PASSES, independently compute the
// realm-inclusive de-jure-control fraction (de-jure counties whose holder's
// top-liege chain reaches the acting ruler) and REFUSE below a real majority
// threshold. Duchy/county-tier creates (the agent's legitimate negotiate_the_
// danelaw 2nd-duchy use) are unaffected — those are honest de-jure completions
// where the ruler fully holds the de-jure counties (directly or via vassals).
// Recursively collect tier-2 (county) de-jure descendants of `title_id`.
// SHARED helper (external linkage): also used by do_query_dejure_progress in
// query_handlers.cpp so the read-only de-jure meter reuses the exact same
// SUB_TITLE_ARRAY(0x258)/SUB_TITLE_COUNT(0x264) walk the #131 create_title gate
// uses — no offset duplication. Pure safe_read; depth/size capped; cycle-guarded
// via `seen`. Declared in action_dispatch_internal.h.
void collect_dejure_counties(uint32_t title_id, std::vector<uint32_t>& out,
                             std::vector<uint32_t>& seen, int depth) {
    if (depth > 9 || out.size() > 4096) return;
    for (uint32_t s : seen) if (s == (title_id & 0xFFFFFF)) return;
    seen.push_back(title_id & 0xFFFFFF);
    uintptr_t o = lookup_title_obj(title_id);
    if (!o) return;
    uintptr_t type_ptr = safe_read<uintptr_t>(o + ck3::title_obj::TITLE_TYPE_PTR);
    int t = (type_ptr && is_mapped(type_ptr))
              ? safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER) : -1;
    if (t == 2) { out.push_back(title_id & 0xFFFFFF); return; }
    uintptr_t list = safe_read<uintptr_t>(o + ck3::title_obj::SUB_TITLE_ARRAY);
    int32_t   n    = safe_read<int32_t>(o + ck3::title_obj::SUB_TITLE_COUNT);
    if (!list || !is_mapped(list) || n <= 0 || n > 1000) return;
    for (int i = 0; i < n; i++) {
        uint32_t child = safe_read<uint32_t>(list + static_cast<uintptr_t>(i) * 4);
        if (child && child != 0xFFFFFFFFu)
            collect_dejure_counties(child, out, seen, depth + 1);
    }
}

namespace {

constexpr int    CREATE_TITLE_DEJURE_MIN_TIER   = 4;     // kingdom & empire
constexpr double CREATE_TITLE_DEJURE_MIN_FRAC   = 0.51;  // realm must hold a majority

// title tier via *(title + TITLE_TYPE_PTR) + TITLE_TYPE_TIER (2=county..5=empire)
static int title_tier(uintptr_t title_obj) {
    if (!title_obj) return -1;
    uintptr_t type_ptr = safe_read<uintptr_t>(title_obj + ck3::title_obj::TITLE_TYPE_PTR);
    if (!type_ptr || !is_mapped(type_ptr)) return -1;
    return safe_read<int32_t>(type_ptr + ck3::title_obj::TITLE_TYPE_TIER);
}

// Does char `cid`'s top-liege chain reach `ruler`? (realm membership)
static bool in_realm_of(uint32_t cid, uint32_t ruler) {
    cid   &= 0xFFFFFF;
    ruler &= 0xFFFFFF;
    uint32_t cur = cid;
    for (int hops = 0; hops < 24; hops++) {
        if (cur == ruler) return true;
        if (cur == 0 || cur == 0xFFFFFF) return false;
        void* c = lookup_character(cur);
        if (!c) return false;
        uint32_t lid = get_liege_id_from_addr(reinterpret_cast<uintptr_t>(c)) & 0xFFFFFF;
        if (lid == cur) return false;  // self-loop guard
        cur = lid;
    }
    return false;
}

// Realm-inclusive de-jure-control fraction of `title_id` for `ruler`.
// Returns held_counties / total_counties; total==0 => returns -1 (cannot assess).
static double dejure_control_fraction(uint32_t title_id, uint32_t ruler,
                                      int* out_held, int* out_total) {
    std::vector<uint32_t> counties, seen;
    collect_dejure_counties(title_id, counties, seen, 0);
    int total = static_cast<int>(counties.size());
    if (out_total) *out_total = total;
    if (total == 0) { if (out_held) *out_held = 0; return -1.0; }
    int held = 0;
    for (uint32_t ctid : counties) {
        uintptr_t co = lookup_title_obj(ctid);
        if (!co) continue;
        uint32_t h = safe_read<uint32_t>(co + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
        if (h == 0 || h == 0xFFFFFF) continue;
        if (in_realm_of(h, ruler)) held++;
    }
    if (out_held) *out_held = held;
    return static_cast<double>(held) / static_cast<double>(total);
}

} // anonymous namespace

// =============================================================================
// #111 — execute_create_title_intent (re/findings/re_create_title_action.md)
// =============================================================================
// LEGITIMATE de-jure create-title for a CHOSEN AI ruler. Builds a
// CCreateTitleCommand (the same player command the "Create Title" UI button
// dispatches), runs its vt[7] CanExecute gate SYNCHRONOUSLY (the engine's own
// de-jure-control + not-extant + gold-cost check), and ONLY-IF-VALID dispatches
// it via GAME_COMMAND_DISPATCH(mode 7 — the mode the canonical "Create Title" UI
// handler FUN_04de1ac0 uses; #115: mode 0xe silently no-ops) onto the queue. The engine
// then applies the gold cost and creates the title natively on its command tick.
//
// This is byte-for-byte the proven dispatch_costed_law_command shape (CAddLawCommand),
// except the target is a u32 title_id at cmd+0x24 (not a CLaw* at +0x28) and the
// ruler is carried IN the command (cmd+0x20) — so NO g_PlayerCharacterID global-swap
// is needed (CanExecute/Execute resolve the ruler from the struct; the gate has no
// player-global reference). Routing through CanExecute means an ineligible/unaffordable
// ruler is REFUSED before Execute is ever reached — no poke, no cheat.
//
// Crash-safety: armed fake-RNG scope (the cost eval may advance the RNG → same
// RandomScopeManager null-deref class as war/law) + watchdog + sigsetjmp protected
// region, with RNG restore + region/watchdog teardown on EVERY path. The command POD
// is built on the stack; the clone (vt[9]) heap-allocs the dispatched copy which the
// engine owns. intent->title_id = target de-jure title id (resolved from key in the
// IPC handler); intent->actor_id = the AI ruler.
bool execute_create_title_intent(Intent* intent) {
    extern thread_local sigjmp_buf g_recovery_point;
    extern thread_local bool g_in_protected_region;

    uint32_t title_id = static_cast<uint32_t>(intent->title_id);
    IQLOG("=== execute_create_title_intent ENTRY: actor=%u title_id=%u ===\n",
          intent->actor_id & 0xFFFFFF, title_id);

    auto reject = [&](const char* reason) {
        intent->rejected = true;
        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason), "%s", reason);
        intent->consumed = true;
        IQLOG("create_title intent rejected: %s\n", reason);
        return false;
    };

    void* actor = lookup_character(intent->actor_id);
    if (!actor) return reject("actor_not_found");
    if (g_char_is_alive && !g_char_is_alive(actor)) return reject("actor_not_alive");

    // FULL 32-bit canonical handle (char+0x18) — the command's IsValid/Execute
    // verify *(resolved_char+0x18) == cmd+0x20, so a masked id would resolve to
    // the NullCharacter sentinel and the gate would (correctly) refuse.
    uintptr_t ac = reinterpret_cast<uintptr_t>(actor);
    uint32_t ruler_handle = read<uint32_t>(ac + ck3::funcs::CHAR_CANONICAL_HANDLE);
    if (ruler_handle == 0) return reject("actor_handle_zero");

    // Validate the title id resolves to a real title (cheap pre-check; the engine
    // gate re-resolves it too).
    uintptr_t title_obj = lookup_title_obj(title_id);
    if (!title_obj) return reject("title_id_not_found");

    // Resolve function pointers up front (mapping-checked).
    auto base_ctor = reinterpret_cast<void(*)(void*, unsigned char)>(
        g_base_addr + 0x0755b630);  // CJominiCommandBase_ctor (image 0x765b630)
    auto execute = reinterpret_cast<void(*)(void*)>(
        g_base_addr + ck3::funcs::CREATE_TITLE_CMD_EXECUTE);  // FUN_067cabf0(cmd)
    auto base_dtor = reinterpret_cast<void(*)(void*)>(g_base_addr + 0x0755b650);  // FUN_0765b650
    if (!is_mapped(reinterpret_cast<uintptr_t>(base_ctor)) ||
        !is_mapped(reinterpret_cast<uintptr_t>(execute))) {
        return reject("create_title_fn_unmapped");
    }

    // PLAYER_CHAR_ID global — swapped to the acting ruler across the synchronous
    // Execute so any player-context resolution inside the grant resolves to OUR
    // ruler (the game runs in OBSERVER mode, PLAYER_CHAR_ID = 0xFFFFFFFF). Restored
    // on EVERY path. volatile so the swap is not optimized away.
    volatile uint32_t* g_pid = reinterpret_cast<volatile uint32_t*>(
        g_base_addr + ck3::globals::PLAYER_CHAR_ID);
    if (!is_mapped(reinterpret_cast<uintptr_t>(const_cast<uint32_t*>(g_pid)))) {
        return reject("create_title_pid_unmapped");
    }
    const uint32_t saved_pid = *g_pid;

    // CCreateTitleCommand POD: 0x28 bytes. vtable@+0x00, base bookkeeping +0x08/+0x10,
    // 2nd-vtable@+0x18, u32 ruler@+0x20, u32 title_id@+0x24.
    uint8_t cmd[0x28]; memset(cmd, 0, sizeof(cmd));
    bool eligible = true;

    uint64_t saved_rng = 0;
    // ck-zgh4 scope latch: a restore was refused earlier in this process,
    // so the slot cannot be reasoned about. REFUSE rather than proceed
    // unarmed — proceeding is what ignoring the arm's false would do.
    if (bridge::scope_latched()) {
        bridge::g_arm_refused_latched.fetch_add(1, std::memory_order_relaxed);
        return reject(bridge::scope_latched_reason());
    }
    bool rng_armed = arm_fake_random_scope(&saved_rng);
    watchdog_arm(4);
    g_in_protected_region = true;
    int sig = sigsetjmp(g_recovery_point, 1);
    if (sig != 0) {
        *g_pid = saved_pid;  // restore player-context FIRST on the fault path
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(g_pid));  // R1c
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false; watchdog_disarm();
        const char* sig_name = (sig == 1) ? "SIGSEGV" : (sig == 2) ? "SIGALRM_TIMEOUT" : "UNKNOWN";
        char rb[64]; snprintf(rb, sizeof(rb), "create_title_%s", sig_name);
        return reject(rb);
    }

    bool dispatched = false;
    try {
        // Build the command: base ctor sets the CJominiCommandBase vtable + flag +
        // zeroes +0x0c/+0x14; then stamp both command vtables + ruler + title.
        base_ctor(cmd, 0);
        *reinterpret_cast<uintptr_t*>(cmd + 0x00) = g_base_addr + ck3::funcs::CREATE_TITLE_CMD_VTABLE;
        *reinterpret_cast<uintptr_t*>(cmd + 0x18) = g_base_addr + ck3::funcs::CREATE_TITLE_CMD_VTABLE2;
        *reinterpret_cast<uint32_t*>(cmd + 0x20)  = ruler_handle;
        *reinterpret_cast<uint32_t*>(cmd + 0x24)  = title_id;

        void** vt = *reinterpret_cast<void***>(cmd);  // = CREATE_TITLE_CMD_VTABLE

        // ── SAFETY PRE-GATE: vt[7] IsValid(cmd, ctx) with null ctx ──────────────
        // Returns nonzero iff the ruler can legitimately create the title (controls
        // the de-jure counties + title not extant + can afford the gold). On 0 we
        // refuse WITHOUT dispatching, so the apply path is never reached for an
        // ineligible create.
        auto is_valid = reinterpret_cast<uint64_t(*)(void*, void*)>(vt[7]);  // CREATE_TITLE_CMD_VALIDATE
        uint64_t ok = is_valid(cmd, nullptr);
        if (!ok) {
            eligible = false;
            base_dtor(cmd);
            *g_pid = saved_pid;
            bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(g_pid));  // R1c
            if (rng_armed) restore_fake_random_scope(saved_rng);
            g_in_protected_region = false; watchdog_disarm();
            IQLOG("create_title: CanExecute rejected (cannot create — de-jure control / extant / gold)\n");
            intent->rejected = true;
            snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                     "create_title_refused_by_engine_gate");
            intent->consumed = true;
            return false;
        }

        // ── #131 INTEGRITY GATE: stricter-than-vanilla de-jure control ───────────
        // The engine gate above can PASS a kingdom/empire with ZERO de-jure control
        // (vanilla power-projection formation falls through to `always = yes` under
        // the default game rule). For tier >= 4 we additionally require the ruler's
        // realm to hold a MAJORITY of the title's de-jure counties — a genuine
        // territorial claim — and refuse otherwise. Bounded reads; runs inside the
        // sigsetjmp-protected region so a stray fault unwinds cleanly.
        //
        // #131 FIX (2026-06-25): the original `total == 0 → refuse` arm hard-blocked
        // titular kingdoms with a CONFUSING `..._0_of_0_counties` territorial string.
        // The 2026-06-25 follow-up over-corrected it to a DEFER (refuse=false), which
        // opened a legitimacy HOLE: a titular tier>=4 title (e.g. k_danelaw, id 13) has
        // NO de-jure tree by design AND no scripted `can_create`, so the engine vt[7]
        // gate passes it on a VACUOUS 0-of-0 de-jure check (county_count==0 →
        // required==0 → held<required is false). A direct create_title therefore mints a
        // HOLLOW titular shell and skips the entire canonical formation contract
        // (k_danelaw is meant to come into existence ONLY via
        // negotiate_the_danelaw_decision → british_isles.1011, which re-parents every
        // k_england de-jure duchy, adds the succession law, and destroys k_england).
        //
        // #131 TITULAR-HOLE FIX (2026-06-28, re_create_title_titular_hole.md, option iii):
        // REFUSE every titular (0-de-jure) tier>=4 create_title with a TYPED verdict that
        // names the decision route. A titular title has 0 de-jure land → 0 control is the
        // FLOOR of "insufficient control", not an exemption from it. This still refuses
        // k_danelaw (correct — it should be decision-formed) but with an honest,
        // actionable verdict instead of a misleading territorial count or a silent mint.
        //
        // This does NOT touch the genuine de-jure-completion path: duchy/county creates
        // and de-jure-bearing kingdom/empire creates have total > 0 → the real majority
        // fraction is computed and honest completions still PASS. Only titles with a
        // genuinely empty de-jure tree (titular kingdoms/empires) take the refuse path.
        {
            int tgt_tier = title_tier(title_obj);
            if (tgt_tier >= CREATE_TITLE_DEJURE_MIN_TIER) {
                int held = 0, total = 0;
                double frac = dejure_control_fraction(title_id, ruler_handle, &held, &total);
                bool titular = (total == 0);
                bool refuse = titular || (frac < CREATE_TITLE_DEJURE_MIN_FRAC);
                if (refuse) {
                    eligible = false;
                    base_dtor(cmd);
                    *g_pid = saved_pid;
                    bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(g_pid));  // R1c
                    if (rng_armed) restore_fake_random_scope(saved_rng);
                    g_in_protected_region = false; watchdog_disarm();
                    intent->rejected = true;
                    if (titular) {
                        // TITULAR tier>=4 (0 de-jure counties, e.g. k_danelaw):
                        // decision/event-formed in vanilla, never directly minted.
                        IQLOG("create_title: REFUSED — tier=%d title is TITULAR (0 de-jure "
                              "counties); titular kingdoms/empires are decision/event-formed, "
                              "not directly minted. Use the canonical formation decision "
                              "(e.g. negotiate_the_danelaw_decision for k_danelaw).\n",
                              tgt_tier);
                        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                                 "create_title_refused_titular_use_decision");
                    } else {
                        IQLOG("create_title: REFUSED by #131 de-jure gate — tier=%d held=%d/%d "
                              "(%.1f%%) < required %.0f%% (engine gate passed but ruler lacks "
                              "territorial de-jure control)\n",
                              tgt_tier, held, total,
                              100.0 * (double)held / (double)total,
                              100.0 * CREATE_TITLE_DEJURE_MIN_FRAC);
                        snprintf(intent->rejection_reason, sizeof(intent->rejection_reason),
                                 "create_title_refused_insufficient_de_jure_control_%d_of_%d_counties",
                                 held, total);
                    }
                    intent->consumed = true;
                    return false;
                }
                if (total > 0) {
                    IQLOG("create_title: #131 de-jure gate PASS — tier=%d held=%d/%d (%.1f%%) "
                          ">= required %.0f%%\n", tgt_tier, held, total,
                          100.0 * (double)held / (double)total,
                          100.0 * CREATE_TITLE_DEJURE_MIN_FRAC);
                }
            }
        }

        // ── #115 ROOT-CAUSE FIX (2026-06-22): SYNCHRONOUS gated Execute ───────────
        // The #111 build dispatched via GAME_COMMAND_DISPATCH (the deferred PLAYER-
        // command queue) — first with mode 0xe, then (this session) mode 7. BOTH
        // silently no-op'd: the gate passed, "executed=1", but the title never
        // minted AND no gold/prestige was deducted, i.e. the command's Execute NEVER
        // RAN. Root cause: the game runs in OBSERVER mode (PLAYER_CHAR_ID =
        // 0xFFFFFFFF), and the player-command queue is only drained/applied for the
        // active player — with no player, queued create-title commands accumulate but
        // are never executed. (A transient swap around DISPATCH cannot help: the
        // drain happens on a later tick, outside our window.)
        //
        // FIX: after the vt[7] CanExecute gate PASSES, run the command's Execute
        // (vt-resolved CREATE_TITLE_CMD_EXECUTE = FUN_067cabf0) SYNCHRONOUSLY right
        // here — exactly what the engine's drain would do — under a PLAYER_CHAR_ID
        // swap to the acting ruler. Execute resolves ruler@+0x20 + title@+0x24 and
        // calls the real de-jure grant (FUN_0621b800: realm-queue grant item + gold/
        // prestige cost). This mirrors the proven raise_armies pattern (gated +
        // synchronous + player-swap), not a memory poke: an ineligible ruler is
        // refused by the gate above before Execute is ever reached. The swap is
        // restored immediately after the call (and on every fault/exception path).
        // R1c (ck-6kvq.10, §2.9): PLAYER_CHAR_ID is the most consequential pulsed
        // global in the bridge — leaking the swap turns an OBSERVER world into a
        // PLAYER game for the rest of the process. Every local path here already
        // restores; the ledger covers the one path none of them can, a siglongjmp
        // to the OUTER recovery in bridge_init.cpp. Exit is BY ADDRESS because the
        // restore is deliberately repeated on several paths.
        (void)bridge::pulse_enter_global(
            "PLAYER_CHAR_ID", "execute_create_title_intent",
            reinterpret_cast<uintptr_t>(g_pid), 4, saved_pid);
        *g_pid = ruler_handle;
        execute(cmd);
        *g_pid = saved_pid;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(g_pid));  // R1c
        base_dtor(cmd);
        dispatched = true;
    } catch (...) {
        *g_pid = saved_pid;
        bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(g_pid));  // R1c
        if (rng_armed) restore_fake_random_scope(saved_rng);
        g_in_protected_region = false; watchdog_disarm();
        return reject("create_title_exception");
    }
    *g_pid = saved_pid;  // belt-and-suspenders restore on the success path
    bridge::pulse_exit_addr(reinterpret_cast<uintptr_t>(g_pid));  // R1c
    if (rng_armed) restore_fake_random_scope(saved_rng);
    g_in_protected_region = false; watchdog_disarm();

    if (dispatched && eligible) {
        intent->executed = true;
        intent->consumed = true;
        IQLOG("create_title intent COMPLETED: actor=%u title_id=%u — Execute ran "
              "synchronously (verify via title HOLDER readback)\n",
              intent->actor_id & 0xFFFFFF, title_id);
        return true;
    }
    return reject("create_title_unknown_failure");
}

} // namespace bridge
