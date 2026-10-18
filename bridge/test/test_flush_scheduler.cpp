// test/test_flush_scheduler.cpp — unit tests for the R1a flush scheduler (ck-6kvq.8).
//
// Standalone: compiles flush_scheduler.cpp with NO engine, NO game, NO CK3
// binary and — unlike test_session_registry.cpp — not even a stub global,
// because the scheduler is a pure state machine by construction.
//
//   cmake -DBUILD_BRIDGE_TESTS=ON .. && make -j$(nproc) test_flush_scheduler
//   ./test_flush_scheduler
//
// What is asserted maps 1:1 onto the bead's acceptance list; see the banner
// printed by each section.

#include "flush_scheduler.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace bridge;

// ---------------------------------------------------------------------------
// Assertion harness (independent of NDEBUG).
// ---------------------------------------------------------------------------
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!((a) == (b))) {                                                  \
            ++g_failures;                                                     \
            std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                     \
    } while (0)

static void section(const char* name) { std::printf("[ %s ]\n", name); }

// ---------------------------------------------------------------------------
// A tiny interpreter for a plan, so "the clock ends RUNNING" is a simulated
// fact rather than a field we trust.
// ---------------------------------------------------------------------------
struct ClockSim {
    ClockReading state;
    int pauses = 0;
    int unpauses = 0;
    int speed_sets = 0;
    size_t served = 0;
};

static ClockSim apply_plan(const FlushPlan& plan) {
    ClockSim sim;
    sim.state = plan.clock_before;
    for (const PlanStep& s : plan.steps) {
        switch (s.kind) {
            case StepKind::Pause:
                // Must never be issued twice, and never on an already-paused clock.
                CHECK(!sim.state.paused);
                sim.state.paused = true;
                ++sim.pauses;
                break;
            case StepKind::Unpause:
                CHECK(sim.state.paused);
                sim.state.paused = false;
                ++sim.unpauses;
                break;
            case StepKind::SetSpeed:
                sim.state.speed = s.speed;
                ++sim.speed_sets;
                break;
            case StepKind::ServeAny:
                sim.served += s.calls.size();
                break;
            case StepKind::ServePaused:
                CHECK(sim.state.paused);  // paused reads must run on a stopped clock
                sim.served += s.calls.size();
                break;
            case StepKind::DrainTick: {
                CHECK(!sim.state.paused);  // the drain only runs while the sim ticks
                // §2.2: within the tick batch every READ precedes every WRITE.
                bool seen_write = false;
                for (const BufferedCall& c : s.calls) {
                    if (c.polarity == Polarity::TICK_WRITE) seen_write = true;
                    else if (c.polarity == Polarity::TICK_READ) CHECK(!seen_write);
                }
                sim.served += s.calls.size();
                break;
            }
        }
    }
    return sim;
}

static int step_index(const FlushPlan& plan, StepKind k) {
    for (size_t i = 0; i < plan.steps.size(); ++i) {
        if (plan.steps[i].kind == k) return static_cast<int>(i);
    }
    return -1;
}

static size_t count_in(const std::vector<BufferedCall>& v, const std::string& sess) {
    size_t n = 0;
    for (const BufferedCall& c : v) if (c.session_id == sess) ++n;
    return n;
}

static void submit_n(FlushScheduler& s, const char* sess, const char* action,
                     Polarity p, int n, uint64_t now_ms) {
    for (int i = 0; i < n; ++i) {
        CallRequest r;
        r.session_id = sess;
        r.action = action;
        r.polarity = p;
        SubmitResult res = s.submit(r, now_ms);
        CHECK(res.accepted);
    }
}

