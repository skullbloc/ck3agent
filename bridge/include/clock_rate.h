// clock_rate.h — ck-ijfa: HOW FAST THE WORLD ACTUALLY RAN, measured, in the
// condition the episode actually ran in.
//
// Design of record: docs/design/event-pipeline-conductor.md §2.7. Filed by
// ck-ijfa off ck-6kvq.5.
//
// WHY THIS EXISTS. §2.7's covariate is denominated in GAME-DAYS; its gate is in
// WALL-SECONDS; and every gap has to be recorded in BOTH, at the rate the run
// actually ran at. The conversion between the two units is the clock rate, and
// until this module the bridge never measured it. episode_policy.h's derivation
// was therefore anchored to a rate nobody had measured IN AN ARM, and it cited
// one measured in a different condition (E0 null mode) as though it were
// evidence for the arms — 26x apart. A threshold with no dimensional check is a
// number that can be off by 26x while every test stays green.
//
// WHAT IT MEASURES, precisely: game-days per WALL-second, over the whole window
// from the first sample to the last, AS THE EPISODE EXPERIENCED IT — pauses
// included, speed changes included. That is deliberate. The thresholds are
// converted against how long an agent may be silent in the real run, not
// against a clean tick rate the run never sustained. The condition is published
// beside the number (speeds seen, paused fraction, sample count) because a rate
// without its condition is exactly the defect this module was filed against.
//
// IT NEVER INVENTS A NUMBER. `measured` stays false until the window is long
// enough to mean something (kMinWallSeconds of wall time AND a game-day that
// actually moved). A consumer that finds `measured == false` must say "not
// measured", never substitute an estimate. A world RELOAD (the game day going
// backwards) RE-ANCHORS the window rather than averaging across a discontinuity.
//
// PURE. No engine dependencies; host-tested with no CK3 binary
// (bridge/test/test_clock_rate.cpp). Wall time is passed IN — this module reads
// no clock of its own, so a test can drive a year of episode in a microsecond.

#pragma once

#include "tick_bookkeeping.h"

#include <cstdint>
#include <mutex>

namespace bridge {

// Below this much wall time the quotient is noise: one conductor iteration's
// jitter is a large fraction of the denominator. 60s is the smallest window in
// which a 2-minute heartbeat claim is worth restating at all.
constexpr double kMinWallSeconds = 60.0;

struct ClockRateSample {
    // FALSE means "not measured", and it is not the same as zero. A consumer
    // must publish null / omit rather than substitute a guess.
    bool     measured = false;

    int64_t  first_day     = 0;
    int64_t  last_day      = 0;
    uint64_t first_wall_ms = 0;
    uint64_t last_wall_ms  = 0;

    int64_t  game_days    = 0;     // last_day - first_day
    double   wall_seconds = 0.0;   // (last_wall_ms - first_wall_ms) / 1000
    double   game_days_per_wall_second = 0.0;
    double   game_years_per_wall_hour  = 0.0;   // the same number, the units
                                                // A.19 and the design use

    // THE CONDITION. A rate without one is not evidence (ck-ijfa).
    uint64_t samples        = 0;
    uint64_t paused_samples = 0;
    int      speed_min      = 0;   // 0 == never observed
    int      speed_max      = 0;
    uint64_t reanchors      = 0;   // world reloads that restarted the window
};

class ClockRateMeter {
public:
    // One observation: the world's game-day number, the monotonic wall clock in
    // ms, the game speed setting, and whether the game was paused. Called from
    // the conductor loop beside unavailability().note_day(), which is the one
    // site that already reads both clocks.
    // ck-zffq: THE POLICY COMES IN AND THE RECORDING OWNS IT. It used to be a
    // gate at the CALL SITE, which is a decision living one level away from the
    // thing it governs — and a call site can silently diverge from the policy it
    // is supposed to be following. That is the same shape as the defect this
    // meter was fixed for: a decision at the wrong level, invisible to anything
    // that could check it.
    //
    // Now there is NO call-site gate to get wrong: the call is unconditional and
    // this returns early. Breaking it means editing THE METER, and the meter is
    // driven by clock_start_probe — so the mutant that was uncatchable is caught
    // by the lane that already exists, without a grep.
    //
    // RETURNS BEFORE TAKING THE LOCK, deliberately: E0 x8 runs on the node, and
    // an unconditional call must cost a branch rather than a mutex in the modes
    // that record nothing.
    //
    // THAT ORDERING IS NOT WITNESSED, AND IT IS NOT UNWITNESSABLE — the
    // distinction matters because the first sentence would otherwise be read as
    // the second by whoever inherits this. Moving the return BELOW the lock
    // changes nothing observable: the meter records nothing either way, so it is
    // a COST claim and no behavioural check can see it. A check on source order
    // would be a spelling check, and a timing test measures the machine.
    //
    // IT IS CLOSABLE BY COUNTING LOCK ACQUISITIONS (ck-t5bc): have the meter
    // increment a counter each time it takes the mutex, and a probe driving a
    // NO-RECORD policy asserts it stayed at zero. That converts "the return is
    // before the lock" — a fact about source order — into "the lock is never
    // taken when not recording", a fact about EXECUTION, which reordering cannot
    // fool because reordering is exactly what it observes.
    //
    // DEFERRED ON COST, NOT ON POSSIBILITY: test-only instrumentation on a path
    // that runs every conductor iteration has its own cost and its own risk, and
    // the claim it would defend is a performance note rather than a correctness
    // one. NOT CLOSED, CLOSABLE BY A LOCK-ACQUISITION COUNTER, DEFERRED — so the
    // next person inherits a decision rather than an impossibility.
    void note(const TickBookkeeping& bk, int64_t game_day, uint64_t wall_ms,
              int speed, bool paused);

    ClockRateSample get() const;

    // Test/reset seam, and the ck-hxh7 world-reset hook's entry point.
    void reset();

private:
    mutable std::mutex mu_;
    bool     have_      = false;
    int64_t  first_day_ = 0;
    int64_t  last_day_  = 0;
    uint64_t first_ms_  = 0;
    uint64_t last_ms_   = 0;
    uint64_t samples_   = 0;
    uint64_t paused_    = 0;
    int      speed_min_ = 0;
    int      speed_max_ = 0;
    uint64_t reanchors_ = 0;
};

ClockRateMeter& clock_rate();

// Days per game-year, for the game-days <-> game-years conversion that both the
// design text and Appendix A.19 are written in. CK3's calendar is 365 days with
// no leap year; the bridge's own day numbering is `(raw_date - DATE_EPOCH)/24`,
// which counts those days exactly.
constexpr double kGameDaysPerGameYear = 365.0;

}  // namespace bridge
