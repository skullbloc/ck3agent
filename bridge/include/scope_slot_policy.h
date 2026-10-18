// scope_slot_policy.h — ck-2gtk. Who may write RandomScopeManager+0x20.
//
// THE ENGINE PUBLISHES THAT SLOT WITH `xchg`, AND KEEPS EACH FRAME'S SAVED
// PREDECESSOR INSIDE ITS OWN OBJECT. Disassembled from the shipped binary
// (md5 09ba163bbff47f2eb8a69692536307e6), the push at ELF 0x7748e60:
//
//     lea  0x20(%r14),%rax     ; &mgr->slot
//     mov  %rax,0x10(%rbx)     ; scope->+0x10 = &slot      BACKLINK
//     xchg %rax,0x20(%r14)     ; ATOMIC: slot = scope, rax = PREVIOUS
//     mov  %rax,0x18(%rbx)     ; scope->+0x18 = PREVIOUS   SAVED LINK
//     movb $0x1,0x20(%rbx)     ; scope->+0x20 = 1          ACTIVE FLAG
//
// It is a LOCK-FREE LINKED STACK. There is no mutex; correctness rests entirely
// on that exchange, and each participant's undo lives in the node it pushed.
//
// The bridge was not a participant. arm_fake_random_scope did
// `saved = *slot; *slot = &g_fake_scope;` and restore_fake_random_scope did
// `*slot = saved;` — plain loads and stores, spanning an engine call, issued
// from a DIFFERENT thread. Against a lock-free stack that is an ABA by
// construction: between the arm's read and the restore's write the engine can
// pop the node `saved` names, and the restore then RESURRECTS a popped scope.
//
// That is not hypothetical. From the pid-174022 core: the pulse ledger's last
// entry (handle 21, flush 135, the final flush before death) recorded
// site="arm_fake_random_scope", addr=0x5616b57b1570 — the slot that faulted —
// and saved=0x7f79f4e69aa0 — the pointer that faulted — entered by a BRIDGE
// thread (174036) while the pointer named a scope object on an ENGINE worker's
// stack (174143). The slot's value at death WAS that `saved`. The engine then
// read *(that) and found 0xa, reused stack, and dereferenced it: 0xa+0x10=0x1a.
//
// The bridge did not write the 0xa. It wrote the pointer TO it, after the
// object stopped being a scope.
//
// THE RULE: never write a value the engine cannot be holding. A restore is
// legitimate only while the slot still holds OUR OWN fake scope — that is the
// one state in which we know the engine has not moved on. Anything else and
// the engine has published something newer, which we must not clobber and must
// not replace with a pointer whose liveness we cannot know.
//
// Checking "is `saved` pointer-shaped" does NOT work and was the first thing
// proposed: `saved` was a perfectly good pointer when captured. What went
// stale is the OBJECT, on another thread, and no inspection of the pointer can
// see that.
//
// ── ck-zgh4: AND THE COMPARE-EXCHANGE CANNOT SEE IT EITHER ───────────────────
// This paragraph used to end "Only the compare-exchange can." That is wrong,
// and it is the sentence that let the deploy-50 wedge run for a whole episode.
//
// The CAS can only see a change since OUR OWN ARM. It cannot see that `saved`
// was ALREADY dead at the moment we captured it. THE GUARD VALIDATES THE SLOT;
// NOTHING VALIDATES `saved`. So once the slot holds a foreign value F:
//
//     arm      loads saved=F, CAS F->fake    succeeds
//     restore  observes our fake -> Write    puts F back
//
// every condition the rule checks is satisfied, on every cycle, forever. In
// deploy-50 slot01 that is exactly what happened: ONE refused restore
// (restore_refused 0->1 at log line 11028) left a foreign value in the slot,
// and the next ~97 arms each re-displaced it and each put it back, with
// restore_refused frozen at 1 the whole time, until an engine thread read it
// and died on 0xa+0x10 — the pid-174022 signature.
//
// Because no inspection of `saved` can establish its liveness, before the arm
// or after, the only sound posture is NEVER TO CAPTURE IT FROM A NON-NULL SLOT
// AT ALL. That is what the latch below enforces, and it is why the remedy is
// not a better restore value: there is no value the engine can be guaranteed
// to accept. NULL re-creates the null-deref the arm exists to prevent (any
// thread then inside a scope check faults), and leaving our own fake in place
// leaves a scope tagged with OUR tid, which the engine checks — the d50 fault
// landed on +0x10, the tid field, so that check demonstrably runs.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace bridge {