// ===========================================================================
// 1. Partition by polarity, including the unknown-action default.
// ===========================================================================
static void test_partition() {
    section("partition by polarity (+ unknown default)");
    FlushScheduler s;

    CallRequest any{"s1", "get_tick_info", Polarity::ANY};
    CallRequest rd {"s1", "query_realm", Polarity::PAUSED};
    CallRequest wr {"s1", "declare_war", Polarity::TICK_WRITE};
    CallRequest nu {"s1", "brand_new_handler_nobody_classified", Polarity::UNKNOWN};

    SubmitResult r_any = s.submit(any, 0);
    SubmitResult r_rd  = s.submit(rd, 0);
    SubmitResult r_wr  = s.submit(wr, 0);
    SubmitResult r_nu  = s.submit(nu, 0);
    CHECK(r_any.accepted && r_rd.accepted && r_wr.accepted && r_nu.accepted);

    // Tickets are minted, unique and non-zero.
    CHECK(r_any.ticket != 0);
    CHECK(r_any.ticket != r_rd.ticket && r_rd.ticket != r_wr.ticket &&
          r_wr.ticket != r_nu.ticket);

    // The unknown action is reported back to its caller as PAUSED — the same
    // safe-read default the MCP gateway uses. Never silently a mutation.
    CHECK(r_nu.polarity == Polarity::PAUSED);
    CHECK(r_wr.polarity == Polarity::TICK_WRITE);
    CHECK(r_any.polarity == Polarity::ANY);

    FlushPlan p = s.plan_flush(10000, ClockReading{false, 3}, FlushTrigger::Cadence);

    CHECK_EQ(p.any_calls.size(), size_t(1));
    CHECK_EQ(p.tick_write_calls.size(), size_t(1));
    CHECK_EQ(p.paused_calls.size(), size_t(2));  // query_realm + the unknown one
    CHECK_EQ(p.unclassified, size_t(1));
    CHECK(p.any_calls[0].action == "get_tick_info");
    CHECK(p.tick_write_calls[0].action == "declare_war");
    bool found_unknown = false;
    for (const BufferedCall& c : p.paused_calls) {
        if (c.action == "brand_new_handler_nobody_classified") {
            found_unknown = true;
            CHECK(c.polarity == Polarity::PAUSED);
            CHECK(c.polarity_was_unknown);
        }
    }
    CHECK(found_unknown);

    // The whole buffer was consumed and attributed.
    CHECK_EQ(s.buffer_depth(), size_t(0));
    CHECK_EQ(p.per_session_counts["s1"], size_t(4));
    CHECK_EQ(p.flush_id, uint64_t(1));
}

// ===========================================================================
// 2. THE CRITICAL CASE: an all-ANY buffer produces NO clock change whatsoever.
// ===========================================================================
static void test_all_any_never_touches_clock() {
    section("all-ANY buffer: zero clock change");
    FlushScheduler s;
    submit_n(s, "s1", "get_tick_info", Polarity::ANY, 2, 100);
    submit_n(s, "s2", "get_bridge_state", Polarity::ANY, 1, 100);

    CHECK(s.due(101) == FlushTrigger::AllAny);

    FlushPlan p = s.plan_flush(101, ClockReading{false, 3}, FlushTrigger::AllAny);
    CHECK(!p.clock_touched());
    CHECK_EQ(p.pause_count(), size_t(0));
    CHECK_EQ(p.unpause_count(), size_t(0));
    CHECK_EQ(p.steps.size(), size_t(1));
    CHECK(p.steps[0].kind == StepKind::ServeAny);
    CHECK_EQ(p.steps[0].calls.size(), size_t(3));
    CHECK(p.clock_after.paused == p.clock_before.paused);
    CHECK(p.clock_after.speed == p.clock_before.speed);
    CHECK(!p.clock_drift_observed);

    ClockSim sim = apply_plan(p);
    CHECK_EQ(sim.pauses, 0);
    CHECK_EQ(sim.unpauses, 0);
    CHECK_EQ(sim.speed_sets, 0);
    CHECK_EQ(sim.served, size_t(3));

    // Even at a non-resting speed, and even when the observed clock has DRIFTED
    // to paused, the hot path touches nothing — it flags the drift instead.
    submit_n(s, "s1", "who_holds_the_lock", Polarity::ANY, 1, 200);
    FlushPlan p2 = s.plan_flush(201, ClockReading{true, 5}, FlushTrigger::AllAny);
    CHECK(!p2.clock_touched());
    CHECK_EQ(p2.pause_count(), size_t(0));
    CHECK_EQ(p2.unpause_count(), size_t(0));
    CHECK(p2.clock_drift_observed);
    CHECK(p2.clock_after.paused == true && p2.clock_after.speed == 5);

    // An empty heartbeat flush is likewise clock-silent.
    FlushPlan p3 = s.plan_flush(20000, ClockReading{false, 3}, FlushTrigger::Cadence);
    CHECK(!p3.clock_touched());
    CHECK_EQ(p3.steps.size(), size_t(0));
    CHECK_EQ(p3.selected_count(), size_t(0));
}

