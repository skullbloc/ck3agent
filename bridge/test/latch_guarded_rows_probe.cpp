// lane l6, witness #3 MAJOR 2: construction_check's list mode must not arm the
// fake random scope while the scope latch is set.
//
// A row whose restore trips the latch (the slot moved under the arm,
// scope_slot_policy.h) used to leave every later row of the same list arming while
// latched: the handler tested the latch once, at entry. latch_guarded_rows.h tests
// it before every row; this probe drives that loop with a row that trips the latch
// mid-list and reads the invariant `arm_called_while_latched == 0`.
//
// The row's ask here ARMS UNCONDITIONALLY (no latch test of its own), so the loop
// is the only guard under test. The negative control runs the same asks in a plain
// loop and must see the arms the guard prevents: a probe whose counter could not
// see them would pass for any loop.
#include "latch_guarded_rows.h"
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static void row(const char* n, bool ok, const std::string& d) {
    printf("%-58s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) failures++;
}

static void reset() {
    bridge::g_scope_latched.store(false);
    bridge::g_arm_called_while_latched.store(0);
    bridge::g_arm_refused_latched.store(0);
}

// An arm as arm_fake_random_scope counts it: a caller that reaches it while
// latched is counted there (intent_dispatch.cpp).
static void arm() {
    if (bridge::scope_latched()) bridge::g_arm_called_while_latched.fetch_add(1);
}

int main() {
    const size_t N = 5, TRIP = 1;   // the second row's restore trips the latch

    // ── the guard ──────────────────────────────────────────────────────────
    reset();
    std::vector<size_t> asked, refused;
    bridge::for_each_row_under_latch(N,
        [&](size_t i) {
            asked.push_back(i);
            arm();
            if (i == TRIP) bridge::g_scope_latched.store(true);   // the restore trips it
        },
        [&](size_t i) { refused.push_back(i); });
    row("no_arm_while_latched_after_a_mid_list_trip",
        bridge::g_arm_called_while_latched.load() == 0,
        "arm_called_while_latched=" + std::to_string(bridge::g_arm_called_while_latched.load()));
    row("the_rows_up_to_the_trip_were_asked",
        asked == std::vector<size_t>{0, 1}, std::to_string(asked.size()) + " asked");
    row("every_later_row_is_refused_rng_scope_latched",
        refused == std::vector<size_t>{2, 3, 4}, std::to_string(refused.size()) + " refused");
    row("each_refusal_is_counted",
        bridge::g_arm_refused_latched.load() == 3,
        "arm_refused_latched=" + std::to_string(bridge::g_arm_refused_latched.load()));

    // ── a list with no trip asks every row ─────────────────────────────────
    reset();
    size_t n_asked = 0, n_refused = 0;
    bridge::for_each_row_under_latch(N, [&](size_t) { ++n_asked; arm(); },
                                     [&](size_t) { ++n_refused; });
    row("a_clear_latch_asks_every_row", n_asked == N && n_refused == 0,
        std::to_string(n_asked) + " asked");

    // ── negative control: the pre-fix shape (one test at entry, then a plain
    //    loop) arms the three later rows while latched ─────────────────────────
    reset();
    if (!bridge::scope_latched()) {
        for (size_t i = 0; i < N; ++i) {
            arm();
            if (i == TRIP) bridge::g_scope_latched.store(true);
        }
    }
    row("CONTROL_a_plain_loop_arms_while_latched",
        bridge::g_arm_called_while_latched.load() == N - TRIP - 1,
        "arm_called_while_latched=" + std::to_string(bridge::g_arm_called_while_latched.load()) +
        " (the counter sees what the guard prevents)");

    reset();
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
