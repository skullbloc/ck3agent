// test_pipeline_mode.cpp — R8 host unit tests: what `null` MEANS (ck-6kvq.4).
//
// Design of record: docs/design/event-pipeline-conductor.md §6.3 ("no control
// sessions, no windows, clock free-runs at a fixed speed, observer lane samples
// on its game-date cadence, harness stops at the horizon. Zero tokens.").
//
// No CK3 binary, no game, no socket. Two pure translation units —
// pipeline_mode.cpp and flush_scheduler.cpp — carry every behaviour this bead
// had to change, which is why the bead's bridge half is host-testable at all.
//
// ck-1ek9 DEVIATION FROM 425c20b. As written this file had three pure TUs and
// a fifth policy column, both involving the CHECKPOINT CLOCK GATE. That gate
// was withdrawn from the experiment path on 2026-09-12 (G3' reversed by the
// progenitor — episode_policy.h:14-18), so section 5 and half of section 4
// tested a subsystem that no longer exists. They are removed rather than
// stubbed: a test kept alive against a deleted subsystem is a test that can
// only ever pass. Everything that bears on THIS bead — the downgrade — is
// untouched, and the negative control still fails on two independent columns
// (buffering and control sessions), so removing the gate did not cost the
// suite its teeth.
//
// WHAT THIS PINS, AND WHY EACH ONE HAD TEETH
// ==========================================
// Before this bead `null` was `pipeline` with clock ownership switched off:
//
//   * it buffered and ran the conductor, so the flush plan's Pause / Unpause /
//     SetSpeed steps drove the clock in the one mode defined by the clock
//     free-running (section 3 shows the plan it was cutting);
//   * it admitted CONTROL sessions, so "no steering, zero tokens" was enforced
//     by nothing (section 4);
//
// THE NEGATIVE CONTROL. `test_pipeline_mode_negative_control` is this same
// source against pipeline_mode.cpp built with -DCK3_NULL_MODE_LEGACY_POLICY=1,
// which restores the pre-bead policy. It MUST FAIL. Without it, a test of a
// policy table is just the table written twice.

#include "pipeline_mode.h"
#include "flush_scheduler.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace bridge;

// ---------------------------------------------------------------- harness ---
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);       \
        }                                                                     \
    } while (0)