// ===========================================================================
// 3. A mixed buffer produces EXACTLY ONE pause and ONE unpause.
// 4. Ordering: paused reads before ticked writes.
// ===========================================================================
static void test_one_span_and_ordering() {
    section("mixed buffer: exactly one pause / one unpause, reads before writes");
    FlushScheduler s;
    submit_n(s, "s1", "get_tick_info", Polarity::ANY, 1, 0);
    submit_n(s, "s1", "query_realm", Polarity::PAUSED, 3, 0);
    submit_n(s, "s2", "query_character", Polarity::PAUSED, 4, 0);
    submit_n(s, "s1", "declare_war", Polarity::TICK_WRITE, 2, 0);
    submit_n(s, "s2", "send_interaction", Polarity::TICK_WRITE, 1, 0);

    FlushPlan p = s.plan_flush(10000, ClockReading{false, 3}, FlushTrigger::Cadence);

    CHECK_EQ(p.pause_count(), size_t(1));    // ONE span for SEVEN paused reads
    CHECK_EQ(p.unpause_count(), size_t(1));
    CHECK_EQ(p.paused_calls.size(), size_t(7));
    CHECK_EQ(p.tick_write_calls.size(), size_t(3));
    CHECK_EQ(p.any_calls.size(), size_t(1));

    // All seven paused reads live in the SAME ServePaused step: one coherent
    // world-view per flush (§2.6).
    int i_serve_paused = step_index(p, StepKind::ServePaused);
    CHECK(i_serve_paused >= 0);
    CHECK_EQ(p.steps[static_cast<size_t>(i_serve_paused)].calls.size(), size_t(7));

    int i_any    = step_index(p, StepKind::ServeAny);
    int i_pause  = step_index(p, StepKind::Pause);
    int i_unpaus = step_index(p, StepKind::Unpause);
    int i_drain  = step_index(p, StepKind::DrainTick);
    CHECK(i_any >= 0 && i_pause >= 0 && i_unpaus >= 0 && i_drain >= 0);
    CHECK(i_any < i_pause);              // ANY first, clock untouched
    CHECK(i_pause < i_serve_paused);     // pause, then serve
    CHECK(i_serve_paused < i_unpaus);    // ...the whole batch inside the span
    CHECK(i_unpaus < i_drain);           // PAUSED READS BEFORE TICKED WRITES

    ClockSim sim = apply_plan(p);
    CHECK_EQ(sim.pauses, 1);
    CHECK_EQ(sim.unpauses, 1);
    CHECK(!sim.state.paused);
    CHECK_EQ(sim.served, size_t(11));

    // PAUSED-only: still exactly one span, and it is closed again.
    FlushScheduler s2;
    submit_n(s2, "a", "query_realm", Polarity::PAUSED, 5, 0);
    FlushPlan pp = s2.plan_flush(10000, ClockReading{false, 3}, FlushTrigger::Cadence);
    CHECK_EQ(pp.pause_count(), size_t(1));
    CHECK_EQ(pp.unpause_count(), size_t(1));
    CHECK(!apply_plan(pp).state.paused);

    // TICK-only on an already-running clock AT THE RESTING SPEED: the clock is
    // not touched at all ("ensure unpaused" is a no-op when it already is, and
    // so is "restore the resting speed" when it already is that).
    //
    // ck-on4u: THE SPEED HERE IS LOAD-BEARING AND USED TO BE A COINCIDENCE. It
    // was the literal 3, which happened to equal the planner's hard-coded
    // resting speed, so the restore step found nothing to do. That hard-coded 3
    // is the defect this bead removed — it was a second home for a
    // pre-registered constant declared as 2 elsewhere — so the reading now says
    // what it always meant: the clock is AT REST.
    FlushScheduler s3;
    submit_n(s3, "a", "declare_war", Polarity::TICK_WRITE, 2, 0);
    FlushPlan pt = s3.plan_flush(10000, ClockReading{false, kRegisteredGameSpeed},
                                 FlushTrigger::Cadence);
    CHECK_EQ(pt.pause_count(), size_t(0));
    CHECK_EQ(pt.unpause_count(), size_t(0));
    CHECK(!pt.clock_touched());
    CHECK_EQ(step_index(pt, StepKind::DrainTick), 0);

    // TICK-only on a drifted (paused) clock: one unpause, no pause.
    FlushScheduler s4;
    submit_n(s4, "a", "declare_war", Polarity::TICK_WRITE, 2, 0);
    FlushPlan pd = s4.plan_flush(10000, ClockReading{true, 3}, FlushTrigger::Cadence);
    CHECK_EQ(pd.pause_count(), size_t(0));
    CHECK_EQ(pd.unpause_count(), size_t(1));
    CHECK(!apply_plan(pd).state.paused);
}

