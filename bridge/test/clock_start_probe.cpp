// clock_start_probe.cpp — DOES THE CLOCK START WHEN THE EPISODE DOES? (ck-zen6)
//
// In pipeline mode the bridge owns the clock from LOAD and the world runs ~1.9
// game-days per SECOND. Between world-ACTIVE and episode-open the driver does
// five provenance legs, a steer probe and a separation witness — and that
// preflight burned more than the 30-day start_witness threshold, so every arm
// episode opened at a drifted date. A shakedown world was measured 806 game-days
// past 1066.9.15 with start_witness correctly reading "diverged".
//
// THE RULE: while no episode observer holds the read-lane privilege there is no
// episode, so the world RESTS PAUSED. The observer's declaration starts the
// clock. Bring-up and preflight are not the episode.
//
// Drives the REAL bridge/src/flush_scheduler.cpp. Wall time is passed in; no
// game, no bridge, no socket. Prints one TSV row per case.
#include "flush_scheduler.h"
#include "tick_bookkeeping.h"
#include "clock_rate.h"

#include <cstdio>
#include <utility>
#include <string>

using namespace bridge;

namespace {

// Run `flushes` cadence flushes and report what the clock did. `unpauses` is the
// count of Unpause steps ever emitted — the thing that would let the world run.
struct Run { int unpauses = 0, pauses = 0, setspeeds = 0;
             bool ended_paused = true; int ended_speed = -1; };

Run drive(bool hold, const char* observer, int flushes) {
    FlushConfig cfg;
    cfg.hold_clock_until_observer = hold;
    FlushScheduler s(cfg);
    s.register_session("observer");
    if (observer && *observer) s.set_privileged_reader(observer);

    Run r;
    // The world arrives from LOAD already running, which is the state this gate
    // has to correct — starting it paused would make the test assume the answer.
    // CK3 boots at speed 3 and the declared pin is 2 — the world arrives in
    // the wrong state on BOTH axes, which is what made an arm unable to start.
    ClockReading clock{false, 3};
    uint64_t now = 0;
    for (int i = 0; i < flushes; ++i) {
        // A PAUSED-POLARITY READ EACH FLUSH — the preflight shape. The driver
        // does five provenance legs, a steer probe and a separation witness
        // before the episode opens, and those are reads. WITHOUT WORK a flush is
        // an all-ANY heartbeat that touches the clock at all by an invariant the
        // planner calls absolute, so a probe that submits nothing exercises
        // nothing: my first version drove twenty empty flushes and reported
        // zeros for every case, including the defect.
        CallRequest q;
        q.session_id = "observer";
        q.action = "query_bridge_state";
        q.polarity = Polarity::PAUSED;
        s.submit(q, now);
        now += cfg.period_ms + 1;
        const FlushPlan p = s.tick(now, clock);
        for (const PlanStep& st : p.steps) {
            if (st.kind == StepKind::Unpause)  ++r.unpauses;
            if (st.kind == StepKind::Pause)    ++r.pauses;
            if (st.kind == StepKind::SetSpeed) ++r.setspeeds;
        }
        clock = p.clock_after;          // the world keeps what the plan left it
        r.ended_paused = clock.paused;
        r.ended_speed = clock.speed;
    }
    return r;
}

void row(const char* name, const Run& r) {
    std::printf("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", name, r.unpauses, r.pauses,
                r.setspeeds, r.ended_paused ? 1 : 0, r.ended_speed, 0, 0);
}

}  // namespace