#define CHECK_STR(a, b, msg)                                                  \
    do {                                                                      \
        ++g_checks;                                                           \
        std::string _a = (a), _b = (b);                                       \
        if (_a != _b) {                                                       \
            ++g_failures;                                                     \
            printf("  FAIL: %s  (got \"%s\", want \"%s\")  (%s:%d)\n", (msg), \
                   _a.c_str(), _b.c_str(), __FILE__, __LINE__);               \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                   \
    do {                                                                      \
        ++g_checks;                                                           \
        auto _a = (a); auto _b = (b);                                         \
        if (!(_a == _b)) {                                                    \
            ++g_failures;                                                     \
            printf("  FAIL: %s  (got %lld, want %lld)  (%s:%d)\n", (msg),     \
                   (long long)_a, (long long)_b, __FILE__, __LINE__);         \
        }                                                                     \
    } while (0)

static void section(const char* n) { printf("\n[%s]\n", n); }

static const PipelineMode kModes[] = {
    PipelineMode::Legacy, PipelineMode::Pipeline, PipelineMode::Null,
};

// ═══════════════════════════════════════════════════════════════════════════
// 1. The wire strings and the parse
// ═══════════════════════════════════════════════════════════════════════════
static void test_names_and_parse() {
    section("names + parse");

    // These three strings are the wire contract: run_summary.json's
    // `pipeline_mode`, and get_tick_info's `pipeline.mode`, which
    // scripts/episode_abort.sh compares against the literal "legacy".
    CHECK_STR(pipeline_mode_name(PipelineMode::Legacy),   "legacy",   "legacy name");
    CHECK_STR(pipeline_mode_name(PipelineMode::Pipeline), "pipeline", "pipeline name");
    CHECK_STR(pipeline_mode_name(PipelineMode::Null),     "null",     "null name");

    bool ok = false;
    CHECK(parse_pipeline_mode("legacy", &ok)   == PipelineMode::Legacy,   "parse legacy");
    CHECK(ok, "legacy recognised");
    CHECK(parse_pipeline_mode("pipeline", &ok) == PipelineMode::Pipeline, "parse pipeline");
    CHECK(ok, "pipeline recognised");
    CHECK(parse_pipeline_mode("null", &ok)     == PipelineMode::Null,     "parse null");
    CHECK(ok, "null recognised");

    // Unset is the NORMAL case, not a mistake: legacy, recognised.
    CHECK(parse_pipeline_mode(nullptr, &ok) == PipelineMode::Legacy, "unset -> legacy");
    CHECK(ok, "unset is recognised, not a typo");
    CHECK(parse_pipeline_mode("", &ok) == PipelineMode::Legacy, "empty -> legacy");
    CHECK(ok, "empty is recognised");

    // A TYPO is a mistake and must be loud. CK3_PIPELINE_MODE=pipelien silently
    // starting a legacy episode that the bundle then labels `pipeline` is the
    // exact "wrong answer that looks like a right one" class this epic keeps
    // finding.
    CHECK(parse_pipeline_mode("pipelien", &ok) == PipelineMode::Legacy, "typo -> legacy");
    CHECK(!ok, "typo is NOT recognised");
    CHECK(parse_pipeline_mode("NULL", &ok) == PipelineMode::Legacy, "case-sensitive");
    CHECK(!ok, "wrong case is NOT recognised");

    // A null out-param must not crash anything.
    CHECK(parse_pipeline_mode("null", nullptr) == PipelineMode::Null, "null out-param ok");
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. The policy table, cell by cell
// ═══════════════════════════════════════════════════════════════════════════
static void test_policy_table() {
    section("policy table");

    // legacy: does nothing new, admits everything. G11's whole promise.
    CHECK(!mode_buffers_calls(PipelineMode::Legacy),   "legacy does not buffer");
    CHECK(!mode_runs_flush_loop(PipelineMode::Legacy), "legacy runs no conductor");
    CHECK(!mode_owns_clock(PipelineMode::Legacy),      "legacy does not own the clock");
    CHECK(mode_admits_control_sessions(PipelineMode::Legacy), "legacy admits control");
    CHECK(!mode_admits_observer_clock_verbs(PipelineMode::Legacy),
          "legacy observers do NOT set the clock — legacy has control sessions");

    // pipeline: the flush loop, everything on.
    CHECK(mode_buffers_calls(PipelineMode::Pipeline),   "pipeline buffers");
    CHECK(mode_runs_flush_loop(PipelineMode::Pipeline), "pipeline runs the conductor");
    CHECK(mode_owns_clock(PipelineMode::Pipeline),      "pipeline owns the clock (G1)");
    CHECK(mode_admits_control_sessions(PipelineMode::Pipeline), "pipeline admits control");
    CHECK(!mode_admits_observer_clock_verbs(PipelineMode::Pipeline),
          "pipeline observers do NOT set the clock — the pipeline OWNS it");

    // null: E0. The four NOs are the arm.
    // (`may_arm_checkpoint_gate` was the fifth — removed with its subsystem.)
    CHECK(!mode_buffers_calls(PipelineMode::Null),
          "NULL MUST NOT BUFFER — buffering with no conductor is an unbounded "
          "client hang, and the drain is what emits the clock steps");
    CHECK(!mode_runs_flush_loop(PipelineMode::Null),
          "NULL MUST NOT RUN THE CONDUCTOR — it is the only thing that pauses "
          "the clock");
    CHECK(!mode_owns_clock(PipelineMode::Null),
          "null does not take clock ownership: the harness sets a speed once "
          "and the world free-runs");
    CHECK(!mode_admits_control_sessions(PipelineMode::Null),
          "NULL MUST ADMIT NO CONTROL SESSION — this IS E0's zero-token arm");
    CHECK(mode_admits_observer_clock_verbs(PipelineMode::Null),
          "NULL MUST LET AN OBSERVER SET THE CLOCK — it is the only mode with "
          "no control session to do it, and an episode that cannot pin its own "
          "declared speed refuses at open (ck-1ek9)");

    // Totality: every predicate answers for every mode, no aborts, no UB.
    for (PipelineMode m : kModes) {
        (void)mode_buffers_calls(m);
        (void)mode_runs_flush_loop(m);
        (void)mode_owns_clock(m);
        (void)mode_admits_control_sessions(m);
        (void)mode_admits_observer_clock_verbs(m);
        CHECK(pipeline_mode_name(m)[0] != '\0', "every mode has a name");
    }

    // A downgrade is typed, and the token is shared by the log line, the
    // handshake path and this test.
    CHECK_STR(pipeline_mode_control_downgrade_reason(), "null_mode_observer_only",
              "downgrade reason is the agreed token");
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. Why "the clock free-runs" follows from "null does not buffer"
// ═══════════════════════════════════════════════════════════════════════════
// The scheduler has NO mode awareness — it is a pure TU and cannot see
// PipelineMode. So it cannot be asked "do not touch the clock in null mode";
// the only thing that keeps null clock-silent is that nothing is ever submitted
// to it. This section proves the implication by showing the plan the scheduler
// DOES cut for a paused-polarity call, and then pinning the two predicates that
// guarantee such a call is never submitted in null mode.
static void test_null_free_runs_because_it_never_buffers() {
    section("null free-runs because it never buffers");

    FlushConfig cfg;
    cfg.period_ms = 10000;
    FlushScheduler sched(cfg);
    sched.register_session("s1");

    CallRequest req;
    req.session_id = "s1";
    req.action     = "query_character";
    req.polarity   = Polarity::PAUSED;
    SubmitResult sr = sched.submit(req, 1000);
    CHECK(sr.accepted, "a paused-polarity call is accepted into the buffer");

    ClockReading clock;
    clock.paused = false;
    clock.speed  = 3;
    FlushPlan plan = sched.plan_flush(12000, clock, FlushTrigger::Cadence);

    // THIS is what the pre-bead null mode was doing to the world every flush.
    CHECK(plan.clock_touched(),
          "a flush carrying PAUSED work DOES touch the clock — so a null mode "
          "that buffers is a null mode that stops the world");
    CHECK_EQ(plan.pause_count(), (size_t)1, "exactly one pause span");
    CHECK_EQ(plan.unpause_count(), (size_t)1, "exactly one unpause");

    // And this is why it no longer happens.
    CHECK(!mode_buffers_calls(PipelineMode::Null),
          "null submits nothing, so no such plan is ever cut");
    CHECK(!mode_runs_flush_loop(PipelineMode::Null),
          "and nothing would apply it if it were");

    // The converse, so the assertion is about NULL and not about the scheduler:
    // an all-ANY buffer never touches the clock in any mode. Null's observer
    // reads go down the legacy inline ladder, not through here at all.
    FlushScheduler sched2(cfg);
    sched2.register_session("s1");
    CallRequest any;
    any.session_id = "s1";
    any.action     = "get_tick_info";
    any.polarity   = Polarity::ANY;
    sched2.submit(any, 1000);
    FlushPlan p2 = sched2.plan_flush(12000, clock, FlushTrigger::AllAny);
    CHECK(!p2.clock_touched(), "an all-ANY flush touches nothing (control)");
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. No control sessions
// ═══════════════════════════════════════════════════════════════════════════
static void test_null_admits_no_control_session() {
    section("null admits no control session");

    CHECK(!mode_admits_control_sessions(PipelineMode::Null),
          "the handshake must downgrade a control request in null mode");

    // ck-1ek9: the second half of this section asserted that the checkpoint
    // gate never waits on an OBSERVE session. The gate is withdrawn; the claim
    // has no subsystem left to hold it, so it is gone rather than stubbed.
    CHECK(mode_admits_control_sessions(PipelineMode::Legacy) &&
          mode_admits_control_sessions(PipelineMode::Pipeline),
          "and ONLY null refuses — the downgrade must not leak into the arms");
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. THE CLOCK PIN IS A CARVE-OUT IN THE MUTATION GATE, NOT IN THE BAN
// ═══════════════════════════════════════════════════════════════════════════
// The pin has to work in the one mode that admits no control session, and the
// two columns that make that true must never BOTH be on for any mode: a mode
// that admits control sessions has no business also letting observers drive
// the clock, and a mode where observers drive the clock must be one where
// nothing else can.
static void test_clock_pin_is_exactly_complementary() {
    section("clock pin is exactly complementary to the control ban");

    for (PipelineMode m : kModes) {
        CHECK(!(mode_admits_control_sessions(m) &&
                mode_admits_observer_clock_verbs(m)),
              "no mode both admits control AND lets observers set the clock");
    }

    // And the pin is possible in EVERY mode, by one route or the other — which
    // is what stops the downgrade from making an episode unpinnable.
    for (PipelineMode m : kModes) {
        const bool pinnable = mode_admits_control_sessions(m) ||
                              mode_admits_observer_clock_verbs(m);
        CHECK(pinnable, "every mode can pin its declared speed somehow");
    }

    // Null specifically: no control session, so the observer route is the ONLY
    // one, and it must be open.
    CHECK(!mode_admits_control_sessions(PipelineMode::Null) &&
          mode_admits_observer_clock_verbs(PipelineMode::Null),
          "null pins via the observer route, because it has no other");
}

static void test_legacy_unchanged() {
    section("legacy unchanged (G11)");

    // Every column that could make a legacy bridge behave differently is off,
    // and the one that would take something AWAY from it is on.
    CHECK(!mode_buffers_calls(PipelineMode::Legacy),   "no buffering");
    CHECK(!mode_runs_flush_loop(PipelineMode::Legacy), "no conductor thread");
    CHECK(!mode_owns_clock(PipelineMode::Legacy),      "clock verbs still work");
    CHECK(mode_admits_control_sessions(PipelineMode::Legacy),
          "the live campaign's control session is untouched");
}

int main() {
    printf("test_pipeline_mode — R8 null-agent mode policy (ck-6kvq.4)\n");
    test_names_and_parse();
    test_policy_table();
    test_null_free_runs_because_it_never_buffers();
    test_null_admits_no_control_session();
    test_clock_pin_is_exactly_complementary();
    test_legacy_unchanged();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