// ===========================================================================
// 4b. THE TICK SPLIT (§2.2, §2.3): TICK_READ drains before TICK_WRITE.
//
// A deferred-drain read (CB validity, interaction acceptance, trigger/decision
// eval, conquest targets) needs a live tick for an engine verdict, but it is
// still a READ. Ordered behind the writes of its own flush it would answer from
// a world those writes had already moved — which silently widens the §2.6
// coherence claim past what the paper can defend.
// ===========================================================================
static void test_tick_read_before_tick_write() {
    section("tick split: TICK_READ drains before TICK_WRITE");

    // Names and the class predicate.
    CHECK(std::string(polarity_name(Polarity::TICK_READ)) == "tick_read");
    CHECK(std::string(polarity_name(Polarity::TICK_WRITE)) == "tick_write");
    CHECK(is_tick_polarity(Polarity::TICK_READ));
    CHECK(is_tick_polarity(Polarity::TICK_WRITE));
    CHECK(!is_tick_polarity(Polarity::PAUSED));
    CHECK(!is_tick_polarity(Polarity::ANY));
    CHECK(!is_tick_polarity(Polarity::UNKNOWN));

    // Partition: the two tick classes land in separate buckets, and neither
    // leaks into paused_calls (which would make an engine-verdict read run on a
    // stopped world, where the verdict cannot be computed at all).
    {
        FlushConfig cfg;
        cfg.max_tick_calls_per_flush = 16;
        FlushScheduler s(cfg);
        submit_n(s, "s1", "get_tick_info", Polarity::ANY, 1, 0);
        submit_n(s, "s1", "query_realm", Polarity::PAUSED, 2, 0);
        submit_n(s, "s1", "declare_war", Polarity::TICK_WRITE, 2, 0);
        submit_n(s, "s1", "query_valid_cbs_strict", Polarity::TICK_READ, 3, 0);

        FlushPlan p = s.plan_flush(10000, ClockReading{false, 3}, FlushTrigger::Cadence);
        CHECK_EQ(p.any_calls.size(), size_t(1));
        CHECK_EQ(p.paused_calls.size(), size_t(2));
        CHECK_EQ(p.tick_read_calls.size(), size_t(3));
        CHECK_EQ(p.tick_write_calls.size(), size_t(2));
        CHECK_EQ(p.tick_calls.size(), size_t(5));
        CHECK_EQ(p.selected_count(), size_t(8));
        for (const BufferedCall& c : p.tick_read_calls) {
            CHECK(c.polarity == Polarity::TICK_READ);
        }
        for (const BufferedCall& c : p.tick_write_calls) {
            CHECK(c.polarity == Polarity::TICK_WRITE);
        }

        // tick_calls is the batch the DrainTick step actually applies, and it
        // is the reads-first concatenation of the two halves.
        CHECK_EQ(p.tick_calls.size(), size_t(5));
        for (size_t i = 0; i < p.tick_read_calls.size(); ++i) {
            CHECK(p.tick_calls[i].ticket == p.tick_read_calls[i].ticket);
        }
        for (size_t i = 0; i < p.tick_write_calls.size(); ++i) {
            CHECK(p.tick_calls[p.tick_read_calls.size() + i].ticket ==
                  p.tick_write_calls[i].ticket);
        }

        // THE ORDER, end to end: ANY (clock untouched) → pause → all paused
        // reads in one span → unpause → the tick batch.
        const int i_any   = step_index(p, StepKind::ServeAny);
        const int i_pause = step_index(p, StepKind::Pause);
        const int i_sp    = step_index(p, StepKind::ServePaused);
        const int i_unp   = step_index(p, StepKind::Unpause);
        const int i_dt    = step_index(p, StepKind::DrainTick);
        CHECK(i_any >= 0 && i_pause >= 0 && i_sp >= 0 && i_unp >= 0 && i_dt >= 0);
        CHECK(i_any < i_pause);
        CHECK(i_pause < i_sp);
        CHECK(i_sp < i_unp);
        CHECK(i_unp < i_dt);

        // THE INVARIANT THIS SECTION EXISTS FOR, stated over the batch the drain
        // will actually walk: no write is applied before any read.
        const std::vector<BufferedCall>& batch = p.steps[static_cast<size_t>(i_dt)].calls;
        CHECK_EQ(batch.size(), size_t(5));
        bool seen_write = false;
        for (const BufferedCall& c : batch) {
            if (c.polarity == Polarity::TICK_WRITE) seen_write = true;
            else                                    CHECK(!seen_write);
        }
        CHECK(seen_write);   // the case is only meaningful if writes were present

        ClockSim sim = apply_plan(p);
        CHECK_EQ(sim.pauses, 1);
        CHECK_EQ(sim.unpauses, 1);
        CHECK(!sim.state.paused);
        CHECK_EQ(sim.served, size_t(8));
    }

    // TICK_READ alone: no write step is emitted, and the read still gets a live
    // clock (an already-running clock is not touched).
    {
        FlushScheduler s;
        submit_n(s, "s1", "query_conquest_targets", Polarity::TICK_READ, 2, 0);
        // ck-on4u: at the RESTING speed, so "an already-running clock is not
        // touched" is about the pause and not about a literal that used to
        // agree with a hard-coded resting speed by accident.
        FlushPlan p = s.plan_flush(10000, ClockReading{false, kRegisteredGameSpeed},
                                   FlushTrigger::Cadence);
        CHECK_EQ(p.tick_read_calls.size(), size_t(2));
        CHECK_EQ(p.tick_write_calls.size(), size_t(0));
        CHECK_EQ(p.tick_calls.size(), size_t(2));
        CHECK_EQ(step_index(p, StepKind::DrainTick), 0);
        CHECK(!p.clock_touched());
        CHECK(!apply_plan(p).state.paused);
    }

    // TICK_READ alone on a DRIFTED (paused) clock: one unpause, no pause, and
    // the resting state is restored.
    {
        FlushScheduler s;
        submit_n(s, "s1", "query_conquest_targets", Polarity::TICK_READ, 1, 0);
        FlushPlan p = s.plan_flush(10000, ClockReading{true, 3}, FlushTrigger::Cadence);
        CHECK_EQ(p.pause_count(), size_t(0));
        CHECK_EQ(p.unpause_count(), size_t(1));
        CHECK(step_index(p, StepKind::Unpause) < step_index(p, StepKind::DrainTick));
        CHECK(!p.clock_after.paused);
    }

    // The batch bound is SHARED across the tick phase — it is one
    // MAX_ACTIONS_PER_TICK budget, not one per class.
    {
        FlushConfig cfg;
        cfg.max_tick_calls_per_flush = 8;
        cfg.early_flush_size = 1000;
        cfg.per_session_capacity = 1000;
        FlushScheduler s(cfg);
        submit_n(s, "s1", "query_valid_cbs_strict", Polarity::TICK_READ, 10, 0);
        submit_n(s, "s1", "declare_war", Polarity::TICK_WRITE, 10, 0);

        FlushPlan p = s.plan_flush(10000, ClockReading{false, 3}, FlushTrigger::Cadence);
        CHECK_EQ(p.tick_calls.size(), size_t(8));   // NOT 16
        CHECK_EQ(p.deferred.size(), size_t(12));
        CHECK_EQ(s.buffer_depth(), size_t(12));
        // Nothing was dropped: every ticket is either selected or deferred.
        CHECK_EQ(p.tick_calls.size() + p.deferred.size(), size_t(20));
    }

    // Fairness across the tick phase is by SESSION, not by class: one session
    // spamming tick-reads must not starve another session's tick-writes.
    {
        FlushConfig cfg;
        cfg.max_tick_calls_per_flush = 6;
        cfg.early_flush_size = 1000;
        cfg.per_session_capacity = 1000;
        FlushScheduler s(cfg);
        submit_n(s, "reader", "query_valid_cbs_strict", Polarity::TICK_READ, 20, 0);
        submit_n(s, "writer", "declare_war", Polarity::TICK_WRITE, 4, 0);

        FlushPlan p = s.plan_flush(10000, ClockReading{false, 3}, FlushTrigger::Cadence);
        CHECK_EQ(p.tick_calls.size(), size_t(6));
        CHECK_EQ(count_in(p.tick_read_calls, "reader"), size_t(3));
        CHECK_EQ(count_in(p.tick_write_calls, "writer"), size_t(3));  // NOT 0
        // ...and the reads are still applied ahead of the writes.
        CHECK_EQ(p.tick_calls.size(), size_t(6));
        CHECK(p.tick_calls[0].polarity == Polarity::TICK_READ);
        CHECK(p.tick_calls[5].polarity == Polarity::TICK_WRITE);
    }

    // Arrival order is preserved WITHIN each class.
    {
        FlushConfig cfg;
        cfg.max_tick_calls_per_flush = 16;
        FlushScheduler s(cfg);
        submit_n(s, "s1", "query_valid_cbs_strict", Polarity::TICK_READ, 3, 0);
        submit_n(s, "s1", "declare_war", Polarity::TICK_WRITE, 3, 0);
        FlushPlan p = s.plan_flush(10000, ClockReading{false, 3}, FlushTrigger::Cadence);
        for (size_t i = 1; i < p.tick_read_calls.size(); ++i) {
            CHECK(p.tick_read_calls[i - 1].seq < p.tick_read_calls[i].seq);
        }
        for (size_t i = 1; i < p.tick_write_calls.size(); ++i) {
            CHECK(p.tick_write_calls[i - 1].seq < p.tick_write_calls[i].seq);
        }
        // Every read precedes every write in the executed order, even though
        // some writes arrived before some reads would have in a FIFO.
        CHECK(p.tick_read_calls.back().seq < p.tick_write_calls.front().seq);
    }

    // A TICK_READ does not make an otherwise-ANY buffer skip the fast path by
    // accident, and it does not qualify for the all-ANY early flush.
    {
        FlushScheduler s;
        submit_n(s, "s1", "get_tick_info", Polarity::ANY, 2, 0);
        CHECK(s.due(1) == FlushTrigger::AllAny);
        submit_n(s, "s1", "query_valid_cbs_strict", Polarity::TICK_READ, 1, 1);
        CHECK(s.due(2) == FlushTrigger::None);
    }
}