// What a restore is allowed to do, decided from the slot's observed value.
// Pure, so it can be proved on a host; the atomics live at the call site.
enum class RestoreVerdict {
    Write,          // the slot still holds our fake scope — putting `saved` back is safe
    SlotChanged,    // the engine published something newer — DO NOT WRITE
    NothingToDo,    // the slot already holds `saved` — the restore is a no-op
};

inline RestoreVerdict restore_verdict(uint64_t observed,
                                      uint64_t our_fake_scope,
                                      uint64_t saved) {
    if (observed == our_fake_scope) return RestoreVerdict::Write;
    if (observed == saved)          return RestoreVerdict::NothingToDo;
    return RestoreVerdict::SlotChanged;
}

inline const char* restore_verdict_name(RestoreVerdict v) {
    switch (v) {
        case RestoreVerdict::Write:       return "write";
        case RestoreVerdict::SlotChanged: return "slot_changed_under_us";
        case RestoreVerdict::NothingToDo: return "already_saved";
    }
    return "unknown";
}

// ── what the slot held at the arm: THREE states, not two ────────────────────
// `saved != 0` was used as a proxy for "we are displacing a LIVE engine
// scope", and the proxy is wrong in a way this very file documents: eight call
// sites place their arm AFTER their `sigsetjmp`, so a fault leaves OUR OWN
// `g_fake_scope` in the slot for the rest of the process, and every later arm
// then loads it as `saved` and counts an engine displacement that never
// happened. deploy-50's arm_displaced_live=97 is a sum over both populations
// and cannot be split after the fact. Hence a classifier, not a predicate.
enum class SlotState {
    Null,       // the designed case — no scope published. Arming is safe.
    OwnFake,    // our own fake, left by an arm whose restore never ran.
    Foreign,    // somebody else's pointer, liveness unknowable. The hazard.
};

inline SlotState slot_state(uint64_t saved, uint64_t our_fake_scope) {
    if (saved == 0) return SlotState::Null;
    if (saved == our_fake_scope) return SlotState::OwnFake;
    return SlotState::Foreign;
}

inline const char* slot_state_name(SlotState s) {
    switch (s) {
        case SlotState::Null:    return "null";
        case SlotState::OwnFake: return "own_fake";
        case SlotState::Foreign: return "foreign";
    }
    return "unknown";
}

// ── the latch (ck-zgh4, progenitor ruling "Latch + measure") ────────────────
// From the FIRST refused restore, the bridge stops touching the slot for the
// rest of the PROCESS. Not the first non-NULL slot: non-NULL was 97 events in
// d50 and all of them were downstream of the single refusal, so latching on
// non-NULL would cost every arming verb for the rest of any episode that
// wedges, while latching on the refusal costs them only after the one event
// that actually proves the slot untrustworthy.
//
// NEVER CLEARED. A clean run does not clear it, because the thing it records
// is that this process once observed a slot it could not reason about, and
// nothing later in the process makes that observation false.
//
// Trips on SlotChanged ONLY. NothingToDo is a benign double-restore and must
// not end an episode's verbs.
inline std::atomic<bool> g_scope_latched{false};

inline bool scope_latched() {
    return g_scope_latched.load(std::memory_order_relaxed);
}

// ── the refusal, in ONE place ───────────────────────────────────────────────
// 29 call sites refuse on this condition. Written out at each one they would
// drift into 29 wordings, and a seat reading two of them would have no way to
// tell it was the same condition. Here they are host-assertable.
//
// `retryable` is FALSE and the note says "for the rest of this episode". This
// is not a transient busy slot: the latch is a decision the process made and
// never revisits, so "try again" would be a lie that costs the seat a cycle
// every time it believed it.
inline const char* scope_latched_reason() { return "rng_scope_latched"; }

inline const char* scope_latched_note() {
    return "unavailable for the rest of this episode: the bridge stopped using "
           "the engine's RNG scope after a safety check failed";
}

