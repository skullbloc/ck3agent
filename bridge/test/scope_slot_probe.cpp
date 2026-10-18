// ck-2gtk — the restore must never write a value the engine cannot be holding.
//
// The pure half of the fix: given what the slot actually holds, may we write?
// The atomics live at the call site; this is the decision, host-provable.
#include "scope_slot_policy.h"
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
static void row(const char* n, bool ok, const char* d) {
    printf("%-52s %s  %s\n", n, ok ? "PASS" : "FAIL", d);
    if (!ok) failures++;
}

int main() {
    using V = bridge::RestoreVerdict;
    const uint64_t FAKE  = 0x00007f7a399106a0ull;   // &g_fake_scope
    const uint64_t SAVED = 0x00007f79f4e69aa0ull;   // the pid-174022 pointer
    const uint64_t NEWER = 0x00007f79f4e69808ull;   // a scope the engine pushed since

    row("our_own_fake_scope_may_be_replaced",
        bridge::restore_verdict(FAKE, FAKE, SAVED) == V::Write,
        "the one state in which the engine demonstrably has not moved on");

    // THE CRASH. The engine popped SAVED and pushed NEWER during our window.
    // The pre-fix code plain-stored SAVED back over NEWER.
    row("a_newer_engine_scope_is_NOT_clobbered",
        bridge::restore_verdict(NEWER, FAKE, SAVED) == V::SlotChanged,
        "this is pid-174022: writing SAVED here resurrects a popped scope");

    row("an_already_restored_slot_is_a_noop",
        bridge::restore_verdict(SAVED, FAKE, SAVED) == V::NothingToDo,
        "distinguished from SlotChanged: different fact, different counter");

    // The engine's own no-active-scope state must not read as permission.
    row("an_empty_slot_is_NOT_ours_to_fill",
        bridge::restore_verdict(0, FAKE, SAVED) == V::SlotChanged,
        "0 is the engine's designed empty state, not an invitation");

    // THE REJECTED DESIGN, asserted so it cannot quietly come back. The first
    // instrument proposed was "refuse a `saved` that is not a plausibly-mapped
    // pointer". SAVED *is* a plausibly-mapped pointer — a live stack address —
    // so that test would have been GREEN through the entire crash. What went
    // stale is the OBJECT, on another thread; the pointer cannot show it.
    const bool saved_looks_fine = (SAVED > 0x10000) && ((SAVED & 7) == 0);
    row("pointer_shape_alone_would_NOT_have_caught_it",
        saved_looks_fine && bridge::restore_verdict(NEWER, FAKE, SAVED) == V::SlotChanged,
        "shape-green yet correctly refused — only the slot's value decides");

    // Every verdict names itself; a refusal that cannot be read is not typed.
    row("every_verdict_has_a_name",
        std::strcmp(bridge::restore_verdict_name(V::Write), "write") == 0 &&
        std::strcmp(bridge::restore_verdict_name(V::SlotChanged),
                    "slot_changed_under_us") == 0 &&
        std::strcmp(bridge::restore_verdict_name(V::NothingToDo),
                    "already_saved") == 0,
        "the refusal names the leg");

    // Counters start at zero and separate; a clean run must be able to SAY so.
    // ck-zgh4: asserted as the WHOLE string, deliberately. This is the
    // heartbeat's format contract — if a field is dropped or renamed, the
    // operator's grep goes quietly empty, so the probe would rather red.
    char b[1024];
    bridge::format_scope_slot_counters(b, sizeof b);
    const char* kClean =
        "arm_displaced_live=0 arm_cas_failed=0 restore_refused=0 restore_noop=0 "
        "scope_latched=0 arm_refused_latched=0 arm_called_while_latched=0 "
        "war_validity_skipped_latched=0 law_diagnostic_skipped_latched=0 "
        "arm_slot_null=0 arm_slot_own_fake=0 arm_slot_foreign=0 "
        "arm_saved_node_unmapped=0 "
        "arm_published=0 restore_ran=0 arm_never_restored=0 "
        "arm_tid_distinct=0 arm_tid_overflow=0 "
        "arm_overlap_other_tid_unleaked=0 arm_open_tids_max=0";
    row("a_clean_run_states_every_counter_as_zero",
        std::strcmp(b, kClean) == 0, b);

    // THE TRUNCATION GUARD. ipc_server.cpp formats this into a fixed buffer,
    // and snprintf truncates SILENTLY: a short buffer would drop the newest
    // counters off the end and leave the heartbeat looking healthy. 192 bytes
    // held the original four fields and would now cut the line in half.
    {
        bridge::g_arm_displaced_live = 18446744073709551615ull;
        bridge::g_arm_cas_failed = 18446744073709551615ull;
        bridge::g_restore_refused = 18446744073709551615ull;
        bridge::g_restore_noop = 18446744073709551615ull;
        bridge::g_arm_published.store(18446744073709551615ull);
        bridge::g_arm_slot_foreign.store(18446744073709551615ull);
        char wide[1024];
        bridge::format_scope_slot_counters(wide, sizeof wide);
        const size_t worst = std::strlen(wide);
        row("the_widest_line_fits_the_heartbeat_buffer",
            worst < 768, "ipc_server.cpp declares char ss[768]");
        bridge::g_arm_displaced_live = 0; bridge::g_arm_cas_failed = 0;
        bridge::g_restore_refused = 0;    bridge::g_restore_noop = 0;
        bridge::g_arm_published.store(0); bridge::g_arm_slot_foreign.store(0);
    }

    bridge::g_arm_displaced_live = 1;   // the pid-174022 arm displaced a live scope
    bridge::g_restore_refused    = 2;
    bridge::format_scope_slot_counters(b, sizeof b);
    row("the_counters_separate",
        std::strstr(b, "arm_displaced_live=1 arm_cas_failed=0 "
                       "restore_refused=2 restore_noop=0") == b, b);
    bridge::g_arm_displaced_live = 0;
    bridge::g_restore_refused    = 0;

    // ═══ ck-zgh4: the three states, the wedge, the latch ═══════════════════
    using S = bridge::SlotState;

    // THE DEFECT THE THREE STATES EXIST FOR. `saved != 0` was read as "we
    // displaced a LIVE ENGINE scope", and it is also true when the slot holds
    // OUR OWN fake, left by an arm whose restore never ran. deploy-50's 97 is
    // a sum over both and cannot be split after the fact.
    row("a_nonzero_slot_is_not_necessarily_an_engine_scope",
        bridge::slot_state(FAKE, FAKE) == S::OwnFake &&
        bridge::slot_state(SAVED, FAKE) == S::Foreign &&
        (FAKE != 0 && SAVED != 0),
        "both are nonzero; only one is somebody else's");
    row("an_empty_slot_is_its_own_state",
        bridge::slot_state(0, FAKE) == S::Null, "the designed case");
    row("every_slot_state_has_a_name",
        std::strcmp(bridge::slot_state_name(S::Null), "null") == 0 &&
        std::strcmp(bridge::slot_state_name(S::OwnFake), "own_fake") == 0 &&
        std::strcmp(bridge::slot_state_name(S::Foreign), "foreign") == 0,
        "absence is a state, not a default");

    // THE WEDGE DETECTOR. In the d50 fault the node's first qword was 0xa.
    // This is the check the header said no inspection of the pointer could
    // make; it cannot prove liveness, but it does catch a pointer into dead
    // memory, which is what actually killed the process.
    bridge::note_arm_published(S::Foreign, /*saved_node_mapped=*/false, 1001);
    row("an_unmapped_node_is_counted_as_a_wedge",
        bridge::g_arm_saved_node_unmapped.load() == 1 &&
        bridge::g_arm_slot_foreign.load() == 1,
        "0xa+0x10=0x1a — the fault, visible AT THE ARM");
    bridge::note_arm_published(S::Null, /*saved_node_mapped=*/false, 1001);
    row("an_empty_slot_is_never_a_wedge",
        bridge::g_arm_saved_node_unmapped.load() == 1,
        "there is no node to be unmapped; a NULL slot must not inflate it");

    // never_restored BY SUBTRACTION, not from a depth a longjmp left stale.
    row("arms_whose_restore_never_ran_are_countable",
        bridge::arm_never_restored() == 2, "published 2, ran 0");
    bridge::note_restore_ran(1001);
    bridge::note_restore_ran(1001);
    row("and_go_back_to_zero_when_they_do_run",
        bridge::arm_never_restored() == 0, "published 2, ran 2");

    // THE FALSE-POSITIVE THE OVERLAP COUNTER MUST NOT HAVE. Once a fault skips
    // a restore, that tid's depth stays above zero for the rest of the
    // process. An unqualified cross-tid overlap counter would then report
    // concurrency on EVERY later arm — manufacturing the hypothesis it exists
    // to test. So it only counts while nothing has leaked.
    row("same_tid_rearm_is_not_cross_tid_concurrency",
        bridge::g_arm_overlap_other_tid_unleaked.load() == 0,
        "two arms on one tid is nesting, not two threads");
    bridge::note_arm_published(S::Null, true, 2002);   // leaks: never restored
    bridge::note_arm_published(S::Null, true, 3003);   // a different tid, post-leak
    row("a_leak_does_not_manufacture_concurrency",
        bridge::arm_never_restored() == 2 &&
        bridge::g_arm_overlap_other_tid_unleaked.load() == 0,
        "the second arm saw tid 2002 open, but 2002 had LEAKED");
    row("distinct_arming_tids_are_counted",
        bridge::g_arm_tid_distinct.load() == 3,
        "does more than one thread arm at all — exact, and leak-proof");

    // THE LATCH. Starts clear, trips once, and NEVER clears: a clean run does
    // not make the earlier observation false.
    row("the_latch_starts_clear", !bridge::scope_latched(),
        "an unlatched process arms normally");
    bridge::g_scope_latched.store(true);
    row("the_latch_reads_set_once_tripped", bridge::scope_latched(), "");
    bridge::format_scope_slot_counters(b, sizeof b);
    row("the_latch_rides_the_heartbeat",
        std::strstr(b, "scope_latched=1") != nullptr,
        "a latch that ends an episode's verbs must be visible without the trace");

    // ONE refusal text, so 29 call sites cannot drift into 29 wordings.
    const std::string j = bridge::scope_latched_refusal_json("set_council_task");
    row("the_refusal_is_not_retryable",
        j.find("\"retryable\":false") != std::string::npos &&
        j.find("\"reason\":\"rng_scope_latched\"") != std::string::npos &&
        j.find("\"action\":\"set_council_task\"") != std::string::npos,
        "the latch is never revisited, so `try again` would be a lie");
    row("the_refusal_says_for_how_long",
        j.find("rest of this episode") != std::string::npos,
        "not transient: the seat must not retry every cycle");
    row("the_read_refusal_does_not_call_itself_an_action",
        std::strstr(bridge::scope_latched_read_note(), "decision listing") != nullptr &&
        std::strstr(bridge::scope_latched_read_note(), "action") == nullptr,
        "discover(decisions) is a read; a latch costs a read too");

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