// ===========================================================================
// 5. Round-robin fairness by session under an unequal load.
// ===========================================================================
static void test_round_robin_fairness() {
    section("round-robin fairness under unequal load");
    FlushConfig cfg;
    cfg.max_tick_calls_per_flush = 8;   // == MAX_ACTIONS_PER_TICK
    cfg.early_flush_size = 1000;        // keep the size trigger out of the way
    cfg.per_session_capacity = 1000;
    FlushScheduler s(cfg);

    // The greedy session speaks first AND loudest — a global FIFO would give it
    // all eight slots and starve the other two.
    submit_n(s, "greedy", "declare_war", Polarity::TICK_WRITE, 20, 0);
    submit_n(s, "polite_a", "send_interaction", Polarity::TICK_WRITE, 2, 0);
    submit_n(s, "polite_b", "send_interaction", Polarity::TICK_WRITE, 2, 0);

    FlushPlan p = s.plan_flush(10000, ClockReading{false, 3}, FlushTrigger::Cadence);
    CHECK_EQ(p.tick_write_calls.size(), size_t(8));
    CHECK_EQ(count_in(p.tick_write_calls, "polite_a"), size_t(2));
    CHECK_EQ(count_in(p.tick_write_calls, "polite_b"), size_t(2));
    CHECK_EQ(count_in(p.tick_write_calls, "greedy"), size_t(4));  // NOT 8

    // Nothing was lost: the rest is deferred, with its tickets, in arrival order.
    CHECK_EQ(p.deferred.size(), size_t(16));
    CHECK_EQ(s.buffer_depth(), size_t(16));
    for (size_t i = 1; i < p.deferred.size(); ++i) {
        CHECK(p.deferred[i - 1].seq < p.deferred[i].seq);
    }

    // Rotation: the head of the session list does not win every flush.
    FlushConfig c2;
    c2.max_tick_calls_per_flush = 4;
    c2.early_flush_size = 1000;
    c2.per_session_capacity = 1000;
    FlushScheduler r(c2);
    submit_n(r, "A", "declare_war", Polarity::TICK_WRITE, 10, 0);
    submit_n(r, "B", "declare_war", Polarity::TICK_WRITE, 10, 0);

    FlushPlan f1 = r.plan_flush(10000, ClockReading{false, 3}, FlushTrigger::Cadence);
    CHECK_EQ(f1.tick_write_calls.size(), size_t(4));
    CHECK_EQ(count_in(f1.tick_write_calls, "A"), size_t(2));
    CHECK_EQ(count_in(f1.tick_write_calls, "B"), size_t(2));
    CHECK(f1.tick_write_calls[0].session_id == "A");

    FlushPlan f2 = r.plan_flush(20000, ClockReading{false, 3}, FlushTrigger::Cadence);
    CHECK_EQ(f2.tick_write_calls.size(), size_t(4));
    CHECK_EQ(count_in(f2.tick_write_calls, "A"), size_t(2));
    CHECK_EQ(count_in(f2.tick_write_calls, "B"), size_t(2));
    CHECK(f2.tick_write_calls[0].session_id == "B");  // start rotated

    // And no session starves across flushes: everything drains.
    size_t a_total = count_in(f1.tick_write_calls, "A") + count_in(f2.tick_write_calls, "A");
    size_t b_total = count_in(f1.tick_write_calls, "B") + count_in(f2.tick_write_calls, "B");
    CHECK_EQ(a_total, b_total);
}