// The read half. discover(decisions) is not an action, and telling a seat its
// "action is unavailable" when it asked for a listing would misdescribe what
// it lost. Progenitor wording, 2026-10-06.
inline const char* scope_latched_read_note() {
    return "decision listing is unavailable for the rest of this episode: the "
           "bridge stopped using the engine's RNG scope after a safety check "
           "failed";
}

// The action handlers' response shape: {"success":false,"action":...,"reason":
// ...,"retryable":false,"note":...}. Built here so the text cannot drift and
// so a host lane can compare it byte for byte.
inline std::string scope_latched_refusal_json(const char* action) {
    std::string s = "{\"success\":false,\"action\":\"";
    s += (action ? action : "");
    s += "\",\"reason\":\"";
    s += scope_latched_reason();
    s += "\",\"retryable\":false,\"note\":\"";
    s += scope_latched_note();
    s += "\"}";
    return s;
}

// ── counters, unconditional ─────────────────────────────────────────────────
// A diagnostic behind a trace flag cannot support a negative claim, and the
// claim we need is negative: "no arm displaced a live engine scope during this
// run". So these ride the heartbeat, not the trace.

//: arms where the slot already held a scope — the bridge displacing a LIVE
//: engine scope. This is the PRECONDITION for the crash above; zero across a
//: run makes the pathway impossible and clears the bridge on evidence.
inline std::uint64_t g_arm_displaced_live = 0;
//: arms where the compare-exchange lost a race and the arm was abandoned.
inline std::uint64_t g_arm_cas_failed = 0;
//: restores refused because the engine had published something newer. Each one
//: is a crash that did not happen.
inline std::uint64_t g_restore_refused = 0;
//: restores that found the slot already holding `saved`.
inline std::uint64_t g_restore_noop = 0;

// ── ck-zgh4 measurement ─────────────────────────────────────────────────────
// Atomics, unlike the four plain counters above: these are read by the
// heartbeat while several threads arm, and two of them (the tid table and the
// published/ran pair) give wrong ANSWERS rather than merely losing a count if
// they tear. The plain four are a pre-existing race left as it was; widening
// this bead to fix them would put an untested change in a crash-path commit.

//: the three states of the slot at the arm, from slot_state(). These replace
//: `arm_displaced_live` as the thing to read; that counter is kept, still
//: incremented, so a run can be compared against the six prior slots.
inline std::atomic<std::uint64_t> g_arm_slot_null{0};
inline std::atomic<std::uint64_t> g_arm_slot_own_fake{0};
inline std::atomic<std::uint64_t> g_arm_slot_foreign{0};

//: THE WEDGE DETECTOR. `saved` points at a node whose first qword should be a
//: pointer to the RNG object Y; in the d50 fault that qword was 0xa, an
//: unmapped small integer, because the frame had been reused. Non-zero here
//: means the slot held a pointer into dead memory AT THE MOMENT OF THE ARM —
//: which is the state no inspection of `saved` was thought able to see. It
//: needs no tid and no live game, and it would have fired on d50's FIRST arm
//: after the refusal instead of ~39,000 log lines later.
inline std::atomic<std::uint64_t> g_arm_saved_node_unmapped{0};

//: handler calls REFUSED at the call site because the latch was set. Each one
//: is a verb the seat did not get, so this is the treatment-visible cost.
inline std::atomic<std::uint64_t> g_arm_refused_latched{0};

//: callers that reached arm_fake_random_scope DESPITE the latch. ZERO sites
//: are allowed to, so the invariant is simply
//:
//:     arm_called_while_latched == 0
//:
//: and ANY non-zero value is a site whose latch guard is missing — a refusal
//: that silently became "proceed without arming", the outcome the ruling
//: rejected.
//:
//: THIS COMMENT USED TO CLAIM `== war_validity_skipped_latched`, AND THAT WAS
//: WRONG TWICE (found by ck-t5bc). The war site I named as the one legitimate
//: reacher tests the latch in an `else if` BEFORE the arm, so it never
//: reaches arm at all and contributes ZERO, not one. Meanwhile
//: run_effect_tree_walk_hardened DID reach the arm while latched, because it
//: armed first and tested after — so the counter was neither zero nor equal
//: to the war count, and a `called > skipped` test MASKED a real unguarded
//: site in exactly the episodes the "skip the check, make the war" ruling
//: creates: one unguarded site plus one latched war is (1,1), which that test
//: reads as clean. The walk site now tests the latch before arming, which is
//: what makes `== 0` true by construction rather than by argument.
inline std::atomic<std::uint64_t> g_arm_called_while_latched{0};

