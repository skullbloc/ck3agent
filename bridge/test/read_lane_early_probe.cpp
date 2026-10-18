// read_lane_early_probe.cpp — (D') COSTED AND CONSTRAINED (ck-h6nx).
//
// Drives the SHIPPING flush_scheduler.cpp with wall time passed in. Emits one
// TSV row per shape so scripts/acceptance_read_lane_early.py can assert on
// numbers rather than on prose.
//
// TWO THINGS EVERY ROW REPORTS, because the second is gating:
//   waits        the settlement latency the bead was filed for
//   pause_spans  ck-cyhr's blast radius was proportional to the NUMBER of pause
//                windows, not their duration — the meter opens one per read, so
//                a defect keyed on window count fires on EVERY SAMPLE.
//
// AND THE ONE THAT DECIDES ADMISSIBILITY: by_read_flush, the count of agent
// TICK_WRITEs carried by a read-lane flush. It must be ZERO. Plain (D) — an
// observer trigger that serves the whole buffer — was ruled out precisely
// because an agent's action would then land at a moment chosen by the meter.
//
// THE RUN IS SPECIFIED IN CADENCE SWEEPS, NEVER SECONDS. The sweep count in a
// fixed wall window is a function of GAME SPEED, and game speed is a
// pre-registered condition — so a run stated in seconds silently depends on the
// speed pin and says nothing if the pin moves.

#include "flush_scheduler.h"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace bridge;

namespace {

void submit(FlushScheduler& s, const char* sess, Polarity p, uint64_t now) {
    CallRequest r; r.session_id = sess; r.action = "query_dejure_progress";
    r.polarity = p; s.submit(r, now);
}

struct Row {
    int polls = 0, pause_spans = 0, by_read_flush = 0, writes_served = 0;
    uint64_t wait_total = 0, wait_max = 0;
};

// `privileged` empty  -> today's scheduler behaviour (no (D') trigger).
// `bypass`            -> (B), the read never enters the buffer.
Row run(uint64_t dur_ms, uint64_t poll_gap_ms, uint64_t agent_gap_ms,
        const char* privileged, bool bypass) {
    FlushConfig cfg;
    if (privileged && *privileged) cfg.privileged_reader = privileged;
    FlushScheduler s(cfg);
    s.register_session("observer");
    if (agent_gap_ms) s.register_session("agent");

    Row out;
    uint64_t next_agent = agent_gap_ms, next_poll = 0, issued = 0;
    bool outstanding = false;
    for (uint64_t now = 0; now < dur_ms; ++now) {
        if (agent_gap_ms && now >= next_agent) {
            submit(s, "agent", Polarity::TICK_WRITE, now);
            next_agent += agent_gap_ms;
        }
        if (!outstanding && now >= next_poll) {
            if (bypass) { ++out.polls; next_poll = now + poll_gap_ms; }
            else { submit(s, "observer", Polarity::PAUSED, now);
                   issued = now; outstanding = true; }
        }
        if (s.due(now) != FlushTrigger::None) {
            const FlushPlan p = s.tick(now, ClockReading{false, 3});
            for (const PlanStep& st : p.steps)
                if (st.kind == StepKind::Pause) ++out.pause_spans;
            out.writes_served += (int)p.tick_calls.size();
            if (p.trigger == FlushTrigger::ReadLaneEarly)
                out.by_read_flush += (int)p.tick_calls.size();
            if (outstanding && !p.paused_calls.empty()) {
                const uint64_t w = now - issued;
                out.wait_total += w; if (w > out.wait_max) out.wait_max = w;
                ++out.polls; outstanding = false; next_poll = now + poll_gap_ms;
            }
        }
    }
    return out;
}

void emit(const char* shape, uint64_t dur, uint64_t gap, uint64_t agent,
          const Row& r) {
    std::printf("%s\t%llu\t%llu\t%llu\t%d\t%llu\t%llu\t%d\t%d\t%d"
                "\t%d\t%d\t%d\n",
                shape, (unsigned long long)dur, (unsigned long long)gap,
                (unsigned long long)agent, r.polls,
                (unsigned long long)(r.wait_total / (r.polls ? r.polls : 1)),
                (unsigned long long)r.wait_max, r.pause_spans,
                r.by_read_flush, r.writes_served, 0, 0, 0);
}

}  // namespace