// ===========================================================================
// 6. Back-pressure: buffer_full, never a silent drop.
// ===========================================================================
static void test_back_pressure() {
    section("back-pressure returns buffer_full, never drops");
    FlushConfig cfg;
    cfg.buffer_capacity = 4;
    cfg.per_session_capacity = 100;
    cfg.early_flush_size = 1000;
    cfg.retry_after_ms = 250;
    FlushScheduler s(cfg);

    std::vector<uint64_t> tickets;
    for (int i = 0; i < 4; ++i) {
        SubmitResult r = s.submit(CallRequest{"s1", "query_realm", Polarity::PAUSED}, 0);
        CHECK(r.accepted);
        tickets.push_back(r.ticket);
    }

    SubmitResult over = s.submit(CallRequest{"s1", "query_realm", Polarity::PAUSED}, 0);
    CHECK(!over.accepted);
    CHECK(over.reason == "buffer_full");
    CHECK_EQ(over.retry_after_ms, uint64_t(250));
    CHECK_EQ(over.ticket, uint64_t(0));          // no ticket minted for a refusal
    CHECK(over.limit == BackPressureLimit::Global);

    // The refusal did not evict anything: all four earlier calls still flush.
    CHECK_EQ(s.buffer_depth(), size_t(4));
    FlushPlan p = s.plan_flush(10000, ClockReading{false, 3}, FlushTrigger::Cadence);
    CHECK_EQ(p.paused_calls.size(), size_t(4));
    for (size_t i = 0; i < tickets.size(); ++i) {
        bool present = false;
        for (const BufferedCall& c : p.paused_calls) if (c.ticket == tickets[i]) present = true;
        CHECK(present);
    }
    // Capacity is released by the flush.
    CHECK(s.submit(CallRequest{"s1", "query_realm", Polarity::PAUSED}, 10001).accepted);

    // Per-session bound: one socket cannot fill a global buffer.
    FlushConfig c2;
    c2.buffer_capacity = 100;
    c2.per_session_capacity = 2;
    c2.early_flush_size = 1000;
    FlushScheduler s2(c2);
    CHECK(s2.submit(CallRequest{"hog", "declare_war", Polarity::TICK_WRITE}, 0).accepted);
    CHECK(s2.submit(CallRequest{"hog", "declare_war", Polarity::TICK_WRITE}, 0).accepted);
    SubmitResult hog = s2.submit(CallRequest{"hog", "declare_war", Polarity::TICK_WRITE}, 0);
    CHECK(!hog.accepted);
    CHECK(hog.reason == "buffer_full");
    CHECK(hog.limit == BackPressureLimit::Session);
    CHECK(s2.submit(CallRequest{"other", "declare_war", Polarity::TICK_WRITE}, 0).accepted);
    CHECK_EQ(s2.buffer_depth(), size_t(3));
}