//: the law-refusal DIAGNOSTIC (action_handlers.cpp, do_change_realm_law's
//: !eligible branch) skipped because of the latch. The law refusal still goes
//: out; only its specific reason degrades to the coarse key. Separate from
//: `arm_refused_latched` because the seat still gets its answer — no verb was
//: lost — and separate from the 1989 counter because nothing here mutates.
inline std::atomic<std::uint64_t> g_law_diagnostic_skipped_latched{0};

//: 1989 only: the latch made us skip the OPTIONAL CB-validity pre-check and
//: proceed through the engine's own fail-open path. Progenitor ruling
//: 2026-10-06, "Skip the check, make the war" — counted separately because it
//: is the one latched site that still MUTATES, so it can never be read off
//: `arm_refused_latched`.
inline std::atomic<std::uint64_t> g_war_validity_skipped_latched{0};

//: arms that published our fake, and restores that actually ran their CAS.
//: never_restored = published - ran, EXACTLY, with no inference: the eight
//: sites that arm after their `sigsetjmp` cannot restore on the fault path,
//: and this is the only figure that says so without guessing.
inline std::atomic<std::uint64_t> g_arm_published{0};
inline std::atomic<std::uint64_t> g_restore_ran{0};

// ── the arming-thread table ─────────────────────────────────────────────────
// Answers the precondition of the concurrency question — does more than one
// thread ever arm? — which is exact and leak-proof, before it tries to answer
// the harder one.
inline constexpr int SCOPE_TID_SLOTS = 16;
inline std::atomic<std::uint64_t> g_arm_tid[SCOPE_TID_SLOTS]{};
inline std::atomic<int> g_arm_tid_depth[SCOPE_TID_SLOTS]{};
inline std::atomic<std::uint64_t> g_arm_tid_distinct{0};
inline std::atomic<std::uint64_t> g_arm_tid_overflow{0};

//: an arm published while a DIFFERENT tid had one open AND nothing had leaked
//: yet. The "unleaked" qualifier is load-bearing: once a fault skips a
//: restore, that tid's depth stays above zero for the rest of the process, and
//: an unqualified overlap counter would then report concurrency on every
//: subsequent arm — manufacturing the very hypothesis it exists to test.
inline std::atomic<std::uint64_t> g_arm_overlap_other_tid_unleaked{0};

//: the largest number of tids seen holding an open arm at once. Inflated by a
//: leak in the same way, so read it beside never_restored.
inline std::atomic<std::uint64_t> g_arm_open_tids_max{0};