int main(int argc, char** argv) {
    // Duration is handed in as SWEEPS x ms-per-sweep by the caller, which owns
    // the speed-pin arithmetic; this file never converts seconds to sweeps.
    const uint64_t dur = (argc > 1) ? strtoull(argv[1], nullptr, 10) : 600000;
    std::printf("shape\tdur_ms\tpoll_gap_ms\tagent_gap_ms\tpolls\tmean_wait_ms"
                "\tmax_wait_ms\tpause_spans\tby_read_flush\twrites_served"
                // ck-t5bc: THEIR OWN COLUMNS, not a reuse of by_read_flush. A
                // column that means one thing in most rows and another in one
                // row is read wrongly by anything that scans all rows — §D does
                // exactly that, and my first version of this row tripped it.
                "\tdeferred_want\tdeferred_got\ttrigger_cadence\n");

    for (uint64_t gap : {2000ULL, 300ULL, 100ULL}) {
        emit("e0_observer_alone", dur, gap, 0ULL, run(dur, gap, 0, "", false));
        emit("today_arm",  dur, gap, 120000ULL, run(dur, gap, 120000, "", false));
        emit("dprime_arm", dur, gap, 120000ULL,
             run(dur, gap, 120000, "observer", false));
        emit("b_bypass",   dur, gap, 120000ULL, run(dur, gap, 120000, "", true));
    }
    // THE ADVERSARIAL AGENT CADENCES. The 120 s figure is an ASSUMPTION nobody
    // could verify, so by_read_flush must read zero across a RANGE, including a
    // cadence chosen to MAXIMISE the number of buffered writes available to be
    // wrongly carried. A property demonstrated over an unknown number of
    // opportunities is still a demonstration; this makes the number known.
    for (uint64_t ag : {120000ULL, 10000ULL, 1000ULL, 100ULL, 10ULL, 1ULL}) {
        emit("dprime_adversarial", dur, 2000ULL, ag,
             run(dur, 2000, ag, "observer", false));
    }
    // ── THE DISCRIMINATING INTERLEAVING ────────────────────────────────────
    // THE ADVERSARIAL CADENCE SWEEP ABOVE CANNOT PRODUCE THE CASE, and that is
    // worth stating rather than discovering: ReadLaneEarly is placed AFTER
    // condition (b), so whenever an agent has just spoken, (b) fires and serves
    // everything. A FAST agent therefore makes (b) fire constantly and
    // ReadLaneEarly never runs at all — the sweep maximises write VOLUME and
    // never the INTERLEAVING that matters.
    //
    // The case that matters is narrow: writes DEFERRED by the per-flush tick
    // bound (max_tick_calls_per_flush = 8) surviving into a later flush in which
    // THE AGENT HAS BEEN SILENT, so ReadLaneEarly is the trigger that reaches
    // them. Constructed here explicitly, because a shape that cannot contain the
    // case cannot be evidence about it.
    {
        FlushConfig cfg; cfg.privileged_reader = "observer";
        FlushScheduler s(cfg);
        s.register_session("observer"); s.register_session("agent");
        const int SUBMITTED = 20;
        for (int i = 0; i < SUBMITTED; ++i) {
            CallRequest w; w.session_id = "agent"; w.action = "a";
            w.polarity = Polarity::TICK_WRITE; s.submit(w, 100);
        }
        CallRequest r0; r0.session_id = "observer"; r0.action = "q";
        r0.polarity = Polarity::PAUSED; s.submit(r0, 100);
        const FlushPlan p0 = s.tick(100, ClockReading{false, 3});
        CallRequest r1; r1.session_id = "observer"; r1.action = "q";
        r1.polarity = Polarity::PAUSED; s.submit(r1, 200);
        const FlushPlan p1 = s.tick(200, ClockReading{false, 3});
        // ── DEFERRED IS NOT DROPPED (ck-t5bc's mutant) ──────────────────────
        // by_read_flush == 0 is IDENTICAL whether ReadLaneEarly handed the
        // writes to the next cadence flush or THREW THEM AWAY. "Did not happen
        // early" and "happened later" are different facts and every other row
        // measures only the first. Deleting the single `tick_def.push_back(tc)`
        // clears the writes and defers nothing; on the arm path that is a
        // manipulation that never reached the game.
        //
        // MEASURE CONSERVATION, NOT THE DEFERRED SET. My first attempt counted
        // TICK_WRITEs in p1.deferred and required it to be non-zero — and the
        // MUTANT PASSED IT 12/12, because `deferred` also carries the writes the
        // ordinary batch bound never selected. Those are still there when the
        // deferral is deleted, so the number I measured moved for a reason that
        // had nothing to do with the thing under test. The set that vanishes is
        // exactly the one ReadLaneEarly CLEARED, and it is unobservable in the
        // plan it was cleared from.
        //
        // So: submit N, drain to quiescence, and require ALL N to be SERVED.
        // A dropped write cannot hide in a total.
        int served_total = 0;
        for (int i = 0; i < 40 && served_total < SUBMITTED; ++i) {
            const FlushPlan pn = s.tick(200 + (i + 1) * (uint64_t)cfg.period_ms + 1,
                                        ClockReading{false, 3});
            served_total += (int)pn.tick_calls.size();
            if (pn.tick_calls.empty() && pn.deferred.empty()) break;
        }
        served_total += (int)p0.tick_calls.size() + (int)p1.tick_calls.size();

        std::printf("deferred_then_silent\t0\t0\t0\t2\t0\t0\t%d\t%d\t%d"
                    "\t0\t0\t0\n",
                    (int)(p0.steps.size() && p1.steps.size() ? 2 : 0),
                    (int)(p1.trigger == FlushTrigger::ReadLaneEarly
                              ? p1.tick_calls.size() : -1),
                    (int)p0.tick_calls.size());
        // ITS OWN COLUMNS: by_read_flush stays 0 here and keeps its meaning, so
        // §D's scan over every row reads this row correctly.
        //   deferred_want := writes SUBMITTED, deferred_got := writes SERVED
        std::printf("deferred_arrival\t0\t0\t0\t0\t0\t0\t0\t0\t0"
                    "\t%d\t%d\t%d\n",
                    SUBMITTED, served_total, 1);
    }
    return 0;
}