// ===========================================================================
// 7. Early flush: each of the three conditions fires, and none fires spuriously.
// ===========================================================================
static void test_early_flush_conditions() {
    section("early-flush conditions (a)(b)(c) + no spurious fire");

    // (a) all-ANY.
    {
        FlushScheduler s;
        CHECK(s.due(0) == FlushTrigger::None);          // empty buffer, not due
        submit_n(s, "s1", "get_tick_info", Polarity::ANY, 1, 0);
        CHECK(s.due(1) == FlushTrigger::AllAny);
        // ...but one non-ANY call in the buffer kills the fast path.
        submit_n(s, "s1", "query_realm", Polarity::PAUSED, 1, 1);
        CHECK(s.due(2) == FlushTrigger::None);
    }

    // (b) every registered session has submitted since the last flush.
    {
        FlushScheduler s;
        s.register_session("A");
        s.register_session("B");
        CHECK_EQ(s.registered_session_count(), size_t(2));
        submit_n(s, "A", "query_realm", Polarity::PAUSED, 1, 0);
        CHECK(s.due(1) == FlushTrigger::None);          // B has not spoken
        submit_n(s, "B", "query_realm", Polarity::PAUSED, 1, 2);
        CHECK(s.due(3) == FlushTrigger::AllSessionsSubmitted);

        FlushPlan p = s.plan_flush(3, ClockReading{false, 3}, FlushTrigger::AllSessionsSubmitted);
        CHECK_EQ(p.selected_count(), size_t(2));

        // The set resets: the next flush needs a fresh round of submissions, and
        // a silent B never holds the world — the cadence still fires.
        submit_n(s, "A", "query_realm", Polarity::PAUSED, 1, 4);
        CHECK(s.due(5) == FlushTrigger::None);
        CHECK(s.due(10003) == FlushTrigger::Cadence);
    }

    // (c) size bound.
    {
        FlushConfig cfg;
        cfg.early_flush_size = 3;
        FlushScheduler s(cfg);
        submit_n(s, "s1", "query_realm", Polarity::PAUSED, 2, 0);
        CHECK(s.due(1) == FlushTrigger::None);
        submit_n(s, "s1", "query_realm", Polarity::PAUSED, 1, 1);
        CHECK(s.due(2) == FlushTrigger::SizeBound);
    }

    // No spurious fire: a partial buffer, below the size bound, with a session
    // still silent, before the period, is NOT due.
    {
        FlushConfig cfg;
        cfg.period_ms = 10000;
        cfg.early_flush_size = 32;
        FlushScheduler s(cfg);
        s.register_session("A");
        s.register_session("B");
        submit_n(s, "A", "declare_war", Polarity::TICK_WRITE, 5, 0);
        CHECK(s.due(1) == FlushTrigger::None);
        CHECK(s.due(9999) == FlushTrigger::None);
        CHECK(s.due(10000) == FlushTrigger::Cadence);   // and then only the cadence

        // A not-due tick() is a no-op: no flush id burned, no buffer consumed.
        FlushScheduler s2(cfg);
        s2.register_session("A");
        s2.register_session("B");
        submit_n(s2, "A", "declare_war", Polarity::TICK_WRITE, 5, 0);
        FlushPlan noop = s2.tick(1, ClockReading{false, 3});
        CHECK(noop.trigger == FlushTrigger::None);
        CHECK_EQ(noop.steps.size(), size_t(0));
        CHECK(!noop.clock_touched());
        CHECK_EQ(s2.buffer_depth(), size_t(5));
        CHECK_EQ(s2.last_flush_id(), uint64_t(0));
    }
}