// Record an arm that published. `node_mapped` is the wedge check, performed at
// the call site because `is_mapped` lives in the bridge and this header is
// host-compilable on purpose.
inline void note_arm_published(SlotState st, bool saved_node_mapped,
                               std::uint64_t tid) {
    switch (st) {
        case SlotState::Null:    g_arm_slot_null.fetch_add(1, std::memory_order_relaxed); break;
        case SlotState::OwnFake: g_arm_slot_own_fake.fetch_add(1, std::memory_order_relaxed); break;
        case SlotState::Foreign: g_arm_slot_foreign.fetch_add(1, std::memory_order_relaxed); break;
    }
    if (st != SlotState::Null && !saved_node_mapped) {
        g_arm_saved_node_unmapped.fetch_add(1, std::memory_order_relaxed);
    }

    const std::uint64_t published = g_arm_published.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint64_t ran = g_restore_ran.load(std::memory_order_relaxed);
    const bool unleaked = (published - ran) <= 1;   // only this arm is open

    int mine = -1;
    int other_open = 0;
    for (int i = 0; i < SCOPE_TID_SLOTS; ++i) {
        std::uint64_t t = g_arm_tid[i].load(std::memory_order_relaxed);
        if (t == tid) { mine = i; continue; }
        if (t == 0) {
            if (mine < 0) {
                std::uint64_t expect = 0;
                if (g_arm_tid[i].compare_exchange_strong(expect, tid,
                                                         std::memory_order_relaxed)) {
                    mine = i;
                    g_arm_tid_distinct.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (expect == tid) { mine = i; continue; }
            }
            continue;
        }
        if (g_arm_tid_depth[i].load(std::memory_order_relaxed) > 0) ++other_open;
    }
    if (mine < 0) { g_arm_tid_overflow.fetch_add(1, std::memory_order_relaxed); return; }
    g_arm_tid_depth[mine].fetch_add(1, std::memory_order_relaxed);

    if (other_open > 0 && unleaked) {
        g_arm_overlap_other_tid_unleaked.fetch_add(1, std::memory_order_relaxed);
    }
    const std::uint64_t open_now = static_cast<std::uint64_t>(other_open) + 1;
    std::uint64_t prev = g_arm_open_tids_max.load(std::memory_order_relaxed);
    while (open_now > prev &&
           !g_arm_open_tids_max.compare_exchange_weak(prev, open_now,
                                                      std::memory_order_relaxed)) {
    }
}

inline void note_restore_ran(std::uint64_t tid) {
    g_restore_ran.fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < SCOPE_TID_SLOTS; ++i) {
        if (g_arm_tid[i].load(std::memory_order_relaxed) == tid) {
            if (g_arm_tid_depth[i].load(std::memory_order_relaxed) > 0) {
                g_arm_tid_depth[i].fetch_sub(1, std::memory_order_relaxed);
            }
            return;
        }
    }
}

inline std::uint64_t arm_never_restored() {
    const std::uint64_t published = g_arm_published.load(std::memory_order_relaxed);
    const std::uint64_t ran = g_restore_ran.load(std::memory_order_relaxed);
    return published > ran ? published - ran : 0;
}

inline const char* format_scope_slot_counters(char* buf, std::size_t n) {
    std::snprintf(buf, n,
                  "arm_displaced_live=%llu arm_cas_failed=%llu "
                  "restore_refused=%llu restore_noop=%llu "
                  "scope_latched=%d arm_refused_latched=%llu arm_called_while_latched=%llu "
                  "war_validity_skipped_latched=%llu law_diagnostic_skipped_latched=%llu "
                  "arm_slot_null=%llu arm_slot_own_fake=%llu arm_slot_foreign=%llu "
                  "arm_saved_node_unmapped=%llu "
                  "arm_published=%llu restore_ran=%llu arm_never_restored=%llu "
                  "arm_tid_distinct=%llu arm_tid_overflow=%llu "
                  "arm_overlap_other_tid_unleaked=%llu arm_open_tids_max=%llu",
                  (unsigned long long)g_arm_displaced_live,
                  (unsigned long long)g_arm_cas_failed,
                  (unsigned long long)g_restore_refused,
                  (unsigned long long)g_restore_noop,
                  scope_latched() ? 1 : 0,
                  (unsigned long long)g_arm_refused_latched.load(std::memory_order_relaxed),
                  (unsigned long long)g_arm_called_while_latched.load(std::memory_order_relaxed),
                  (unsigned long long)g_war_validity_skipped_latched.load(std::memory_order_relaxed),
                  (unsigned long long)g_law_diagnostic_skipped_latched.load(std::memory_order_relaxed),
                  (unsigned long long)g_arm_slot_null.load(std::memory_order_relaxed),
                  (unsigned long long)g_arm_slot_own_fake.load(std::memory_order_relaxed),
                  (unsigned long long)g_arm_slot_foreign.load(std::memory_order_relaxed),
                  (unsigned long long)g_arm_saved_node_unmapped.load(std::memory_order_relaxed),
                  (unsigned long long)g_arm_published.load(std::memory_order_relaxed),
                  (unsigned long long)g_restore_ran.load(std::memory_order_relaxed),
                  (unsigned long long)arm_never_restored(),
                  (unsigned long long)g_arm_tid_distinct.load(std::memory_order_relaxed),
                  (unsigned long long)g_arm_tid_overflow.load(std::memory_order_relaxed),
                  (unsigned long long)g_arm_overlap_other_tid_unleaked.load(std::memory_order_relaxed),
                  (unsigned long long)g_arm_open_tids_max.load(std::memory_order_relaxed));
    return buf;
}

}  // namespace bridge