int main() {
    std::printf("case\tunpauses\tpauses\tsetspeeds\tended_paused\tended_speed"
                // ck-8o5j: THEIR OWN COLUMNS. Packing a release-transition
                // into unpauses/pauses/setspeeds made `after` land in the
                // column labelled `pauses` — the same column-reuse mistake I
                // had already written down once tonight. A field that means
                // one thing in most rows and another in one row is read
                // wrongly by anything that scans all rows.
                "\tunpauses_before_release\tpaused_at_release\n");

    // THE DEFECT, and the shape of bring-up: gate ARMED, nobody has declared.
    // Twenty cadence periods of preflight must cost ZERO game time.
    row("armed_no_observer", drive(true, "", 20));

    // AN OBSERVER AND NO EPISODE. Under ck-zen6 this released the clock; it is
    // now the HAZARD CASE and must stay PAUSED. The only declarer in the tree is
    // the standalone meter lane, which an operator can start against a pipeline
    // bridge before the driver opens the episode — and its declaration then ran
    // the world through the preflight, which is ck-zen6's own defect by the
    // other door (ck-9rto).
    row("observer_only_no_episode", drive(true, "sess_meter", 20));

    // E0's FREE RUN. Null mode does not own the clock, so the gate is off and
    // nothing changes — a control that must stay green, because gating it would
    // freeze the baseline.
    row("unarmed_no_observer", drive(false, "", 20));

    // THE TRANSITION, which is the claim that matters: the same scheduler,
    // paused through preflight, then declaring. The clock starts exactly there.
    {
        FlushConfig cfg;
        cfg.hold_clock_until_observer = true;
        FlushScheduler s(cfg);
        s.register_session("observer");
        ClockReading clock{false, 3};
        uint64_t now = 0;
        int before = 0, after = 0;
        auto submit = [&](uint64_t at) {
            CallRequest q; q.session_id = "observer";
            q.action = "query_bridge_state"; q.polarity = Polarity::PAUSED;
            s.submit(q, at);
        };
        for (int i = 0; i < 10; ++i) {          // preflight
            submit(now);
            now += cfg.period_ms + 1;
            const FlushPlan p = s.tick(now, clock);
            for (const PlanStep& st : p.steps)
                if (st.kind == StepKind::Unpause) ++before;
            clock = p.clock_after;
        }
        const bool paused_at_declare = clock.paused;
        // AN OBSERVER DECLARES AND NOTHING HAPPENS — the retirement, driven.
        s.set_privileged_reader("sess_meter");
        for (int i = 0; i < 10; ++i) {
            submit(now);
            now += cfg.period_ms + 1;
            const FlushPlan p = s.tick(now, clock);
            for (const PlanStep& st : p.steps)
                if (st.kind == StepKind::Unpause) ++after;
            clock = p.clock_after;
        }
        std::printf("observer_declares_mid_preflight\t%d\t0\t0\t%d\t%d\t%d\t%d\n",
                    after, clock.paused ? 1 : 0, clock.speed,
                    before, paused_at_declare ? 1 : 0);
    }
    // ── ck-8o5j: RELEASED BY THE EPISODE, WITH NO OBSERVER EVER ──────────
    // The arm path never declares an episode observer — the flag is sent in
    // exactly one place in the tree, meter_lane.run_live(), and the driver
    // embeds the CLASS on its own client. So the gate must also release on the
    // episode's own opening, which disarms the hold.
    {
        FlushConfig cfg;
        cfg.hold_clock_until_observer = true;
        FlushScheduler s(cfg);
        s.register_session("driver");
        ClockReading clock{false, 3};
        uint64_t now = 0;
        auto submit = [&](uint64_t at) {
            CallRequest q; q.session_id = "driver";
            q.action = "query_bridge_state"; q.polarity = Polarity::PAUSED;
            s.submit(q, at);
        };
        int before = 0, after = 0;
        for (int i = 0; i < 10; ++i) {            // preflight, nobody declares
            submit(now); now += cfg.period_ms + 1;
            const FlushPlan p = s.tick(now, clock);
            for (const PlanStep& st : p.steps)
                if (st.kind == StepKind::Unpause) ++before;
            clock = p.clock_after;
        }
        const bool paused_at_open = clock.paused;
        // THE EPISODE OPENS — no observer, no privileged_reader, ever.
        s.set_hold_clock_until_observer(false);
        for (int i = 0; i < 10; ++i) {
            submit(now); now += cfg.period_ms + 1;
            const FlushPlan p = s.tick(now, clock);
            for (const PlanStep& st : p.steps)
                if (st.kind == StepKind::Unpause) ++after;
            clock = p.clock_after;
        }
        std::printf("episode_open_no_observer\t%d\t0\t0\t%d\t%d\t%d\t%d\n",
                    after, clock.paused ? 1 : 0, clock.speed,
                    before, paused_at_open ? 1 : 0);
    }

    // ── ck-zffq: THE CLOCK RATE IS NOT EPISODE-SCOPED ───────────────────────
    // Both recordings used to sit inside one `if (episode_is_experiment())`, so
    // on a world that had run away unattended the meter read measured=false,
    // samples=0 WHILE THE CLOCK ADVANCED 3,351 GAME-DAYS. An instrument gated on
    // the condition it would detect cannot detect its violation.
    //
    // DRIVEN, not read: the scoping is a NAMED PURE POLICY and the meter is
    // driven through it in BOTH modes. THE FREE-PLAY ROW IS THE WHOLE POINT —
    // the experiment row passes with or without the fix, so on its own it would
    // be a check that cannot fail for the reason it names.
    {
        const TickBookkeeping exp = tick_bookkeeping(true);
        const TickBookkeeping fre = tick_bookkeeping(false);

        // And the meter must MEASURE from what free play would feed it.
        // PAST kMinWallSeconds ON PURPOSE. "NOT MEASURED is a real answer" and
        // a window too short publishes nothing — so a free-play meter fed a
        // 5-second window would read measured=false for the RIGHT reason and
        // this row would pass whether or not the gate had been removed. The
        // window is chosen relative to the bound it must clear.
        ClockRateMeter m;
        uint64_t wall = 0;
        for (int i = 0; i < 90; ++i) {
            m.note(fre, 100 + i * 2, wall, 2, false);   // UNCONDITIONAL, as the
            wall += 1000;                                // conductor now calls it
        }
        const ClockRateSample cs = m.get();

        // AND THE METER HONOURS A NON-RECORDING POLICY. This is the leg that
        // closes Z3: the decision now lives INSIDE note(), so a version that
        // ignored the policy would record here and this row would red. Before
        // the move the gate was at the CALL SITE and nothing on a host could
        // see a caller that re-added it.
        ClockRateMeter off;
        uint64_t w2 = 0;
        for (int i = 0; i < 90; ++i) {
            off.note(TickBookkeeping{true, false}, 100 + i * 2, w2, 2, false);
            w2 += 1000;
        }
        const ClockRateSample cso = off.get();

        std::printf("bk_experiment_notes_both\t%d\t0\t0\t%d\t0\t0\t0\n",
                    exp.note_unavailability ? 1 : 0, exp.note_clock_rate ? 1 : 0);
        std::printf("bk_freeplay_rate_only\t%d\t0\t0\t%d\t0\t0\t0\n",
                    fre.note_unavailability ? 1 : 0, fre.note_clock_rate ? 1 : 0);
        std::printf("bk_freeplay_meter_MEASURES\t%d\t0\t0\t%llu\t0\t0\t0\n",
                    cs.measured ? 1 : 0, (unsigned long long)cs.samples);
        std::printf("bk_meter_HONOURS_a_no_record_policy\t%d\t0\t0\t%llu\t0\t0\t0\n",
                    cso.measured ? 1 : 0, (unsigned long long)cso.samples);
    }

    // ── ck-43u3: THE TRIGGER IS COUNTED, AND THE COUNT MOVES ────────────────
    // An arm's reads cost 10.0 s apiece — flush_period_ms to the millisecond —
    // and working out WHY meant reading the scheduler's trigger list in source,
    // because the trigger was never published. Publishing "the keys exist" is
    // not enough: a version that NEVER RECORDS looks identical on a probe that
    // runs no flushes. So drive two flushes with DIFFERENT triggers and require
    // the counts to move independently.
    {
        FlushConfig cfg;
        cfg.hold_clock_until_observer = false;
        FlushScheduler s(cfg);
        s.register_session("reader");
        // A SECOND REGISTERED SESSION THAT NEVER SPEAKS, and it is not scenery.
        // With ONE session, trigger (b) ALL-SPOKE fires first and SHADOWS the
        // read lane — my first version of this case counted 0 for
        // read_lane_early and looked like a broken counter. It was the trigger
        // ORDER, and it is exactly the arm's situation: several clients share
        // the socket, one of them is quiet, so (b) cannot fire and the reads
        // fall through to CADENCE unless (c) is available to them.
        s.register_session("quiet_peer");
        s.set_privileged_reader("reader");
        ClockReading clock{true, 2};
        uint64_t now = 0;

        // AN ALL-ANY BUFFER: the fast path, which needs no clock cycle at all.
        CallRequest q; q.session_id = "reader";
        q.action = "get_tick_info"; q.polarity = Polarity::ANY;
        s.submit(q, now);
        now += 1;
        FlushPlan p1 = s.tick(now, clock);
        clock = p1.clock_after;
        const uint64_t all_any_after_1 = s.trigger_count(FlushTrigger::AllAny);

        // A PAUSED read from the PRIVILEGED reader: not all-ANY, so the read
        // lane's own trigger is the one that must fire.
        CallRequest q2; q2.session_id = "reader";
        q2.action = "query_character"; q2.polarity = Polarity::PAUSED;
        s.submit(q2, now);
        now += 1;
        FlushPlan p2 = s.tick(now, clock);
        const uint64_t rle_after_2 = s.trigger_count(FlushTrigger::ReadLaneEarly);
        const bool last_is_rle = (s.last_trigger() == FlushTrigger::ReadLaneEarly);

        std::printf("trigger_all_any_counted\t%llu\t0\t0\t0\t0\t0\t0\n",
                    (unsigned long long)all_any_after_1);
        std::printf("trigger_read_lane_counted\t%llu\t0\t0\t%d\t0\t0\t0\n",
                    (unsigned long long)rle_after_2, last_is_rle ? 1 : 0);
    }

    // ── ck-y6fp: DOES THE RELEASE HAVE AN INVERSE? ───────────────────────────
    // A refusal AFTER the release leaves the world running — 601 game-days with
    // no episode, driver or agent, measured on the deploy-13 arm-C pilot. The
    // release is a state transition with no inverse on the failure path.
    //
    // BEFORE designing a trigger, establish that re-arming RE-PAUSES A RUNNING
    // WORLD at all. It is not obvious: the armed branch rests PAUSED, but
    // "rests paused" could mean "declines to unpause" rather than "pauses what
    // is already running", and those differ by exactly the case this bead is
    // about. Reading cannot tell them apart — both defects here were invisible
    // to reading and obvious to one sampler.
    {
        FlushConfig cfg;
        cfg.hold_clock_until_observer = true;
        FlushScheduler s(cfg);
        s.register_session("driver");
        ClockReading clock{false, 3};
        uint64_t now = 0;
        auto submit = [&](uint64_t at) {
            CallRequest q; q.session_id = "driver";
            q.action = "query_bridge_state"; q.polarity = Polarity::PAUSED;
            s.submit(q, at);
        };
        auto run = [&](int n) {
            int unpauses = 0, pauses = 0;
            for (int i = 0; i < n; ++i) {
                submit(now); now += cfg.period_ms + 1;
                const FlushPlan p = s.tick(now, clock);
                for (const PlanStep& st : p.steps) {
                    if (st.kind == StepKind::Unpause) ++unpauses;
                    if (st.kind == StepKind::Pause)   ++pauses;
                }
                clock = p.clock_after;
            }
            return std::pair<int,int>(unpauses, pauses);
        };
        run(5);                                   // preflight, held
        const int paused_held = clock.paused ? 1 : 0;
        s.set_hold_clock_until_observer(false);   // THE EPISODE OPENS
        run(5);
        const int running_after_release = clock.paused ? 0 : 1;
        // THE REFUSAL. Everything downstream fails; the hold is re-armed.
        s.set_hold_clock_until_observer(true);
        const auto rearm = run(5);
        const int paused_after_rearm = clock.paused ? 1 : 0;
        const int speed_after_rearm = clock.speed;
        // AND THE RE-ARM MUST NOT BE ONE-WAY EITHER. A hold that re-pauses but
        // leaves the world unable to resume at the registered speed would turn
        // a spurious re-arm into a dead episode, which is the ck-zen6 failure
        // wearing the fix's clothes.
        s.set_hold_clock_until_observer(false);
        run(5);
        std::printf("rearm_after_release\t%d\t%d\t0\t%d\t%d\t%d\t%d\n",
                    rearm.first, rearm.second, paused_after_rearm,
                    speed_after_rearm, paused_held, running_after_release);
        std::printf("rearm_then_release_again\t0\t0\t0\t%d\t%d\t0\t0\n",
                    clock.paused ? 1 : 0, clock.speed);
    }
    return 0;
}