// ===========================================================================
// 8. The resting state after every flush is RUNNING at the configured speed.
// ===========================================================================
static void test_resting_state() {
    section("resting state after a flush: RUNNING at the configured speed");
    const int kResting = 3;

    struct Case { bool paused_before; int speed_before; bool any, paused, tick; };
    const Case cases[] = {
        {false, 3, false, true,  false},
        {false, 3, false, false, true },
        {false, 3, true,  true,  true },
        {true,  3, false, true,  true },
        {true,  1, true,  false, true },
        {false, 5, true,  true,  false},
        {true,  5, false, true,  false},
        {false, 5, false, false, true },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const Case& c = cases[i];
        FlushConfig cfg;
        cfg.resting_speed = kResting;
        cfg.early_flush_size = 1000;
        FlushScheduler s(cfg);
        if (c.any)    submit_n(s, "s1", "get_tick_info", Polarity::ANY, 1, 0);
        if (c.paused) submit_n(s, "s1", "query_realm", Polarity::PAUSED, 2, 0);
        if (c.tick)   submit_n(s, "s2", "declare_war", Polarity::TICK_WRITE, 2, 0);

        FlushPlan p = s.plan_flush(10000, ClockReading{c.paused_before, c.speed_before},
                                   FlushTrigger::Cadence);
        ClockSim sim = apply_plan(p);

        // Simulated end state and the plan's own claim must agree...
        CHECK(sim.state.paused == p.clock_after.paused);
        CHECK(sim.state.speed == p.clock_after.speed);
        // ...and both must be the resting state: RUNNING at the configured speed.
        CHECK(!p.clock_after.paused);
        CHECK_EQ(p.clock_after.speed, kResting);
        // At most one pause span per flush, always.
        CHECK(p.pause_count() <= size_t(1));
        CHECK(p.unpause_count() <= size_t(1));
    }

    // The all-ANY row is the documented exception to "ends RUNNING": it ends in
    // whatever state it found, because it changes nothing. Asserted in
    // test_all_any_never_touches_clock().
}

int main() {
    std::printf("test_flush_scheduler (ck-6kvq.8)\n");
    test_partition();
    test_all_any_never_touches_clock();
    test_one_span_and_ordering();
    test_tick_read_before_tick_write();
    test_round_robin_fairness();
    test_back_pressure();
    test_early_flush_conditions();
    test_resting_state();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
